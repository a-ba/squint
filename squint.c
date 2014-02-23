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
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif
#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Config
struct {
	const char* src_monitor_name;
	const char* dst_monitor_name;

	gboolean opt_version, opt_window, opt_disable;
} config;

// State
gboolean enabled = FALSE;
gboolean fullscreen = FALSE;
int src_monitor, dst_monitor;


GtkWidget* gtkwin = NULL;
GdkWindow* gdkwin = NULL;
GtkStatusIcon* status_icon = NULL;
GtkWidget* menu = NULL;
Window root_window = 0;
Window window = 0;
Pixmap pixmap = -1;
int depth = -1, screen = -1;
GC gc = NULL;
GC gc_white = NULL;
Display* display = NULL;
GdkScreen* gscreen = NULL;
int raised = 0;
GdkPixbuf* icon_enabled  = NULL;
GdkPixbuf* icon_disabled = NULL;
gint refresh_timer = 0;

GdkRectangle src_rect, dst_rect;
GdkPoint offset;
GdkPoint cursor;

#ifdef HAVE_XI
gboolean track_cursor = FALSE;
int xi_opcode = 0;
#endif

#ifdef USE_XDAMAGE
gboolean use_xdamage = FALSE;
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
uint32_t* cursor_pixels;
uint8_t*  cursor_mask_pixels;
#define CURSOR_MASK_SIZE (CURSOR_SIZE*CURSOR_SIZE/8)
#endif

#ifdef HAVE_XRANDR
int xrandr_event_base = 0;
#endif

void disable();

void
show_about_dialog()
{
	static const char* authors[] = { "Anthony Baire", NULL };
	gtk_show_about_dialog(
		NULL,
		"copyright",	"© 2013, 2014 Anthony Baire",
		"license-type",	GTK_LICENSE_GPL_3_0,
		"logo",		icon_enabled,
		"program-name",	APPNAME " " VERSION,
		"website",	"https://bitbucket.org/a_ba/squint",
		"website-label","https://bitbucket.org/a_ba/squint",
		NULL
	);
}

void
error (const char* msg)
{
#ifdef HAVE_LIBNOTIFY
	if (notify_is_initted())
	{
		NotifyNotification* notif = notify_notification_new(
			APPNAME " error",
			msg,
			NULL
		);
		if(icon_enabled) {
			notify_notification_set_image_from_pixbuf(
				notif, icon_enabled);
		}

		gboolean ok = notify_notification_show(notif, NULL);
		g_object_unref(notif);
		if(ok)
			return;
	}
#endif
	fprintf(stderr, "error: %s\n", msg);
}

void
show()
{
	if (!raised)
	{
		raised = 1;
		if (fullscreen) {
			gtk_widget_show(gtkwin);
		} else {
			gdk_window_raise(gdkwin);
		}
	}
}

void
do_hide()
{
	raised = 0;
	if (fullscreen) {
		gtk_widget_hide(gtkwin);
	} else {
		gdk_window_lower(gdkwin);
	}
}

void
hide()
{
	if(raised)
	{
		do_hide();
	}
}

