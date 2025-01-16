/*
All the low level routines for talking to the camera and what
registers to look at and poke at are supplied in the PhotoPC package.
PhotoPC is the work of Eugene G.  Crosser and Bruce D.  Lightner.

We need Charles Forsyth's port of PhotoPC to Plan9. It is available at:

http://www.caldo.demon.co.uk/plan9/soft/

The fs part is my doing.  Send the bug reports to me: fst@9netics.com
The fs has only been tested on my Sanyo VPC-X360.

The eventual goal is to have an fs with this general layout:

	ctl	# for commands to the camera -- eventually
	pics/
		pic01
		pic02
		...
	seqs/
		seq01
		seq02
		...
	clips/
		clip01
		clip02
		...

reading ctl should perhaps return the camera
ID information, etc. commands that can be written
to ctl should be:
	snap		- take a snapshot
	speed xxx	- set port speed
	port xxx		- ??? set to a different port. Not sure about this
	???

for now, just build the tree upfront based on the
current number of images in the camera, and make
the thing read only. don't supply close and so the
buffers for the actual content will be around after the
first time it is read, so it works sort of like a cache.
*/

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "eph_io.h"

static long speed = MAX_SPEED;
static char *device = "/dev/eia0";

typedef struct Camfile	Camfile;
struct Camfile {
	char *data;
	int len;	/* actual length of 'data' */
	int slot;	/* id to poke into reg 12 */
};

static void fsattach(Req *);
static void fsread(Req *);
static void fscleanup(Srv*);
static void dcfscreatefile(char *, Camfile *, Dir *);

Srv dcfs = {
	.attach=	fsattach,
	.read=	fsread,
	.end=	fscleanup,
};

static int
bldidir(void)
{
	Dir d;
	char fname[256];
	Tm *tm;
	Camfile *c;
	char *buffer;
	long bufsize;
	long res, max;
	register int i, j, k;
	eph_iob *iob = (eph_iob *) dcfs.aux;

	if (chatty9p) fprint(2, "getting image count\n");
	if (eph_getint(iob, 10, &max)) {
		return 0;
	}

	if (chatty9p) fprint(2, "image count: %lud\n", max);

	for (j = 1; j <= max; j++) {
		/* set image index */
		if (eph_setint(iob, 4, j) != 0) {
			return 0;
		}

		/* get image size */
		if (eph_getint(iob,12, &res) != 0) {
			if (chatty9p)  fprint(2, "eph_getint failed image size(reg 12), index(%d)\n", j);
			return 0;
		}

		d.length = (int) res;		/* Dir:length */

		/* get image creation time */
		/* goofyass way this library works, you've got to malloc all buffers */
		bufsize=32;
		buffer = emalloc9p(bufsize);

		if (eph_getvar(iob, 47, (char**)&buffer, &bufsize) != 0) {
			sysfatal("can't get the image mtime");
		}

		/* bytes 20-24 are creation time (UNIX format) */
		if (chatty9p)  fprint(2, "extracting image date/time\n");
		for (res = 0, i = 20, k = 0; i  < 24; i++, k += 8) {
			res += (long) buffer[i] << k;
		}
		free(buffer);

		if (res == -1L) res = time(0);
		d.atime = d.mtime = (int) res;		/* Dir:mtime */
		d.mode = 0444;				/* read only */

		tm = gmtime(res);

		sprint(fname, "pics/%4.4d%2.2d%2.2d_%3.3d.jpg",
			1900+tm->year, tm->mon+1, tm->mday, j);
		c = emalloc9p(sizeof *c);
		c->len = d.length;
		c->slot = j;

		if (chatty9p)
			fprint(2, "creating file %s\n", fname);
		dcfscreatefile(fname, c, &d);
	}

	return 1;
}

static int
fetchimg(Camfile *cf)
{
	off_t got;
	int err;
	eph_iob *iob = (eph_iob *) dcfs.aux;

	assert(cf->len > 0);
	assert(cf->data);

	if (err = eph_setint(iob, 4, (long) cf->slot)) {
		if (chatty9p) fprint(2, "eph_setint(reg=4,slot=%d) returns %d\n", cf->slot, err);
		return 0;
	}

	got = cf->len;
	if (err = eph_getvar(iob, 14, &cf->data, &got)) {
		if (chatty9p) fprint(2, "eph_getvar(reg=14) returns %d\n", err);
		free(cf->data);
		cf->data = 0;
		return 0;
	}
	assert(got == cf->len);

	return 1;
}

static void
ecreatefile(File *root, char *name, char *user, ulong mode, void *aux)
{
	File *f;
	f = createfile(root, name, user, mode, aux);
	if(f == nil)
		sysfatal("creating %s", name);
}

static int
caminit(void)
{
	eph_iob *iob = (eph_iob *) dcfs.aux;
	long ret;

	if (eph_open(iob, device, speed) != 0) {
		if (chatty9p) fprint(2, "eph_open failed\n");
		return 0;
	}

	if (eph_getint(iob, 1, &ret) != 0) {
		if (chatty9p) fprint(2, "probe failed\n");
		eph_close(iob, 1);	/* turn off */
		return 0;
	}
	return 1;
}

