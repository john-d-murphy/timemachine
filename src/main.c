/*
 *  Copyright (C) 2004 Steve Harris
 *  Copyright (C) 2006 Garett Shulman
 *  Copyright (C) 2009 Adam Sampson
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <getopt.h>
#include <gtk/gtk.h>
#include <jack/jack.h>
#include <math.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_LASH
#include <lash/lash.h>

lash_client_t* lash_client;
#endif

#ifdef HAVE_LIBREADLINE
#include <readline/history.h>
#include <readline/readline.h>
#endif

#ifdef HAVE_LIBLO
#include <lo/lo.h>
#endif

#include "interface.h"
#include "main.h"
#include "meters.h"
#include "support.h"
#include "threads.h"

#define DEBUG(lvl, txt...) \
  if (verbosity >= lvl) fprintf(stderr, PACKAGE ": " txt)

const static int verbosity = 0;

gboolean idle_cb(gpointer data);
void cleanup();

GtkWidget* main_window;

int num_ports = DEFAULT_NUM_PORTS;
unsigned int buf_length = DEFAULT_BUF_LENGTH;

char* client_name = DEFAULT_CLIENT_NAME;
char* prefix = DEFAULT_PREFIX;
char* format_name = DEFAULT_FORMAT;
int format_sf = 0;
int safe_filename = 0;

jack_port_t* ports[MAX_PORTS];
jack_client_t* client;

GdkPixbuf *img_on, *img_off, *img_busy;
GdkPixbuf *icon_on, *icon_off;

#ifdef HAVE_LIBLO
//| @region osc-handlers
//| /capture (no args) — capture last buf_length seconds
//| /capture i (int)   — capture last N seconds
int osc_capture_handler(const char* path, const char* types, lo_arg** argv,
                        int argc, lo_message msg, void* user_data);
int osc_capture_handler_nox(const char* path, const char* types, lo_arg** argv,
                            int argc, lo_message msg, void* user_data);
char* osc_port = DEFAULT_OSC_PORT;
//| @end
#endif

int main(int argc, char* argv[]) {
  unsigned int i;
  int opt;
  int help = 0;
#ifdef HAVE_LIBREADLINE
  int console = 0;
#endif
  char port_name[32];
  pthread_t dt;
#ifdef HAVE_LASH
  lash_args_t* lash_args = lash_extract_args(&argc, &argv);
  lash_event_t* event;
#endif

  while ((opt = getopt(argc, argv, "hic:t:n:p:f:so:")) != -1) {
    switch (opt) {
      case 'h':
        help = 1;
        break;
      case 'i':
#ifdef HAVE_LIBREADLINE
        console = 1;
#endif
        break;
      case 'c':
        num_ports = atoi(optarg);
        DEBUG(1, "ports: %d\n", num_ports);
        break;
      case 't':
        buf_length = atoi(optarg);
        DEBUG(1, "buffer: %ds\n", buf_length);
        break;
      case 'n':
        client_name = optarg;
        DEBUG(1, "client name: %s\n", client_name);
        break;
      case 'p':
        prefix = optarg;
        DEBUG(1, "prefix: %s\n", prefix);
        break;
      case 'f':
        format_name = optarg;
        break;
      case 's':
        safe_filename = 1;
        break;
      case 'o':
#ifdef HAVE_LIBLO
        osc_port = optarg;
#endif
        break;
      default:
        num_ports = 0;
        break;
    }
  }

  if (optind != argc) {
    num_ports = argc - optind;
  }

  if (num_ports < 1 || num_ports > MAX_PORTS || help) {
    fprintf(stderr,
            "Usage %s: [-h] [-i] [-c channels] [-n jack-name]\n\t"
            "[-t buffer-length] [-p file prefix] [-f format]\n\t"
            "[-s] [-o osc-port] [port-name ...]\n\n",
            argv[0]);
    fprintf(stderr, "\t-h\tshow this help\n");
    fprintf(stderr, "\t-i\tinteractive mode (console instead of X11)\n");
    fprintf(stderr, "\t-c\tnumber of recording channels (1-%d, default %d)\n",
            MAX_PORTS, DEFAULT_NUM_PORTS);
    fprintf(stderr, "\t-n\tJACK client name (default \"%s\")\n",
            DEFAULT_CLIENT_NAME);
    fprintf(stderr, "\t-t\tlookback buffer length in seconds (default %d)\n",
            DEFAULT_BUF_LENGTH);
    fprintf(stderr,
            "\t-p\tsaved file prefix, may include path (default \"%s\")\n",
            DEFAULT_PREFIX);
    fprintf(stderr, "\t-s\tuse safer characters in filename\n");
    fprintf(stderr, "\t-f\tfile format: wav, w64, flac (default '%s')\n",
            DEFAULT_FORMAT);
#ifdef HAVE_LIBLO
    fprintf(stderr, "\t-o\tOSC control port (default %s)\n", DEFAULT_OSC_PORT);
#endif
    fprintf(stderr, "\n");
    exit(1);
  }

  /* print startup config */
  printf(
      "timemachine: %d channels, %d second lookback, format %s, prefix '%s'\n",
      num_ports, buf_length, format_name, prefix);

  if (!strcasecmp(format_name, "wav")) {
    format_sf = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  }
