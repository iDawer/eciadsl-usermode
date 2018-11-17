/*
  Author : Benoit PAPILLAULT <benoit.papillault@free.fr>
  Creation : 20/07/2001
  Licence : GPL

**************************************************************************
 File           :       $RCSfile: eciadsl-synch.c,v $
 Version        :       $Revision: 1.20 $
 Modified by    :       $Author: flashcode $ ($Date: 2007/04/03 17:34:12 $)
**************************************************************************
  
  Used to initalized the secondary USB device (created by eciadsl-firmware).
  This should synchronize the ADSL link (fixed led).

  Use a file generated by : ./eciadsl-vendor-device.pl < adsl-wan-modem-3.log

  23/11/2001 : Major changes. We use the Wanadoo kit to generate the log.
  On a first time, we will only read/write vendor device.

  31/07/2002, Benoit PAPILLAULT
  Added a verbose mode. By default, few lines are output. This removes
  a bug when eciadsl-synch is launch by initlog (like in boot script).

  17/12/2002 Benoit PAPILLAULT
  We use a shared semaphore to synchronize child and father process.
  The child is required to increment the semaphore whenever it
  receives a packet on the interrupt endpoint (whose size != 64). The father
  decrements the semaphore (hence waiting for the child to increment it).
  
  21/11/2003 Kolja (gavaatbergamoblog.it)
  Enanchement for gs7470 chipset support
  
  07/07/2004 kolja (gavaatbergamoblog.it)
  manage signal interrupt in a special child thrade (fork), 
  check every 10 secs the process status and restart it if necessary
  The entire process is now on cycle (def 10 times) to repeat synch phase 
  if needed.
  Now difference from the 2 GS chipset (GS7070 GS7470) are managed on runtime
  using a eci_device structure contaning all the specials infos inittialized
  on read_config_file but can be changend in any process initialize phase using 
  gsinterface function set_eci_modem_chipset.
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>

#include "pusb.h"
/* modem.h deprecated - changed to common GS interface include - kolja */
/*#include "modem.h"*/
#include "gsinterface.h"
#include "util.h"
#include "semaphore.h"
/* dsp handling interface */
#include "eci-common/interrupt.h"

#define TIMEOUT 2000
#define INFINITE_TIMEOUT 24*60*60*1000 /* 24 hours should be enough */

#define ECILOAD_TIMEOUT 300
/*timeout for a waiting cycle */
#define WAITING_TIMEOUT 20
/*number of retry */
#define NBR_SYNCH_RETRY 10

static unsigned int waiting_timeout = WAITING_TIMEOUT;
static unsigned int nbr_synch_retry = NBR_SYNCH_RETRY;
static unsigned int cnt_synch_retry =0;
static unsigned int nbr_urbs_sent=0;
static unsigned int option_timeout = ECILOAD_TIMEOUT;
static unsigned int cnt_option_timeout=0;

static char* exec_filename;
static int option_verbose = 0;

/* the shared semaphore, defined as a global variable */
static int shared_sem = -1;
/* alt interface value */
static unsigned int altInterface=0;
static char* chipset="GSNONE";

/* thread pointers & attributes - kolja */
static pthread_t th_id_ep_int;
static pthread_t th_id_sig_int;
static pthread_attr_t attr_ep_int;
static pthread_attr_t attr_sig_int;

extern struct config_t config;
extern struct eci_device_t eci_device;

struct int_handling_data
{
	struct timeval tv;
	unsigned short int bWaiting;
	unsigned short int wTimeOut;
};

static struct int_handling_data* intHandData;
static unsigned char sEpResponse[2];	

struct usb_block{
	unsigned char   request_type;
	unsigned char   request;
	unsigned short  value;
	unsigned short  index;
	unsigned short  size;
	unsigned char* buf; /* buf's content is stored in the .bin file
							only for OUT request (ie request_type & 0x80)=0 */
};

/* Dsp structured variable */
static struct gs7x70_dsp eciDeviceDsp;

/* endpoint var for ep int handling */ 
static pusb_endpoint_t ep_int=NULL;


/* for ident(1) command */
static const char id[] = "@(#) $Id: eciadsl-synch.c,v 1.20 2007/04/03 17:34:12 flashcode Exp $";

