#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <X11/Xlib.h>          /* of course */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>


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

int opt_full = 1;

GdkRectangle rect, offset;

void
xerror (const char* msg, Display* display, int code)
{
	char buff[128];

	XGetErrorText (display, code, buff, 128);

	fprintf (stderr, "%s: %s\n", msg, buff);
}

void show()
{
	if (!raised)
	{
		raised = 1;
		gdk_window_raise(gdkwin);
		if (opt_full) {
			gdk_window_fullscreen(gdkwin);
		}
	}
}

void do_hide()
{
	raised = 0;
	if (opt_full) {
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

gboolean
refresh_image (gpointer data)
{
	int code;
	int inside_rect=0;

	switch (code = XCopyArea (display, root_window, pixmap, gc,
					rect.x, rect.y,
					rect.width, rect.height,
					0, 0))
	{
	case BadDrawable:
	case BadGC:
	case BadMatch:
	case BadValue:
		xerror ("XCopyArea() failed", display, code);
		exit(2);
	}

	{
		Window root_return;
		int x, y;
		Window c;
		int cx, cy;
		int mask;
		if ((XQueryPointer (display, root_window, &root_return, &c,
				&x, &y, &cx, &cy, &mask)
			== True) && (root_window == root_return))
		{
			x -= rect.x;
			y -= rect.y;
			if (	(x>=0) && (x< rect.width) &&
				(y>=0) && (y< rect.height)	)
			{
				inside_rect = 1;

				#define LEN 3
				XDrawLine (display, pixmap, gc_white, x-(LEN+1), y, x+(LEN+2), y);
				XDrawLine (display, pixmap, gc_white, x, y-(LEN+1), x, y+(LEN+2));
				XDrawLine (display, pixmap, gc, x-LEN, y, x+LEN, y);
				XDrawLine (display, pixmap, gc, x, y-LEN, x, y+LEN);
			}
		}
	}

	// force refreshing the background of the window
	XClearWindow(display, window);

	if (inside_rect) {
		/* raise the window when the pointer enters the duplicated screen */
		show();
	} else {
		/* lower the window when the pointer leaves the duplicated screen */
		hide();
	}	

	XFlush (display);

	return TRUE;
}

void print_help()
{
	printf(
		"usage: squint [-w] [MonitorName]\n"
		"\n"
		"	MonitorName	name of the monitor to be duplicated (from xrandr)\n"
		"	-v		display version information and exit\n"
		"	-w		run inside a window instead of going fullscreen\n"
	);
	exit(1);
}

int
main (int argc, char *argv[])
{
	const char* monitor_name;

	gtk_init (&argc, &argv);

	int opt;
	while ((opt = getopt(argc, argv, "vw")) != -1)
	{
		switch(opt)
		{
		case 'v':
			puts(APPNAME " " VERSION);
			return 0;
		case 'w':
			opt_full = 0;
			break;
		default:
			print_help();
		}
	}

	switch (argc-optind)
	{
		case 0:
			monitor_name = NULL;
			break;
		case 1:
			monitor_name = argv[optind];
			break;
		default:
			print_help();
	}

	GdkDisplay* gdisplay = gdk_display_get_default();
	if (!gdisplay) {
		fprintf (stderr, "error: no display available\n");
		return 1;
	}
	GdkScreen* gscreen = gdk_display_get_default_screen (gdisplay);

	{
		int n = gdk_screen_get_n_monitors (gscreen);
		if ((n < 2) && !monitor_name) {
			fprintf (stderr, "error: there is only *one* monitor here, what am I supposed to do?\n");
			return 1;
		}

		int i;
		int min_area = INT_MAX;
		GdkRectangle r;

		if (monitor_name == NULL)
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

			if (monitor_name == NULL) {
				if (area == min_area)
					break;
			} else {
				if (strcmp (monitor_name, gdk_screen_get_monitor_plug_name (gscreen, i)) == 0)
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

	display = gdk_x11_get_default_xdisplay();
	//printf("display: %p\n", display);

	screen = DefaultScreen (display);
	//printf("screen: %d\n", screen);

	depth = XDefaultDepth (display, screen);
	//printf("depth: %d\n", depth);

	// get the root window
	root_window = XDefaultRootWindow(display);
	
	{
		XGCValues values;
		values.subwindow_mode = IncludeInferiors;

		gc = XCreateGC (display, root_window, GCSubwindowMode, &values);
		switch ((long)gc)
		{
		case BadAlloc:
		case BadDrawable:
		case BadFont:
		case BadGC:
		case BadMatch:
		case BadPixmap:
		case BadValue:
			xerror ("XCreateGC() failed", display, (long)gc);
			return 1;
		}
		//printf("gc: %d\n", gc);

		values.line_width = 3;
		values.foreground = 0xe0e0e0;
		gc_white = XCreateGC (display, root_window, GCLineWidth | GCForeground, &values);
		switch ((long)gc_white)
		{
		case BadAlloc:
		case BadDrawable:
		case BadFont:
		case BadGC:
		case BadMatch:
		case BadPixmap:
		case BadValue:
			xerror ("XCreateGC() failed", display, (long)gc_white);
			return 1;
		}
		//printf("gc_white: %d\n", gc_white);
	}

	// create my own window
	gtk_widget_show_all (gtkwin);

	gdkwin = gtk_widget_get_window(gtkwin);

	offset.x = 0;
	offset.y = 0;
	if (opt_full) {
		// get the monitor on which the window is displayed
		int mon = gdk_screen_get_monitor_at_window(gscreen, gdkwin);
		GdkRectangle wa;
		gdk_screen_get_monitor_workarea(gscreen, mon, &wa);

		if ((rect.x == wa.x) && (rect.y == wa.y)) {
			// same as the source monitor
			// -> do NOT go fullscreen
			opt_full = 0;
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

	// quit on window closed
	g_signal_connect (gtkwin, "destroy", G_CALLBACK (gtk_main_quit), NULL);

	// hide the window on click
	g_signal_connect (gtkwin, "button-press-event", G_CALLBACK (on_window_button_press_event), NULL);
	gdk_window_set_events (gdkwin, gdk_window_get_events(gdkwin) | GDK_BUTTON_PRESS_MASK);

	// create the pixmap
	pixmap = XCreatePixmap (display, root_window, rect.width, rect.height, depth);
	switch (pixmap)
	{
	case BadAlloc:
	case BadDrawable:
	case BadPixmap:
	case BadValue:
		xerror ("XCreatePixmap() failed", display, pixmap);
		return 1;
	}
	//printf("pixmap: %p\n", pixmap);
	
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



	XFlush (display);

	GValue v = G_VALUE_INIT;
	g_value_init (&v, G_TYPE_INT);

	g_value_set_int (&v, rect.width);
	g_object_set_property (G_OBJECT(gtkwin), "width-request", &v);

	g_value_set_int (&v, rect.height);
	g_object_set_property (G_OBJECT(gtkwin), "height-request", &v);


	g_timeout_add (40, &refresh_image, NULL);

	gtk_main ();

	XFreePixmap (display, pixmap);

	return 0;
}
