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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <fnmatch.h>
#include <semaphore.h>
#include <poll.h>

#include "common.h"
#include "hashset.h"

#ifndef RTLD_NEXT
#error RTLD_NEXT undefined; please provide a definition of the constant if your\
 system supports it
#endif

#ifndef PAGESIZE
#define PAGESIZE sysconf(_SC_PAGE_SIZE)
#endif

/* The bits which indicate read/write access. */
#define ACCESS_FLAGS (O_RDONLY|O_WRONLY|O_RDWR)

/* Semaphore to lock state shared in the open() implementation etc.
 * Only initialised if has_semaphore is true.
 */
static sem_t semaphore;
/* Indicates whether semaphore has been initialised.
 * This is not used to track whether the semaphore must yet be initialised, but
 * rather whether it could be opened at all.
 */
static int has_semaphore;
static inline void lock(void) {
  int old_errno = errno;
  errno = 0;
  if (has_semaphore)
    /* Wait until success or an error that is not interruption.
     * If waiting errors for any other reason, continue on and hope for the
     * best.
     */
    while (sem_wait(&semaphore) && errno == EINTR);

  errno = old_errno;
}
static inline void unlock(void) {
  int old_errno;
  /* Release the semaphore.
   * First, preserve the old errno in case sem_post somehow fails.
   */
  old_errno = errno;
  if (has_semaphore)
    sem_post(&semaphore);
  errno = old_errno;
}

/* The time the program began exectution */
static time_t execution_start_time;

/* Pipe fd to which to write commands for the background reader process.
 * Zero if there is no background process.
 */
static int command_output;
/* The pid of the daemon process. Zero if none. */
static pid_t lps_daemon;

/* Whether to enable libchistka or to just pass through to libc open. The
 * latter is done for the chistka daemon so the library does not recurse.
 */
static int shim_enabled;

/* The open() function from the next library in the chain (usually libc). */
static int (*copen)(const char*, int, ...) = NULL;
/* Similar */
static int (*cpoll)(struct pollfd*, nfds_t, int) = NULL;
static ssize_t (*cread)(int, void*, size_t) = NULL;
static ssize_t (*cwrite)(int, void*, size_t) = NULL;
static void (*csync)(void) = NULL;
static int (*csyncfs)(int) = NULL;
static int (*cfsync)(int) = NULL;
static int (*cfdatasync)(int) = NULL;
static int (*cmsync)(void*, size_t, int) = NULL;

/* Set of files exempted from the DENY option. */
static hashset deny_exempt;

/* Returns whether the given file's existence should be denied. */
static int should_deny(char* name);

/* Returns the readahead amount (overridable in CHISTKA_READAHEAD) in
 * megabytes.
 */
static unsigned config_readahead() {
  static int has_extracted = 0;
  static unsigned value = 64;

  if (!has_extracted && getenv("CHISTKA_READAHEAD"))
    value = atoi(getenv("CHISTKA_READAHEAD"));

  has_extracted = 1;
  return value;
}

#ifndef __const
#define __const
#endif

/* Performs the bare minimum initialisation. This should be called when the
 * library is loaded. However, it is not necessarily --- using exec() seems to
 * be able able to bypass it's call.
 *
 * Initialises copen, shim_enabled, semaphore
 */
static void __attribute__((constructor)) libchistka_init(void) {
  char* message;

  execution_start_time = time(NULL);
  command_output = 0;
  lps_daemon = 0;

  /* Get the next open() function */
  dlerror(); /* Clear any DL error */
#define SYMBOL(symbol) \
  c##symbol = dlsym(RTLD_NEXT, #symbol); \
  if (message = dlerror()) { \
    if (cwrite) \
      (*cwrite)(STDERR_FILENO, message, strlen(message)); \
    exit(-1); \
  }
  SYMBOL(write);
  SYMBOL(read);
  SYMBOL(open);
  SYMBOL(poll);
  SYMBOL(sync);
  SYMBOL(syncfs);
  SYMBOL(fsync);
  SYMBOL(fdatasync);
  SYMBOL(msync);
#undef SYMBOL

  /* See if further operation is disabled */
  if (getenv("CHISTKA_DISABLE")) {
    shim_enabled = 0;
    return;
  }

  /* OK, Enabled */
  shim_enabled = 1;

  /* Open a semaphore */
  has_semaphore = !sem_init(&semaphore, 0, 1);
}

/* Additional startup logic that must run after everything else is
 * initialised. It is called on the first invocation of open().
 */
