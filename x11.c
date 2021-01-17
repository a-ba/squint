#include "config.h"

#include <stdint.h>

#include <gdk/gdkx.h>

#include "squint.h"

#include <X11/Xlib.h>
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
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

static Window root_window = 0;
static GdkRectangle root_window_rect;
static Window window = 0;
static Pixmap pixmap = -1;
static int depth = -1, screen = -1;
static GC gc = NULL;
static GC gc_white = NULL;
static Display* display = NULL;
static gint refresh_timer = 0;
static Atom net_active_window_atom = 0;

static Window active_window = 0;

static GdkPoint offset;
static GdkPoint cursor;

#ifdef HAVE_XI
static gboolean track_cursor = FALSE;
static int xi_opcode = 0;
#endif

#ifdef USE_XDAMAGE
static gboolean use_xdamage = FALSE;
static int window_mapped = 1;
static int xdamage_event_base;
static Damage damage = 0;
static XserverRegion screen_region = 0;
static int min_refresh_period=0;
static Time next_refresh=0;
static gint refresh_timeout=0;
#endif

#ifdef HAVE_XFIXES
static int xfixes_event_base;
#endif

#ifdef COPY_CURSOR
#define CURSOR_SIZE 64
static int copy_cursor = 0;
static Window cursor_window = 0;
static Pixmap cursor_pixmap = 0;
static XImage* cursor_image = NULL;
static Pixmap  cursor_mask_pixmap = 0;
static XImage* cursor_mask_image = NULL;
static GC	cursor_mask_gc = NULL;
static int cursor_xhot=0;
static int cursor_yhot=0;
static uint32_t* cursor_pixels;
static uint8_t*  cursor_mask_pixels;
#define CURSOR_MASK_SIZE (CURSOR_SIZE*CURSOR_SIZE/8)
#endif

#ifdef HAVE_XRANDR
static int xrandr_event_base = 0;
#endif



void
x11_adjust_offset_value(gint* offset, gint src, gint dst, gint cursor)
{
	if (dst >= src)
	{
		// dst window is enough big 
		// -> static offset
		*offset = (dst - src) / 2;
	} else {
		// dynamic offset
		if (cursor >= 0) {
			// on-screen
			int v = cursor + *offset;
			
			if (v < 0) {
				*offset -= v;
			} else if (v >= dst) {
				*offset -= v - dst;
			}
		} else {
			// off-screen
			// -> update only if there is unused space
			if (*offset > 0) {
				*offset = 0;
			} else {
				int min_offset = dst - src;
				if (*offset < min_offset) {
					*offset = min_offset;
				}
			}
		}
	}
}

void
x11_fix_offset()
{
	GdkPoint offset_bak = {offset.x, offset.y};

	// Adjust the offsets
	x11_adjust_offset_value(&offset.x, src_rect.width,  dst_rect.width,  cursor.x);
	x11_adjust_offset_value(&offset.y, src_rect.height, dst_rect.height, cursor.y);
	
	
	if (memcmp(&offset, &offset_bak, sizeof(offset)))
	{
		// offset was updated
		// -> move the windows
		XMoveWindow(display, window, offset.x, offset.y);
#ifdef COPY_CURSOR
		if (copy_cursor) {
			XMoveWindow (display, cursor_window,
					cursor.x - cursor_xhot + offset.x,
					cursor.y - cursor_yhot + offset.y);
		}
#endif
		// force redrawing the window
		XClearWindow(display, window);
	}
}


