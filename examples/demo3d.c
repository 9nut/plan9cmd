// Test code for libgeometry; see:
// arith3(2), matrix(2), qball(2)

#include <u.h>
#include <libc.h>
#include <draw.h>
#include <event.h>
#include <geometry.h>

#pragma	varargck    type	"@" Point3
#pragma	varargck	type	"M"	Matrix
#pragma	varargck	type	"Z" Space
#pragma varargck	type	"Q" Quaternion

Space *world = nil;
Quaternion orient = {1, 0, 0, 0};
Point3 *sphere, *cube;

// number of random points on the sphere
#define NP 100

// width/height of points
#define DOTX 3
#define DOTY 3

// initialized later.
Image *bg, *fg1, *fg2, *fg3;	// background and forground colors
double ar; // aspect ratio

// index of cube edges (start and end points)
int edges[12][2] = {
	{0,1}, {1,2}, {2,3}, {3,0}, // top
	{4,5}, {5,6}, {6,7}, {7,4}, // bottom
	{0,4}, {1,5}, // side
	{2,6}, {3,7}  // side
};

// custom format functions
int
Qfmt(Fmt *f)
{
	Quaternion q;
	q = va_arg(f->args, Quaternion);
	return fmtprint(f,"{%-1.5g+%-1.5gi,%-1.5gj,%-1.5gk}", q.r, q.i, q.j, q.k);
}

int
P3fmt(Fmt *f)
{
	Point3 p;
	p = va_arg(f->args, Point3);
	return fmtprint(f,"(%g,%g,%g,%g)", p.x, p.y, p.z, p.w);
}

int
Mfmt(Fmt *f)
{
	char buf[1024];
	int l = 0;
	double *m = va_arg(f->args, double*);

	l += snprint(&buf[l], 1024-l, "[\n\t");
	for (int i = 0; i < 4; i++) {
		l += snprint(&buf[l], 1024-l, "[");
		for (int j = 0; j < 4; j++) {
			l += snprint(&buf[l], 1024-l, "%g,", *(m+(i*4+j)));
		}
		l += snprint(&buf[l], 1024-l, "],\n\t");
	}
	snprint(&buf[l], 1024-l, "\n]\n");
	return fmtstrcpy(f, buf);
}

int
Zfmt(Fmt *f)
{
	Space *s;
	s = va_arg(f->args, Space*);
	return fmtprint(f, "t:\n%M, tinv:\n%M\n", s->t, s->tinv);
}

void fatal(char *msg, ...)
{
	char buf[1024], *out;
	va_list arg;

	out = seprint(buf, buf+sizeof(buf), "Fatal error: ");
	va_start(arg, msg);
	out = vseprint(out, buf+sizeof(buf), msg, arg);
	va_end(arg);
	write(2, buf, out-buf);
	exits("fatal error");
}

// generate NP random points on a sphere of radius r
// centered on 0,0,0; use w=1 for points in the
// homogeneous coordinates system.
//
// φ = acos(2*frand() - 1)
// θ = frand() * 2 * π
// x = c.x + r*sin(φ)*cos(θ)
// y = c.y + r*sin(φ)*sin(θ)
// z = c.z + r*cos(φ)
Point3 *
genSphere(double r, double w)
{
	Point3 *points = calloc(NP, sizeof(Point3));
	srand(time(0));
	for(int i = 0; i < NP; i++){
		double φ = acos(2 * frand() - 1);	// 0..π
		double θ = 2 * PI * frand();		// 0..2π
		points[i].x = r * sin(φ) * cos(θ);
		points[i].y = r * sin(φ) * sin(θ);
		points[i].z = r * cos(φ);
		points[i].w = w;
	}
	return points;
}

// generate the vertexes of a cube or length r
// and W component of w
Point3 *
genCube(double r, double w)
{
	Point3 *vertices = calloc(8, sizeof(Point3));

	vertices[0] = (Point3){-r, +r, +r, w};
	vertices[1] = (Point3){+r, +r, +r, w};
	vertices[2] = (Point3){+r, -r, +r, w};
	vertices[3] = (Point3){-r, -r, +r, w};
	vertices[4] = (Point3){-r, +r, -r, w};
	vertices[5] = (Point3){+r, +r, -r, w};
	vertices[6] = (Point3){+r, -r, -r, w};
	vertices[7] = (Point3){-r, -r, -r, w};
	return vertices;
}

