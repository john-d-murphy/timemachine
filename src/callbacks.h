#include <gtk/gtk.h>

void on_togglebutton1_clicked(GtkButton* button, gpointer user_data);

gboolean on_window_delete_event(GtkWidget* widget, GdkEvent* event,
                                gpointer user_data);

#ifdef HAVE_LIBLO
#include <lo/lo.h>

int osc_capture_handler(const char* path, const char* types, lo_arg** argv,
                        int argc, lo_message msg, void* user_data);
int osc_capture_handler_nox(const char* path, const char* types, lo_arg** argv,
                            int argc, lo_message msg, void* user_data);
#endif
