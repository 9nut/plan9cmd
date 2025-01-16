/*
	Copyright (c) 1997,1998 Eugene G. Crosser
	Copyright (c) 1998 Bruce D. Lightner (DOS/Windows support)

	You may distribute and/or use for any purpose modified or unmodified
	copies of this software if you preserve the copyright notice above.

	THIS SOFTWARE IS PROVIDED AS IS AND COME WITH NO WARRANTY OF ANY
	KIND, EITHER EXPRESSED OR IMPLIED.  IN NO EVENT WILL THE
	COPYRIGHT HOLDER BE LIABLE FOR ANY DAMAGES RESULTING FROM THE
	USE OF THIS SOFTWARE.
*/

#include <u.h>
#include <libc.h>
#include <bio.h>
#include "eph_io.h"

#define RETRIES              5

#define INITTIMEOUT    3000000L
#define DATATIMEOUT     200000L
#define BIGDATATIMEOUT 1500000L
#define ACKTIMEOUT      400000L
#define BIGACKTIMEOUT   800000L
#define EODTIMEOUT      400000L
#define CMDTIMEOUT    15000000L

/* Bruce and others say that adding 1ms delay before all writes is good.
   I think that they should rather be fine-tuned. */
#ifndef OTHERDELAY
#define WRTPKTDELAY       1250L
#define WRTCMDDELAY       1250L
#define WRTPRMDELAY       1500L
#define WRTDELAY          2000L
#else
#define WRTPKTDELAY        250L
#define WRTCMDDELAY        250L
#define WRTPRMDELAY        500L
#define WRTDELAY          1000L
#endif
#define SPEEDCHGDELAY   100	/* msec */

#define SKIPNULS           200

typedef struct Pkthdr Pkthdr;
struct Pkthdr {
	unsigned char typ;
	unsigned char seq;
};

enum {
	/* protocol characters */
	ACK = 0x06,
//	DC1 = 0x11,
	NAK = 0x15,
	/* NAK = 0x11, */
	SIG = 0x15,	/* initial signature byte */

	/* commands */
	CMD_SETINT = 0,
	CMD_GETINT = 1,
	CMD_ACTION = 2,
	CMD_SETVAR = 3,
	CMD_GETVAR = 4,

	/* packet types (Pkthdr.typ) */
	PKT_CMD = 0x1B,
	PKT_DATA = 0x02,
	PKT_LAST = 0x03,

	/* command packet subtype in seq field (Pkthdr.seq) */
	SEQ_INITCMD = 0x53,
	SEQ_CMD = 0x43,
};

void eph_error(Camio *iob,int err,char *fmt,...);
static int flushinput(Camio *iob);
static void writeinit(Camio *iob);
static void writeack(Camio *iob);
static void writenak(Camio *iob);
static int waitack(Camio *iob,long usec);
static int waitcomplete(Camio *iob);
static int waitsig(Camio *iob);
static int waiteot(Camio *iob);

static int writepkt(Camio *iob,int typ,int seq,void *data,long length);
static int writecmd(Camio *iob,void *data,long length);
static int writeicmd(Camio *iob,void *data,long length);
static int readpkt(Camio *iob,Pkthdr *pkthdr,void *buf,long *length,long usec);

static int setispeed(Camio *iob,long val);

#define	ERRNO	0

#define DEFSPEED 19200

