/*
 *  Copyright (C) 2004 Steve Harris
 *  Copyright (C) 2026 John D. Murphy
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include "threads.h"

#include <gtk/gtk.h>
#include <jack/jack.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "main.h"
#include "meters.h"
#include "support.h"

//| @region ring-buffer
//| Single circular buffer per port, always written by the RT thread.
//| ring_head is monotonic — never wraps. Readers compute ring_head % ring_size.
//| This replaces the original two-buffer model (pre_buffer + disk_buffer)
//| where pre_buffer was written when idle and disk_buffer when recording.
//| The old model cleared pre_buffer on capture (memset to 0) and required
//| a recording state machine. This model has no state — the buffer is
//| always rolling and capture is a non-destructive snapshot.
static float* ring[MAX_PORTS];
static unsigned int ring_size;          /* in frames, = buf_length * sr */
static volatile unsigned int ring_head; /* monotonic, written by RT only */
//| @end

//| @region capture-state
//| Capture is request-based: the control thread sets capture_seconds and
//| then capture_pending. The writer thread wakes, snapshots ring_head,
//| and dumps the last capture_seconds of audio. The ring keeps rolling
//| during the write — nothing is cleared, nothing blocks.
static volatile int capture_pending = 0;
static unsigned int capture_seconds = 0;
static volatile int quiting = 0;
volatile int capture_done = 0;
volatile int need_ui_sync = 0;
//| @end

/* Peak data for meters */
static volatile float peak[MAX_PORTS];

//| @region process-callback
//| JACK RT callback. Unconditionally writes all input into the ring.
//| No branching on recording state — the ring is always hot.
int process(jack_nframes_t nframes, void* arg) {
  unsigned int i, port;
  unsigned int pos = ring_head;

  for (port = 0; port < num_ports; port++) {
    jack_default_audio_sample_t* in;

    /* port may not be registered yet during startup */
    if (ports[port] == NULL) {
      break;
    }

    in = (jack_default_audio_sample_t*)jack_port_get_buffer(ports[port],
                                                            nframes);

    if (!in) {
      fprintf(stderr, "timemachine: bad buffer on port %u\n", port);
      break;
    }

    /* write into ring at current head position */
    for (i = 0; i < nframes; i++) {
      ring[port][(pos + i) % ring_size] = in[i];
    }

    /* update peak for meters */
    for (i = 0; i < nframes; i++) {
      if (fabsf(in[i]) > peak[port]) {
        peak[port] = fabsf(in[i]);
      }
    }
  }

  /* advance head — monotonic, never wraps */
  ring_head = pos + nframes;

  return 0;
}
//| @end

//| @region writer-thread
//| Disk writer thread. Sleeps until capture_pending is set, then
//| snapshots the ring head, computes the lookback window, and dumps
//| the interleaved audio to a WAV file. The ring keeps rolling during
//| the write. The only constraint is capture_seconds <= buf_length
//| so the RT thread can't overwrite data we're still reading.
int writer_thread(void* d) {
  unsigned int i, j, port;
  char* filename;
  SNDFILE* out;
  SF_INFO info;
  float buf[BUF_SIZE * MAX_PORTS];
  time_t t;
  struct tm* parts;

again:
  /* sleep until a capture is requested or we're quiting */
  while (!capture_pending && !quiting) {
    usleep(1000);
  }

  if (quiting) {
    capture_done = 1;
    return 0;
  }

  /* snapshot the current ring position */
  unsigned int head = ring_head;
  unsigned int sr = jack_get_sample_rate(client);

  /* clamp capture to available data */
  unsigned int frames = capture_seconds * sr;
  if (frames > ring_size) {
    frames = ring_size;
  }
  /* don't capture more than we've actually recorded */
  if (frames > head) {
    frames = head;
  }

  unsigned int start = head - frames;

  /* ISO 8601 timestamp backdated to the start of the capture window */
  time(&t);
  t -= (time_t)capture_seconds;
  parts = localtime(&t);

  if (safe_filename) {
    filename = g_strdup_printf("%s%04d-%02d-%02dT%02d-%02d-%02d.%s", prefix,
                               parts->tm_year + 1900, parts->tm_mon + 1,
                               parts->tm_mday, parts->tm_hour, parts->tm_min,
                               parts->tm_sec, format_name);
  } else {
    filename = g_strdup_printf("%s%04d-%02d-%02dT%02d:%02d:%02d.%s", prefix,
                               parts->tm_year + 1900, parts->tm_mon + 1,
                               parts->tm_mday, parts->tm_hour, parts->tm_min,
                               parts->tm_sec, format_name);
  }

  /* open output file */
  info.samplerate = sr;
  info.channels = num_ports;
  info.format = format_sf;

  if (!sf_format_check(&info)) {
    fprintf(stderr, "timemachine: output file format error\n");
  }

  out = sf_open(filename, SFM_WRITE, &info);
  if (!out) {
    fprintf(stderr, "timemachine: cannot open '%s' for writing: %s\n", filename,
            sf_strerror(NULL));
    free(filename);
    capture_pending = 0;
    need_ui_sync = 1;
    goto again;
  }

  printf("capturing %u seconds -> '%s'\n", capture_seconds, filename);

  /* dump the lookback window in chunks */
  unsigned int written = 0;
  while (written < frames) {
    unsigned int chunk = BUF_SIZE;
    if (written + chunk > frames) {
      chunk = frames - written;
    }

    /* interleave ports into the write buffer */
    for (i = 0; i < chunk; i++) {
      for (port = 0; port < num_ports; port++) {
        buf[i * num_ports + port] =
            ring[port][(start + written + i) % ring_size];
      }
    }

    j = sf_writef_float(out, buf, chunk);
    if (j != chunk) {
      fprintf(stderr, "timemachine: short write (%u of %u frames)\n", j, chunk);
      break;
    }
    written += chunk;
  }

  sf_close(out);
  printf("captured %u frames (%u seconds) -> '%s'\n", written, capture_seconds,
         filename);
  free(filename);

  /* signal completion */
  capture_pending = 0;
  need_ui_sync = 1;

  if (!quiting) goto again;

  capture_done = 1;
  return 0;
}
//| @end

