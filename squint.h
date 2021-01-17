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
//GdkMonitor* src_monitor = NULL;
//GdkMonitor* dst_monitor = NULL;
//
//
//GApplication* gtkapp = NULL;
GtkWidget* gtkwin;
GdkWindow* gdkwin;
GdkDisplay* gdisplay;
//Display* display = NULL;
//int raised = 0;
//GIcon* gicon = NULL;

GdkRectangle src_rect, dst_rect, active_window_rect;

void show();
void hide();
void disable();


void error(const char* msg);

gboolean x11_init();
void x11_enable();
void x11_disable();

void x11_enable_window();
void x11_disable_window();