/* InsertChar:
  Inserts a char in to an array of char at a specified
  position.
*/
static inline char* InsertChar(char* src, char chr, int pos){
	char *target;
	target=malloc((strlen(src)+1));
	strncpy(target, src, strlen(src)+1);
	strcpy((target+pos+1),(src+pos));
    target[pos]=chr;
    return(target);
}

static inline int usb_block_read(FILE* fp, struct usb_block* p){
	unsigned char b[8];
	int r;

	if ((r = fread(b, sizeof(char), sizeof(b), fp)) != sizeof(char)*sizeof(b))
	{
		printf("usb_block_read: read %d bytes instead of %d\n", r, sizeof(char)*sizeof(b));
		return(0);
	}

	p->request_type = b[0];
	p->request      = b[1];
	p->value        = (b[2]<<8) | b[3];
	p->index        = (b[4]<<8) | b[5];
	p->size         = (b[6]<<8) | b[7];

	if (p->size != 0)
	{
		p->buf = (unsigned char*)realloc(p->buf, p->size);
		if (p->buf == NULL)
		{
			printf("usb_block_read: can't allocate %d bytes\n",
				   p->size);
			return(0);
		}

		if ((p->request_type & 0x80) == 0)
			if ((r=fread(p->buf, sizeof(char), p->size, fp)) != p->size)
			{
				printf("usb_block_read: read %d bytes instead of %d\n",
					   r, p->size);
				return(0);
			}
	}

	return(1);
}

static inline void print_char(unsigned char c){

	if (c>=' ' && c<0x7f)
		printf("%c", c);
	/*
	else if (c=='\r' || c=='\n' || c=='\t' || c=='\b')
	printf("%c", c);
	*/
	else
		printf(".");
}
static inline void dump(unsigned char* buf, int len){
  int i, j;
  
	for (i = 0; i < len; i+=16)	{
		for (j = i; j < len && j < i + 16; j++)
			printf("%02x ", buf[j]);
		for (; j < i + 16; j++)
			printf("   ");
		for (j = i; j < len && j < i + 16; j++)
			print_char(buf[j]);
		printf("\n");
	}
	
}

static void sigtimeout(){
	unsigned int wtime=0;
	unsigned int t;
	struct timeval tvn;

	if(intHandData->bWaiting){
		gettimeofday(&tvn, NULL);
		t = ((int)(tvn.tv_sec - intHandData->tv.tv_sec));
		if (option_verbose)
			printf("sigtimeout : waiting time = %d (secs)\n", t);
		if (t>=waiting_timeout){
			intHandData->wTimeOut=1;
			wtime = waiting_timeout;
			if (option_verbose)
			printf("sigtimeout : TIMEOUT REACHED!!\n");
            if (semaphore_incr(shared_sem, 1) == -1){
				intHandData->wTimeOut=99;            	
                printf("\rERROR eciadsl-synch: error incrementing the shared semaphore from timing interrupt function \n");
				fflush(stdout);
				return;	
            }
		}else{
			wtime = waiting_timeout - t;
		}
	}else{
		wtime = waiting_timeout;
	}
	cnt_option_timeout+= wtime;
	if (option_verbose)
		printf("timeout : %d | reached : %d | next waiting : %d\n",option_timeout, cnt_option_timeout, wtime);
	if(option_timeout <cnt_option_timeout){
		intHandData->wTimeOut=99;
		printf("\rERROR eciadsl-synch: timeout                                             \n");
		return;
	}
	alarm(wtime);
}

static inline void * fn_handle_int_signal(void* ignore){

	fflush(stdout);
	fflush(stderr);

	signal(SIGALRM, sigtimeout);
	
	alarm(waiting_timeout);
	while(1){
		sleep(5);
		if(intHandData->wTimeOut==99){
		if (option_verbose)
			printf("SIGTIMEOUT - Ended cause error\n");
			break;
		}
	}
	if (option_verbose)
		printf("SIGTIMEOUT - Ended correctly\n");
	return(ignore);
}