void
adjust_offset_value(gint* offset, gint src, gint dst, gint cursor)
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
fix_offset()
{
	GdkPoint offset_bak = {offset.x, offset.y};

	// Adjust the offsets
	adjust_offset_value(&offset.x, src_rect.width,  dst_rect.width,  cursor.x);
	adjust_offset_value(&offset.y, src_rect.height, dst_rect.height, cursor.y);
	
	
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

gboolean
on_window_button_press_event(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	GdkEventButton* ev = (GdkEventButton*) event;

	if (enabled
		&& (ev->type == GDK_2BUTTON_PRESS)
		&& (ev->button = 1)
	) {
		gdk_window_unmaximize(gdkwin);
		gdk_window_resize(gdkwin, src_rect.width, src_rect.height);
	}
	return FALSE;
}

gboolean
on_window_delete_event(GtkWidget* widget, GdkEvent* event, gpointer data)
{
	disable();
	return TRUE;
}

#define ITEM_MASK		0xffffff00
#define ITEM_ENABLE		(1<<8)
#define ITEM_FULLSCREEN		(1<<9)
#define ITEM_QUIT		(1<<10)
#define ITEM_SRC_MONITOR	(1<<11)
#define ITEM_DST_MONITOR	(1<<12)
#define ITEM_ABOUT		(1<<13)
#define ITEM_AUTO		0xff

void
update_monitor_config(const char** monitor_name, int id)
{
	if (*monitor_name) {
		g_free((gpointer)*monitor_name);
	}
	*monitor_name = NULL;

	if (id != ITEM_AUTO)
	{
		*monitor_name = gdk_screen_get_monitor_plug_name(gscreen, id);
	}
}

void
on_menu_item_activate(gpointer pointer, gpointer user_data)
{
	intptr_t code = (intptr_t)user_data;
	switch (code & ITEM_MASK)
	{
	case ITEM_ENABLE:
		on_status_icon_activated(NULL, NULL);
		break;
	
	case ITEM_FULLSCREEN:
		config.opt_window = !config.opt_window;
		goto reset;

	case ITEM_QUIT:
		gtk_main_quit();
		break;
	
	case ITEM_SRC_MONITOR:
		update_monitor_config(&config.src_monitor_name, code & ITEM_AUTO);
		goto reset;
		
	case ITEM_DST_MONITOR:
		update_monitor_config(&config.dst_monitor_name, code & ITEM_AUTO);
		goto reset;
	
	case ITEM_ABOUT:
		show_about_dialog();
		break;
	}
	return;
reset:
	if (enabled) {
		disable();
		enable();
	}
}

gboolean
on_status_icon_activated(GtkWidget* widget, gpointer data)
{
	if (enabled) {
		disable();
	} else {
		enable();
	}
	return FALSE;
}

void populate_menu_with_monitor_config(GtkWidget* menu, const char* description, const char** config_name, int active_id, intptr_t userdata)
{
	GtkWidget* item;

	item = gtk_separator_menu_item_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	item = gtk_menu_item_new_with_label(description);
	gtk_widget_set_sensitive(item, FALSE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	GtkWidget* auto_item;
	auto_item = gtk_check_menu_item_new_with_label("Auto");
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(auto_item), !*config_name);
	g_signal_connect (auto_item, "activate", G_CALLBACK (on_menu_item_activate),
			(gpointer) (userdata | ITEM_AUTO));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), auto_item);

	int i, n = gdk_screen_get_n_monitors (gscreen);
	int current = -1;
	char buff[64];
	for (i=0 ; i<n ; i++)
	{
		char* name = gdk_screen_get_monitor_plug_name(gscreen, i);
		if (enabled && !*config_name && (i==active_id)) {
			current = i;
			g_snprintf(buff, 64, "Auto (%s)", name);
			gtk_menu_item_set_label(GTK_MENU_ITEM(auto_item), buff);
		}

		GdkRectangle r;
		gdk_screen_get_monitor_geometry (gscreen, i, &r);

		g_snprintf(buff, 64, "%s %d×%d", name , r.width, r.height);

		item = gtk_check_menu_item_new_with_label(buff);
		
		if (*config_name && !strcmp(*config_name, name)) {
			current = i;
			gtk_check_menu_item_set_active(
				GTK_CHECK_MENU_ITEM(item), TRUE);
		}
		g_signal_connect (item, "activate", G_CALLBACK (on_menu_item_activate),
				(gpointer) (userdata | (i&0xff)));
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

		g_free(name);
	}
	if ((current<0) && *config_name) {
		// choosen monitor is not active
		item = gtk_check_menu_item_new_with_label(*config_name);
		gtk_check_menu_item_set_active(
			GTK_CHECK_MENU_ITEM(item), TRUE);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	}
}

void
destroy_widget(GtkWidget* widget, gpointer data)
{
	gtk_widget_destroy(widget);
}