void
x11_refresh_cursor_location(gboolean force)
{
	Window root_return, w;
	int wx, wy;
	unsigned int mask;
	GdkPoint c;
	XQueryPointer(display, root_window, &root_return, &w,
			&c.x, &c.y, &wx, &wy, &mask);

	c.x -= src_rect.x;
	c.y -= src_rect.y;

	if ((c.x<0) | (c.y<0) | (c.x>=src_rect.width) | (c.y>=src_rect.height))
	{
		// cursor is outside the duplicated screen
		c.x = c.y = -1;
	}

	gboolean entered_screen, left_screen;

	if (force) {
		entered_screen = (c.x >=0);
		left_screen = !entered_screen;
	} else {
		entered_screen = (cursor.x<0) && (c.x>=0);
		left_screen    = (cursor.x>=0) && (c.x<0);
	}

	// cursor was really moved
	cursor = c;

#ifdef COPY_CURSOR
	if (copy_cursor) {
		if (left_screen) {
			XUnmapWindow(display, cursor_window);
		} else {
			if (entered_screen) {
				XMapWindow(display, cursor_window);
			}
			// move the cursor window to the new location of the cursor
			XMoveWindow (display, cursor_window,
					cursor.x - cursor_xhot + offset.x,
					cursor.y - cursor_yhot + offset.y);

			// force redrawing the window
			XClearWindow(display, cursor_window);
		}
	}
#endif

	if (cursor.x >= 0) {
		/* raise the window when the pointer enters the duplicated screen */
		squint_show();
	} else {
		/* lower the window when the pointer leaves the duplicated screen */
		squint_hide();
	}

	x11_fix_offset();
}