int
eph_open(Camio *iob,char *devname,long speed)
{
	char ctlname[100];
	long ephspeed;
	int rc;
	int count=0;

	if (speed == 0) speed=MAX_SPEED;

	switch (speed) {
	case 9600:
		ephspeed=1;	break;
	case 19200:
		ephspeed=2;	break;
	case 38400:
		ephspeed=3;	break;
	case 57600:
		ephspeed=4;	break;
	case 115200:
		ephspeed=5;	break;
	default:
		eph_error(iob,ERR_BADSPEED,"specified speed %ld invalid",speed);
		return -1;
	}

	iob->timeout=DATATIMEOUT+((2048000000L)/speed)*10;
	if (iob->debug) print("set timeout to %lud\n",DATATIMEOUT+iob->timeout);

	if ((iob->fd=open(devname,ORDWR)) < 0) {
		if (strlen(devname) < 400) /* we have 512 byte buffer there */
			eph_error(iob,ERRNO,"open %s error %r", devname);
		return -1;
	}
	sprint(ctlname, "%sctl", devname);
	iob->cfd = open(ctlname, ORDWR);
	if(iob->cfd >= 0){
		fprint(iob->cfd, "b%d", DEFSPEED);
		/* other parameters are fine */
//		fprint(iob->cfd, "m1");
		fprint(iob->cfd, "i1");
		/* could set xon/xoff flow control */
		fprint(iob->cfd, "q65536");
	}

	do {
		if (flushinput(iob)) {
			eph_error(iob,ERRNO,"error flushing input: %r");
			close(iob->cfd);
			close(iob->fd);
			return -1;
		}
		writeinit(iob);
		rc=waitsig(iob);
		if (rc)
			sleep(3);
	} while (rc && (count++ < RETRIES));
	if (rc) {
		close(iob->cfd);
		close(iob->fd);
		return -1;
	}

	if (setispeed(iob,ephspeed)) {
		eph_error(iob,ERRNO,"could not switch camera speed %d: %r",ephspeed);
		close(iob->cfd);
		close(iob->fd);
		return -1;
	}

	if(iob->cfd >= 0 && fprint(iob->cfd, "b%ld", speed) < 0){
		close(iob->cfd);
		close(iob->fd);
		return -1;
	}

	sleep(SPEEDCHGDELAY);
	return 0;
}

int
eph_close(Camio *iob,int switchoff)
{

	if (switchoff) {
		char zero=0;

		eph_action(iob,4,&zero,1);
		/* Oly 600 does not send EOT if switched off by command
		waiteot(iob); */
	} else {
		setispeed(iob,0L);
	}

	if(iob->cfd >= 0){
		close(iob->cfd);
		iob->cfd = -1;
	}
	return close(iob->fd);
}

/*
 * eph commands
 */

enum {
	EPHBSIZE = 2048,
	TMPBUF_SIZE = EPHBSIZE,
};

#define MAYRETRY(rc) ((rc == -2) || (rc == NAK))

static int
writecmd(Camio *iob, void *data, long length)
{
	return writepkt(iob,PKT_CMD,SEQ_CMD,data,length);
}

static int
writeicmd(Camio *iob, void *data, long length)
{
	return writepkt(iob,PKT_CMD,SEQ_INITCMD,data,length);
}

static int
setispeed(Camio *iob,long val)
{
	unsigned char buf[6];
	int rc;
	int count=0;

	buf[0]=CMD_SETINT;
	buf[1]=REG_SPEED;
	buf[2]=(val)&0xff;
	buf[3]=(val>>8)&0xff;
	buf[4]=(val>>16)&0xff;
	buf[5]=(val>>24)&0xff;
	do {
		if ((rc=writeicmd(iob,buf,6))) return rc;
		rc=waitack(iob,ACKTIMEOUT);
	} while (rc && (count++ < RETRIES));
	if (count >= RETRIES)
		eph_error(iob,ERR_EXCESSIVE_RETRY,
				"excessive retries on setispeed");
	return rc;
}

int
eph_setint(Camio *iob, int reg, long val)
{
	uchar buf[6];
	int rc;
	int count=0;

	buf[0]=CMD_SETINT;
	buf[1]=reg;
	buf[2]=(val)&0xff;
	buf[3]=(val>>8)&0xff;
	buf[4]=(val>>16)&0xff;
	buf[5]=(val>>24)&0xff;

	do{
		if ((rc = writecmd(iob,buf,6)) != 0)
			return rc;
		rc = waitack(iob,(reg == REG_FRAME)?BIGACKTIMEOUT:ACKTIMEOUT);
		if(!MAYRETRY(rc))
			return rc;
	}while(count++ < RETRIES);
	eph_error(iob, ERR_EXCESSIVE_RETRY, "excessive retries on setint");
	return rc;
}

