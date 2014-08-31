/*
Convert BDF to Plan9 Subfont.

References:
 "Glyph Bitmap Distribution Format (BDF) Specification"
Version 2.2, March 22nd, 1993 -- Adobe Systems Inc.

Outline of the conversion process is from Rob Pike, as described in an
email on the Plan9 mailing list:

``
It's like this, schematically:

	s = allocimage(0, 0, HUGE, height);
	x = 0;
	for(i in number of characters) {
		c = allocmemimage(0, 0, c.width, height);
		convert bdf to character in c;
		draw(s, Rect(x, 0, x+c.width, height), c, nil, ZP);
		fontchar[i] = {...., x, ...};
		x += c.width;
	}
	t = allocimage(0, 0, x, height);
	draw(t, t->r, s, nil, ZP);

Or you could calculate the subfont width before allocating s by
adding another loop.
''

*/
#include	<u.h>
#include	<libc.h>
#include	<bio.h>
#include	<draw.h>
#include	<memdraw.h>
#include	<event.h>
#include	"bdf2subf.h"

void
usage (void)
{
	fprint(2, "usage: bdf2subf -f font.bdf\nor\nbdf2subf font.bdf [hex-hex]\n");
	exits("usage");
}

void
eresized(int x)
{
	USED(x);
}

int
cmpchars(void *_1, void *_2)
{
	return ((BDFchar*)_1)->enc - ((BDFchar*)_2)->enc;
}

void
adjustrange(int *emin, int *emax)
{
	/* look for a contiguous range from emin to approximately emax */
	register BDFchar *p;
	register int i;
	int lastenc;

	for (i = 0, p = bdfont->glyphs; i < bdfont->n; i++, p++)
		if (p->enc == *emin)
				break;

	if (i >= bdfont->n) {
		fprint(2, "range %x-%x not in file\n", *emin, *emax);
		exits("range not found");
	}
	lastenc = p->enc;
	for (i++, p++; i < bdfont->n; i++, p++) {
		if (p->enc == *emax)
			break;

		if (p->enc != lastenc+1) {
			*emax = lastenc;	/* adjust maxenc */
			break;
		}
		lastenc++;
	}
}

void
bdf2subf(int fd, int minenc, int maxenc)
{
	register int i, k, x, nglyphs;
	Rectangle r;
	Memimage *s;	/* subfont */
	Memimage *c;	/* glyph */
	Subfont *sf;
	BDFchar *bdfchar;
	Fontchar *fontchar;

	nglyphs = maxenc - minenc + 1;	// inclusive
	bdfchar = bdfont->glyphs;

	/*
	figuring out the exact size of the memimage for the output in this step,
	saves us one call to memimagedraw later. this is a big saving when the
	font range is huge.
	*/
	for (x = 0, i = 0, k = 0; i < bdfont->n; i++) {
		if (bdfchar[i].enc < minenc || bdfchar[i].enc > maxenc)
			continue;
		k++;
		x += bdfchar[i].bbx.w;
	}

	if (k != nglyphs) {
		fprint(2, "mismatch in the number of glyphs, nglyphs=%d, k=%d\n",
			nglyphs, k);
		exits("glyph count mismatch");
	}

	r = Rect(0, 0, x, bdfont->fbbx.h);
	s = allocmemimage(r, GREY1);

	/* see cachechars(2) for an explanation of the extra Fontchar */
	fontchar = malloc((nglyphs+1) * sizeof(Fontchar));

	r = Rect(0, 0, bdfont->fbbx.w, bdfont->fbbx.h);
	c = allocmemimage(r, GREY1);

	for(x = 0, i = 0, k = 0; i < bdfont->n; i++) {
		if (bdfchar[i].enc < minenc || bdfchar[i].enc > maxenc)
			continue;

		r = Rect(0, 0, bdfchar[i].bbx.w, bdfchar[i].bbx.h);

		/* convert bdf to character in c; */
		loadmemimage(c, r, bdfchar[i].bitmap, bdfchar[i].bmlen);

		fontchar[k].x = x;
		fontchar[k].top = 0;
		fontchar[k].bottom = bdfchar[i].bbx.h;
		fontchar[k].left = bdfchar[i].bbx.xoff;
		fontchar[k].width = bdfchar[i].bbx.w;
		k++;

		r = Rect(x, 0, x+bdfchar[i].bbx.w, bdfchar[i].bbx.h);
		memimagedraw(s, r, c, c->r.min, nil, ZP, SoverD);
		x += bdfchar[i].bbx.w;
	}

	fontchar[k].x = x;		/* see cachechars(2) */

	freememimage(c);

	if (! (sf = malloc(sizeof(Subfont)))) {
		fprint(2, "malloc failed\n");
		exits("memory exhusted");
	}

	sf->name = bdfont->name;
	sf->n = k;
	sf->height = bdfont->fbbx.h;
	sf->ascent = bdfont->fbbx.h+bdfont->fbbx.yoff;
	sf->info = fontchar;
	sf->ref = 1;

	writememimage(fd, s);
	writesubfont(fd, sf);

	free(fontchar);
	free(sf);
	freememimage(s);
}

