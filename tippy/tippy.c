#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <cursor.h>

/*
 * tippy: the mouse spy
 */

enum
{
	STACK = 2048,
};

int oldb = 0;
Image *back;
Image *bup;
Image *bdown;

void
writemouse(void *)
{
	char buf[1+4*12];
	int n;

	for (;;) {
		n = read(1, buf, sizeof buf);
		if (n < 0)
			sysfatal("read 1: %r");
		if (write(0, buf, n) != n)
			sysfatal("write 0: %r");
	}
}

void
spymouse(void *arg)
{
	Mouse m;
	char buf[1+4*12];
	int n;
	Channel *c = arg;

	for (;;) {
		n = read(0, buf, sizeof buf);
		if (n < 0)
			sysfatal("read 0: %r");
		if (n != sizeof buf)
			continue;
		m.xy.x =  atoi(buf+1+0*12);
		m.xy.y =  atoi(buf+1+1*12);
		m.buttons =  atoi(buf+1+2*12);
		m.msec =  atoi(buf+1+3*12);
		send(c, &m);
		if (write(1, buf, n) != n)
			sysfatal("write 1: %r");
	}
}

void
drawbuttons(int b)
{
	Rectangle r;
	int i;

	r = screen->r;

	draw(screen, screen->r, back, nil, ZP);

	for (i = 0; i < 3; i++) {
		Point t = subpt(r.max, r.min);
		int w = t.x / 12;
		int x = (t.x*(i+1)) / 4;
		int y = t.y / 2;
		int h = t.y / 3;

		fillellipse(screen, addpt(r.min, Pt(x,y)), w, h, bup, ZP);

		if (b & (1 << i))
			fillellipse(screen, addpt(r.min, Pt(x,y)), w, h, bdown, ZP);
	}

	flushimage(display, 1);
}


void
threadmain(int, char **)
{
	Mouse mr, ms;
	Mousectl *mc;
	int t;
	Alt a[] = {
	/*	 c		v		op   */
		{nil,	&ms,	CHANRCV},	/* mouse being spied on */
		{nil,	&mr,	CHANRCV},	/* our mouse */
		{nil,	&t,	CHANRCV},	/* resize */
		{nil,	nil,	CHANEND},
	};

	memset(&mr, 0, sizeof mr);
	memset(&ms, 0, sizeof ms);

	if (newwindow("-r 0 0 300 200") < 0)
		sysfatal("newwindow: %r");

	if (initdraw(nil, nil, "tippy") < 0)
		sysfatal("initdraw: %r");

	mc = initmouse(nil, screen);
	if (! mc)
		sysfatal("initmouse: %r");

	back = allocimagemix(display, DPalebluegreen, DWhite);
	bup = allocimage(display, Rect(0,0,1,1), CMAP8, 1, DPalegreen);
	bdown = allocimage(display, Rect(0,0,1,1), CMAP8, 1, DBluegreen);

	a[0].c = chancreate(sizeof ms, 1);
	a[1].c = mc->c;
	a[2].c = mc->resizec;
	proccreate(spymouse, a[0].c, STACK);
	proccreate(writemouse, nil, STACK);

	drawbuttons(oldb);
	for (;;) {
		switch (alt(a)) {
		case 0:	/* mouse being spied on */
			if (ms.buttons != oldb) {
				drawbuttons(ms.buttons);
				oldb = ms.buttons;
			}
			break;
		case 1:	/* good for testing */
			if (mr.buttons != oldb) {
				drawbuttons(mr.buttons);
				oldb = mr.buttons;
			}
			break;
		case 2:
			getwindow(display, Refnone);
			drawbuttons(oldb);
			break;
		default:
			sysfatal("can't happen");
		}
	}
}
