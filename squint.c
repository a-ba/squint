#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include <X11/Xlib.h>          /* of course */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>


Window my_w = 0;
Window root_window = 0;
Pixmap pixmap = -1;
int depth = -1, screen = -1;
GC gc = NULL;
GC gc_white = NULL;
Display* display = NULL;
int raised = 0;

GdkRectangle rect;

void
xerror (const char* msg, Display* display, int code)
{
	char buff[128];

	XGetErrorText (display, code, buff, 128);

	fprintf (stderr, "%s: %s\n", msg, buff);
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

	switch (code = XCopyArea (display, pixmap, my_w, gc,
					0, 0,
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

	if (inside_rect && !raised) {
		/* raise the window when the pointer enters the duplicated screen */
		raised = 1;
		int v = XRaiseWindow(display, my_w);
		printf("raise %d", v);
	} else if (!inside_rect && raised) {
		/* lower the window when the pointer leaves the duplicated screen */
		raised = 0;
		int v = XLowerWindow(display, my_w);
		printf("lower %d", v);
	}	

	XFlush (display);

	return TRUE;
}



int
main (int argc, char *argv[])
{
	GtkWidget *window;
	const char* monitor_name;

	gtk_init (&argc, &argv);

	switch (argc)
	{
		case 1:
			monitor_name = NULL;
			break;
		case 2:
			monitor_name = argv[1];
			break;
		default:
			fprintf (stderr, "usage: %s [MonitorName]\n", argv[0]);
			return 1;
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
			fprintf (stderr, "error: you do not have multiple monitors\n");
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


	window = gtk_window_new (GTK_WINDOW_TOPLEVEL);

	{
		// load the icon
		GError* err = NULL;
		GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file (PREFIX "/share/squint/squint.png", &err);
		if (pixbuf)
		{
			gtk_window_set_icon (GTK_WINDOW(window), pixbuf);
			g_object_unref (pixbuf);
		} else {
			fprintf (stderr, "warning: no icon: %s\n", err->message);
			g_error_free (err);
		}
	}

	g_signal_connect (window, "destroy", G_CALLBACK (gtk_main_quit), NULL);

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
	gtk_widget_show_all (window);
//	my_w = XCreateSimpleWindow (display, root_window, 40, 40, 512, 512, 1, 0, 0xffffff);
	my_w = GDK_WINDOW_XID(gtk_widget_get_window (window));

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


	XFlush (display);

	GValue v = G_VALUE_INIT;
	g_value_init (&v, G_TYPE_INT);

	g_value_set_int (&v, rect.width);
	g_object_set_property (G_OBJECT(window), "width-request", &v);

	g_value_set_int (&v, rect.height);
	g_object_set_property (G_OBJECT(window), "height-request", &v);


	g_timeout_add (40, &refresh_image, NULL);

	gtk_main ();

	XFreePixmap (display, pixmap);

	return 0;
}