static void* fn_read_endpoint(void * ignore){
	static unsigned char lbuf[0x40];
	static int ret;
	/* indicates if we already received 16 bytes packets */
	static int pkt16_received = 0;

	fflush(stdout);
	fflush(stderr);

	for (;;)	{
		/*
		  24/11/2001
		  Warning : we use a TIMEOUT. This is very important and
		  this is caused by the current implementation of the pusb library.
		  Without a timeout, we can have several process trying to reap URB at
		  once. And what can happen and what happens really, is that one
		  process reaps the URB from the other process. As as result, it mixes
		  data from various endpoint

		  Solution : we use a big timeout. We assume that 24 hours should be 
		  enought.

		  25/11/2001
		  Warning again : if we use a TIMEOUT, pusb implementation on Linux
		  use USBDEVFS_BULK, which locks down a big semaphore and then wait.
		  Thus preventing any other ioctl() and URB to be submitted on the
		  device. So we revert to the old behaviour : NO TIMEOUTS ...
		*/

		ret = pusb_endpoint_read_int(ep_int, lbuf, sizeof(lbuf));
		
		if (ret < 0){
			printf("\r ERROR reading interrupts                              \n");
			break;
		}
		
		/*Copy bytes to shared field - kolja*/
	    memcpy(sEpResponse,(lbuf+4),2);
		
		if (option_verbose){
			printf("endpoint Int, packet len = %d\n", ret);
			dump(lbuf, ret);			
			fflush(stdout);
			fflush(stderr);
		}

		/* try to stop the sleep() function in our father, seems to work */
		/* Take off epInt value check cause problem compare it to e #define value
		 * shouldn't receive ep different from Interrupt one in this phase.. - kolja
		 * some modem receive also 0 length response and we do not need to consider it
		 * (thnx giancarlo) may-2004 - kolja*/
		 
		if (ret!=0 && ret != 64){
			/*check dsp status - kolja */
			dsp_parse_interrupt_buffer(lbuf, ret, &eciDeviceDsp);
            fflush(stdout);
            if (semaphore_incr(shared_sem, 1) == -1){
                printf("\rERROR eciadsl-synch: error incrementing the shared semaphore               \n");
				fflush(stdout);
            }
        }

		if (ret == 16){
			pkt16_received = 1;
		}

		/* we consider that a 64 bytes packet means that the line is
		   synchronized. So we stop. We wait first to have received
		   a 16 bytes packets, in order to be sure that this 64 bytes
		   packet is not an unread packet from a previous instance of this
		   program (or another)
		*/

		if (pkt16_received && ret == 64)
			break;
		
		if (intHandData->wTimeOut==99){
			if (option_verbose)
				printf("\rERROR - Ep interrupt process stopped (timedout)                  \n");
				break;
		}
	}
	return(ignore);
}

static inline int eci_load2_send_synch_data(char* file, pusb_device_t dev, short unsigned int doCycle){
	unsigned char sEpResponseOK[2];
	struct usb_block block;
 	FILE* fp;
	long size;
	int r, i;
	struct timeval tv2;
	
	if (option_verbose)
		printf("urb data file : %s  \n", file);

	fp = fopen(file, "rb");
	if (fp == NULL)	{
		perror(file);
		return(0);
	}

	/* compute the file size */

	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	if (doCycle==1){
		if ((r = fread(sEpResponseOK, sizeof(char), 2, fp)) != sizeof(char)*2){
			printf("read ep Reponse OK : read %d bytes instead of %d\n", r, sizeof(char)*2);
		return(0);
	}
		if (option_verbose)
			printf ("responseOK : %02x%02x\n", sEpResponseOK[0], sEpResponseOK[1]);
	}

	memset(&block, 0, sizeof(block));
	intHandData->bWaiting =0;
	while ((ftell(fp) < size) && (doCycle==0 || (memcmp(sEpResponse, sEpResponseOK,2)!=0)))	{
		nbr_urbs_sent++;
	    /*printf ("test : 0 - %02x %02x ; 1 - %02x %02x\n", sEpResponse[0], sEpResponseOK[0], sEpResponse[1], sEpResponseOK[1]);*/
		if (!usb_block_read(fp, &block)){
			printf("can't read block\n");
			pusb_close(dev);
			fclose(fp);
			return(0);
		}

		if (!option_verbose)
				printprogres();

		if (option_verbose)
			printf ("Block %3d: request_type=%02x request=%02x value=%04x index=%04x size=%04x\n",
					nbr_urbs_sent, block.request_type, block.request, block.value,block.index, block.size);

		if ((r=pusb_control_msg(dev, block.request_type, block.request,
							 block.value, block.index, block.buf,
							 block.size, TIMEOUT)) != block.size){
			if (option_verbose)
				printf("failed r=%d\n", r);

			pusb_close(dev);
			fclose(fp);
			return(0);
		}

		if (r != block.size)
			printf("(expected %d, got %d) ", block.size, r);

		if (option_verbose){
			if ((block.request_type & 0x80))
			{
				for (i = 0; i < r; i++)
					printf("%02x ", block.buf[i]);
				printf("\n");
			}
		}

		if (block.request_type == 0x40 && block.request == 0xdc
			&& block.value == 0x00 && block.index == 0x00)
		{
			if (option_verbose)	{
				printf("waiting.. ");
				fflush(stdout);
				fflush(stderr);
			}
			gettimeofday(&intHandData->tv, NULL);
            /* decrement the shared semaphore, thus waiting for the child */
			intHandData->bWaiting=1;
			
            if (semaphore_decr(shared_sem, 1) == -1){
                printf("\ERROR eciadsl-synch: error decrementing the shared semaphore\n");
            }
			intHandData->bWaiting=0;
			if (option_verbose){
				gettimeofday(&tv2, NULL);
				printf("waited for %ld ms\n",
					   (long)(tv2.tv_sec - intHandData->tv.tv_sec) * 1000
					   + (long)(tv2.tv_usec - intHandData->tv.tv_usec) / 1000);
		}

            if (intHandData->wTimeOut!=0){
				fclose(fp);
				if(intHandData->wTimeOut==1){
					intHandData->wTimeOut=0;
				    if (option_verbose)
				    	printf("timeout reached, restarting synch sequence ----\n");					
					return(-99);							
				}else{
					return(0);					
				}
            }
		}
		fflush(stdout);
		fflush(stderr);

	/*restart reading STEP1 file ('b' file) if no valid EP response received and file is at the and  - kolja*/
		if(ftell(fp)==size && doCycle==1){
			fseek(fp, 2, SEEK_SET);
		}	
	}
	fclose(fp);
	return(1);		
}

