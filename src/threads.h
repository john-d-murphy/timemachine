#ifndef PROCESS_H
#define PROCESS_H

#include <gtk/gtk.h>
#include <jack/jack.h>

//| @region ring-constants
//| BUF_SIZE is the chunk size for disk writes, not the ring itself.
//| Ring size is computed at runtime from buf_length * sample_rate.
#define BUF_SIZE 4096
//| @end

int process(jack_nframes_t nframes, void* arg);

void process_init(unsigned int time);

int writer_thread(void* d);

//| @region capture-api
//| capture_start replaces recording_start/recording_stop.
//| The buffer is always rolling — there is no "recording" state.
//| capture_start snapshots the ring and writes the last N seconds to disk.
void capture_start(unsigned int seconds);
void capture_quit(void);
//| @end

gboolean meter_tick(gpointer data);

extern volatile int capture_done;
extern volatile int need_ui_sync;

#endif
