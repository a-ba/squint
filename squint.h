#include <gtk/gtk.h>
#include <gdk/gdk.h>



// Config
struct {
	const char* src_monitor_name;
	const char* dst_monitor_name;

	gboolean opt_version, opt_window, opt_disable;
	gint opt_limit, opt_rate;
} config;


// State
gboolean enabled;
gboolean fullscreen;
int raised;

GtkWidget* gtkwin;
GdkWindow* gdkwin;
GdkDisplay* gdisplay;

GdkRectangle src_rect, dst_rect, active_window_rect;

void squint_show();
void squint_hide();
void squint_disable();

void squint_error(const char* msg);

gboolean x11_init();
void x11_enable();
void x11_disable();