static inline int eci_load2(char* file, unsigned short vid2, unsigned short pid2){
	FILE* fp ;
	long foo;
	uint approx_timeout;
	pusb_device_t dev;
	int ret;
	long size;	
	unsigned short int doRetry;

	/* open the file */

	fp = fopen(file, "rb");
	if (fp == NULL)	{
		perror(file);
		return(0);
	}

	/* compute the file size */
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	/* divide by 1024, div0 exception free */
	approx_timeout=size>>10;
	/* substract 1/1000 of its squared elevation (non linear) */
	/* TODO : should be found a better way to calculate timeout - 
	 *               GS7470 in same cases has more than one synch file - kolja */
	foo=(approx_timeout*approx_timeout)/1000;
	approx_timeout-=foo;
	if (approx_timeout<ECILOAD_TIMEOUT)
		approx_timeout=ECILOAD_TIMEOUT;
	if (option_timeout==0)
		option_timeout=approx_timeout;
	if (option_verbose)
		printf("timeout set to %usec\n", option_timeout);

	/* open the USB device */
	dev = pusb_search_open(vid2, pid2);
	if (dev == NULL){
		printf("\rERROR can't find your GlobeSpan %s USB ADSL WAN Modem \n", get_chipset_descr(eci_device.eci_modem_chipset));
		fclose(fp);
		return(0);
	}

	/* initialize the USB device */
	if (pusb_set_configuration(dev, 1) < 0){
		printf("\rERROR can't set configuration 1                               \n");
		pusb_close(dev);
		fclose(fp);
		return(0);
	}

	if (pusb_claim_interface(dev, 0) < 0){
		printf("\rERROR can't claim interface 0                                  \n");
		pusb_close(dev);
		fclose(fp);
		return(0);
	}

	/* warning : orginal setting is 0,4 (source : Windows driver) */
	if (pusb_set_interface(dev, 0, altInterface) < 0){
		printf("\rERROR can't set interface 0 to use alt setting %d             \n", altInterface);
		pusb_close(dev);
		fclose(fp);
		return(0);
	}
	
	/* retreive altiface endpoint infos - kolja */
	gsGetDeviceIfaceInfo(dev, altInterface);	

	/*open ep int endpoint*/
	/* here, we are running in a child process */
	ep_int = pusb_endpoint_open(dev, eci_device.eci_int_ep, O_RDONLY);
	if (ep_int == NULL)
	{
		printf("\rERROR can't set interrupt endpoint %d                         \n", eci_device.eci_int_ep);
		pusb_close(dev);
		fclose(fp);
		return(0);
	}

	/* sig int thread handler - kolja*/
	pthread_attr_init(&attr_sig_int);
	pthread_attr_setdetachstate(&attr_sig_int, PTHREAD_CREATE_DETACHED);

	if (pthread_create(&th_id_sig_int, &attr_sig_int, fn_handle_int_signal, NULL) != 0)
	{
		printf("error creating thread - [SIG INT handler]                       \n");
		return(0);
	}
	
	/* ep int thread handler - kolja*/
	pthread_attr_init(&attr_ep_int);
	pthread_attr_setdetachstate(&attr_ep_int, PTHREAD_CREATE_DETACHED);

	if (pthread_create(&th_id_ep_int, &attr_ep_int, fn_read_endpoint, NULL) != 0)
	{
		printf("error creating thread - [EP INT handler]                        \n");
		return(0);
	}

	ret=1;
	doRetry = 1;	
	while((nbr_synch_retry==0 || cnt_synch_retry<nbr_synch_retry) && doRetry){
		doRetry=0;
		cnt_synch_retry++;

		if (option_verbose)
			printf ("RETRY : %d (until %d)\n", cnt_synch_retry, nbr_synch_retry);
		if (config.modem_chipset== ECIDC_GS7470){
		    char* afile;
		    char* bfile;

		    afile = InsertChar(file, 'a', strlen(file)-4);
		    bfile = InsertChar(file, 'b', strlen(file)-4);
			fp = fopen(afile, "rb");
			if (fp == NULL){
				if (option_verbose){
					printf("File init Synch missing.. try to go through (file : %s)\n", afile);
				}
			}else{
				fclose(fp);			
				ret = eci_load2_send_synch_data(afile, dev, 0);
				if (ret==-99){
					ret=1;
					doRetry=1;
				}
				if (!ret){
					pusb_endpoint_close(ep_int);
					pusb_release_interface(dev, 0);
					pusb_close(dev);
					printf("\rERROR eciadsl-synch INITIALIZING: failed                                     \n");
					fflush(stdout);
					return(0);
				}						
				if (!doRetry)	
					ret = eci_load2_send_synch_data(bfile, dev, 1);
				if (ret==-99){
					ret=1;
					doRetry=1;
				}
				if (!ret){
					pusb_endpoint_close(ep_int);
					pusb_release_interface(dev, 0);
					pusb_close(dev);
					printf("\rERROR eciadsl-synch CYCLING: failed                                     \n");
					fflush(stdout);
					return(0);
				}
			}
		}
		if (!doRetry){	
			ret = eci_load2_send_synch_data(file, dev, 0);
			if (ret==-99){
				ret=1;
				doRetry=1;
			}
			if (!ret){
				pusb_endpoint_close(ep_int);
				pusb_release_interface(dev, 0);
				pusb_close(dev);			
				printf("\rERROR eciadsl-synch SYNCHING: failed                                     \n");
				fflush(stdout);
				return(0);
			}
		}
	}

	if((nbr_synch_retry!=0 && cnt_synch_retry>=nbr_synch_retry))
		return(0);	
		
	pusb_endpoint_close(ep_int);
	/*pusb_release_interface(dev, 0);*/
	pusb_close(dev);
	return(1);
}