int
eph_getint(Camio *iob,int reg,long *val)
{
	unsigned char buf[4];
	Pkthdr pkt;
	int rc;
	long size=4;
	int count=0;

	(*val)=0L;
	buf[0]=CMD_GETINT;
	buf[1]=reg;

writeagain:
	if ((rc=writecmd(iob,buf,2))) return rc;
readagain:
	rc=readpkt(iob,&pkt,buf,&size,BIGDATATIMEOUT);
	if (MAYRETRY(rc) && (count++ < RETRIES)) goto writeagain;
	if ((rc == 0) && (pkt.typ == PKT_LAST) && (pkt.seq == 0)) {
		(*val)=((unsigned long)buf[0]) | ((unsigned long)buf[1]<<8) |
			((unsigned long)buf[2]<<16) | ((unsigned long)buf[3]<<24);
		writeack(iob);
		return 0;
	} else if ((rc == -1) && (count++ < RETRIES)) {
		writenak(iob);
		goto readagain;
	}
	if (count >= RETRIES)
		eph_error(iob,ERR_EXCESSIVE_RETRY,
				"excessive retries on getint");

	return rc;
}

int
eph_action(Camio *iob,int reg,char *val,long length)
{
	unsigned char buf[EPHBSIZE+2];
	int rc;
	int count=0;

	if (length > EPHBSIZE) {
		eph_error(iob, ERR_DATA_TOO_LONG, "arg action length %ld", length);
		return -1;
	}

	buf[0]=CMD_ACTION;
	buf[1]=reg;
	memmove(buf+2, val, length);

writeagain:
	if ((rc=writecmd(iob,buf,length+2))) return rc;
	rc=waitack(iob,ACKTIMEOUT);

	if (MAYRETRY(rc) && (count++ < RETRIES)) goto writeagain;

	if (rc == 0) rc=waitcomplete(iob);
	if (count >= RETRIES)
		eph_error(iob, ERR_EXCESSIVE_RETRY, "excessive retries on action");
	return rc;
}

int
eph_setvar(Camio *iob,int reg,void *val,long length)
{
	unsigned char buf[EPHBSIZE];
	int rc=0,seq=-1;
	int count=0;
	int pkttyp,pktseq;
	long pktsize,maywrite;
	long written=0;
	unsigned char *getpoint,*putpoint;

	getpoint=val;
	while (length && !rc) {
		if (seq == -1) {
			pkttyp=PKT_CMD;
			pktseq=SEQ_CMD;
			buf[0]=CMD_SETVAR;
			buf[1]=reg;
			putpoint=buf+2;
			maywrite=sizeof(buf)-2;
			pktsize=2;
		} else {
			pkttyp=PKT_DATA;
			pktseq=seq;
			putpoint=buf;
			maywrite=sizeof(buf);
			pktsize=0;
			(iob->runcb)(written);
		}
		if (length <= maywrite) {
			maywrite=length;
			if (pkttyp == PKT_DATA) pkttyp=PKT_LAST;
		}
		memcpy(putpoint,getpoint,maywrite);
		pktsize+=maywrite;
		length-=maywrite;
		getpoint+=maywrite;
		written+=maywrite;
		seq++;
writeagain:
		if ((rc=writepkt(iob,pkttyp,pktseq,buf,pktsize)))
			return rc;
		rc=waitack(iob,ACKTIMEOUT);
		if (MAYRETRY(rc) && (count++ < RETRIES)) goto writeagain;
	}
	if (count >= RETRIES)
		eph_error(iob,ERR_EXCESSIVE_RETRY,
				"excessive retries on setvar");
	return rc;
}

