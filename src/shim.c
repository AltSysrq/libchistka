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
#endif

/* Allow the user to provide their own definition of RTLD_NEXT if such
 * functionality exists on their system but _GNU_SOURCE won't get it.
 */
#ifndef RTLD_NEXT
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>

#include "common.h"
#include "hashset.h"

#ifndef RTLD_NEXT
#error RTLD_NEXT undefined; please provide a definition of the constant if your\
 system supports it
#endif

/* The time the program began exectution */
static time_t execution_start_time;

/* Pipe fd to which to write commands for the background reader process.
 * Zero if there is no background process.
 */
static int command_output;
/* The pid of the daemon process. Zero if none. */
static pid_t lps_daemon;

/* Whether to enable libprereadshim or to just pass through to libc open. The
 * latter is done for the preread daemon so the library does not recurse.
 */
static int shim_enabled;

/* The open() function from the next library in the chain (usually libc). */
static int (*copen)(const char*, int, ...) = NULL;

/* Set of files exempted from the DENY option. */
static hashset deny_exempt;

/* Returns whether the given file's existence should be denied. */
static int should_deny(char* name);

/* Returns the readahead amount (overridable in PREREADSHIM_READAHEAD) in
 * megabytes.
 */
static unsigned config_readahead() {
  static int has_extracted = 0;
  static unsigned value = 64;

  if (!has_extracted && getenv("PREREADSHIM_READAHEAD"))
    value = atoi(getenv("PREREADSHIM_READAHEAD"));

  has_extracted = 1;
  return value;
}

#ifndef __const
#define __const
#endif

void __attribute__((constructor)) libprereadshim_init(void) {
  char* message;

  execution_start_time = time(NULL);
  command_output = 0;
  lps_daemon = 0;

  /* Get the next open() function */
  dlerror(); /* Clear any DL error */
  copen = dlsym(RTLD_NEXT, "open"); /* Get the symbol */
  /* Check for failure */
  if (message = dlerror()) {
    /* Try to print to stderr (though this mightn't work) */
    write(STDERR_FILENO, message, strlen(message));
    exit(-1);
  }

  /* See if further operation is disabled */
  if (getenv("PREREADSHIM_DISABLE")) {
    shim_enabled = 0;
    return;
  }

  /* OK, Enabled */
  shim_enabled = 1;
}

/* Additional startup logic that must run after everything else is
 * initialised. It is called on the first invocation of open().
 */
static void post_init(void) {
  int pipefd[2];
  char* message;
  /* Start the preread daemon */
  /* First, set the PREREADSHIM_DISABLE env variable to prevent recursion */
  putenv("PREREADSHIM_DISABLE=yes");
  /* Set the pipe up */
  if (pipe(pipefd)) {
    message = "Could not open pipe to daemon; libprereadshim disabled\n";
    write(STDERR_FILENO, message, strlen(message));
    shim_enabled = 0;
    return;
  }
  /* Fork and switch in the child. */
  lps_daemon = fork();
  if (!lps_daemon) {
    /* Child. */
    close(pipefd[1]); /* Close write end */
    if (dup2(pipefd[0], STDIN_FILENO)) {
      close(pipefd[0]);
      message = "preread daemon: Could not set input pipe up\n";
      write(STDERR_FILENO, message, strlen(message));
      exit(-1);
    }
    close(pipefd[0]);

    /* Switch to daemon process */
    execlp("prereadshimdaemon", "prereadshimdaemon", NULL);

    /* If we get here, execlp() failed. */
    message = "preread daemon: could not start\n";
    write(STDERR_FILENO, message, strlen(message));
    exit(-1);
  }

  if (lps_daemon == -1) {
    message = "Could not fork preread daemon; libprereadshim disabled\n";
    write(STDERR_FILENO, message, strlen(message));
    shim_enabled = 0;
  }

  /* Keep the write end of the pipe, discard read end */
  command_output = pipefd[1];
  close(pipefd[0]);

  /* Unset the PREREADSHIM_DISABLE env variable so it does not affect children
   * of the host process.
   */
  unsetenv("PREREADSHIM_DISABLE");

  /* Allocate structures */
  deny_exempt = hs_create();
}