//| @region init
//| Allocate the ring buffer. Size is buf_length * sample_rate frames
//| per port. Unlike the original code, there is only one buffer — no
//| separate pre_buffer and disk_buffer.
void process_init(unsigned int time) {
  unsigned int port;

  if (time < 1) {
    fprintf(stderr,
            "timemachine: buffer time must be 1 second or "
            "greater\n");
    exit(1);
  }
  if (time > MAX_TIME) {
    fprintf(stderr,
            "timemachine: buffer time must be %d seconds or "
            "less\n",
            MAX_TIME);
    exit(1);
  }

  ring_size = time * jack_get_sample_rate(client);
  ring_head = 0;

  for (port = 0; port < num_ports; port++) {
    ring[port] = calloc(ring_size, sizeof(float));
    if (!ring[port]) {
      fprintf(stderr,
              "timemachine: failed to allocate ring buffer "
              "for port %u (%u frames)\n",
              port, ring_size);
      exit(1);
    }
  }
  /* null out unused ports */
  for (; port < MAX_PORTS; port++) {
    ring[port] = NULL;
  }

  printf("ring buffer: %u seconds, %u frames/port, %.1f MB total\n", time,
         ring_size,
         (float)ring_size * sizeof(float) * num_ports / (1024 * 1024));
}
//| @end

//| @region control
//| capture_start: request a capture of the last N seconds.
//| If a capture is already in progress, this is a no-op.
//| Sets capture_seconds first, then capture_pending (store order
//| matters for the writer thread on non-x86, but on x86 sequential
//| stores are not reordered).
void capture_start(unsigned int seconds) {
  if (capture_pending) {
    fprintf(stderr, "timemachine: capture already in progress\n");
    return;
  }
  capture_seconds = seconds ? seconds : buf_length;
  capture_pending = 1;
}

void capture_quit(void) { quiting = 1; }
//| @end

//| @region meter-tick
//| GTK idle callback for peak meters and UI state sync.
//| In the new model there is no "recording" state — the UI shows
//| capture-in-progress (busy) or idle (off). The brief "on" flash
//| on capture completion is handled by the callbacks layer.
gboolean meter_tick(gpointer user_data) {
  float data[MAX_PORTS];
  unsigned int i;

  if (need_ui_sync) {
    GtkWidget* img = lookup_widget(main_window, "toggle_image");

    if (capture_pending) {
      gtk_image_set_from_pixbuf(GTK_IMAGE(img), img_busy);
      gtk_window_set_icon(GTK_WINDOW(main_window), icon_on);
    } else {
      gtk_image_set_from_pixbuf(GTK_IMAGE(img), img_off);
      gtk_window_set_icon(GTK_WINDOW(main_window), icon_off);
    }
    gtk_widget_set_sensitive(img, TRUE);

    need_ui_sync = 0;
  }

  for (i = 0; i < MAX_PORTS; i++) {
    data[i] = peak[i];
    peak[i] = peak[i] - 0.1f < 0.0f ? 0.0f : peak[i] - 0.1f;
  }
  update_meters(data);

  return TRUE;
}
//| @end

/* vi:set ts=8 sts=4 sw=4: */
