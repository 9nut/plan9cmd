/****************************************************************
Copyright (C) Lucent Technologies 1997
All Rights Reserved

Permission to use, copy, modify, and distribute this software and
its documentation for any purpose and without fee is hereby
granted, provided that the above copyright notice appear in all
copies and that both that the copyright notice and this
permission notice and warranty disclaimer appear in supporting
documentation, and that the name Lucent Technologies or any of
its entities not be used in advertising or publicity pertaining
to distribution of the software without specific, written prior
permission.

LUCENT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.
IN NO EVENT SHALL LUCENT OR ANY OF ITS ENTITIES BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
****************************************************************/
/* an adaptation of awk/lex.c */

#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include "bdf2subf.h"
#include "y.tab.h"

#ifdef	YYLEXTEST
#define	TRACE(x) fprint x
#else
#define	TRACE(x)
#endif

typedef struct Keyword {
	char *name;
	int tokid;
} Keyword;

Keyword BDF_kw[] = {
	{"BBX", BBX},
	{"BITMAP", BITMAP},
	{"CHARS", CHARS},
	{"COMMENT", COMMENT},
	{"CONTENTVERSION", CONTENTVERSION}, 
	{"DWIDTH", DWIDTH},
	{"DWIDTH1", DWIDTH1},
	{"ENCODING", ENCODING},
	{"ENDCHAR", ENDCHAR},
	{"ENDFONT",ENDFONT},
	{"ENDPROPERTIES", ENDPROPERTIES},
	{"FONT", FONT},
	{"FONTBOUNDINGBOX", FONTBOUNDINGBOX},
	{"METRICSSET", METRICSSET},
	{"SIZE", SIZE},
	{"STARTCHAR", STARTCHAR},
	{"STARTFONT",	STARTFONT},
	{"STARTPROPERTIES", STARTPROPERTIES},
	{"SWIDTH", SWIDTH},
	{"SWIDTH1", SWIDTH1},
	{"VVECTOR", VVECTOR},
};

Keyword *lookup(char *);
int binsearch(char *, Keyword*, int);

#define	dimof(X)	(sizeof(X)/sizeof((X)[0]))
#define	UGBUFSIZ	128

Biobuf *bdf;
static int ugbuf[UGBUFSIZ];
static int ugtop;
static char tokbuf[Bsize];

char *curtok;
int yyline = 0;

Keyword *
lookup(char *name)
{
	int i;

	if ((i = binsearch(name, BDF_kw, dimof(BDF_kw))) < 0)
		return (Keyword *) 0;

	return &BDF_kw[i];
}

/* from awk/lex.c */
int
binsearch(char *w, Keyword *kp, int n)
{
	int cond, low, mid, high;

	low = 0;
	high = n - 1;
	while (low <= high) {
		mid = (low + high) / 2;
		if ((cond = strcmp(w, kp[mid].name)) < 0)
			high = mid - 1;
		else if (cond > 0)
			low = mid + 1;
		else
			return mid;
	}
	return -1;
}

int
getch(void)
{
	int c;
	if (ugtop > 0) {
		if (ugbuf[--ugtop] == '\n') yyline++;
		return ugbuf[ugtop];
	}

	c = Bgetc(bdf);
	if (c == '\n') yyline++;
	return c;
}

void 
ungetch(char c)
{
	if (c == '\n') yyline--;

	if (ugtop < UGBUFSIZ)
		ugbuf[ugtop++] = c;
	else {
		fprint(2, "ungetch: unget stack full\n");
		exits("ungetch error");
	}
}

int
peek(void)
{
	if (ugtop > 0)
		return ugbuf[ugtop-1];
	ugbuf[ugtop] = Bgetc(bdf);
	return ugbuf[ugtop++];
}

void
ungetstr(char *s)
{
	int i, l = strlen(s);

	if (ugtop+l >= UGBUFSIZ) {
		fprint(2, "ungetstr: unget stack full\n");
		return;
	}
	for (i = l-1; i >= 0; i--)
		ungetch(s[i]);
}

char *
getnextoken(void)
{
	int c;
	char *bp = tokbuf;

	for (;;) {
		if ((c = getch()) < 0) return 0;
		if (c ==  ' ' || c == '\t') continue;
		if (c == '\n') {
			*bp++ = c;
			*bp = 0;
			return tokbuf;
		}
		/* start of something */
		ungetch(c);
		break;
	}
	
	for (;;) {
		if ((c = getch()) < 0) {
			ungetch(c);
			if (bp > tokbuf) {
				*bp = 0;
				return tokbuf;
			}
			return 0;
		} else if (c == ' ' || c == '\t' || c == '\n') {
			ungetch(c);
			*bp = 0;
			return tokbuf;
		}
		*bp++ = c;
	}
	return 0;	/* not reached */
}

char *
readupto(char term)
{
	int c;
	char *bp = tokbuf;

	if (peek() < 0) {
		return 0;
	}

	for (;;) {
		if ((c = getch()) < 0 || c == term)  {
			ungetch(c);
			*bp = 0;
			return tokbuf;
		}
		*bp++ = c;
	}
	return 0;	/* not reached */
}

char *
estrdup(char *s)
{
	char *x = strdup(s);
	if (! x) {
		fprint(2, "no memory\n");
		exits("can't strdup");
	}
	return x;
}

int
iskwpat(char *s)
{
	do {
		if (! isupper(*s)) return 0;
	} while (*++s);

	return 1;
}

int
isintpat(char *s)
{
	if (isdigit(*s) || *s == '+' || *s == '-') {
		for (s++; *s; s++)
			if (! isdigit(*s)) return 0;
		return 1;
	}
	return 0;
}