static void post_init(void) {
  /* Use a UNIX socket to talk to the daemon. The socket is located at
   *   /tmp/prereashim:EUID:PROFILE
   * where PROFILE is the environment variable CHISTKA_PROFILE, or the
   * empty string if that is not defined. Any forward slashes in PROFILE are
   * replaced with backslashes.
   *
   * First, try to create the socket with socket(2) and bind(2). If successful,
   * then there is no daemon for this uid-profile, so fork() and dup2() the
   * socket to the child's stdin. If it bind failed due to EADDRINUSE, the
   * socket already exists, so connect() to it and talk to that process.
   */
  int sock;
  struct sockaddr_un addr;
  char* message, profile[256], * ptr;

  sock = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sock == -1) {
    message = "chistka: could not create socket\n";
#ifdef DEBUG
    write(STDERR_FILENO, message, strlen(message));
#endif
    shim_enabled = 0;
    return;
  }

  /* Get and convert the profile name */
  if (getenv("CHISTKA_PROFILE")) {
    strncpy(profile, getenv("CHISTKA_PROFILE"), sizeof(profile)-1);
    profile[sizeof(profile)-1] = 0;
    for (ptr = profile; *ptr; ++ptr)
      if (*ptr == '/')
        *ptr = '\\';
  } else {
    profile[0] = 0;
  }
  /* Construct the address */
  addr.sun_family = AF_UNIX;
  snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/chistka:%d:%s",
           geteuid(), profile);
  if (!bind(sock, &addr,
            offsetof(struct sockaddr_un, sun_path) +
            strlen(addr.sun_path) + 1)) {
    /* Creation of socket was successful, fork the daemon. */
    lps_daemon = fork();
    if (!lps_daemon) {
      /* Child. */
      if (dup2(sock, STDIN_FILENO)) {
        close(sock);
        message = "chistka daemon: Could not set input pipe up\n";
#ifdef DEBUG
        write(STDERR_FILENO, message, strlen(message));
#endif

        /* Remove the socket file so nobody else connects to it. */
        unlink(addr.sun_path);

        _exit(-1);
      }

      /* Close original since we dupped to stdin */
      close(sock);

      /* Switch to daemon process */
      execlp("chistkad", "chistkad", addr.sun_path, NULL);

      /* If we get here, execlp() failed. */
      message = "chistka daemon: could not start\n";
#ifdef DEBUG
      write(STDERR_FILENO, message, strlen(message));
#endif
      /* Remove the socket file so nobody else connects to it. */
      unlink(addr.sun_path);
      _exit(-1);
    }

    if (lps_daemon == -1) {
      message = "Could not fork chistka daemon\n";
#ifdef DEBUG
      write(STDERR_FILENO, message, strlen(message));
#endif
      command_output = 0;
      /* Remove the socket file so nobody else connects to it. */
      unlink(addr.sun_path);
    }

    close(sock);

    /* Unset the CHISTKA_DISABLE env variable so it does not affect children
     * of the host process.
     */
    unsetenv("CHISTKA_DISABLE");
  } else if (errno != EADDRINUSE) {
    /* Some other error, give up. */
    message = "Could not bind() to daemon socket\n";
#ifdef DEBUG
    write(STDERR_FILENO, message, strlen(message));
#endif
    command_output = 0;
    return;
  }

  close(sock);
  /* Connect to the now-running daemon */
  sock = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (sock == -1 ||
      connect(sock, &addr,
              offsetof(struct sockaddr_un, sun_path) +
              strlen(addr.sun_path) + 1)) {
    message = "Could not connect() to daemon\n";
#ifdef DEBUG
    write(STDERR_FILENO, message, strlen(message));
#endif
    command_output = 0;

    if (sock != -1)
      close(sock);

    /* Unlink the socket file if possible.
     * If something happened to the daemon, the next program will then restart
     * it.
     */
    unlink(addr.sun_path);
  } else {
    command_output = sock;
    /* Make it non-blocking so that if the daemon gets stuck or KILLed, it
     * won't negatively affect the host process.
     */
    if (fcntl(sock, F_SETFL, O_NONBLOCK)) {
#ifdef DEBUG
      perror("chistka: fcntl");
#endif
    }
    /* Close on exec() so that programs with children don't (effectively) leak
     * file descriptors.
     */
    if (fcntl(sock, F_SETFD, FD_CLOEXEC)) {
#ifdef DEBUG
      perror("chistka: fcntl");
#endif
    }
  }

  /* Allocate structures */
  deny_exempt = hs_create();
}

