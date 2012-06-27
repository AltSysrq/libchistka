/*
Copyright (c) 2012 Jason Lingle
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of the author nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

/* The daemon reads filenames from stdin, one filename per line. Each filename
 * will schedule any necessary events associated with that file.
 *
 * It is the responsibility of the source to ensure that no filenames sent
 * contain newlines and to not send filenames longer than
 * PREREAD_MAX_FILENAME_LEN as defined in common.h.
 */

#include "hashset.h"
#include "common.h"

static void add_events(char*);
static void add_one_event(void (*)(char*), char*);
static void run_events(void);
static void read_input(void);
static void profile_open(void);
static void profile_log(char*);

/* The event queue.
 * Each event stores when it shoud run, as well as the function to call and the
 * filename to pass to it. The events are stored in a singly-linked list.
 */
typedef struct event {
  time_t when;
  void (*run)(char*);
  char* datum;
  struct event* next;
} event;
static event* event_queue;

/* If non-NULL, incomming filenames are written to this file. It is closed if
 * it exceeds 1 MB in size.
 */
static FILE* profile_output;

int main(void) {
  /* If a profile is set, read it if possible, then open for writing. */
  profile_open();

  /* Reconfigure stdin to be ASYNC and NONBLOCK */
  if (-1 == fcntl(STDIN_FILENO, F_SETFL, O_ASYNC|O_NONBLOCK))
    perror("fcntl(F_SETFL,O_ASYNC|O_NONBLOCK)");
  if (-1 == fcntl(STDIN_FILENO, F_SETOWN, getpid()))
    perror("fcntl(F_SETOWN)");

  /* Run until we exit due to an unexpected error or the input stream closes. */
  event_queue = NULL;
  while (1) {
    read_input();
    run_events();
    pause();
  }

  return 0;
}

/* Reads any pending input and adds events to the event queue.
 *
 * This should be called frequently during event processing so that the host
 * process does not block.
 */
static void read_input(void) {
  char filename[PREREAD_MAX_FILENAME_LEN+2]; /* +2 for LF and NUL */
  while (fgets(filename, sizeof(filename), stdin)) {
    if (!filename[0]) continue;
    /* Remove the trailing newline */
    filename[strlen(filename)-1] = 0;
    /* Schedule any events applying to this file. */
    add_events(filename);
    /* Log if appropriate */
    profile_log(filename);
  }

  /* Any error terminates reading, except for indications that there is
   * /currently/ no input, which we simply ignore.
   *
   * If nothing remains to read, just terminate the program because its
   * usefullness has ceased.
   */
  if (errno != EAGAIN && errno != EWOULDBLOCK)
    exit(0);
  else
    clearerr(stdin);
}

/* Adds any events that should occur given a read from the specified
 * filename.
 */
static void add_events(char* filename) {
  /* TODO */
}

/* Adds the given event to the event queue, scheduling it for
 * $PREREAD_DELAY seconds in the future.
 */
static void add_one_event(void (*f)(char*), char* datum) {
  time_t when;
  event** link, * it;

  static unsigned offset;
  static int has_offset = 0;
  if (!has_offset) {
    has_offset = 1;
    offset = 5;
    if (getenv("PREREAD_DELAY"))
      offset = atoi(getenv("PREREAD_DELAY"));
  }
  when = time(NULL) + offset;

  /* Create event */
  it = malloc(sizeof(event));
  if (!it) return; /* oh well... */
  it->when = when;
  it->run = f;
  it->datum = strdup(datum);
  it->next = NULL;

  /* Add to queue */
  for (link = &event_queue; *link; link = &(**link).next);
  *link = it;
}

/* Executes any pending events scheduled for now and earlier. If any events
 * remain which are scheduled for the future, an alarm is set to occur when the
 * next event is ready to be processed.
 */
static void run_events(void) {
  time_t now;
  event* evt, * nxt;

  evt = event_queue;
  now = time(NULL);
  while (evt && evt->when <= now) {
    /* Run the event */
    (*evt->run)(evt->datum);
    /* Free it and move on to the next */
    nxt = evt->next;
    free(evt->datum);
    free(evt);
    evt = nxt;
    /* Read any new input that may be available */
    read_input();
    /* The time may have changed while executing */
    now = time(NULL);
  }

  /* If there is an event in the future, schedule an alarm */
  if (evt)
    alarm(now - evt->when);
}

static void profile_open(void) {
  /* TODO */
}

static void profile_log(char* filename) {
  if (profile_output) {
    fprintf(profile_output, "%s\n", filename);
    /* Close on error or if it exceeds 1 MB */
    if (ftell(profile_output) < 0 || ftell(profile_output) > 1024*1024) {
      fclose(profile_output);
      profile_output = NULL;
    }
  }
}
