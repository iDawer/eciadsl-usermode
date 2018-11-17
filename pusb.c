/*********************************************************************
 File		: 	$RCSfile: pusb.c,v $
 Version	:	$Revision: 1.3 $
 Modified by	:	$Author: jeanseb $ ($Date: 2002/06/10 17:49:33 $)
 Licence	:	GPL
*********************************************************************/

/* Simple portable USB interface */

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include "pusb-bsd.c"
#elif defined(__linux__)
#include "pusb-linux.c"
#else
#error Unknown operating system
#endif
