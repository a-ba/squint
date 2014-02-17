#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <X11/Xlib.h>          /* of course */
#ifdef HAVE_XI
#include <X11/extensions/XInput2.h>
#endif
#ifdef COPY_CURSOR
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#endif
#ifdef USE_XDAMAGE
#include <X11/extensions/Xdamage.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Config
struct {
	const char* source_monitor_name;

	gboolean opt_version, opt_window;
} config;

// State
gboolean enabled = FALSE;
gboolean fullscreen = FALSE;

GtkWidget* gtkwin = NULL;
GdkWindow* gdkwin = NULL;
Window root_window = 0;
Window window = 0;
Pixmap pixmap = -1;
int depth = -1, screen = -1;
GC gc = NULL;
GC gc_white = NULL;
Display* display = NULL;
int raised = 0;

GdkRectangle rect;
GdkPoint offset;
GdkPoint cursor = {0, 0};

#ifdef HAVE_XI
int track_cursor = 0;
int xi_opcode = 0;
#endif

#ifdef USE_XDAMAGE
GdkRectangle gdkwin_extents;
int use_xdamage = 0;
int window_mapped = 1;
int xdamage_event_base;
Damage damage = 0;
XserverRegion screen_region = 0;
#define MIN_PERIOD 20
Time next_refresh=0;
gint refresh_timeout=0;
#endif

#ifdef COPY_CURSOR
#define CURSOR_SIZE 64
int xfixes_event_base;

int copy_cursor = 0;
Window cursor_window = 0;
Pixmap cursor_pixmap = 0;
XImage* cursor_image = NULL;
Pixmap  cursor_mask_pixmap = 0;
XImage* cursor_mask_image = NULL;
GC	cursor_mask_gc = NULL;
int cursor_xhot=0;
int cursor_yhot=0;
uint32_t cursor_pixels[CURSOR_SIZE*CURSOR_SIZE];
uint8_t  cursor_mask_pixels[CURSOR_SIZE*CURSOR_SIZE];
#endif


void
error (const char* msg)
{
	fprintf(stderr, "error: %s\n", msg);
}

void show()
{
	if (!raised)
	{
		raised = 1;
		gdk_window_raise(gdkwin);
		if (fullscreen) {
			gdk_window_fullscreen(gdkwin);
		}
	}
}

void do_hide()
{
	raised = 0;
	if (fullscreen) {
		gdk_window_unfullscreen(gdkwin);
	}
	gdk_window_lower(gdkwin);
}

void hide()
{
	if(raised)
	{
		do_hide();
	}
}

gboolean on_window_button_press_event(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	do_hide();
	return FALSE;
}

void refresh_cursor_location()
{
	Window root_return, w;
	int wx, wy, mask;
	GdkPoint c;
	XQueryPointer(display, root_window, &root_return, &w,
			&c.x, &c.y, &wx, &wy, &mask);

	c.x -= rect.x;
	c.y -= rect.y;

	if ((c.x<0) | (c.y<0) | (c.x>=rect.width) | (c.y>=rect.height))
	{
		// cursor is outside the duplicated screen
		c.x = c.y = -1;
	}

	if (memcmp(&cursor, &c, sizeof(cursor)) == 0) {
		// nothing to do
		return;
	}

	// cursor was really moved
	cursor = c;

#ifdef COPY_CURSOR
	if (copy_cursor) {
		// move the cursor window to the new location of the cursor
		XMoveWindow (display, cursor_window,
				cursor.x - cursor_xhot + offset.x,
				cursor.y - cursor_yhot + offset.y);

		// force redrawing the window
		XClearWindow(display, cursor_window);
	}
#endif

	if (cursor.x >= 0) {
		/* raise the window when the pointer enters the duplicated screen */
		show();
	} else {
		/* lower the window when the pointer leaves the duplicated screen */
		hide();
	}
}