static inline void version(const int full){
	printf("%s, part of " PACKAGE_NAME PACKAGE_VERSION " (" __DATE__ " " __TIME__ ")\n",
			exec_filename);
	if (full)
		printf("%s\n", id);
	_exit(0);
}

static inline void usage(const int ret)
{
	printf(	"usage:\n"
			"       %s [<switch>..] [[VID2 PID2] <synch.bin>]\n", exec_filename);
	printf(	"switches:\n"
			"       -v or --verbose               be verbose\n"
			"       -h or --help                  show this help message then exit\n"
			"       -V or --version               show version information then exit\n"
			"       -t <num> or --timeout <num>   override the default timeout value (in sec)\n");
	printf(	"       -wt <num> or --waiting_timeout <num>   override the default timeout value used for waiting ep response time (in sec)\n"
			"       -r <num> or --retries <num>   number of retries for an invalid ep response (0 = infinite)\n"
			"       -alt <num> or --alt_interface <num>   override the default altInterface value\n"			
			"       -mc <string> or --modem_chipset <string>   override the default modem chipset value (could be GS7070[default] or GS7470)\n"
			"\n");
	printf(	"If ALL other parameters are omitted, the ones from " CONF_DIR "/eciadsl.conf\n"
			"are assumed.\n"
			"If only the synch.bin is passed, default VID/PID for ECI HiFocus modem\n"
			"are assumed if no VID/PID can be found in the config file.\n");
	_exit(ret);
}