gboolean
x11_refresh_image (gpointer data)
{
#ifdef HAVE_XI
	if (!track_cursor)
#endif
	{
		x11_refresh_cursor_location(FALSE);
	}

	XCopyArea (display, root_window, pixmap, gc,
					src_rect.x, src_rect.y,
					src_rect.width, src_rect.height,
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

#ifdef USE_XDAMAGE
void x11_try_refresh_image (Time timestamp);

gboolean 
x11_try_refresh_image_timeout (gpointer data)
{
	refresh_timeout=0;

	if ((Time)data == next_refresh)
	{
		x11_try_refresh_image(next_refresh);
	}
	return FALSE;
}

void
x11_try_refresh_image (Time timestamp)
{
	if ((timestamp >= next_refresh) || (timestamp < next_refresh - 1000)) {
		if (refresh_timeout) {
			g_source_remove(refresh_timeout);
			refresh_timeout=0;
		}
		next_refresh = timestamp + min_refresh_period;
		x11_refresh_image(NULL);

	} else if (!refresh_timeout) {
		refresh_timeout = g_timeout_add (next_refresh - timestamp, x11_try_refresh_image_timeout, (gpointer)next_refresh);
	}
}
#endif



#ifdef COPY_CURSOR
void
x11_refresh_cursor_image()
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
	memset(cursor_mask_pixels, 0, CURSOR_MASK_SIZE);
	for(y=0 ; y<height ; y++)
	{
		for(x=0 ; x<width ; x++)
		{
			uint8_t alpha = img->pixels[y*img->width + x] >> 24;
			if (alpha > 0xb0)
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

// enable the duplication of the cursor (with XFixes & XShape)
//
// state:
// 	copy_cursor
//
// initialises:
// 	cursor_image
// 	cursor_pixmap
// 	cursor_mask_image
// 	cursor_mask_pixmap
// 	cursor_mask_gc
// 	cursor_window
//
void
x11_enable_copy_cursor()
{
	if (copy_cursor) {
		return;
	}

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
	cursor_pixels = (uint32_t*) malloc(sizeof(*cursor_pixels)*CURSOR_SIZE*CURSOR_SIZE);
	cursor_image = XCreateImage (display, NULL, 24, ZPixmap, 0, (char*)cursor_pixels,
				CURSOR_SIZE, CURSOR_SIZE, 32, 256);
	if (!cursor_image) {
		squint_error("XCreateImage() failed");
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
	cursor_mask_pixels = (uint8_t*) malloc(CURSOR_MASK_SIZE);
	cursor_mask_image = XCreateImage (display, NULL, 1, ZPixmap, 0, (char*)cursor_mask_pixels,
				CURSOR_SIZE, CURSOR_SIZE, 8, CURSOR_SIZE/8);
	if (!cursor_mask_image) {
		squint_error("XCreateImage() failed");
		return;
	}

	// create a context for manipulating the mask
	cursor_mask_gc = XCreateGC(display, cursor_mask_pixmap, 0, NULL); 
	if(!cursor_mask_gc) {
		squint_error("XCreateGC() failed");
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
	x11_refresh_cursor_image();

	x11_refresh_cursor_location(TRUE);

	// request cursor change notifications
	XFixesSelectCursorInput(display, gdk_x11_window_get_xid(gdkwin), XFixesCursorNotify);

	copy_cursor = TRUE;
}

void
x11_disable_copy_cursor()
{
	if (!copy_cursor) {
		return;
	}

	XDestroyWindow(display, cursor_window);
	cursor_window = 0;

	XFreeGC(display, cursor_mask_gc);
	cursor_mask_gc = 0;

	XFreePixmap(display, cursor_pixmap);
	XFreePixmap(display, cursor_mask_pixmap);
	cursor_pixmap = cursor_mask_pixmap = 0;

	XDestroyImage(cursor_image);
	XDestroyImage(cursor_mask_image);
	cursor_image = cursor_mask_image = NULL;

	copy_cursor = FALSE;
}
#endif


void
x11_show_active_window()
{
	if (!active_window)
		return;

	// check if it overlaps more whith the src or the dst window
	GdkRectangle inter_src, inter_dst;
	gdk_rectangle_intersect(&active_window_rect, &src_rect, &inter_src);
	gdk_rectangle_intersect(&active_window_rect, &dst_rect, &inter_dst);

	if((inter_src.height*inter_src.width) > (inter_dst.height*inter_dst.width))
	{
		// the active window overlaps more with the source screen
		squint_show();
	} else {
		// the active window overlaps more with the destination screen
		squint_hide();
	}
}

gboolean
x11_get_window_geometry(Window w, GdkRectangle* r)
{
	gboolean result;
	Window root;
	int x, y;
	unsigned int width, height, border_width, depth;

	gdk_x11_display_error_trap_push(gdisplay);

	if (XGetGeometry(display, w, &root, &x, &y, &width, &height,
			&border_width, &depth))
	{
		r->x = x - border_width;
		r->y = y - border_width;
		r->width  = width  + 2*border_width;
		r->height = height + 2*border_width;

		result = TRUE;
	} else {
		result = FALSE;
	}
	gdk_x11_display_error_trap_pop_ignored(gdisplay);

	return result;
}

void
x11_refresh_active_window_geometry()
{
	if(!active_window)
		return;

	// ignore X11 errors (this function can produce BadWindow errors since
	// it makes queries on windows controlled by other applications)
	gdk_x11_display_error_trap_push(gdisplay);

	{
		Window w = active_window;
		Window root, parent, *children;
		unsigned int nchildren;

		// identify the top-level window
		parent = w;
		while(parent != root_window)
		{
			w = parent;
			if(!XQueryTree(display, w, &root, &parent, &children, &nchildren))
				goto err;
			XFree(children);
		}
		
		// get its coordinates
		x11_get_window_geometry(w, &active_window_rect);
	}
err:	
	gdk_x11_display_error_trap_pop_ignored(gdisplay);
}

Window
x11_get_active_window()
{
	Window result = 0;
	Window* w;
	Atom actual_type_return;
	int  actual_format_return;
	unsigned long nitems_return, bytes_after_return;

	if (XGetWindowProperty(display, root_window, net_active_window_atom, 0, 1,
			FALSE, AnyPropertyType,	&actual_type_return,
			&actual_format_return, &nitems_return,
			&bytes_after_return, (unsigned char**)&w)
		== Success)
	{
		result = *w;
		XFree(w);
	}
	return result;
}

void
x11_active_window_stop_monitoring()
{
	if (!gdk_x11_window_lookup_for_display(gdisplay, active_window))
	{
		// ignore X11 errors (this function can produce BadWindow errors since
		// it makes queries on windows controlled by other applications)
		gdk_x11_display_error_trap_push(gdisplay);

		XSetWindowAttributes attr;
		attr.event_mask = 0;
		XChangeWindowAttributes(display, active_window, CWEventMask, &attr);

		gdk_x11_display_error_trap_pop_ignored(gdisplay);
	} 
	active_window = 0;
}

void
x11_active_window_start_monitoring()
{
	if (active_window)
		x11_active_window_stop_monitoring();

	active_window = x11_get_active_window();
	if (!active_window)
		return;

	x11_refresh_active_window_geometry();
	if (!memcmp(&active_window_rect, &root_window_rect, sizeof(GdkRectangle))) {
		// same geometry as the root window
		// -> ignore it
		// TODO: make the match more loose
		active_window = 0;
		return;
	}

	if (!gdk_x11_window_lookup_for_display(gdisplay, active_window))
	{
		// this is a foreign window
		// -> we need to monitor it explicitely
		
		 
		// ignore X11 errors (this function can produce BadWindow errors since
		// it makes queries on windows controlled by other applications)
		gdk_x11_display_error_trap_push(gdisplay);

		XSetWindowAttributes attr;
		attr.event_mask = StructureNotifyMask;
		XChangeWindowAttributes(display, active_window, CWEventMask, &attr);

		gdk_x11_display_error_trap_pop_ignored(gdisplay);
	}
}

GdkFilterReturn
x11_on_x11_event (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
	XEvent* ev = (XEvent*)xevent;
	XGenericEventCookie *cookie = &ev->xcookie;


	if (ev->type == PropertyNotify)
	{
		XPropertyEvent* pn_ev = (XPropertyEvent*) ev;
		if ((pn_ev->window == root_window) && (pn_ev->atom == net_active_window_atom))
		{
			// property _NET_ACTIVE_WINDOW was changed
			x11_active_window_start_monitoring();
			x11_show_active_window();
			return GDK_FILTER_REMOVE;
		}
		
	}

	if (ev->type == ConfigureNotify)
	{
		XConfigureEvent* c_ev = (XConfigureEvent*) ev;
		if (c_ev->window == active_window)
		{
			x11_refresh_active_window_geometry();
			return GDK_FILTER_CONTINUE;
		}
	}

#ifdef HAVE_XI
	if(track_cursor)
	{
		if (	(cookie->type == GenericEvent)
		    &&	(cookie->extension == xi_opcode))
		{
			switch (cookie->evtype)
			{
			case XI_RawMotion:
				// cursor was moved
				x11_refresh_cursor_location(FALSE);
#ifdef USE_XDAMAGE
#ifdef COPY_CURSOR
				if(!copy_cursor)
#endif
				if(use_xdamage) {
					x11_refresh_image(NULL);
				}
#endif
				return GDK_FILTER_REMOVE;
			case XI_RawKeyPress:
				// a key was pressed
				// -> we ensure that the active window is on screen
				{
					XIRawEvent* xi_ev = (XIRawEvent*) cookie->data;
					GdkKeymap* km = gdk_keymap_get_for_display(gdisplay);
					guint keyval;

					if(gdk_keymap_translate_keyboard_state(km, xi_ev->detail,
							0, // FIXME: do we need the modifier?
							0, // FIXME: how to determine the group?
							&keyval, NULL, NULL, NULL))
					{
						switch(keyval)
						{
						case  GDK_KEY_Control_L:
						case  GDK_KEY_Control_R:
						case  GDK_KEY_Meta_L:
						case  GDK_KEY_Meta_R:
						case  GDK_KEY_Alt_L:
						case  GDK_KEY_Alt_R:
							// ignore modifier keys (except shift)
							// because they may be used by the
							// window manager
							return GDK_FILTER_REMOVE;
						}
					}
				
					x11_show_active_window();
					return GDK_FILTER_REMOVE;
				}
			}
		}
	}
#endif

#ifdef COPY_CURSOR
	if(copy_cursor)
	{
		if (ev->type == xfixes_event_base + XFixesCursorNotify) {
			x11_refresh_cursor_image();

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
			if (!xd_ev->more && !gdk_rectangle_intersect(&src_rect, &dst_rect, NULL)) {
				// check if damage intersects with the screen
				GdkRectangle damage_rect;
				damage_rect.x = xd_ev->area.x;
				damage_rect.y = xd_ev->area.y;
				damage_rect.width  = xd_ev->area.width;
				damage_rect.height = xd_ev->area.height;

				if (gdk_rectangle_intersect(&damage_rect, &src_rect, NULL))
				{
					x11_try_refresh_image(xd_ev->timestamp);
				}
			}
			XDamageSubtract(display, damage, 0, 0);
		}
	}
#endif

#ifdef HAVE_XRANDR
	if (xrandr_event_base)
	{
		if (ev->type == xrandr_event_base + RRScreenChangeNotify) {
			squint_disable();
		}
	}
#endif
	return GDK_FILTER_CONTINUE;
}

#ifdef HAVE_XI
void
x11_set_xi_eventmask(gboolean active)
{
	XIEventMask evmasks[1];
	unsigned char mask1[(XI_LASTEVENT + 7)/8];
	memset(mask1, 0, sizeof(mask1));

	if (active) {
		// select for button and key events from all master devices
		XISetMask(mask1, XI_RawMotion);
		XISetMask(mask1, XI_RawKeyPress);
	}

	evmasks[0].deviceid = XIAllMasterDevices;
	evmasks[0].mask_len = sizeof(mask1);
	evmasks[0].mask = mask1;

	XISelectEvents(display, root_window, evmasks, 1);
}

// enable notification of motion events (XI_RawMotion)
//
// state:
// 	track_cursor
//
// initialises:
// 	xi_opcode
//
void
x11_enable_cursor_tracking()
{
	if (track_cursor)
		return;

	// inspired from: http://keithp.com/blogs/Cursor_tracking/
	
	int event, error;
	if (!XQueryExtension(display, "XInputExtension", &xi_opcode, &event, &error))
		return;
	
	int major=2, minor=2;
	if (XIQueryVersion(display, &major, &minor) != Success)
		return;

	x11_set_xi_eventmask(TRUE);

	track_cursor = TRUE;
}

void
x11_disable_cursor_tracking()
{
	if(!track_cursor)
		return;

	x11_set_xi_eventmask(FALSE);

	track_cursor = FALSE;
}
#endif

#ifdef USE_XDAMAGE
// enable notification of screen updates (XDamageNotify)
//
// state:
// 	use_xdamage
//
// initialises:
// 	damage
// 	screen_region
//
void
x11_enable_xdamage()
{
	if (use_xdamage) {
		return;
	}

	if (config.opt_rate > 0) {
		// user requested fixed refresh rate
		return;
	}

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

	if (config.opt_limit == 0) {
		// no limit
		min_refresh_period = 0;
	} else {
		// 50 fps by default
		min_refresh_period = 1000 / ((config.opt_limit<0) ? 50 : config.opt_limit); 
	}

	damage = XDamageCreate(display, root_window, XDamageReportBoundingBox);

	XRectangle r;
	r.x = src_rect.x;
	r.y = src_rect.y;
	r.width = src_rect.width;
	r.height = src_rect.height;

	screen_region = XFixesCreateRegion(display, &r, 1);

	use_xdamage = TRUE;
}

void
x11_disable_xdamage()
{
	if (!use_xdamage) {
		return;
	}

	if (screen_region) {
		XFixesDestroyRegion(display, screen_region);
		screen_region = 0;
	}

	if (damage) {
		XDamageDestroy(display, damage);
		damage = 0;
	}

	use_xdamage = FALSE;
}
#endif


gboolean
x11_on_window_configure_event(GtkWidget *widget, GdkEvent *event, gpointer   user_data)
{
	GdkEventConfigure* e = (GdkEventConfigure*) event;
	GdkRectangle rect;
	rect.x = e->x;
	rect.y = e->y;
	rect.width  = e->width;
	rect.height = e->height;

	if(!enabled) {
		return TRUE;
	}

	if(!fullscreen) {
		memcpy(&dst_rect, &rect, sizeof(rect));
		x11_fix_offset();
	}

#ifdef USE_XDAMAGE
	if (use_xdamage)
	{
		if (gdk_rectangle_intersect(&rect, &src_rect, NULL)) {
			if (window_mapped) {
				window_mapped = 0;
				XUnmapWindow(display, window);
#ifdef COPY_CURSOR
				if(copy_cursor) {
					XUnmapWindow(display, cursor_window);
				}
#endif
			}
		} else {
			if (!window_mapped) {
				window_mapped = 1;
				XMapWindow(display, window);
#ifdef COPY_CURSOR
				if(copy_cursor) {
					XMapWindow(display, cursor_window);
				}
#endif
			}
		}
	}
#endif
	return TRUE;
}

#ifdef HAVE_XRANDR
void
x11_init_xrandr()
{
	int error;
	if (!XRRQueryExtension(display, &xrandr_event_base, &error)) {
		return;
	}

	XRRSelectInput(display, root_window, RRScreenChangeNotifyMask);
}
#endif

gboolean
x11_init()
{
	display = gdk_x11_get_default_xdisplay();

	screen = DefaultScreen (display);

	depth = XDefaultDepth (display, screen);

	// get the root window
	root_window = XDefaultRootWindow(display);
	
	// create the graphic contextes
	{
		XGCValues values;
		values.subwindow_mode = IncludeInferiors;

		gc = XCreateGC (display, root_window, GCSubwindowMode, &values);
		if(!gc) {
			squint_error("XCreateGC() failed");
			return FALSE;
		}

		values.line_width = 3;
		values.foreground = 0xe0e0e0;
		gc_white = XCreateGC (display, root_window, GCLineWidth | GCForeground, &values);
		if(!gc_white) {
			squint_error("XCreateGC() failed");
			return FALSE;
		}
	}

#ifdef HAVE_XRANDR
	x11_init_xrandr();
#endif

	// atom name
	net_active_window_atom = XInternAtom(display, "_NET_ACTIVE_WINDOW", FALSE);

	return TRUE;
}

//
// Prepare the window to host the duplicated screen (create the pixmap, subwindow)
//
// initialises:
// 	offset
// 	pixmap
// 	window
//	cursor
//

void
x11_enable_window()
{
	cursor.x = -1;
	cursor.y = -1;
	offset.x = 0;
	offset.y = 0;

	if (!fullscreen) {
		// register events
		// - window moved/resized
		g_signal_connect (gtkwin, "configure-event", G_CALLBACK (x11_on_window_configure_event), NULL);
	}

	// create the pixmap
	pixmap = XCreatePixmap (display, root_window, src_rect.width, src_rect.height, depth);
	
	// create the sub-window
	{
		XSetWindowAttributes attr;
		attr.background_pixmap = pixmap;
		window = XCreateWindow (display,
					gdk_x11_window_get_xid(gdkwin),
					offset.x, offset.y,
					src_rect.width, src_rect.height,
					0, CopyFromParent,
					InputOutput, CopyFromParent,
					CWBackPixmap, &attr);
		XMapWindow(display, window);
	}

	// force refreshing the cursor position
	x11_refresh_cursor_location(TRUE);
}

void
x11_disable_window()
{
	XDestroyWindow(display, window);
	window = 0;

	XFreePixmap(display, pixmap);
	pixmap = 0;
}


void
x11_enable_focus_tracking()
{
	memset(&active_window_rect, 0, sizeof(active_window_rect));

	XSetWindowAttributes attr;
	attr.event_mask = PropertyChangeMask;
	XChangeWindowAttributes(display, root_window, CWEventMask, &attr);
}

void
x11_disable_focus_tracking()
{
	XSetWindowAttributes attr;
	attr.event_mask = 0;
	XChangeWindowAttributes(display, root_window, CWEventMask, &attr);
}

void
x11_enable()
{
	x11_enable_window();

	x11_enable_focus_tracking();
	
#ifdef HAVE_XI
	x11_enable_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	x11_enable_copy_cursor();
#endif

#ifdef USE_XDAMAGE
	x11_enable_xdamage();
#endif

	XFlush (display);

	x11_get_window_geometry(root_window, &root_window_rect);
	x11_active_window_start_monitoring();

	// catch all X11 events
	gdk_window_add_filter(NULL, x11_on_x11_event, NULL);

#if USE_XDAMAGE && HAVE_XI
	if (!(use_xdamage && track_cursor))
#endif
	{
		int rate = 25; // default to 25 fps
		if(config.opt_rate > 0) {
			rate = config.opt_rate;
		} else if ((config.opt_limit > 0) && (config.opt_limit < rate)) {
			rate = config.opt_limit;
		}

		refresh_timer = g_timeout_add (1000/rate, &x11_refresh_image, NULL);
	}

	// Redraw the window
	XClearWindow(display, gdk_x11_window_get_xid(gdkwin));
}

void
x11_disable()
{
#ifdef USE_XDAMAGE
	if (refresh_timeout) {
		g_source_remove(refresh_timeout);
		refresh_timeout = 0;
	}
#endif
	if (refresh_timer) {
		g_source_remove(refresh_timer);
		refresh_timer = 0;
	}

	gdk_window_remove_filter(NULL, x11_on_x11_event, NULL);

	x11_active_window_stop_monitoring();

#ifdef HAVE_XI
	x11_disable_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	x11_disable_copy_cursor();
#endif

#ifdef USE_XDAMAGE
	x11_disable_xdamage();
#endif
	x11_disable_focus_tracking();

	x11_disable_window();
}