gboolean
refresh_image (gpointer data)
{
	int code;
	int inside_rect=0;

#ifdef HAVE_XI
	if (!track_cursor)
#endif
	{
		refresh_cursor_location();
	}

	XCopyArea (display, root_window, pixmap, gc,
					rect.x, rect.y,
					rect.width, rect.height,
					0, 0);

	// draw the cursor (crosshair)
#ifdef COPY_CURSOR
	if (!copy_cursor)
#endif
	if (cursor.x >= 0) {
		#define LEN 3
		XDrawLine (display, pixmap, gc_white, cursor.x-(LEN+1), cursor.y, cursor.x+(LEN+2), cursor.y);
		XDrawLine (display, pixmap, gc_white, cursor.x, cursor.y-(LEN+1), cursor.x, cursor.y+(LEN+2));
		XDrawLine (display, pixmap, gc, cursor.x-LEN, cursor.y, cursor.x+LEN, cursor.y);
		XDrawLine (display, pixmap, gc, cursor.x, cursor.y-LEN, cursor.x, cursor.y+LEN);
	}

	// force refreshing the window's background
	XClearWindow(display, window);


	XFlush (display);

	return TRUE;
}

void try_refresh_image (Time timestamp);
gboolean _try_refresh_image_timeout (gpointer data)
{
	refresh_timeout=0;

	if ((Time)data == next_refresh)
	{
		try_refresh_image(next_refresh);
	}
	return FALSE;
}

void try_refresh_image (Time timestamp)
{
	if ((timestamp >= next_refresh) || (timestamp < next_refresh - 1000)) {
		if (refresh_timeout) {
			g_source_remove(refresh_timeout);
			refresh_timeout=0;
		}
		next_refresh = timestamp + MIN_PERIOD;
		refresh_image(NULL);

	} else if (!refresh_timeout) {
		refresh_timeout = g_timeout_add (next_refresh - timestamp, _try_refresh_image_timeout, NULL);
	}
}



#ifdef COPY_CURSOR
void refresh_cursor_image()
{
	XFixesCursorImage* img = XFixesGetCursorImage (display);
	if (!img)
		return;

	int width  = (img->width  < CURSOR_SIZE) ? img->width  : CURSOR_SIZE;
	int height = (img->height < CURSOR_SIZE) ? img->height : CURSOR_SIZE;

	int x, y;
	// copy the cursor image
	for(x=0 ; x<width ; x++)
	{
		for(y=0 ; y<height ; y++)
		{
			cursor_pixels[y*CURSOR_SIZE + x] = img->pixels[y*img->width + x];
		}
	}

	// copy the cursor mask
	memset(cursor_mask_pixels, 0, sizeof(cursor_mask_pixels));
	for(x=0 ; x<width ; x++)
	{
		for(y=0 ; y<height ; y++)
		{
			if(img->pixels[y*img->width + x] >> 24)
			{
				cursor_mask_pixels[y*(CURSOR_SIZE/8) + (x/8)] |= 1 << (x % 8);
			}
		}

	}

	// upload the image & mask
	XPutImage(display, cursor_pixmap, XDefaultGC(display, screen),
			cursor_image, 0, 0, 0, 0, width, height);
	XPutImage(display, cursor_mask_pixmap, cursor_mask_gc,
			cursor_mask_image, 0, 0, 0, 0, CURSOR_SIZE, CURSOR_SIZE);

	// apply the new mask
	XShapeCombineMask(display, cursor_window, ShapeBounding, 0, 0, cursor_mask_pixmap,
			ShapeSet);

	// force redrawing the cursor window
	XClearWindow(display, cursor_window);

	cursor_xhot = img->xhot;
	cursor_yhot = img->yhot;

	XFree(img);
}