void
apply(char *basename, void (*func)(char*, int, int))
{
	register BDFchar *p;
	register int i;
	int s, e;

	p = bdfont->glyphs;		/* assumes glyphs are sorted by enc */
	s = e = p->enc;			/* first one */
	for (i = 1, p++; i <= bdfont->n; i++, p++) {
		e++;	/* last glyphs enc value + 1 */

		/* apply function to a range iff
		the next glyph's enc value is not the same as the last+1, or
		we are at the end of the list, or
		(the last condition is a HACK. It gets around some problem in
		writememimage, where it takes too long to write out the memimage)
		the range is greater than 7FF, which was arrived at experimentally. 
		*/
		if (p->enc != e || (i+1) >= bdfont->n || (e-s) > 0x7ff) {
			func(basename, s, e-1);
			s = e = p->enc;
		}
	}
}

void
genfontfile(char *basename, int min, int max)
{
	fprint(1, "0x%X\t0x%X\t%s.%4.4X-%4.4X\n", min, max, basename, min, max);
}

void
gensubffile(char *basename, int min, int max)
{
	int fd;
	char outf[512];
	snprint(outf, sizeof(outf), "%s.%4.4X-%4.4X", basename, min, max);
	if ((fd = create(outf, OWRITE, 0755)) < 0) {
		sysfatal("can't open output file: %r");
	}
	bdf2subf(fd, min, max);
	close(fd);
}

char *
basename(char *f, char *x)	// x is the file extension; contents of f are changed
{
	char *b;
	int n;
	if (b = utfrrune(f, '/'))
		b++;
	else
		b = f;

	n = strlen(b)-strlen(x);
	if(n >= 0 && !strcmp(b+n, x))
		b[n] = 0;
	return b;
}

void
main(int argc, char **argv)
{
	char *bdfilename = 0;
	int prfontfile = 0;
	int minenc = 0, maxenc = 0, range = 0;
	extern int yyparse(void);
	extern int ishexpat(char*);

	USED(range);

	ARGBEGIN {
	case 'f':
		prfontfile = 1;
		break;
	default:
		fprint(2, "bad flag %c\n", ARGC());
		usage();
	} ARGEND

	if (! argc) {
		fprint(2, "BDF file missing\n");
		usage();
	} else {
		bdfilename = *argv++;
		argc--;
	}

	if (prfontfile && argc) {
		fprint(2, "unexpected argument %s\n", *argv);
		usage();
	}

	if (argc) {
		char *s;
		/* expect a range like xxxx-xxxx of hex values */

		if (! utfrune(*argv, '-')) {
			fprint(2, "malformed range %s\n", *argv);
			usage();
		}
		s = *argv;
		while (*s && *s != '-')
			s++;
		if (*s != '-')	usage();
		*s++ = 0;
		if (!ishexpat(*argv) || !ishexpat(s)) usage();
		minenc = strtoul(*argv, 0, 16);
		maxenc = strtoul(s, 0, 16);
		if (minenc > maxenc) {
			fprint(2, "min > max paradox!\n");
			usage();
		}
		range = 1;
	} else {
		range = 0;
	}

	if (! (bdf = Bopen(bdfilename, OREAD))) {
		fprint(2, "Can't open %s\n", bdfilename);
		exits("open failed");
	}

	memimageinit();
	yyparse();

	if (bdfont->n <= 0) {
		fprint(2, "No glyphs found!\n");
		exits(0);
	}
	qsort(bdfont->glyphs, bdfont->n, sizeof(BDFchar), cmpchars);

	if (prfontfile) {
		/* output the font height and ascent */
		fprint(1, "%d %d\n", bdfont->fbbx.h, bdfont->fbbx.h+bdfont->fbbx.yoff);
		apply(basename(bdfilename, ".bdf"), genfontfile);

	} else if (range)  {
		adjustrange(&minenc, &maxenc);
		bdf2subf(1, minenc, maxenc);	// output is stdout
	} else {
		apply(basename(bdfilename, ".bdf"), gensubffile);
	}

	exits(0);
}

