/*
 *	Interupt.h
 * 
 *	Author : Valette Jean-S�bastien
 *
 * Creation : 30 09 2004
 * 
 *
 * ****************************************************************************
 * File		: 	$RCSfile: interrupt.h,v $
 * Version	:	$Revision: 1.4 $
 * Modified by	:	$Author: kolja_gava $ ($Date: 2005/10/20 07:43:42 $)
 * Licence	:	GPL
 * ****************************************************************************
 * 
 * Include for the dsp int buffer handling.
 * 
*/

#ifdef _KERNEL_
#include <linux/string.h>
#else
#include <string.h>
#endif
#include <linux/errno.h>
#include "dsp.h"

#define INT_MAXPIPE_SIZE 64
#define INT_BUFFER_LEN (INT_MAXPIPE_SIZE *2)
#define INT_MIN_BUFFER_SIZE 8

union ep_int_buf {
	unsigned char raw[INT_BUFFER_LEN];
	struct {
		unsigned char status;				/* 0xf0	-- 0*/
		unsigned char frameid;				/* used to indicate which frame is sent
											   go one by one step, some are unused -- 1*/
		unsigned char unknown1[2];			/* 2 unknown bytes  -- 2 -- 3*/
		unsigned char eoc[36];				/* 36 byte eoc buffer -- 4 -- 39*/
		unsigned char status_buffer[24];	/* 2 unknown bytes  -- 40 -- 64*/
	}buffer_7470;
	struct {
		unsigned char status;				/* 0xf0	-- 0*/
		unsigned char unknown0[2];			/* 2 unknown bytes  -- 2 -- 3*/
		unsigned char frameid;				/* used to indicate which frame is sent
											   go one by one step, some are unused -- 1*/
		unsigned char unknown1[2];			/* 2 unknown bytes  -- 2 -- 3*/
		unsigned char eoc[34];				/* 36 byte eoc buffer -- 4 -- 39*/
		unsigned char status_buffer[24];	/* 2 unknown bytes  -- 40 -- 64*/
	}buffer_7070;
	struct {
		unsigned char	RequestType;		/* A1 ? */
		unsigned char	Notification; 		/* F0 for DSP */
		u_int16_t		Value;				/* Value ?	*/
		u_int16_t		Index; 				/* DSP Number, alway 1 ? */
		u_int16_t      wLength; 			/* Buffer len ? */
		unsigned char Data[1];				/* Buffer data */	
	} small_buffer;
};

inline void dsp_parse_status(union ep_int_buf *buffer,  struct gs7x70_dsp *dsp);
inline int dsp_parse_interrupt_buffer(unsigned char *buffer, int buffer_size,
							struct gs7x70_dsp *dsp);