void init_copy_cursor()
{
	// ensure we are in true color
	if (XDefaultDepth(display, screen) != 24) {
		return;
	}

	// check if xfixes and xshape are available on this display
	int major, minor, error_base;
	if ((	   !XFixesQueryExtension(display, &xfixes_event_base, &error_base)
		|| !XFixesQueryVersion(display, &major, &minor)
		|| (major<1)
		|| !XShapeQueryVersion(display, &major, &minor)
		|| !(((major==1) && (minor >= 1)) || (major >= 2))
	)) {
		return;
	}

	// create an image for storing the cursor
	cursor_image = XCreateImage (display, NULL, 24, ZPixmap, 0, (char*)cursor_pixels,
				CURSOR_SIZE, CURSOR_SIZE, 32, 256);
	if (!cursor_image) {
		error("XCreateImage() failed");
		return;
	}

	// create a pixmap for storing the cursor
	cursor_pixmap = XCreatePixmap (display, root_window,
				CURSOR_SIZE, CURSOR_SIZE,
				XDefaultDepth(display, screen));

	// create a pixmap for storing the mask
	cursor_mask_pixmap = XCreatePixmap (display, root_window,
				CURSOR_SIZE, CURSOR_SIZE, 1);

	// create an image for storing the mask
	cursor_mask_image = XCreateImage (display, NULL, 1, ZPixmap, 0, (char*)cursor_mask_pixels,
				CURSOR_SIZE, CURSOR_SIZE, 8, CURSOR_SIZE/8);
	if (!cursor_mask_image) {
		error("XCreateImage() failed");
		return;
	}

	// create a context for manipulating the mask
	cursor_mask_gc = XCreateGC(display, cursor_mask_pixmap, 0, NULL); 
	if(!cursor_mask_gc) {
		error ("XCreateGC() failed");
		return;
	}

	// create a sub-window displaying the cursor
	XSetWindowAttributes attr;
	attr.background_pixmap = cursor_pixmap;
	cursor_window = XCreateWindow (display,
				gdk_x11_window_get_xid(gdkwin),
				10, 10,
				CURSOR_SIZE, CURSOR_SIZE,
				0, CopyFromParent,
				InputOutput, CopyFromParent,
				CWBackPixmap, &attr);

	// refresh the cursor
	refresh_cursor_image();

	XMapWindow(display, cursor_window);

	// request cursor change notifications
	XFixesSelectCursorInput(display, gdk_x11_window_get_xid(gdkwin), XFixesCursorNotify);

	copy_cursor = 1;
}
#endif

GdkFilterReturn on_x11_event (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
	XEvent* ev = (XEvent*)xevent;
	XGenericEventCookie *cookie = &ev->xcookie;

#ifdef HAVE_XI
	if(track_cursor)
	{
		if (	(cookie->type == GenericEvent)
		    &&	(cookie->extension == xi_opcode)
		    &&	(cookie->evtype == XI_RawMotion))
		{
			// cursor was moved
			refresh_cursor_location();
			return GDK_FILTER_REMOVE;
		}
	}
#endif

#ifdef COPY_CURSOR
	if(copy_cursor)
	{
		if (ev->type == xfixes_event_base + XFixesCursorNotify) {
			refresh_cursor_image();

			return GDK_FILTER_REMOVE;
		}
	}
#endif

#ifdef USE_XDAMAGE
	if(use_xdamage)
	{
		if (ev->type == xdamage_event_base + XDamageNotify)
		{
			XDamageNotifyEvent* xd_ev = (XDamageNotifyEvent*) ev;

			// we do not refresh the window in case it overlaps with
			// the duplicated screen (to avoid any amplification)
			if (!xd_ev->more && !gdk_rectangle_intersect(&rect, &gdkwin_extents, NULL)) {
				// check if damage intersects with the screen
				GdkRectangle damage_rect;
				damage_rect.x = xd_ev->area.x;
				damage_rect.y = xd_ev->area.y;
				damage_rect.width  = xd_ev->area.width;
				damage_rect.height = xd_ev->area.height;

				if (gdk_rectangle_intersect(&damage_rect, &rect, NULL))
				{
					try_refresh_image(xd_ev->timestamp);
				}
			}
			XDamageSubtract(display, damage, 0, 0);
		}
	}
#endif
	return GDK_FILTER_CONTINUE;
}

#ifdef HAVE_XI
void init_cursor_tracking()
{
	// inspired from: http://keithp.com/blogs/Cursor_tracking/
	
	int event, error;
	if (!XQueryExtension(display, "XInputExtension", &xi_opcode, &event, &error))
		return;
	

	int major=2, minor=2;
	if (XIQueryVersion(display, &major, &minor) != Success)
		return;

	XIEventMask evmasks[1];
	unsigned char mask1[(XI_LASTEVENT + 7)/8];
	memset(mask1, 0, sizeof(mask1));

	// select for button and key events from all master devices
	XISetMask(mask1, XI_RawMotion);

	evmasks[0].deviceid = XIAllMasterDevices;
	evmasks[0].mask_len = sizeof(mask1);
	evmasks[0].mask = mask1;

	XISelectEvents(display, root_window, evmasks, 1);

	track_cursor = 1;
}
#endif