/* Converts the given flags to open() to a human-readable string and returns
 * it.
 */
#ifdef DEBUG_SHIM_OPEN
static char* flagstr(int flags) {
  static char str[256];
  char rest[16];

  switch (flags & ACCESS_FLAGS) {
  case O_RDONLY:
    strcpy(str, "R|");
    break;
  case O_WRONLY:
    strcpy(str, "W|");
    break;
  case O_RDWR:
    strcpy(str, "RW|");
    break;
  default:
    sprintf(str, "%X|", (flags & ACCESS_FLAGS));
    break;
  }
  flags &= ~ACCESS_FLAGS;

#define C(f) if (flags & O_##f) strcat(str, #f "|"); flags &= ~O_##f
  C(APPEND);
  C(ASYNC);
  C(CLOEXEC);
  C(CREAT);
  C(DIRECT);
  C(DIRECTORY);
  C(EXCL);
  C(NOATIME);
  C(NOCTTY);
  C(NOFOLLOW);
  C(NONBLOCK);
  C(NDELAY);
  C(SYNC);
  C(TRUNC);
#undef C

  sprintf(rest, "%X", flags);
  strcat(str, rest);
  return str;
}
#endif

int open(__const char* pathname, int flags, ...) {
  va_list args;
  mode_t mode;
  int fd;
  char discard[4096], daemon_input[CHISTKA_MAX_FILENAME_LEN+2];
  unsigned buffers_read, readahead_amt, len;
  struct stat stat;
  off_t old_pos;
  int status, old_errno = errno;

  /* It is possible the constructor function won't be called.
   * Detect this and call it now.
   *
   * PROBLEM: A multithreaded program could already have threads by this point!
   * There is no hope for avoiding the race condition in such a case. However,
   * most programs implicitly call open() during linking etc, so this will
   * generally not be a problem.
   */
  if (!copen) {
    libchistka_init();
    /* Prevent _init()'s actions from affecting errno. */
    errno = old_errno;
  }

  va_start(args, flags);
  mode = va_arg(args, mode_t);
  va_end(args);

#ifdef DEBUG_SHIM_OPEN
  status = readlink("/proc/self/exe", discard, 128);
  if (status == -1)
    strcpy(discard, "(unknown)");
  else
    discard[status] = 0;
  fprintf(stderr, "DEBUG: %s open(%s, %s, %d) = ",
          discard, pathname, flagstr(flags), mode);
#endif

  /* Simple pass-through if disabled */
  if (!shim_enabled) {
    fd = (*copen)(pathname, flags, mode);
#ifdef DEBUG_SHIM_OPEN
    old_errno = errno;
    fprintf(stderr, "(disabled) %d, %d (%s)\n", fd, errno, strerror(errno));
    errno = old_errno;
#endif
    return fd;
  }

  /* Entering shared state. If we have a semaphore, lock it now. */
  lock();

#define RETURN(value) do { status = value; goto end; } while(0)

  /* If enabled but not initialised, call post_init now */
  if (!lps_daemon) {
    post_init();
    /* Prevent leaking errno changes. */
    errno = old_errno;

    /* If the shim was disabled as a result of failed secondary initialisation,
     * just perform passthrough here.
     */
    if (!shim_enabled) {
      unlock();
      fd = (*copen)(pathname, flags, mode);
#ifdef DEBUG_SHIM_OPEN
      old_errno = errno;
      fprintf(stderr, "(failure) %d, %d (%s)\n", fd, errno, strerror(errno));
      errno = old_errno;
#endif
      return fd;
    }
  }

  /* Should we deny it? */
  if (should_deny((char*)pathname)) {
    /* If writing or creating, whitelist it now.
     * Specifically allow creation on read-only. While it doesn't make much
     * sense, it doesn't make any more sense to fail due to the file not
     * existing in that case.
     */
    if (((flags & ACCESS_FLAGS) != O_RDONLY) ||
        (flags & O_CREAT)) {
      hs_put(deny_exempt, (char*)pathname);
    } else {
      /* This is not the file you are looking for... */
      old_errno = errno = ENOENT;
      RETURN(-1);
    }
  }

  /* Remove requests for synchronicity */
  flags &= ~(O_SYNC|O_DSYNC|O_RSYNC);

  /* No other instruction, open normally */
  fd = (*copen)(pathname, flags, mode);
  old_errno = errno;

  if (fd == -1) RETURN(-1);

  /* If reading (O_RDONLY or O_RDWR), perform readahead if this is a regular
   * file.
   */
  if ((flags & ACCESS_FLAGS) == O_RDONLY ||
      (flags & ACCESS_FLAGS) == O_RDWR) {
    /* Make sure it's a normal file */
    if (!fstat(fd, &stat) && S_ISREG(stat.st_mode)) {
      readahead_amt = config_readahead()*(1024*1024/sizeof(discard));
      old_pos = lseek(fd, 0, SEEK_CUR);
      if (old_pos == -1) {
        /* We couldn't get the current position. Ignore the error and just
         * proceed to the next step.
         */
        goto after_readahead;
      }
      if (-1 == lseek(fd, 0, SEEK_SET)) {
        /* We couldn't seek for some reason.
         * Ignore the error and just proceed to any next step.
         */
        goto after_readahead;
      }
      /* Read to EOF or the readahead limit */
      for (buffers_read = 0; buffers_read < readahead_amt; ++buffers_read)
        if (sizeof(discard) != read(fd, discard, sizeof(discard)))
          break;

      /* Seek back to where we started */
      lseek(fd, old_pos, SEEK_SET);
    } else {
      /* Not a regular file or fstat() failed.
       * In case of the latter, reset errno.
       * (Now handled by falling through.)
       */
      /* errno = 0; */
    }
  }
  after_readahead:

  /* If reading, send the filename to the daemon if safe. */
  if (((flags & ACCESS_FLAGS) == O_RDONLY ||
       (flags & ACCESS_FLAGS) == O_RDWR) &&
      !strchr(pathname, '\n') &&
      strlen(pathname) <= CHISTKA_MAX_FILENAME_LEN &&
      command_output) {
    /* If the pathname does not begin with a /, add the current working
     * directory to the pathname followed by a slash.
     * Then add a \n to the end of the pathname, then send it to the daemon.
     */
    if (getcwd(discard, sizeof(discard))) {
      snprintf(daemon_input, sizeof(daemon_input), "%s%s%s\n",
               pathname[0] == '/'? "" : discard,
               pathname[0] == '/'? "" : "/",
               pathname);
      /* If it fit into the buffer (which means that
       * daemon_input[strlen(daemon_input)+1] == '\n'), send it to the daemon.
       */
      len = strlen(daemon_input);
      if (len > 0 && daemon_input[len-1] == '\n' &&
          len != write(command_output, daemon_input, len)) {
        /* Not an error if we get EAGAIN or EWOULDBLOCK: The daemon may be
         * stuck or dead.
         */
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
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
    }
  }

  /* Reset errno to what it was before seeking, statting, etc */
  errno = old_errno;

  RETURN(fd);

  end:
  /* Leaving use of shared state, release the semaphore if there is one. */
  unlock();
#ifdef DEBUG_SHIM_OPEN
  fprintf(stderr, "%d, %d (%s)\n", fd, errno, strerror(errno));
#endif
  errno = old_errno;
  return status;
}

static int should_deny(char* name) {
  char* denylist, * starg, * pattern;

  if (hs_test(deny_exempt, name)) return 0;

  /* Does it match any glob pattern? */
  denylist = getenv("CHISTKA_DENY");
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

int fsync(int fd) {
  if (!cfsync)
    libchistka_init();
  if (!shim_enabled)
    return (*cfsync)(fd);

  /* Do nothing, but make sure fd is valid. */
  errno = 0;
  return fcntl(fd, F_GETFD, 0) == -1? -1 : 0;
}

int fdatasync(int fd) {
  if (!cfdatasync)
    libchistka_init();
  if (!shim_enabled)
    return (*cfdatasync)(fd);

  return fsync(fd);
}

void sync(void) {
  if (!csync)
    libchistka_init();

  if (!shim_enabled)
    (*csync)();
}

int syncfs(int fd) {
  if (!csyncfs)
    libchistka_init();

  if (!shim_enabled)
    return (*csyncfs)(fd);

  return fsync(fd);
}

int msync(void* addr, size_t length, int flags) {
  if (!cmsync)
    libchistka_init();

  if (!shim_enabled)
    return (*cmsync)(addr, length, flags);

  /* Do nothing, but check for errors for compatibility. */

  /* EINVAL: "addr is not a multiple of PAGESIZE" */
  if (((unsigned)addr) % PAGESIZE) {
    errno = EINVAL;
    return -1;
  }

  /* EINVAL: "any bit other than MS_ASYNC|MS_INVALIDATE|MS_SYNC is set in
   * flags"
   */
  if (flags & ~(MS_ASYNC|MS_INVALIDATE|MS_SYNC)) {
    errno = EINVAL;
    return -1;
  }

  /* ENOMEM: "The indicated memory (or part of it) was not mapped" */
  /* We have no easy way of determining this. Let's hope nothing depends on
   * this behaviour.
   */

  errno = 0;
  return 0;
}

/* Tracks the number of times the file descriptors 0..255 have been polled
 * without being read or written. Values are only incremented when poll() exits
 * for a reason other than timeout or error. Whenever read() or write()
 * operates successfully on one of these which was non-zero, the entire array
 * is reset to zero. Otherwise, any read() or write() simply sets its own entry
 * to zero.
 *
 * When an entry exceeds $CHISTKA_POLL_IGNORE (default disabled), poll() will
 * ignore attempts to use that fd in the call.
 */
#define NUM_POLL_IGNORED 256
static unsigned poll_ignored[NUM_POLL_IGNORED] = {0};

int poll(struct pollfd* fds, nfds_t cnt, int timeout) {
  static int timeout_limit, ignore_limit, has_parms = 0;
  /* The indices of the items that were disabled due to neglect */
  char disabled[cnt];
  unsigned i;
  int status;

  /* Initialise if this hasn't happened yet.
   * Don't bother with the daemon, since we won't need it.
   */
  if (!cpoll)
    libchistka_init();

  if (!shim_enabled)
    return (*cpoll)(fds, cnt, timeout);

  /* Lock the shared structures */
  lock();

  /* Get environment parms if we don't have them yet */
  if (!has_parms) {
    has_parms = 1;

    timeout_limit = 0;
    ignore_limit = -1;
    if (getenv("CHISTKA_POLL_TIMEOUT"))
      timeout_limit = atoi(getenv("CHISTKA_POLL_TIMEOUT"));
    if (getenv("CHISTKA_POLL_IGNORE"))
      ignore_limit = atoi(getenv("CHISTKA_POLL_IGNORE"));
  }

  /* Adjust the timeout if needed */
  if (timeout_limit == -1)
    timeout = -1;
  else if (timeout > 0 && timeout < timeout_limit)
    timeout = timeout_limit;

  /* Disable FDs being polled the wrong way */
  memset(disabled, 0, sizeof(disabled));
  if (ignore_limit != -1) {
    for (i = 0; i < (unsigned)cnt; ++i) {
      if (fds[i].fd > 0 && fds[i].fd < NUM_POLL_IGNORED &&
          poll_ignored[fds[i].fd] > (unsigned)ignore_limit) {
        disabled[i] = 1;
        fds[i].fd = -fds[i].fd;
      }
    }
  }

  /* Release the lock so that we don't block other poll()s or open()s on other
   * threads, then call the actual function.
   */
  unlock();
  status = (*cpoll)(fds, cnt, timeout);
  lock();

  /* If successful and not timed out, increment all polled fds */
  if (status > 0) {
    for (i = 0; i < (unsigned)cnt; ++i) {
      if (fds[i].fd > 0 && fds[i].fd < NUM_POLL_IGNORED)
        ++poll_ignored[fds[i].fd];
    }
  }

  /* Reset fds wherever we disabled something */
  for (i = 0; i < (unsigned)cnt; ++i) {
    if (disabled[i]) {
      fds[i].fd = -fds[i].fd;
    }
  }

  /* Done */
  unlock();
  return status;
}

static ssize_t read_write_common(int fd, void* buff, size_t count,
                                 ssize_t (*f)(int, void*, size_t)) {
  ssize_t status;
  status = (*f)(fd, buff, count);
  /* We don't need to lock the below since the only modifications to shared
   * structure involve setting values to zero. Any interference with poll()
   * will simply result in poll() seing values somewhere between what they were
   * and zero.
   */
  if (fd >= 0 && fd < NUM_POLL_IGNORED) {
    /* If successful (to any extent) and this read has been poll()ed before,
     * reset all to zero.
     */
    if (status > 0 && poll_ignored[fd])
      memset(poll_ignored, 0, sizeof(poll_ignored));
    else
      poll_ignored[fd] = 0;
  }

  return status;
}

ssize_t read(int fd, void* buff, size_t count) {
  if (!cread) libchistka_init();
  return read_write_common(fd, buff, count, cread);
}

ssize_t write(int fd, __const void* buff, size_t count) {
  if (!cwrite) libchistka_init();
  return read_write_common(fd, (void*)buff, count, cwrite);
}