int
eph_getvar(Camio *iob,int reg,char **buffer,long *bufsize)
{
	unsigned char buf[2];
	Pkthdr pkt;
	int rc;
	int count=0;
	unsigned char expect=0;
	long index;
	long readsize;
	char *ptr;
	char *tmpbuf=nil;
	long tmpbufsize=0;

	if (iob->debug) print("just inside eph_getvar\n");	// fst

	if ((buffer == nil) && (iob->storecb == nil)) {
		eph_error(iob,ERR_BADARGS,
			"nil buffer and no store callback for getvar");
		return -1;
	}

	if (buffer == nil) {
		tmpbuf = malloc(TMPBUF_SIZE);
		tmpbufsize=TMPBUF_SIZE;
		if (tmpbuf == nil) {
			eph_error(iob,ERR_NOMEM,
				"could not alloc %lu for tmpbuf in getvar",
				(long)TMPBUF_SIZE);
			return -1;
		}
	}

	buf[0]=CMD_GETVAR;
	buf[1]=reg;

writeagain:
	if ((rc=writecmd(iob,buf,2))) return rc;
	index=0;
readagain:
	if (buffer) { /* read to memory reallocating it */
		if ((*bufsize - index) < 2048) {
			if (iob->debug)
				print("reallocing %lud",(unsigned long)(*bufsize));
			/* multiply current size by 2 and round up to 2048 boundary */
			*bufsize =(((*bufsize * 2)-1)/2048+1)*2048;
			if (iob->debug)
				print(" -> %lud\n",(ulong)*bufsize);
			*buffer = realloc(*buffer,*bufsize);
			if (*buffer == nil) {
				eph_error(iob,ERR_NOMEM, "could not realloc %lud for getvar",
					(long)*bufsize);
				return -1;
			}
		}
		ptr=(*buffer)+index;
		readsize=(*bufsize)-index;
	} else { /* pass data to store callback */
		ptr=tmpbuf;
		readsize=tmpbufsize;
	}
	rc=readpkt(iob,&pkt,ptr,&readsize,
			(expect || ((reg != REG_IMG) || (reg != REG_TMN)))?
						DATATIMEOUT:BIGDATATIMEOUT);
	if (MAYRETRY(rc) && (expect == 0) && (count++ < RETRIES)) {
		writenak(iob);
		if (rc == -2) goto readagain;
		else goto writeagain;
	}
	if ((rc == 0) &&
	    ((pkt.seq == expect) || (pkt.seq  == (expect-1)))) {
		count=0;
		if (pkt.seq == expect) {
			index+=readsize;
			expect++;
			(iob->runcb)(index);
			if (buffer == nil) {
				if (iob->debug)
					print("storing %lud at %08lux\n",
						(unsigned long)readsize,
						(unsigned long)ptr);
				if ((iob->storecb)(ptr,readsize))
					return -1;
			}
		}
		writeack(iob);
		if (pkt.typ == PKT_LAST) {
			if (buffer) (*bufsize)=index;
			if (tmpbuf) free(tmpbuf);
			return 0;
		}
		else goto readagain;
	}
	if ((rc <= 0) && (count++ < RETRIES)) {
		writenak(iob);
		goto readagain;
	}
	if (tmpbuf) free(tmpbuf);
	if (count >= RETRIES)
		eph_error(iob,ERR_EXCESSIVE_RETRY,
				"excessive retries on getvar");
	return rc;
}

/*
 * packet i/o
 */

static void
shortsleep(int nsec)
{
	sleep((nsec+999)/1000+1);
}

static struct _chunk {
	long offset;
	long size;
	unsigned long delay;
} chunk[] = {
	{	0,	1,	WRTPKTDELAY	},
	{	1,	3,	WRTCMDDELAY},
	{	4,	0,	WRTPRMDELAY	}
};
#define MAXCHUNK 3