#ifdef USE_XDAMAGE
void init_xdamage()
{
	int error_base, major, minor;
	if (	   !XFixesQueryExtension(display, &xfixes_event_base, &error_base)
		|| !XFixesQueryVersion(display, &major, &minor)
		|| (major<2)
		|| !XDamageQueryExtension(display, &xdamage_event_base, &error_base)
		|| !XDamageQueryVersion(display, &major, &minor)
		|| (major<1)
	) {
		return;
	}

	damage = XDamageCreate(display, root_window, XDamageReportBoundingBox);

	XRectangle r;
	r.x = rect.x;
	r.y = rect.y;
	r.width = rect.width;
	r.height = rect.height;

	screen_region = XFixesCreateRegion(display, &r, 1);

	use_xdamage = 1;
}

gboolean on_window_configure_event(GtkWidget *widget, GdkEvent *event, gpointer   user_data)
{
	GdkEventConfigure* e = (GdkEventConfigure*) event;
	gdkwin_extents.x = e->x;
	gdkwin_extents.y = e->y;
	gdkwin_extents.width  = e->width;
	gdkwin_extents.height = e->height;

	if (gdk_rectangle_intersect(&gdkwin_extents, &rect, NULL)) {
		if (window_mapped) {
			window_mapped = 0;
			XUnmapWindow(display, window);
			XUnmapWindow(display, cursor_window);
		}
	} else {
		if (!window_mapped) {
			window_mapped = 1;
			XMapWindow(display, window);
			XMapWindow(display, cursor_window);
		}
	}
	return TRUE;
}
#endif

GOptionEntry option_entries[] = {
  { "version",	'v',	0,	G_OPTION_ARG_NONE,	&config.opt_version,	"Display version information and exit", NULL},
  { "window",	'w',	0,	G_OPTION_ARG_NONE,	&config.opt_window,	"Run inside a window instead of going fullscreen", NULL},
  { NULL }
};

