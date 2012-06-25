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

/* The daemon reads filenames from stdin, one filename per line. Each filename
 * will schedule any necessary events associated with that file.
 *
 * It is the responsibility of the source to ensure that no filenames sent
 * contain newlines and to not send filenames longer than
 * PREREAD_MAX_FILENAME_LEN as defined in common.h.
 */

#include "hashset.h"
#include "common.h"

/* True if the input stream is still open. */
static int reading_input;

static void input_available(int parm) {
  char filename[PREREAD_MAX_FILENAME_LEN+2]; /* +2 for LF and NUL */
  while (fgets(filename, sizeof(filename), stdin)) {
    /* filename already has \n at end */
    printf("daemon: %s", filename);
  }

  /* Any error terminates reading, except for indications that there is
   * /currently/ no input, which we simply ignore.
   */
  if (errno != EAGAIN && errno != EWOULDBLOCK)
    reading_input = 0;
  else
    clearerr(stdin);
}

int main(void) {
  struct sigaction sigio = {};
  /* Reconfigure stdin to be ASYNC and NONBLOCK */
  if (-1 == fcntl(STDIN_FILENO, F_SETFL, O_ASYNC|O_NONBLOCK))
    perror("fcntl(F_SETFL,O_ASYNC|O_NONBLOCK)");
  if (-1 == fcntl(STDIN_FILENO, F_SETOWN, getpid()))
    perror("fcntl(F_SETOWN)");

  /* Set signal handlers */
  sigio.sa_handler = input_available;
  sigaction(SIGIO, &sigio, NULL);

  /* Process signals until input is closed.
   * Once input is closed, the parent process has exited, so continuing with
   * events would have no benefit.
   */
  reading_input = 1;
  do {
    /* Read any input that may have occurred when there was no signal handler */
    input_available(0);
    if (feof(stdin) || !reading_input) break;
    pause();
  } while (!feof(stdin) && reading_input);

  return 0;
}