// calculate the aspect ratio and create the colors
void
initscr(void)
{
	char *scrinfo[5];	// /dev/screen returns: depth,minx,miny,maxx,maxy
	char buf[5*12 + 1];	// each field is 12 bytes long

	int fd = open("/dev/screen", OREAD);
	if(fd < 0)
		fatal("can't open /dev/screen: %r");
	if(read(fd, buf, 5*12) != 5*12)
		fatal("can't read /dev/screen: %r");
	close(fd);
	buf[5*12] = 0;
	if(tokenize(buf, scrinfo, 5) != 5)
		fatal("bad read /dev/screen: %s", buf);

	// scrinfo[0] is color depth
	int wd = atoi(scrinfo[3]) - atoi(scrinfo[1]);
	int ht = atoi(scrinfo[4]) - atoi(scrinfo[2]);

	ar = (double)wd/(double)ht;
	bg = allocimage(display, Rect(0, 0, 1, 1), RGB24, 1, DWhite);
	fg1 = allocimage(display, Rect(0, 0, 1, 1), RGB24, 1, DRed);
	fg2 = allocimage(display, Rect(0, 0, 1, 1), RGB24, 1, DGreen);
	fg3 = allocimage(display, Rect(0, 0, 1, 1), RGB24, 1, DBlue);
}

// create the world space. this only changes when the
// window is resized to change the viewport setting
void
initspace(void)
{
	Point3 eye = (Point3){-10, 0, 0, 1};
	Point3 at = (Point3){0, 0, 0, 1};
	Point3 up = (Point3){0, 0, 10, 0};

	int near = 1;	// z of near plane of frustum
	int far = 10;	// z of far plane of frustum
	int fov = 60;	// degrees

	if(world){
		popmat(world);
	}

	// stack world, 
	world = pushmat(nil);

	// lookat * pespective * viewport transform

	viewport(world, screen->r, ar);
	if(persp(world, fov, near, far) < 0){
		fatal("perspective not possible");
	}
	look(world, eye, at, up);
	// print("world: \n%Z\n", world);
}

// redraw the objects; apply the orientation transform to
// the world space created by initspace.
void
redraw(void)
{
	Space *s;
	Point3 q[8];
	Point3 qq[NP];
	Point p1, p2;
	int i;
	char buf[128];
	Rectangle r = screen->r;

	draw(screen, r, display->white, nil, ZP);

	// stack the orientation transform
	s = pushmat(world);
	qrot(s, orient);

	for(i = 0; i < 8; i++){
		q[i] = xformpointd(cube[i], nil, s);
		// print("cube[%d]%@->%@\n", i, cube[i], q[i]);
	}

	for(i = 0; i < 12; i++){
		p1 = (Point){q[edges[i][0]].x, q[edges[i][0]].y};
		p2 = (Point){q[edges[i][1]].x, q[edges[i][1]].y};
		line(screen, p1, p2, Endsquare, Endsquare, 1, fg3, ZP);
	}

	// add the move transform for the sphere 
	move(s, 2.0, 2.0, 2.0);
	for(i = 0; i < NP; i++){
		qq[i] = xformpointd(sphere[i], nil, s);
		// print("sphere[%d]%@->%@\n", i, sphere, qq[i]);
		Point pt = (Point){qq[i].x, qq[i].y};
		draw(screen, Rect(pt.x, pt.y, pt.x+DOTX, pt.y+DOTY), fg1, nil, ZP);
	}

	border(screen, insetrect(r, 30), 1, display->black, ZP);
	snprint(buf, 128, "Hold mouse B1 to rotate objects; 'q': to quit");
	string(screen, addpt(r.min, Pt(10,10)), display->black, ZP, display->defaultfont, buf);
	snprint(buf, 128, "Orientation: %Q", orient);
	string(screen, Pt(r.min.x+10, r.max.y-20), display->black, ZP, display->defaultfont, buf);
	flushimage(display, 1);

	// print("redraw space: %Z\n", s);
	popmat(s);
}

void
eresized(int new)
{
	if(new && getwindow(display, Refnone) < 0)
		sysfatal("getwindow: %r");

	initspace();
	redraw();
}

void
main(int, char **)
{
	char *rect = "-r 0 0 532 400";
	Event e;

	fmtinstall('@', P3fmt);
	fmtinstall('M', Mfmt);
	fmtinstall('Z', Zfmt);
	fmtinstall('Q', Qfmt);

	if(newwindow(rect) < 0)
		sysfatal("newwindow: %r");
	if(initdraw(0, 0, "demo3d") < 0)
		sysfatal("initdraw: %r");

	initscr();
	cube = genCube(1.0, 1.0);
	sphere = genSphere(1.0, 1.0);

	einit(Emouse|Ekeyboard);

	// Initializes the world space and redraws
	eresized(0);

	for(;;){
		switch(event(&e)){
		case Emouse:
			if(e.mouse.buttons & 1){
				// as long as mouse button 1 is held down, qball calculates
				// the orientation based on mouse movement and calls redraw.
				qball(screen->r, &e.mouse, &orient, redraw, nil);
			}
			break;
		case Ekeyboard:
			switch(e.kbdc){
			case 'q':
				exits(nil);
			case 'r':
				redraw();
				break;
			}
			break;
		}
	}
}