#ifdef HAVE_W64
  if (!strcasecmp(format_name, "w64")) {
    format_sf = SF_FORMAT_W64 | SF_FORMAT_FLOAT;
  }
#endif
#ifdef HAVE_FLAC
  if (!strcasecmp(format_name, "flac")) {
    format_sf = SF_FORMAT_FLAC | SF_FORMAT_PCM_24;
  }
#endif

  if (format_sf == 0) {
    fprintf(stderr, "Unknown format '%s'\n", format_name);
    exit(1);
  }

  /* Register with JACK */
  if ((client = jack_client_open(client_name, 0, NULL)) == 0) {
    DEBUG(0, "jack server not running?\n");
    exit(1);
  }
  DEBUG(1, "registering as %s\n", client_name);

  process_init(buf_length);

#ifdef HAVE_LASH
  lash_client = lash_init(lash_args, "TimeMachine", 0, LASH_PROTOCOL(2, 0));
  if (!lash_client) {
    DEBUG(1, "could not initialise LASH\n");
  }
  event = lash_event_new_with_type(LASH_Client_Name);
  lash_event_set_string(event, client_name);
  lash_send_event(lash_client, event);
#endif

  jack_set_process_callback(client, process, 0);

  if (jack_activate(client)) {
    DEBUG(0, "cannot activate JACK client");
    exit(1);
  }
#ifdef HAVE_LASH
  lash_jack_client_name(lash_client, client_name);
#endif

  /* Create the JACK ports */
  for (i = 0; i < num_ports; i++) {
    jack_port_t* port;

    snprintf(port_name, 31, "in_%d", i + 1);
    ports[i] = jack_port_register(client, port_name, JACK_DEFAULT_AUDIO_TYPE,
                                  JackPortIsInput, 0);
    if (optind != argc) {
      port = jack_port_by_name(client, argv[optind + i]);
      if (port == NULL) {
        fprintf(stderr, "Can't find port '%s'\n", argv[optind + i]);
        continue;
      }
      if (jack_connect(client, argv[optind + i], jack_port_name(ports[i]))) {
        fprintf(stderr, "Cannot connect port '%s' to '%s'\n", argv[optind + i],
                jack_port_name(ports[i]));
      }
    }
  }

  /* Start the disk writer thread */
  pthread_create(&dt, NULL, (void*)&writer_thread, NULL);