int
ishexpat(char *s)
{
	do {
		if (! isxdigit(*s)) return 0;
	} while (*++s);

	return 1;
}

int
yylex(void)
{
	static enum {
		Initial = 0,
		Strings,
		Comments,
		Properties,
		Proplist,
		Bitmap,
		Bitlist
	} state = Initial;

	static int skiplines = 0;

	for (;;) {
		register char *tok;
		register Keyword *x;

		switch (state) {
		default:
			if (! (tok = getnextoken())) {
				/* EOF, clean up and terminate */
				return 0;
			}

TRACE((2,"state 0: looking at %s\n", tok));
			curtok = tok;
			if (iskwpat(tok)) {
				if (x = lookup(tok)) {
					switch (x->tokid) {
					case COMMENT:
						state = Comments;
TRACE((2,"state 0: tokid %d >> state %d\n", x->tokid, state));
						break;

					case STARTPROPERTIES:
						state = Properties;
TRACE((2,"state 0: return tokid %d >> state %d\n", x->tokid, state));
						return x->tokid;

					case STARTFONT:
					case STARTCHAR:
					case FONT:
						state = Strings;
TRACE((2,"state 0: return tokid %d >> state %d\n", x->tokid, state));
						return x->tokid;


					case BITMAP:
						state = Bitmap;
TRACE((2,"state 0: return tokid %d >> state %d\n", x->tokid, state));
						return x->tokid;

					default:
TRACE((2,"state 0: return tokid %d >> state %d\n", x->tokid, state));
						return x->tokid;
					}
				} else {
					yylval.s = estrdup(tok);
TRACE((2,"state 0: return STRING\n"));
					return STRING;
				}
			} else if (isintpat(tok)) {
				yylval.i = atoi(tok);
TRACE((2, "state 0: return INTEGER\n"));
				return INTEGER;
			} else if (*tok == '\n') {
TRACE((2, "state 0: return '\\n'\n"));
				return '\n';
			} else {
				fprint(2, "yylex: unkown token %s\n", tok);
				exits("lexical error");
			}
			break;

		case Strings:
			state = 0;
			if (! (tok = readupto('\n'))) {
				fprint(2, "yylex: unexpected EOF\n");
				exits("lexical error");
			}
TRACE((2, "state Strings: looking at %s\n", tok));
			curtok = tok;
			yylval.s = estrdup(tok);
			return STRING;

		case Bitmap:
			if (! (tok = getnextoken())) {
				fprint(2, "yylex: unexpected EOF in Bitmap\n");
				return 0;
			}
			curtok = tok;

TRACE((2, "state Bitmap: looking at %s\n", (*tok == '\n')?"\\n":tok));
			if (*tok == '\n') {
				state = Bitlist;
TRACE((2, "state Bitmap: return \\n >> state %d\n", state));
				return '\n';
			} else {
				state = 0;
				yylval.s = estrdup(tok);
TRACE((2, "state Bitmap: return STRING >> state %d\n", state));
				return STRING;	/* let yyparse take care of it */
			}

		case Bitlist:
			if (! (tok = getnextoken())) {
				fprint(2, "yylex: unexpected EOF in BITMAP\n");
				return 0;
			}

TRACE((2, "state Bitlist: looking at %s\n", (*tok == '\n')?"\\n":tok));
			curtok = tok;
			if (ishexpat(tok)) {
				yylval.s = estrdup(tok);
TRACE((2, "state Bitlist: return HEXBUFF >> state %d\n", state));
				return HEXBUFF;
			} else if (*tok == '\n') {
TRACE((2, "state Bitlist: return \\n >> state %d\n", state));
				return '\n';
			}
			ungetstr(tok);
			state = 0;
			break;

		case Comments:
			if (! (tok = readupto('\n'))) {
				fprint(2, "yylex: unexpected EOF in COMMENT\n");
				return 0;
			}
			USED(tok);
			if (peek() == '\n') getch();
			state = 0;
TRACE((2, "state Comments: token \"%s\" >> state %d\n", tok, state));
			break;

		case Properties:
			if (! (tok = getnextoken())) {
				fprint(2, "yylex: unexpected EOF in PROPERTIES\n");
				return 0;
			}

TRACE((2, "state Properties: looking at \"%s\"\n", tok));
			if (isintpat(tok)) {
				skiplines = yylval.i = atoi(tok);
TRACE((2, "state Properties: return INTEGER >> state %d\n", state));
				return INTEGER;
			}
			else if (*tok == '\n') {
				state = Proplist;
TRACE((2, "state Properties: return \\n >> state %d\n", state));
				return '\n';
			}
			/* probably an error, let yyparse sort it out */
			fprint(2, "yylex: unknown token %s in PROPERTIES\n", tok);
			exits("lexical error");

		case Proplist:	/* property list keywords can go here */
			if (skiplines) {
				if (! (tok = readupto('\n'))) {
					fprint(2, "yylex: unexpected EOF in PROPERTIES");
					return 0;
				}
				yylval.s = estrdup(tok);
				if (peek() == '\n') getch();

				skiplines--;
TRACE((2, "state Proplist: return BUFFER >> state %d\n", state));
				return BUFFER;
			}
			state = 0;
TRACE((2, "state Proplist: >> state %d\n", state));
			break;
		}
	}
	return 0;	/* not reached */
}

#ifdef	YYLEXTEST
YYSTYPE yylval;

main(int ac, char *av[])
{
	Biobuf bin;
	bdf = &bin;
	Binit(bdf, 0, OREAD);

	while (yylex())
		;
}
#endif
