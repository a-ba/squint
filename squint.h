#include <gtk/gtk.h>
#include <gdk/gdk.h>



// Config
extern struct config {
	const char* src_monitor_name;
	const char* dst_monitor_name;

	gboolean opt_version, opt_window, opt_disable, opt_passive;
	gint opt_limit, opt_rate;
} config;


// State
extern gboolean enabled;
extern gboolean fullscreen;
extern gboolean raised;

extern GtkWidget* gtkwin;
extern GdkWindow* gdkwin;
extern GdkDisplay* gdisplay;

extern GdkRectangle src_rect, dst_rect, active_window_rect;

void squint_show();
void squint_hide();
void squint_disable();

void squint_error(const char* msg);

gboolean x11_init();
void x11_enable();
void x11_disable();
