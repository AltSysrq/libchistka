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
#include <dirent.h>
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

/* directories_traversed lists directories whose contents have been
 * iterated. directories_read lists directories whose immediate child files
 * have been read.
 */
static hashset directories_traversed, directories_read;

static void add_events(char*);
static void add_one_event(void (*)(char*), char*);
static void run_events(void);
static void read_input(void);
static void profile_open(void);
static void profile_log(char*);
static void event_print(char*);
static void event_read_siblings(char*);
static void event_iterate_directory(char*);
static void event_play_profile(char*);
static int parent_dir(char*);

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

static void signal_ignore(int parm) {}

int main(void) {
  struct sigaction sig = {};

  directories_traversed = hs_create();
  hs_defunct_at(directories_traversed, 4096);
  directories_read = hs_create();
  hs_defunct_at(directories_read, 4096);

  /* If a profile is set, read it if possible, then open for writing. */
  profile_open();

  /* Don't die on SIGIO or SIGALRM */
  sig.sa_handler = signal_ignore;
  sigaction(SIGIO, &sig, NULL);
  sigaction(SIGALRM, &sig, NULL);

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
  add_one_event(event_print, filename);
  if (parent_dir(filename)) {
    if (!hs_test(directories_read, filename)) {
      add_one_event(event_read_siblings, filename);
      hs_put(directories_read, filename);
    }

    do {
      if (!hs_test(directories_traversed, filename)) {
        add_one_event(event_iterate_directory, filename);
        hs_put(directories_traversed, filename);
      }
    } while (parent_dir(filename));
  }
}

/* Adds the given event to the event queue, scheduling it for
 * $PREREADSHIM_DELAY seconds in the future.
 */
static void add_one_event(void (*f)(char*), char* datum) {
  time_t when;
  event** link, * it;

  static unsigned offset;
  static int has_offset = 0;
  if (!has_offset) {
    has_offset = 1;
    offset = 5;
    if (getenv("PREREADSHIM_DELAY"))
      offset = atoi(getenv("PREREADSHIM_DELAY"));
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

/* Destructively modifies the given string to point it to the parent directory
 * of the input file.
 *
 * Returns 1 if the result is meaningful --- that is, it is not the empty
 * string or the root directory.
 */
static int parent_dir(char* file) {
  char* lastSlash = NULL, * curr;
  for (curr = file; *curr; ++curr)
    if (*curr == '/')
      lastSlash = curr;

  /* If there's no slash, this will result in an empty string. */
  if (!lastSlash) return 0;
  /* If the only slash is at position 0 or 1, we have reached the root. */
  if (lastSlash == file || lastSlash == file+1)
    return 0;

  /* Kill the slash and return success */
  *lastSlash = 0;
  return 1;
}

/* Executes any pending events scheduled for now and earlier. If any events
 * remain which are scheduled for the future, an alarm is set to occur when the
 * next event is ready to be processed.
 */
static void run_events(void) {
  time_t now;
  event* nxt;

  now = time(NULL);
  while (event_queue && event_queue->when <= now) {
    /* Run the event */
    (*event_queue->run)(event_queue->datum);
    /* Free it and move on to the next */
    nxt = event_queue->next;
    free(event_queue->datum);
    free(event_queue);
    event_queue = nxt;
    /* Read any new input that may be available */
    read_input();
    /* The time may have changed while executing */
    now = time(NULL);
  }

  /* If there is an event in the future, schedule an alarm */
  if (event_queue)
    alarm(event_queue->when - now > 0? event_queue->when - now : 1);
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

/* Dummy event for testing */
static void event_print(char* filename) {
  fprintf(stderr, "daemon: %s\n", filename);
}

static void event_iterate_directory(char* directory) {
  DIR* dir = opendir(directory);
  fprintf(stderr, "daemon: traverse %s: %d\n",
          directory, !!dir);
  if (!dir) return;

  while (readdir(dir));
  closedir(dir);
}

static void event_read_siblings(char* directory) {
  static unsigned read_limit;
  static int has_read_limit = 0;
  char discard[4096], subfile[4096];
  unsigned data_read, amt;
  DIR* dir;
  FILE* file;
  struct dirent* ent;
  struct stat st;
  int is_regular;
  fprintf(stderr, "daemon: siblings %s\n", directory);

  if (!has_read_limit) {
    has_read_limit = 1;
    read_limit = 16;
    if (getenv("PREREADSHIM_SIBLINGS"))
      read_limit = atoi(getenv("PREREADSHIM_SIBLINGS"));

    read_limit *= 1024*1024;
  }

  dir = opendir(directory);
  if (!dir) return;

  data_read = 0;
  /* Iterate through the directory and read any regular file encountered */
  while (data_read < read_limit && (ent = readdir(dir))) {
    if (sizeof(subfile) > strlen(directory) + 1 /* slash */ +
        strlen(ent->d_name)) {
      strcpy(subfile, directory);
      strcat(subfile, "/");
      strcat(subfile, ent->d_name);
    } else {
      /* Path name too long */
      continue;
    }

    /* If possible, get the type of the file from the dirent. If we get
     * unknown, or the system does not have DT_REG and DT_UNKNOWN, resort to
     * stat()ing the file to find out what it is.
     */
#if defined(DT_REG) && defined(DT_UNKNOWN)
    if (ent->d_type == DT_REG) {
      is_regular = 1;
    } else if (ent->d_type == DT_UNKNOWN) {
#endif
      /* Either the filesystem can't tell us what the file is, or the system
       * doesn't support returning file types within the dirent.
       *
       * Stat the file to find out what it is.
       */
      is_regular = !stat(subfile, &st) && S_ISREG(st.st_mode);
#if defined(DT_REG) && defined(DT_UNKNOWN)
    } else {
      /* Known and not REG */
      is_regular = 0;
    }
#endif

    if (is_regular && (file = fopen(subfile, "r"))) {
      fprintf(stderr, "daemon: sibling read: %s\n", subfile);
      do {
        amt = fread(discard, 1, sizeof(discard), file);
        data_read += amt;
      } while (amt == sizeof(discard));

      fclose(file);
    }
  }

  closedir(dir);
}