int main(int argc, char** argv){
	static char* file;
	int /*status,*/ ret;
	static unsigned int vid2;
	static unsigned int pid2;
	static int i, j;

  	th_id_ep_int = th_id_sig_int = 0;

	intHandData = (struct int_handling_data*)malloc(sizeof(struct int_handling_data));
	if(!intHandData){
        printf("\rERROR eciadsl-synch: failed initializing data memory\n");
		fflush(stdout);
        return(-1);
	}	

	/* read the configuration file */
	read_config_file();
	
	exec_filename=basename(*argv);

	/* parse command line options */
	for (i = 1, j = 1; i < argc; i++)
	{
		if ((strcmp(argv[i], "-V") == 0) || (strcmp(argv[i], "--version") == 0))
			version(0);
		else
		if (strcmp(argv[i], "--full-version") == 0)
			version(1);
		else
		if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0))
			usage(0);
		else
		if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--verbose") == 0))
			option_verbose = 1;
		else
		if (((strcmp(argv[i], "-t") == 0) || (strcmp(argv[i], "--timeout") == 0) ) && (i + 1 < argc))
			get_unsigned_value(argv[++i], &option_timeout);
		else
		if (((strcmp(argv[i], "-wt") == 0) || (strcmp(argv[i], "--waiting_timeout") == 0) ) && (i + 1 < argc))
			get_unsigned_value(argv[++i], &waiting_timeout);
		else
		if (((strcmp(argv[i], "-r") == 0) || (strcmp(argv[i], "--retries") == 0) ) && (i + 1 < argc))
			get_unsigned_value(argv[++i], &nbr_synch_retry);
		else
		if (((strcmp(argv[i], "-alt") == 0) || (strcmp(argv[i], "--alt_interface") == 0) ) && (i + 1 < argc))
			get_unsigned_value(argv[++i], &altInterface);
		else
		if (((strcmp(argv[i], "-mc") == 0) || (strcmp(argv[i], "--modem_chipset") == 0) ) && (i + 1 < argc))
			strDup(&chipset, argv[++i]);
		else		
			argv[j++] = argv[i];
	}

	argc = j;
	
	if (argc == 2){
		file = argv[1];
		if (config.vid2)
			vid2 = config.vid2;
		else
			vid2 = GS_VENDOR;
		if (config.pid2)
			pid2 = config.pid2;
		else
			pid2 = GS_PRODUCT;
	}
	else
	if (argc == 4)
	{
		file = argv[3];
		get_hexa_value(argv[1], &vid2);
		get_hexa_value(argv[2], &pid2);
	}
	else
	{
		/* try to assume default values from the config file */
		if (config.vid2 && config.pid2 && config.synch)
		{
			vid2 = config.vid2;
			pid2 = config.pid2;
			file = config.synch;
		}
		else
		{
			printf("no default parameters found in config file, couldn't assume default values\n");
			usage(-1);
		}
	}

	if (strcmp(chipset,"GSNONE")!=0){
		set_eci_modem_chipset(chipset);
	}

	if (altInterface==0){
		altInterface = config.alt_interface_synch;
	}
		
    /* create the shared semaphore with a count of 0 */
    shared_sem = semaphore_init(0);
    if (shared_sem == -1){
        printf("\rERROR eciadsl-synch: failed to create shared semaphore");
		fflush(stdout);
        return(-1);
    }

	if (init_pusb()==-1) {
		printf("Error during pusb pointers malloc\nDriver will abort!\n");
		exit(1);
	}

	eciDeviceDsp.type = eci_device.eci_modem_chipset;

	ret=eci_load2(file, vid2, pid2);

	if (!ret){
		printf("\rERROR eciadsl-synch: failed                                                         \n");
	}else{
		printf("\rOK eciadsl-synch: success                                                           \n");	
	}
	
	fflush(stdout);

	semaphore_done(shared_sem);

	/* kill process handling ep Int urb messages */
  	if(th_id_ep_int)
  		kill(th_id_ep_int, SIGTERM);

	/* kill process handling signal interrupt */
	if(th_id_sig_int)
		kill(th_id_sig_int, SIGTERM);

	/* Free pointers! */
	deinit_pusb();

	free(intHandData);
	
	if (!ret) 
		return 1;
		
	return(0);
}