int
main (int argc, char *argv[])
{
	GError *err = NULL;
	GOptionContext *context;

	memset(&config, 0, sizeof(config));

	context = g_option_context_new (NULL);
	g_option_context_add_main_entries (context, option_entries, NULL);
	g_option_context_add_group (context, gtk_get_option_group (TRUE));

	if (!gtk_init_with_args (&argc, &argv, "[MonitorName]", option_entries, NULL, &err))
	{
		error(err->message);
		return 1;
	}

	if (config.opt_version) {
		puts(APPNAME " " VERSION);
		return 0;
	}

	switch (argc)
	{
		case 1:
			config.source_monitor_name = NULL;
			break;
		case 2:
			config.source_monitor_name = argv[1];
			break;
		default:
			error("invalid arguments");
			return 1;
	}

	// initialisation

	GdkDisplay* gdisplay = gdk_display_get_default();
	if (!gdisplay) {
		fprintf (stderr, "error: no display available\n");
		return 1;
	}
	GdkScreen* gscreen = gdk_display_get_default_screen (gdisplay);

	gtkwin = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	{
		// load the icon
		GError* err = NULL;
		GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file (PREFIX "/share/squint/squint.png", &err);
		if (pixbuf)
		{
			gtk_window_set_icon (GTK_WINDOW(gtkwin), pixbuf);
			g_object_unref (pixbuf);
		} else {
			fprintf (stderr, "warning: no icon: %s\n", err->message);
			g_error_free (err);
		}
	}
	// quit on window closed
	g_signal_connect (gtkwin, "destroy", G_CALLBACK (gtk_main_quit), NULL);

	// hide the window on click
	g_signal_connect (gtkwin, "button-press-event", G_CALLBACK (on_window_button_press_event), NULL);
	gdk_window_set_events (gdkwin, gdk_window_get_events(gdkwin) | GDK_BUTTON_PRESS_MASK);

	display = gdk_x11_get_default_xdisplay();

	screen = DefaultScreen (display);

	depth = XDefaultDepth (display, screen);

	// get the root window
	root_window = XDefaultRootWindow(display);
	
	{
		XGCValues values;
		values.subwindow_mode = IncludeInferiors;

		gc = XCreateGC (display, root_window, GCSubwindowMode, &values);
		if(!gc) {
			error ("XCreateGC() failed");
			return 1;
		}

		values.line_width = 3;
		values.foreground = 0xe0e0e0;
		gc_white = XCreateGC (display, root_window, GCLineWidth | GCForeground, &values);
		if(!gc_white) {
			error ("XCreateGC() failed");
			return 1;
		}
	}

	// map my window
	gtk_widget_show_all (gtkwin);

	gdkwin = gtk_widget_get_window(gtkwin);

	// activation
	{
		int n = gdk_screen_get_n_monitors (gscreen);
		if ((n < 2) && !config.source_monitor_name) {
			fprintf (stderr, "error: there is only *one* monitor here, what am I supposed to do?\n");
			return 1;
		}

		int i;
		int min_area = INT_MAX;
		GdkRectangle r;

		if (config.source_monitor_name == NULL)
		{
			// find the smallest screen
			for (i=0 ; i<n ; i++)
			{
				gdk_screen_get_monitor_geometry (gscreen, i, &r);
				int area = r.width * r.height;
				if (area < min_area)
					min_area = area;
			}
		}

		for (i=0 ; i<n ; i++)
		{
			gdk_screen_get_monitor_geometry (gscreen, i, &r);
			int area = r.width * r.height;

			if (config.source_monitor_name == NULL) {
				if (area == min_area)
					break;
			} else {
				if (strcmp (config.source_monitor_name, gdk_screen_get_monitor_plug_name (gscreen, i)) == 0)
					break;
			}
		}

		if (i == n)
		{
			fprintf (stderr, "error: invalid monitor\n");
			return 1;
		}

		rect = r;
		printf("Using monitor %d (%s) %dx%d  +%d+%d\n",
			i, gdk_screen_get_monitor_plug_name (gscreen, i),
			rect.width, rect.height,
			rect.x, rect.y
		);
	}



	offset.x = 0;
	offset.y = 0;
	fullscreen = !config.opt_window;
	if (fullscreen) {
		// get the monitor on which the window is displayed
		int mon = gdk_screen_get_monitor_at_window(gscreen, gdkwin);
		GdkRectangle wa;
		gdk_screen_get_monitor_workarea(gscreen, mon, &wa);

		if ((rect.x == wa.x) && (rect.y == wa.y)) {
			// same as the source monitor
			// -> do NOT go fullscreen
			fullscreen = FALSE;
			fprintf(stderr, "error: cannot duplicate the output on the same monitor, falling back to window mode\n");
		} else {
			// black background
			GdkRGBA black = {0,0,0,1};
			gtk_widget_override_background_color(gtkwin, 0, &black);

			// go full screen
			gdk_window_fullscreen (gdkwin);

			// adjust the offset to draw the screen in the center of the window
			int margin_x = wa.width - rect.width;
			if (margin_x > 0) {
				offset.x = margin_x / 2;
			}
			int margin_y = wa.height - rect.height;
			if (margin_y > 0) {
				offset.y = margin_y /2;
			}
		}
	}


	// create the pixmap
	pixmap = XCreatePixmap (display, root_window, rect.width, rect.height, depth);
	
	// create the sub-window
	{
		XSetWindowAttributes attr;
		attr.background_pixmap = pixmap;
		window = XCreateWindow (display,
					gdk_x11_window_get_xid(gdkwin),
					offset.x, offset.y,
					rect.width, rect.height,
					0, CopyFromParent,
					InputOutput, CopyFromParent,
					CWBackPixmap, &attr);
		XMapWindow(display, window);
	}

#ifdef HAVE_XI
	init_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	init_copy_cursor();
#endif

#ifdef USE_XDAMAGE
	init_xdamage();
#endif

	XFlush (display);

	GValue v = G_VALUE_INIT;
	g_value_init (&v, G_TYPE_INT);

	g_value_set_int (&v, rect.width);
	g_object_set_property (G_OBJECT(gtkwin), "width-request", &v);

	g_value_set_int (&v, rect.height);
	g_object_set_property (G_OBJECT(gtkwin), "height-request", &v);

	// catch all X11 events
	gdk_window_add_filter(NULL, on_x11_event, NULL);
#ifdef USE_XDAMAGE
	if (use_xdamage)
	{
		g_signal_connect (gtkwin, "configure-event", G_CALLBACK (on_window_configure_event), NULL);
	}
#endif
	{
		g_timeout_add (40, &refresh_image, NULL);
	}

	// Redraw the window
	XClearWindow(display, gdk_x11_window_get_xid(gdkwin));

	gtk_main ();

	XFreePixmap (display, pixmap);

	return 0;
}