static int
writepkt(Camio *iob,int typ,int seq,void *adata,long length)
{
	ushort crc=0;
	uchar buf[2054];
	int i, j;
	uchar *data = adata;

	if (length > (sizeof(buf)-6)) {
		eph_error(iob,ERR_DATA_TOO_LONG,
			"trying to write %ld in one pkt",(long)length);
		return -1;
	}

	buf[0]=typ;
	buf[1]=seq;
	buf[2]=length&0xff;
	buf[3]=length>>8;
	i = 4;
	for (j=0;j<length;j++) {
		crc += (uchar)data[j];
		buf[i++] = data[j];
	}
	buf[i++]=crc&0xff;
	buf[i++]=crc>>8;
	if (iob->debug) {
		print("> (%d)",i);
		for (j=0;j<i;j++) {
			print(" %.2x",buf[j]);
		}
		print("\n");
	}

	for (j=0;j<MAXCHUNK;j++) {
		long sz=(chunk[j].size)?(chunk[j].size)
						:(i-chunk[j].offset);
		shortsleep(chunk[j].delay);
		if (write(iob->fd,buf+chunk[j].offset,sz) != sz) {
			eph_error(iob,ERRNO,"pkt write chunk %d(%d) error %r",j,(int)sz);
			return -1;
		}
	}
	return 0;
}

static void
putbyte(Camio *iob, int c)
{
	uchar buf[1];

	buf[0] = c;
	if(iob->debug)
		print("> %.2x\n", c);
	shortsleep(WRTDELAY);
	if(write(iob->fd, buf, sizeof(buf)) != sizeof(buf))
		eph_error(iob, ERRNO, "%.2x write error %r", c);
}

static void
writeinit(Camio *iob)
{
	putbyte(iob, 0);	/* INIT */
}

static void
writeack(Camio *iob)
{
	putbyte(iob, ACK);
}

static void
writenak(Camio *iob)
{
	putbyte(iob, NAK);
}

static	int	first = 1;

static int
ding(void *a, char *msg)
{
	USED(a);
	if(strcmp(msg, "alarm") == 0)
		return 1;
	return 0;
}

static long
readt(Camio *iob,void *buf,long length,long usec,int *rc)
{
	char err[ERRMAX];
	int n;

	if(first){
		first = 0;
		atnotify(ding, 1);
	}
	if (length == 0)
		return 0;
	alarm((usec+1000)/1000);
	n = read(iob->fd, buf, length);
	alarm(0);
	if(n < 0){
		err[0] = 0;
		errstr(err, sizeof err);
		if(strcmp(err, "interrupted") == 0){
			errstr(err, sizeof err);
			*rc = 0;
			return 0;
		}
		errstr(err, sizeof err);
	}
	*rc = n<0?-1:1;
	return n;
}

