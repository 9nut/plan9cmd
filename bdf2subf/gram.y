%{
#include	<u.h>
#include	<libc.h>
#include	<bio.h>
#include	<draw.h>
#include	<memdraw.h>
#include	<ctype.h>
#include	"bdf2subf.h"

extern int yylex(void);
extern void yyerror(char *);

BDFont *bdfont;
%}

%union {
	char *s;
	int i;
};

%token	STARTFONT COMMENT CONTENTVERSION FONT SIZE
%token	FONTBOUNDINGBOX METRICSSET SWIDTH DWIDTH SWIDTH1
%token	DWIDTH1 VVECTOR STARTPROPERTIES ENDPROPERTIES
%token	CHARS STARTCHAR ENCODING BBX BITMAP ENDCHAR ENDFONT

%token	<i>	INTEGER
%token	<s>	STRING BUFFER HEXBUFF

%%
	/*
	The grammar needs to handle blank lines everywhere, but it
	doesn't. It is not obvious from the spec whether blank lines
	are allowed everywhere for now just expect them at the end
	of the file (based on observation)
	*/
bdfdesc
	: beginning /* comments */ misc font props characters blanks
	;

blanks
	: /* nothing: all this is for handling silly newlines at the end of a the file */
	| '\n'
	| blanks '\n'
	;

beginning
	: STARTFONT STRING '\n' { free($2); }
	;

misc: /* nothing */
	| CONTENTVERSION INTEGER '\n'
	;

font: font_id font_desc
	;

font_id
	: FONT STRING '\n'
	{
		if (! bdfont && ! (bdfont = malloc(sizeof(BDFont)))) {
			fprint(2, "memory exhusted\n");
			exits("malloc failed");
		}
		bdfont->name = $2;
	}
	;

font_desc
	: font_attrs
	| font_desc font_attrs
	;

font_attrs
	: SIZE INTEGER INTEGER INTEGER '\n'
	{
		bdfont->size = $2;
	}
	| FONTBOUNDINGBOX INTEGER INTEGER INTEGER INTEGER '\n'
	{
		bdfont->fbbx = (Boundingbox){ $2, $3, $4, $5 };
	}
	| METRICSSET INTEGER '\n'
	/* ignored */
	| SWIDTH INTEGER INTEGER '\n'	/* ignored */
	| DWIDTH INTEGER INTEGER '\n'
	{
		bdfont->dw = (Vector) { $2, $3 };
	}
	| SWIDTH1 INTEGER INTEGER '\n'	/* ignored */
	| DWIDTH1 INTEGER INTEGER '\n'	/* ignored */
	;

props
	: /* optional */
	| STARTPROPERTIES INTEGER '\n' properties ENDPROPERTIES '\n'
	/* ignore these */
	;

properties
	: BUFFER			{ free($1); /* ignored */ }
	| properties BUFFER	{ free($2); /* ignored */ }

characters
	: begin_chars char_desc_list end_chars
	;

begin_chars
	: CHARS INTEGER  '\n'
	{
		BDFchar *g;

		g = malloc($2*sizeof(BDFchar));
		if (! g) {
			fprint(2, "memory exhusted\n");
			exits("malloc failed");
		}
		memset(g, $2*sizeof(BDFchar), 0);
		bdfont->glyphs = g;
		bdfont->n = $2;
		bdfont->cur = 0;
	}
	;

end_chars
	: ENDFONT '\n'
	;

char_desc_list
	: char_desc
	| char_desc_list char_desc
	;

char_desc
	: STARTCHAR STRING '\n' char_attrs ENDCHAR '\n'
	{
		bdfont->cur++;
		if (bdfont->cur > bdfont->n) {
			fprint(2, "BDF file lied about number of chars (%d)\n", bdfont->n);
			exits("parser");
		}
		free($2);
	}
	;

char_attrs
	: char_attr
	| char_attrs char_attr
	;

char_attr
	: ENCODING INTEGER '\n'
	{
		BDFchar *curchar = bdfont->glyphs+bdfont->cur;
		curchar->enc = $2;
		curchar->bmlen = 0;
	}
	| SWIDTH INTEGER INTEGER '\n'	/* ignored */
	| DWIDTH INTEGER INTEGER '\n'
	{
		bdfont->glyphs[bdfont->cur].dw = (Vector){$2, $3};
	}
	| SWIDTH1 INTEGER INTEGER '\n'	/* ignored */
	| DWIDTH1 INTEGER INTEGER '\n'	/* ignored */
	| BBX INTEGER INTEGER INTEGER INTEGER '\n'
	{
		BDFchar *curchar = bdfont->glyphs+bdfont->cur;
		curchar->bbx = (Boundingbox){$2,$3,$4,$5};
		if (! ($2 && $3)) {
			fprint(2, "line %d: bogus BBX %d %d %d %d\n", yyline, $2, $3, $4, $5);
			exits("syntax error");
		}
		curchar->bitmap = malloc(sizeof(uchar)*$3*(($2+7)/8));
		if (! curchar->bitmap) {
			fprint(2, "memory exhusted\n");
			exits("malloc failed");
		}
	}
	| BITMAP '\n' hexbuff
	| VVECTOR INTEGER INTEGER '\n'
	;

hexbuff
	: HEXBUFF '\n'
	{
		BDFchar *cur;
		uchar *d;
		char *s;
		int bmsize;

		cur = bdfont->glyphs+bdfont->cur;
		if (! cur->bitmap) {
			fprint(2, "line %d: BITMAP out of order\n", yyline);
			exits("syntax error");
		}
		bmsize = cur->bbx.h*((cur->bbx.w+7)/8);
		d = cur->bitmap+cur->bmlen;
		s = $1;
		while (*s && *(s+1)) {
			if (cur->bmlen+1 > bmsize) {
				fprint(2, "line %d: BITMAP larger than BBX indicates\n", yyline);
				exits("syntax error");
			}
			*d = ((isdigit(*s)) ? *s - '0' : ((toupper(*s) - 'A') + 10)) << 4;
			s++;
			*d |= ((isdigit(*s)) ? *s - '0' : ((toupper(*s) - 'A') + 10));
			s++; d++;
			cur->bmlen++;
		}
		free($1);
	}
	| hexbuff HEXBUFF '\n'
	{
		BDFchar *cur;
		uchar *d;
		char *s;
		int bmsize;

		cur = bdfont->glyphs+bdfont->cur;
		if (! cur->bitmap) {
			fprint(2, "line %d: BITMAP out of order\n", yyline);
			exits("syntax error");
		}
		bmsize = cur->bbx.h*((cur->bbx.w+7)/8);
		d = cur->bitmap+cur->bmlen;
		s = $2;
		while (*s && *(s+1)) {
			if (cur->bmlen+1 > bmsize) {
				fprint(2, "line %d: BITMAP larger than BBX indicates\n", yyline);
				exits("syntax error");
			}
			*d = ((isdigit(*s)) ? *s - '0' : (toupper(*s) - 'A') + 10) << 4;
			s++;
			*d |= ((isdigit(*s)) ? *s - '0' : (toupper(*s) - 'A') + 10);
			s++; d++;
			cur->bmlen++;
		}
		free($2);
	}
	;

%%

void
yyerror(char *s)
{
	extern char *curtok;
	fprint(2, "line %d: %s near token %s\n", yyline, s, curtok);
	exits("syntax error");
}