static void
camfini(void)
{
	eph_close((eph_iob*) dcfs.aux, 1);	/* turn the camera off and close */
}

static void
fsattach(Req *r)
{
	char *spec;
	eph_iob *iob;

	spec = r->ifcall.aname;		/* special args to mount? */
	if (spec && spec[0]) {			/* we don't expect any */
		respond(r, "invalid attach specifier");
		return;
	}

	if (! caminit()) {
		respond(r, "can't initialize camera");
		return;
	}

	if (bldidir()) {
		iob = (eph_iob *) dcfs.aux;
		iob->debug = chatty9p;
		r->fid->qid = dcfs.tree->root->qid;
		r->ofcall.qid = r->fid->qid;
		respond(r, nil);
	}
	else
		respond(r, "can't get image list");

	camfini();
}

static void
fsread(Req *r)
{
	Camfile *cf;
	vlong offset;
	long count;

	cf = r->fid->file->aux;
	offset = r->ifcall.offset;
	count = r->ifcall.count;

	if (! cf->data) {
		int aok;

		if (! caminit()) {
			respond(r, "can't initialize camera");
			return;
		}

		cf->data = emalloc9p(cf->len);

		aok = fetchimg(cf);
		camfini();

		if (! aok) {
			respond(r, "fetchimg failed");
			return;
		}
	}

	if(offset >= cf->len){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}

	if(offset+count >= cf->len)
		count = cf->len - offset;

	memmove(r->ofcall.data, cf->data+offset, count);
	r->ofcall.count = count;
	respond(r, nil);
}

static void
fscleanup(Srv *srv)
{
	if (srv->aux) eph_free((eph_iob *) srv->aux);
}

static void
fsdestroyfile(File *f)
{
	Camfile *cf;

	if (chatty9p) fprint(2, "clunk\n");

	cf = f->aux;
	if (cf) {
		if (cf->data) free(cf->data);
		free(cf);
	}
}

/* from /sys/src/cmd/archfs.c*/
static File*
createpath(File *f, char *name, char *u, ulong m)
{
	char *p;
	File *nf;

	if(chatty9p)
		fprint(2, "createpath %s\n", name);
	incref(f);
	while(f && (p = strchr(name, '/'))) {
		*p = '\0';
		if(strcmp(name, "") != 0 && strcmp(name, ".") != 0){
			/* this would be a race if we were multithreaded */
			incref(f);	/* so walk doesn't kill it immediately on failure */
			if((nf = walkfile(f, name)) == nil)
				nf = createfile(f, name, u, DMDIR|0777, nil);
			decref(f);
			f = nf;
		}
		*p = '/';
		name = p+1;
	}
	if(f == nil)
		return nil;

	incref(f);
	if((nf = walkfile(f, name)) == nil)
		nf = createfile(f, name, u, m, nil);
	decref(f);
	return nf;
}

static void
dcfscreatefile(char *name, Camfile *photo, Dir *d)
{
	File *f;
	f = createpath(dcfs.tree->root, name, "dcfs", d->mode);
	if(f == nil)
		sysfatal("creating %s: %r", name);
	free(f->gid);
	/* f->gid = estrdup9p(d->gid); */
	f->gid = estrdup9p("dcfs");
	f->aux = photo;
	f->mtime = d->mtime;
	f->length = d->length;
	decref(f);
}

void
usage(void)
{
	fprint(2, "usage: dcfs [-D] [-s srvname] [-m mtpt] [-b bitrate] [-l device]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *srvname = nil;
	char *mtpt = nil;

	if (! (dcfs.tree = alloctree(nil, nil, DMDIR|0555, fsdestroyfile)))
		sysfatal("creating tree");

	ecreatefile(dcfs.tree->root, "pics", "dcfs", DMDIR|0555, nil);

	/* later when I figure out how to get these out of the beast */
	ecreatefile(dcfs.tree->root, "seqs", "dcfs", DMDIR|0555, nil);
	ecreatefile(dcfs.tree->root, "clips", "dcfs", DMDIR|0555, nil);

	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'l':
		device=ARGF();
		break;
	case 'b':
		speed=atol(ARGF());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND;

	if(argc)
		usage();
	if(srvname == nil && mtpt == nil)
		sysfatal("Missing both -s and -m options. Must provide at least one.");

	if(chatty9p)
		fprint(2, "dcfs.nopipe %d srvname %s mtpt %s\n", dcfs.nopipe, srvname, mtpt);

	dcfs.aux = (void*) eph_new(nil, nil, nil,  0);
	if (! dcfs.aux) {
		if (chatty9p) fprint(2, "eph_new failed\n");
		sysfatal("eph_new failed");
	}

	postmountsrv(&dcfs, srvname, mtpt, MREPL|MCREATE);
	exits(0);
}