static int
readpkt(Camio *iob,Pkthdr *pkthdr,char *buffer,long *bufsize,long usec)
{
	ushort length,got;
	ushort crc1=0,crc2;
	unsigned char buf[4];
	int i,rc;

	i=readt(iob,buf,1,usec,&rc);
	if (iob->debug)
		print("pktstart: i=%d rc=%d char=0x%.2ux\n",i,rc,*buf);
	if (i < 0) {
		eph_error(iob,ERRNO,"pkt start read error %r");
		return -1;
	} else if ((i == 0) && (rc == 0)) {
		eph_error(iob,ERR_TIMEOUT,"pkt start read timeout (%ld)",
				usec);
		return -2;
	} else if (i != 1) {
		eph_error(iob,ERR_BADREAD,"pkt start read %d, expected 1",i);
		return -1;
	}
	pkthdr->typ=buf[0];
	if ((*buf != PKT_DATA) && (*buf != PKT_LAST)) {
		if ((*buf != NAK) && (*buf != DC1))
			eph_error(iob,ERR_BADDATA,"pkt start got 0x%.2ux",*buf);
		return *buf;
	}
	got=0;
	while ((i=readt(iob,buf+1+got,3-got,DATATIMEOUT,&rc)) > 0) {
		got+=i;
	}
	if (got != 3) {
		if (i < 0) {
			eph_error(iob,ERRNO,"pkt hdr read error %r (got %d)",got);
			return -1;
		} else if ((i == 0) && (rc == 0)) {
			eph_error(iob,ERR_TIMEOUT,"pkt hdr read timeout (%ld)",
					DATATIMEOUT);
			return -2;
		} else {
			eph_error(iob,ERR_BADREAD,"pkt hdr read return %d rc %d",
					i,rc);
			return -1;
		}
	}
	if (iob->debug)
		print("header: %.2x %.2x %.2x %.2x\n", buf[0],buf[1],buf[2],buf[3]);
	pkthdr->seq=buf[1];
	length=(buf[3]<<8)|buf[2];
	if (length > *bufsize) {
		eph_error(iob,ERR_DATA_TOO_LONG,
			"length in pkt header %lud bigger than buffer size %lud", (ulong)length,(ulong)*bufsize);
		return -1;
	}

	got=0;
	while ((i=readt(iob,buffer+got,length-got,iob->timeout,&rc)) > 0) {
		got+=i;
	}
	if (got != length) {
		if (i < 0) {
			eph_error(iob,ERRNO,"pkt data read error %r");
			return -1;
		} else if ((i == 0) && (rc == 0)) {
			eph_error(iob,ERR_TIMEOUT,"pkt data read timeout (%ld)",
				iob->timeout);
			return -2;
		} else {
			eph_error(iob,ERR_BADREAD,
				"pkt read return %d, rc %d",i,rc);
			return -1;
		}
	}

	for (i=0;i<length;i++) {
		crc1+=(unsigned char)buffer[i];
	}

	got=0;
	while ((i=readt(iob,buf+got,2-got,DATATIMEOUT,&rc)) > 0) {
		got+=i;
	}
	if (iob->debug)
		print("crc: %.2ux %.2ux i=%d rc=%d\n",buf[0],buf[1],i,rc);
	if (got != 2) {
		if (i < 0) {
			eph_error(iob,ERRNO,"pkt crc read error %r");
			return -1;
		} else if ((i == 0) && (rc == 0)) {
			eph_error(iob,ERR_TIMEOUT,"pkt crc read timeout (%ld)",
					DATATIMEOUT);
			return -2;
		} else {
			eph_error(iob,ERR_BADREAD,"pkt crc read return %d rc %d",
					i,rc);
			return -1;
		}
	}

	crc2=(buf[1]<<8)|buf[0];
	if (crc1 != crc2) {
		if (iob->debug) print("crc %04x != %04x\n",crc1,crc2);
		eph_error(iob,ERR_BADCRC,
			"crc received=0x%04x counted=0x%04x",crc2,crc1);
		return -1;
	}
	if (iob->debug) {
		int j;

		print("< %d,%d (%d)",pkthdr->typ,pkthdr->seq,length);
		if (iob->debug > 1) for (j=0;j<length;j++) {
			print(" %.2ux",(unsigned char)buffer[j]);
		} else print(" ...");
		print("\n");
		print("< %d,%d (%d)",pkthdr->typ,pkthdr->seq,length);
		if (iob->debug > 1) for (j=0;j<length;j++) {
			print(" %c ",(buffer[j] >= ' ' && buffer[j] < 127)
							? buffer[j] : '.');
		} else print(" ...");
		print("\n");
	}
	(*bufsize)=length;
	return 0;
}

static int
flushinput(Camio *iob) {
	unsigned char buf;
	int i,rc;

	i=readt(iob,&buf,1,0L,&rc);
	if (iob->debug)
		print("< %.2ux amount=%d rc=%d\n",buf,i,rc);
	if (i < 0) {
		eph_error(iob,ERRNO,"flushinput read error %r");
		return -1;
	} else if ((i == 0) && (rc == 0)) {
		if (iob->debug)
			print("flushed: read %d amount=%d rc=%d\n",buf,i,rc);
		return 0;
	} else {
		eph_error(iob,ERR_BADREAD,"flushinput read %d expected 0",i);
		return -1;
	}
}

