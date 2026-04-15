#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gtk/gtk.h>

#include "callbacks.h"
#include "gtkmeter.h"
#include "gtkmeterscale.h"
#include "interface.h"
#include "main.h"
#include "support.h"
#include "threads.h"
#ifdef HAVE_LIBLO
#include <lo/lo.h>
#endif

//| @region gtk-capture
//| Click triggers a capture of the full lookback window.
//| Window goes busy while the writer thread dumps, then meter_tick
//| resets it to idle via need_ui_sync.
void on_togglebutton1_clicked(GtkButton* button, gpointer user_data) {
  GtkWidget* img = lookup_widget(main_window, "toggle_image");

  if (!GTK_WIDGET_IS_SENSITIVE(img)) {
    return;
  }

  capture_start(buf_length);

  gtk_widget_set_sensitive(img, FALSE);
  gtk_image_set_from_pixbuf(GTK_IMAGE(img), img_busy);
  gtk_window_set_icon(GTK_WINDOW(main_window), icon_on);
}
//| @end

gboolean on_window_delete_event(GtkWidget* widget, GdkEvent* event,
                                gpointer user_data) {
  gtk_main_quit();

  return FALSE;
}

#ifdef HAVE_LIBLO
//| @region osc-capture-gtk
//| /capture handler for GTK mode — updates the UI in addition to
//| triggering the capture.
int osc_capture_handler(const char* path, const char* types, lo_arg** argv,
                        int argc, lo_message msg, void* user_data) {
  unsigned int seconds = 0;
  if (argc > 0 && types[0] == 'i') {
    seconds = argv[0]->i;
  }

  GtkWidget* img = lookup_widget(main_window, "toggle_image");
  capture_start(seconds);
  gtk_image_set_from_pixbuf(GTK_IMAGE(img), img_busy);
  gtk_window_set_icon(GTK_WINDOW(main_window), icon_on);

  return 0;
}
//| @end

//| @region osc-capture-nox
//| /capture handler for console mode — no GTK calls.
int osc_capture_handler_nox(const char* path, const char* types, lo_arg** argv,
                            int argc, lo_message msg, void* user_data) {
  unsigned int seconds = 0;
  if (argc > 0 && types[0] == 'i') {
    seconds = argv[0]->i;
  }
  capture_start(seconds);

  return 0;
}
//| @end
#endif