void
on_status_icon_popup_menu(GtkStatusIcon* widget, guint button, guint activate_time, gpointer data)
{
	GtkWidget* item;

	// clear the previous menu
	gtk_container_foreach(GTK_CONTAINER(menu), &destroy_widget, NULL);

	item = gtk_menu_item_new_with_label("Squint");
	gtk_widget_set_sensitive(item, FALSE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	item = gtk_check_menu_item_new_with_label("Enabled");
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), enabled);
	g_signal_connect (item, "activate", G_CALLBACK (on_menu_item_activate),
			(gpointer) ITEM_ENABLE);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	item = gtk_check_menu_item_new_with_label("Fullscreen");
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), !config.opt_window);
	g_signal_connect (item, "activate", G_CALLBACK (on_menu_item_activate),
			(gpointer) ITEM_FULLSCREEN);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	
	item = gtk_image_menu_item_new_from_stock(GTK_STOCK_ABOUT, NULL);
	g_signal_connect (item, "activate", G_CALLBACK (on_menu_item_activate),
			(gpointer) ITEM_ABOUT);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	item = gtk_image_menu_item_new_from_stock(GTK_STOCK_QUIT, NULL);
	g_signal_connect (item, "activate", G_CALLBACK (on_menu_item_activate),
			(gpointer) ITEM_QUIT);
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

	populate_menu_with_monitor_config(menu, "Source monitor", &config.src_monitor_name, src_monitor, ITEM_SRC_MONITOR);
	populate_menu_with_monitor_config(menu, "Destination monitor", &config.dst_monitor_name, dst_monitor, ITEM_DST_MONITOR);

	gtk_widget_show_all(menu);
	gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, button, activate_time);
}

void
refresh_status_icon()
{
	if (enabled) {
		if (icon_enabled) {
			gtk_status_icon_set_from_pixbuf(status_icon, icon_enabled);
		} else {
			gtk_status_icon_set_from_stock(status_icon, GTK_STOCK_YES);
		}
		gtk_status_icon_set_tooltip_text(status_icon,
			"squint enabled"
		);
	} else {
		if (icon_disabled) {
			gtk_status_icon_set_from_pixbuf(status_icon, icon_disabled);
		} else {
			gtk_status_icon_set_from_stock(status_icon, GTK_STOCK_NO);
		}
			gtk_status_icon_set_tooltip_text(status_icon,
			"squint disabled"
		);
	}
}

void
refresh_cursor_location(gboolean force)
{
	Window root_return, w;
	int wx, wy, mask;
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
		if (memcmp(&cursor, &c, sizeof(cursor)) == 0) {
			// nothing to do
			return;
		}

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
		show();
	} else {
		/* lower the window when the pointer leaves the duplicated screen */
		hide();
	}

	fix_offset();
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
		refresh_cursor_location(FALSE);
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

void
try_refresh_image (Time timestamp)
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
void
refresh_cursor_image()
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
enable_copy_cursor()
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
	cursor_mask_pixels = (uint8_t*) malloc(CURSOR_MASK_SIZE);
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

	refresh_cursor_location(TRUE);

	// request cursor change notifications
	XFixesSelectCursorInput(display, gdk_x11_window_get_xid(gdkwin), XFixesCursorNotify);

	copy_cursor = TRUE;
}

void
disable_copy_cursor()
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

GdkFilterReturn
on_x11_event (GdkXEvent *xevent, GdkEvent *event, gpointer data)
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
			refresh_cursor_location(FALSE);
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
			if (!xd_ev->more && !gdk_rectangle_intersect(&src_rect, &dst_rect, NULL)) {
				// check if damage intersects with the screen
				GdkRectangle damage_rect;
				damage_rect.x = xd_ev->area.x;
				damage_rect.y = xd_ev->area.y;
				damage_rect.width  = xd_ev->area.width;
				damage_rect.height = xd_ev->area.height;

				if (gdk_rectangle_intersect(&damage_rect, &src_rect, NULL))
				{
					try_refresh_image(xd_ev->timestamp);
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
			disable();
		}
	}
#endif
	return GDK_FILTER_CONTINUE;
}