int
eph_waitchar(Camio *iob, ulong usec)
{
	unsigned char buf;
	int i,rc;

	i=readt(iob,&buf,1,usec,&rc);
	if (iob->debug)
		print("< %.2ux amount=%d rc=%d\n",buf,i,rc);
	if (i < 0) {
		eph_error(iob,ERRNO,"waitchar read error %r");
		return -1;
	} else if ((i == 0) && (rc == 0)) {
		eph_error(iob,ERR_TIMEOUT,"waitchar read timeout (%ld)",
				usec);
		return -2;
	} else if (i != 1) {
		eph_error(iob,ERR_BADREAD,"waitchar read %d expected 1",i);
		return -1;
	}
	return buf;
}

static int
waitack(Camio *iob, long usec)
{
	int rc;
	if ((rc=eph_waitchar(iob,usec)) == ACK) return 0;
	if ((rc != DC1) && (rc != NAK))
		eph_error(iob,ERR_BADREAD,"waitack got %d",rc);
	return rc;
}

static int
waitcomplete(Camio *iob)
{
	int rc;
	if ((rc=eph_waitchar(iob,CMDTIMEOUT)) == 0x05) return 0;
	if ((rc != DC1) && (rc != NAK))
		eph_error(iob,ERR_BADREAD,"waitcomplete got %d",rc);
	return rc;
}

static int
waitsig(Camio *iob)
{
	int rc,count=SKIPNULS;
	while (((rc=eph_waitchar(iob,INITTIMEOUT)) == 0) && (count-- > 0))
		;
	if (rc == SIG)
		return 0;
	eph_error(iob,ERR_BADREAD,"waitsig got %d",rc);
	return rc;
}

static int
waiteot(Camio *iob)
{
	int rc;

	if ((rc=eph_waitchar(iob,EODTIMEOUT)) == 0xff) return 0;
	if ((rc != DC1) && (rc != NAK))
		eph_error(iob,ERR_BADREAD,"waiteot got %d",rc);
	return rc;
}

static void
deferrorcb(int errcode,char *errstr)
{
	fprint(2,"Error %d: %s\n",errcode,errstr);
}

static void
defruncb(long)
{
}

Camio *
eph_new(void (*errorcb)(int errcode,char *errstr),
		void (*runcb)(long count),
		int (*storecb)(char *data,long size),
		int debug)
{
	Camio *iob;

	if(errorcb == nil)
		errorcb = deferrorcb;
	if(runcb == nil)
		runcb = defruncb;
	iob = malloc(sizeof(Camio));
	if(iob == nil)
		return iob;
	memset(iob, 0, sizeof(*iob));
	iob->errorcb = errorcb;
	iob->runcb = runcb;
	iob->storecb = storecb;
	iob->debug = debug;
	iob->fd = -1;
	iob->cfd = -1;
	return iob;
}

void
eph_free(Camio *iob)
{
	free(iob);
}

static char *eph_errmsg[] = {
	/* 10001 */	"Data too long",
	/* 10002 */	"Timeout",
	/* 10003 */	"Unexpected amount of data read",
	/* 10004 */	"Bad packet header received",
	/* 10005 */	"Bad CRC on packet",
	/* 10006 */	"Bad speed value",
	/* 10007 */	"No memory",
	/* 10008 */	"Bad arguments",
	/* 10009 */	"",
	/* 10010 */	"",
	/* 10011 */	"",
	/* 10012 */	"",
	/* 10013 */	"",
	/* 10014 */	"",
	/* 10015 */	"",
};

/*
  We do not do any buffer override checks here because we are sure
  that the function is called *only* from within our library.
*/
void
eph_error (Camio *iob,int err,char *fmt,...)
{
	va_list ap;
	char msgbuf[512];

	va_start(ap,fmt);

	if (fmt) {
		vseprint(msgbuf, msgbuf+sizeof(msgbuf), fmt, ap);
	} else {
		if ((err >= ERR_BASE) && (err < ERR_MAX))
			strcpy(msgbuf, eph_errmsg[err-ERR_BASE]);
		else
			sprint(msgbuf, "%r");
	}
	va_end(ap);
	iob->errorcb(err,msgbuf);
}
