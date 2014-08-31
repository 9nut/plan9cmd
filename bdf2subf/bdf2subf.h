/*
BDF Font to Subfont conversion

height = max(bbx.h) of all chars  or fbbx.h if given
ascent = max(bbx.h+bbx.yoff) of all chars or fbbx.h+fbbx.yoff if given

.yoff can be negative, so it will give the top of the heightest
char to the baseline.  

BDFchar to Fontchar conversion

top = 0
bottom = bbx.h
left = bbx.xoff
width = bbx.w

*/

struct Boundingbox {
	int w, h, xoff, yoff;
};
typedef struct Boundingbox Boundingbox;
struct Vector {
	int x, y;
};
typedef struct Vector Vector;

struct BDFchar {
	Rune enc;		/* value signified by ENCODING keyword */
	Boundingbox bbx;
	Vector dw;	/* actually Δx and Δy to the baseline*/
	/* Vector sdw;	/* same as above but for scaled size -- not used */
	/* Vector dw1;	/* these are for writing directions 1 (e.g. vertical) */
	/* Vector sdw1;/* these are for writing directions 1 (e.g. vertical) */
	int bmlen;		/* size of bitmap */
	uchar *bitmap;
};
typedef struct BDFchar BDFchar;

struct BDFont {
	char *name;
	int size;
	Boundingbox fbbx;
	Vector dw;		/* actually x,y are Δx and Δy to the baseline*/
	/* Vector sdw;	/* same as above but for scaled size -- not used */
	/* Vector dw1;	/* these are for writing directions 1 (e.g. vertical) */
	/* Vector sdw1;/* these are for writing directions 1 (e.g. vertical) */
	int cur;		/* current element index into glyphs */
	int n;
	BDFchar *glyphs;	/* will have nchar members */
};
typedef struct BDFont BDFont;

extern Biobuf *bdf;	/* input file */
extern BDFont *bdfont;
extern int yyline;