#ifdef HAVE_XI
void
set_xi_eventmask(gboolean active)
{
	XIEventMask evmasks[1];
	unsigned char mask1[(XI_LASTEVENT + 7)/8];
	memset(mask1, 0, sizeof(mask1));

	if (active) {
		// select for button and key events from all master devices
		XISetMask(mask1, XI_RawMotion);
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
enable_cursor_tracking()
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

	set_xi_eventmask(TRUE);

	track_cursor = TRUE;
}

void
disable_cursor_tracking()
{
	if(!track_cursor)
		return;

	set_xi_eventmask(FALSE);

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
enable_xdamage()
{
	if (use_xdamage) {
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
disable_xdamage()
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
on_window_configure_event(GtkWidget *widget, GdkEvent *event, gpointer   user_data)
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
		fix_offset();
	}

#ifdef USE_XDAMAGE
	if (use_xdamage)
	{
		if (gdk_rectangle_intersect(&rect, &src_rect, NULL)) {
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
	}
#endif
	return TRUE;
}

#ifdef HAVE_XRANDR
void
init_xrandr()
{
	int error;
	if (!XRRQueryExtension(display, &xrandr_event_base, &error)) {
		return;
	}

	XRRSelectInput(display, root_window, RRScreenChangeNotifyMask);
}
#endif

gboolean
init()
{
#ifdef HAVE_LIBNOTIFY
	notify_init(APPNAME);
#endif

	GdkDisplay* gdisplay = gdk_display_get_default();
	if (!gdisplay) {
		error("No display available");
		return FALSE;
	}
	gscreen = gdk_display_get_default_screen (gdisplay);

	// load the icons
	{
		GError* err = NULL;
		icon_enabled = gdk_pixbuf_new_from_file (PREFIX "/share/squint/squint.png", &err);
		if (!icon_enabled)
		{
			error(err->message);
			g_clear_error (&err);
		}
		icon_disabled = gdk_pixbuf_new_from_file (PREFIX "/share/squint/squint-disabled.png", &err);
		if (!icon_disabled)
		{
			error(err->message);
			g_clear_error (&err);
		}
	}

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
			error ("XCreateGC() failed");
			return FALSE;
		}

		values.line_width = 3;
		values.foreground = 0xe0e0e0;
		gc_white = XCreateGC (display, root_window, GCLineWidth | GCForeground, &values);
		if(!gc_white) {
			error ("XCreateGC() failed");
			return FALSE;
		}
	}

	// create the status icon in the tray
	status_icon = gtk_status_icon_new();
	refresh_status_icon();
	menu = gtk_menu_new();

	// register the events
	// - status_icon clicked
	g_signal_connect (status_icon, "activate", G_CALLBACK(on_status_icon_activated), NULL);

	// - status_icon context menu
	g_signal_connect (status_icon, "popup-menu", G_CALLBACK(on_status_icon_popup_menu), NULL);


#ifdef HAVE_XRANDR
	init_xrandr();
#endif
	return TRUE;
}

// find a monitor by name
// (return -1 if not found)
gint
find_monitor(GdkScreen* scr, const char* name)
{
	int n = gdk_screen_get_n_monitors (scr);
	int i;
	for (i=0 ; i<n ; i++)
	{
		char* n = gdk_screen_get_monitor_plug_name(scr, i);
		gboolean found = !strcmp(name, n);
		g_free(n);

		if (found) {
			return i;
		}
	}
	return -1;
}

gboolean
select_monitor_by_name(GdkScreen* scr, const char* name,
		int* id, GdkRectangle* rect)
{
	*id = find_monitor(scr, name);
	if (*id<0) {
		char buff[128];
		g_snprintf(buff, 128, "Monitor %s is not active", name);
		error(buff);
		return FALSE;
	}
	gdk_screen_get_monitor_geometry (scr, *id, rect);

	return TRUE;
}

gboolean
select_any_monitor_but(GdkScreen* scr, int* id, GdkRectangle* rect, const GdkRectangle* other_rect)
{
	int n = gdk_screen_get_n_monitors (scr);
	int i;
	for (i=0 ; i<n ; i++)
	{
		gdk_screen_get_monitor_geometry (scr, i, rect);
		if (!other_rect || memcmp(rect, other_rect, sizeof(GdkRectangle)))
		{
			*id = i;
			return TRUE;
		}
	}
	return FALSE;
}

//
// select which monitor is going to be duplicated
//
// initialises:
// 	src_rect
// 	src_monitor
// 	dst_rect
// 	dst_monitor
gboolean
select_monitors()
{
	int n, i;
	src_monitor = dst_monitor = -1;

	n = gdk_screen_get_n_monitors (gscreen);
	if ((n < 2) && !config.src_monitor_name) {
		error ("There is only one monitor. What am I supposed to do?");
		return FALSE;
	}

	// first we try to allocate the requested monitors
	if (config.src_monitor_name
		&& !select_monitor_by_name(gscreen, config.src_monitor_name, &src_monitor, &src_rect)) {
			return FALSE;
	}
	if (config.dst_monitor_name
		&& !select_monitor_by_name(gscreen, config.dst_monitor_name, &dst_monitor, &dst_rect)) {
			return FALSE;
	}
	if ((src_monitor>=0) && (dst_monitor>=0) && !memcmp(&src_rect, &dst_rect, sizeof(GdkRectangle)))
	{
		error("Source and destination both refer to the same monitor");
		return FALSE;
	}

	// if the destination monitor is not yet decided, try to allocate one
	// according to the panel name
	if (dst_monitor<0) {
		int id = gdk_screen_get_primary_monitor(gscreen);

		gdk_screen_get_monitor_geometry (gscreen, id, &dst_rect);

		// ensure it is not the same as the dst monitor
		if ((src_monitor < 0) ||
			memcmp(&src_rect, &dst_rect, sizeof(GdkRectangle)))
		{
			// use it !
			dst_monitor = id;
		}
	}

	// if still unsuccessful, then just select any monitor
	if (src_monitor<0) {
		select_any_monitor_but(gscreen, &src_monitor, &src_rect,
			((dst_monitor>=0)?&dst_rect:NULL));
	}
	if (dst_monitor<0) {
		select_any_monitor_but(gscreen, &dst_monitor, &dst_rect,
			((src_monitor>=0)?&src_rect:NULL));
	}

	if ((src_monitor<0) || (dst_monitor<0)) {
		error("Could not find any monitor to be cloned");
		return FALSE;
	}
	return TRUE;
}

//
// Prepare the window to host the duplicated screen (create the pixmap, subwindow)
//
// initialises:
// 	offset
// 	pixmap
// 	gtkwin
// 	gdkwin
// 	window
//	fullscreen
//	cursor
//
void
enable_window()
{
	cursor.x = -1;
	cursor.y = -1;
	offset.x = 0;
	offset.y = 0;
	fullscreen = !config.opt_window;

	if (fullscreen)
	{
		// create the window
		gtkwin = gtk_window_new (GTK_WINDOW_POPUP);

		// resize it to the dimensions of the dst monitor
		gtk_window_resize(GTK_WINDOW(gtkwin),
				dst_rect.width, dst_rect.height);

		// move the window into the cover the destination screen
		gtk_window_move(GTK_WINDOW(gtkwin), dst_rect.x, dst_rect.y);

		// create the gdkwindow
		gtk_widget_realize(gtkwin);
		gdkwin = gtk_widget_get_window(gtkwin);
	} else {
		// create the window
		gtkwin = gtk_window_new (GTK_WINDOW_TOPLEVEL);
		gtk_window_set_resizable(GTK_WINDOW(gtkwin), TRUE);
		if (icon_enabled) {
			gtk_window_set_icon (GTK_WINDOW(gtkwin), icon_enabled);
		}

		// map my window
		gtk_widget_show (gtkwin);
		gdkwin = gtk_widget_get_window(gtkwin);

		// hide it
		gdk_window_lower(gdkwin);

		// resize the window
		int w = src_rect.width;
		int max_w = dst_rect.width - 100;
		int h = src_rect.height;
		int max_h = dst_rect.height - 100;
		gtk_window_resize(GTK_WINDOW(gtkwin),
			((w < max_w) ? w : max_w),
			((h < max_h) ? h : max_h));

		// move the window into the destination screen
		gtk_window_move(GTK_WINDOW(gtkwin), dst_rect.x+50, dst_rect.y+50);

		// register the events
		// - window moved/resized
		g_signal_connect (gtkwin, "configure-event", G_CALLBACK (on_window_configure_event), NULL);

		// - disable on window closed
		g_signal_connect (gtkwin, "delete-event", G_CALLBACK (on_window_delete_event), NULL);

		// - resize the window to src monitor size on double click
		g_signal_connect (gtkwin, "button-press-event", G_CALLBACK (on_window_button_press_event), NULL);
		gdk_window_set_events (gdkwin, gdk_window_get_events(gdkwin) | GDK_BUTTON_PRESS_MASK);
	}

	// black background
	GdkRGBA black = {0,0,0,1};
	gtk_widget_override_background_color(gtkwin, 0, &black);

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

	// possibly show the window
	// (force refreshing the cursor position)
	do_hide();
	refresh_cursor_location(TRUE);
}

void
disable_window()
{
	XDestroyWindow(display, window);
	window = 0;

	XFreePixmap(display, pixmap);
	pixmap = 0;
	
	gtk_widget_destroy(gtkwin);
	gdkwin = NULL;
	gtkwin = NULL;
}

gboolean
enable()
{
	if (enabled) {
		return TRUE;
	}

	if(!select_monitors()) {
		return FALSE;
	}

	enable_window();

	
#ifdef HAVE_XI
	enable_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	enable_copy_cursor();
#endif

#ifdef USE_XDAMAGE
	enable_xdamage();
#endif

	XFlush (display);

	// catch all X11 events
	gdk_window_add_filter(NULL, on_x11_event, NULL);

#ifdef USE_XDAMAGE
	if (!use_xdamage)
#endif
	{
		refresh_timer = g_timeout_add (40, &refresh_image, NULL);
	}

	// Redraw the window
	XClearWindow(display, gdk_x11_window_get_xid(gdkwin));

	enabled = TRUE;
	refresh_status_icon();
	return TRUE;
}

void
disable()
{
	if (refresh_timeout) {
		g_source_remove(refresh_timeout);
		refresh_timeout = 0;
	}
	if (refresh_timer) {
		g_source_remove(refresh_timer);
		refresh_timer = 0;
	}

	gdk_window_remove_filter(NULL, on_x11_event, NULL);

#ifdef HAVE_XI
	disable_cursor_tracking();
#endif

#ifdef COPY_CURSOR
	disable_copy_cursor();
#endif

#ifdef USE_XDAMAGE
	disable_xdamage();
#endif

	disable_window();

	enabled = FALSE;
	refresh_status_icon();
	return;
}


GOptionEntry option_entries[] = {
  { "disable",	'd',	0,	G_OPTION_ARG_NONE,	&config.opt_disable,	"Do not enable screen duplication at startup", NULL},
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

	if (!gtk_init_with_args (&argc, &argv, "[SourceMonitor [DestinationMonitor]]", option_entries, NULL, &err))
	{
		error(err->message);
		return 1;
	}

	g_option_context_free(context);

	if (config.opt_version) {
		puts(APPNAME " " VERSION);
		return 0;
	}

	switch (argc)
	{
		case 3:
			if (strcmp("-", argv[2])) {
				config.dst_monitor_name = g_strdup(argv[2]);
			}
		case 2:
			if (strcmp("-", argv[1])) {
				config.src_monitor_name = g_strdup(argv[1]);
			}
		case 1:
			break;
		default:
			error("invalid arguments");
			return 1;
	}

	// initialisation
	if (!init()) {
		return 1;
	}

	// activation
	if (!config.opt_disable) {
		enable();
	}

	gtk_main ();

	return 0;
}