int open(__const char* pathname, int flags, ...) {
  va_list args;
  mode_t mode;
  int fd;
  char discard[4096], daemon_input[PREREAD_MAX_FILENAME_LEN+2];
  unsigned buffers_read, readahead_amt;

  /* It is possible the constructor function won't be called.
   * Detect this and call it now.
   */
  if (!copen) libprereadshim_init();

  va_start(args, flags);
  mode = va_arg(args, mode_t);
  va_end(args);

  /* Simple pass-through if disabled */
  if (!shim_enabled)
    return (*copen)(pathname, flags, mode);

  /* If enabled but not initialised, call post_init now */
  if (!lps_daemon)
    post_init();

  /* Should we deny it? */
  if (should_deny((char*)pathname)) {
    /* If writing or creating, whitelist it now.
     * Specifically allow creation on read-only. While it doesn't make much
     * sense, it doesn't make any more sense to fail due to the file not
     * existing in that case.
     */
    if (((flags & (O_RDONLY|O_WRONLY|O_RDWR)) != O_RDONLY) ||
        (flags & O_CREAT)) {
      hs_put(deny_exempt, (char*)pathname);
    } else {
      /* This is not the file you are looking for... */
      errno = ENOENT;
      return -1;
    }
  }

  /* No other instruction, return normally */
  fd = (*copen)(pathname, flags, mode);

  if (fd == -1) return -1;

  /* If reading (O_RDONLY or O_RDWR), perform readahead */
  if ((flags & O_RDONLY) == O_RDONLY ||
      (flags & O_RDWR  ) == O_RDWR) {
    readahead_amt = config_readahead()*(1024*1024/sizeof(discard));
    if (-1 == lseek(fd, 0, SEEK_SET)) {
      /* We couldn't seek for some reason.
       * Ignore the error and just proceed to any next step.
       */
      errno = 0;
      goto after_readahead;
    }
    /* Read to EOF or the readahead limit */
    for (buffers_read = 0; buffers_read < readahead_amt; ++buffers_read)
      if (sizeof(discard) != read(fd, discard, sizeof(discard)))
        break;

    after_readahead:
    /* Reopen the file to reset everything to the way it used to be. */
    close(fd);
    fd = (*copen)(pathname, flags, mode);
    if (fd == -1) return -1;
  }

  /* If reading, send the filename to the daemon if safe. */
  if (((flags & O_RDONLY) == O_RDONLY ||
       (flags & O_RDWR  ) == O_RDWR) &&
      !strchr(pathname, '\n') &&
      strlen(pathname) <= PREREAD_MAX_FILENAME_LEN &&
      command_output) {
    /* Add a \n to the end of the pathname, then send it to the daemon */
    sprintf(daemon_input, "%s\n", pathname);
    if (strlen(daemon_input) !=
        write(command_output, daemon_input, strlen(daemon_input))) {
      /* Something went wrong, close the stream and send no more commands.
       *
       * Printing a diagnostic is inappropriate at this point since we're in
       * the middle of the host's execution and it may be using stderr for
       * something.
       */
      close(command_output);
      command_output = 0;
    }
  }

  return fd;
}

static int should_deny(char* name) {
  char* denylist, * starg, * pattern;

  if (hs_test(deny_exempt, name)) return 0;

  /* Does it match any glob pattern? */
  denylist = getenv("PREREADSHIM_DENY");
  if (!denylist) return 0; /* No deny list configured */
  /* Make copy since strtok modifies it. */
  denylist = strdup(denylist);
  if (!denylist) return 0; /* Out of memory to test with */

  /* Examine each element and see if it matches */
  starg = denylist;
  while (pattern = strtok(starg, ":")) {
    starg = NULL;

    if (!fnmatch(pattern, name, 0)) {
      /* Matches, this file should be denied. */
      free(denylist);
      return 1;
    }
  }

  /* Nothing matches, allow. */
  free(denylist);
  return 0;
}