#ifdef HAVE_LIBREADLINE
  if (console || !getenv("DISPLAY") || getenv("DISPLAY")[0] == '\0') {
#ifdef HAVE_LIBLO
    lo_server_thread st = lo_server_thread_new(osc_port, NULL);
    if (st) {
      lo_server_thread_add_method(st, "/capture", "", osc_capture_handler_nox,
                                  NULL);
      lo_server_thread_add_method(st, "/capture", "i", osc_capture_handler_nox,
                                  NULL);
      lo_server_thread_start(st);
      printf(
          "OSC: listening on osc.udp://localhost:%s (/capture, /capture i)\n",
          osc_port);
    }
#endif

    printf("buffer rolling. type 'capture [N]' or 'help'\n");
    int done = 0;
    while (!done) {
      char* line = readline("TimeMachine> ");
      if (!line) {
        printf("EOF\n");
        break;
      }
      if (line && *line) {
        add_history(line);
        if (strncmp(line, "q", 1) == 0) {
          done = 1;
        } else if (strncmp(line, "capture", 7) == 0) {
          unsigned int secs = 0;
          if (strlen(line) > 8) {
            secs = atoi(line + 8);
          }
          capture_start(secs);
        } else if (strncmp(line, "help", 4) == 0) {
          printf("Commands:\n");
          printf(
              "  capture [N]   capture last N seconds (default: full "
              "buffer)\n");
          printf("  quit          exit\n");
        } else {
          printf("Unknown command. Type 'help' for commands.\n");
        }
      }
      free(line);
    }
  } else
#endif
  {
    gtk_init(&argc, &argv);

    add_pixmap_directory(PACKAGE_DATA_DIR "/" PACKAGE "/pixmaps");
    add_pixmap_directory("pixmaps");
    add_pixmap_directory("../pixmaps");

    img_on = create_pixbuf("on.png");
    img_off = create_pixbuf("off.png");
    img_busy = create_pixbuf("busy.png");
    icon_on = create_pixbuf("on-icon.png");
    icon_off = create_pixbuf("off-icon.png");

    main_window = create_window(client_name);
    gtk_window_set_icon(GTK_WINDOW(main_window), icon_off);
    gtk_widget_show(main_window);

    bind_meters();
    g_timeout_add(100, meter_tick, NULL);

#ifdef HAVE_LIBLO
    lo_server_thread st = lo_server_thread_new(osc_port, NULL);
    if (st) {
      lo_server_thread_add_method(st, "/capture", "", osc_capture_handler,
                                  NULL);
      lo_server_thread_add_method(st, "/capture", "i", osc_capture_handler,
                                  NULL);
      lo_server_thread_start(st);
      printf(
          "OSC: listening on osc.udp://localhost:%s (/capture, /capture i)\n",
          osc_port);
    }
#endif

#ifdef HAVE_LASH
    gtk_idle_add(idle_cb, lash_client);
#endif

    gtk_main();
  }

  cleanup();

  return 0;
}

void cleanup() {
  /* Leave the jack graph */
  jack_client_close(client);

  capture_quit();

  while (!capture_done) {
    usleep(1000);
  }

  DEBUG(0, "exiting\n");
  fflush(stderr);

  exit(0);
}

#ifdef HAVE_LASH
gboolean idle_cb(gpointer data) {
  lash_client_t* lash_client = (lash_client_t*)data;
  lash_event_t* event;
  lash_config_t* config;

  while ((event = lash_get_event(lash_client))) {
    if (lash_event_get_type(event) == LASH_Save_Data_Set) {
      /* no state to save */
    } else if (lash_event_get_type(event) == LASH_Quit) {
      cleanup();
    } else {
      DEBUG(0, "unhandled LASH event: type %d, '%s''\n",
            lash_event_get_type(event), lash_event_get_string(event));
    }
  }

  while ((config = lash_get_config(lash_client))) {
    DEBUG(0, "got unexpected LASH config: %s\n", lash_config_get_key(config));
  }

  usleep(10000);

  return TRUE;
}
#endif

/* vi:set ts=8 sts=4 sw=4: */
