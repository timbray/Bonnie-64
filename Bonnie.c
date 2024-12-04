/*
 * This is a file system benchmark which attempts to study bottlenecks -
 * it is named 'Bonnie' after Bonnie Raitt, who knows how to use one.
 *
 * Commentary on Bonnie's operations may be found at
 * http://www.textuality.com/bonnie/intro.html
 *
 * COPYRIGHT NOTICE:
 * Copyright (c) Tim Bray, 1990-2007.
 *
 * Use is subject to license terms - see file "LICENSE"
 */

#pragma ident	"@(#)Bonnie.c	1.2	04/09/01 SMI"

#include <unistd.h>
#include <stdio.h>

#include <strings.h>
#include <string.h>
#if !defined(SysV)
#include <stdlib.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#if defined(SysV)
#include <limits.h>
#include <sys/times.h>
#else
#include <sys/resource.h>
#endif

#define IntSize (sizeof(int))

/*
 * N.B. in seeker_reports, CPU appears and Start/End time, but not Elapsed,
 *  so position 1 is re-used; icky data coupling.
 */
#define CPU (0)
#define Elapsed (1)
#define StartTime (1)
#define EndTime (2)
#define Seeks (4000)
#define UpdateSeek (10)
#define SeekProcCount (3)
#define Chunk (16384)

#define TABLE_STYLE "<td style=\"text-align: center;\" colspan=\"%d\" rowspan=\"%d\">%s</td>"

/* labels for the tests, used as an array index */
typedef enum {
  Putc, ReWrite, FastWrite, Getc, FastRead, Lseek, TestCount
} tests_t;

static void   fill_buffer(int * buf);
static void   dirty_buffer(int * buf);
static double cpu_so_far();
static void   doseek(off_t where, int fd, int update);
static void   get_delta_t(tests_t test);
static void   io_error(char * message);
static void   newfile(char * name, int * fd, FILE * * stream, int create);

#if defined(SysV)
/* System V wrappers for randomizers */
static long   random();
static void   srandom(int seed);
#endif

static void   report(char * machine, off_t size);
static void   write_html_head();
static void   write_html_tail();
static void   write_html(char * machine, off_t size);
static double time_so_far();
static void   timestamp();
static void   usage();

/*
 * Housekeeping variables to build up timestamps for the tests;
 *  global to make it easy to keep track of the progress of time.
 * all of this could have been done with non-global variables,
 *  but the code is easier to read this way and I don't anticipate
 *  much software engineering down the road
 */
static int    basetime;			 /* when we started */
static double delta[(int) TestCount][2]; /* array of DeltaT values */
static double last_cpustamp = 0.0;	 /* for computing delta-t */
static double last_timestamp = 0.0;	 /* for computing delta-t */

int main(int argc, char * argv[]) {
  int	 buf[Chunk / IntSize];
  int	 bufindex;
  char * cbuf;
  int	 chars[256];
  int	 child;
  char * dir;
  int	 html = 0;
  int	 fd;
  double first_start;
  double last_stop;
  int	 lseek_count = 0;
  char * machine;
  char * format;
  char	 name[Chunk];
  int	 next;
  int	 preserve = 0;
  int	 randomize = 0;
  int	 seek_control[2];
  int	 seek_feedback[2];
  char	 seek_tickets[Seeks + SeekProcCount];
  double seeker_report[3];
  off_t	 size;
  FILE * stream;
  off_t	 words;
  int	 c;
  extern int optind;
  extern char *optarg;

  fd = -1;
  basetime = (int) time((time_t *) NULL);
  size = 100;
  dir = ".";
  machine = "";

  /* pick apart args */
  while ((c = getopt(argc, argv, "d:s:m:f:rx")) != EOF) {
    switch (c) {
    case 'd':
      dir = optarg;
      break;
    case 'r':
      randomize = 1;
      break;
    case 's':
      size = atol(optarg);
      break;
    case 'm':
      machine = optarg;
      break;
    case 'f':
      format = optarg;
      if ((strcmp(format, "html") == 0) || (strcmp(format, "HTML") == 0))
	html = 1;
      else if ((strcmp(format, "txt") == 0) || (strcmp(format, "TXT") == 0))
	html = 0;
      else
	usage();
      break;
    case 'u':
      preserve = 1;
      break;
    default:
      usage();
      exit(1);
    }
  }

  if (size < 1)
    usage();

  /* sanity check - 32-bit machines can't handle more than 2047 Mb */
  if (sizeof(off_t) <= 4 && size > 2047) {
    fprintf(stderr, "File too large for 32-bit machine, sorry\n");
    exit(1);
  }

  sprintf(name, "%s/Bonnie.%d", dir, getpid());

  /* size is in meg, rounded down to multiple of Chunk */
  size *= (1024 * 1024);
  size = Chunk * (size / Chunk);
  fprintf(stderr, "File '%s', size: %lld\n", name, (long long) size);

  /* Fill up a file, writing it a char at a time with the stdio putc() call */
  fprintf(stderr, "Writing with putc()...");
  newfile(name, &fd, &stream, 1);
  fill_buffer(buf);
  cbuf = (char *) buf;
  bufindex = 0;
  timestamp();
  if (randomize) {
    for (words = 0; words < size; words++) {
      if (putc(cbuf[bufindex++], stream) == EOF)
	io_error("putc");
      if (bufindex == Chunk) {
	dirty_buffer(buf);
	bufindex = 0;
      }
    }
  } else {
    for (words = 0; words < size; words++)
      if (putc(words & 0x7f, stream) == EOF)
	io_error("putc");
  }

  /*
   * note that we always close the file before measuring time, in an
   *  effort to force as much of the I/O out as we can.	 To be
   *  paranoid we also flush both at stdio and OS level which, if
   *  close/fclose does not flush, should make the timings slightly
   *  more real.
   */
  if (fflush(stream) != 0)
    io_error("fflush after putc");
  if (fsync(fd) == -1)
    io_error("fsync after putc");
  if (fclose(stream) == -1)
    io_error("fclose after putc");
  get_delta_t(Putc);
  fprintf(stderr, "done\n");

  /* Now read & rewrite it using block I/O.  Dirty one word in each block */
  newfile(name, &fd, &stream, 0);
  if (lseek(fd, (off_t) 0, SEEK_SET) == (off_t) -1)
    io_error("lseek(2) before rewrite");
  fprintf(stderr, "Rewriting...");
  timestamp();
  bufindex = 0;
  if ((words = read(fd, (char *) buf, Chunk)) == -1)
    io_error("rewrite read");
  while (words == Chunk) { /* while we can read a block */
    if (bufindex == Chunk / IntSize)
      bufindex = 0;
    buf[bufindex++]++;
    if (lseek(fd, (off_t) -words, SEEK_CUR) == -1)
      io_error("relative lseek(2)");
    if (write(fd, (char *) buf, words) == -1)
      io_error("re write(2)");
    if ((words = read(fd, (char *) buf, Chunk)) == -1)
      io_error("rwrite read");
  } /* while we can read a block */
  if (fsync(fd) == -1)
    io_error("fsync after rewrite");
  if (close(fd) == -1)
    io_error("close after rewrite");
  get_delta_t(ReWrite);
  fprintf(stderr, "done\n");

  /* Write the whole file from scratch, again, with block I/O */
  newfile(name, &fd, &stream, 1);
  fprintf(stderr, "Writing intelligently...");
  if (randomize)
    fill_buffer(buf);
  else
    for (words = 0; words < Chunk / IntSize; words++)
      buf[words] = 0;
  timestamp();
  if (randomize) {
    for (words = 0; words < (size / Chunk); words++) {
      if (write(fd, (char *) buf, Chunk) == -1)
	io_error("write(2)");
      dirty_buffer(buf);
    }
  } else {
    for (words = bufindex = 0; words < (size / Chunk); words++) { /* for each word */
      if (bufindex == (Chunk / IntSize))
	bufindex = 0;
      buf[bufindex++]++;
      if (write(fd, (char *) buf, Chunk) == -1)
	io_error("write(2)");
    } /* for each word */
  }
  if (fsync(fd) == 01)
    io_error("fsync after fast write");
  if (close(fd) == -1)
    io_error("close after fast write");
  get_delta_t(FastWrite);
  fprintf(stderr, "done\n");

  /* read them all back with getc() */
  newfile(name, &fd, &stream, 0);
  for (words = 0; words < 256; words++)
    chars[words] = 0;
  fprintf(stderr, "Reading with getc()...");
  timestamp();
  for (words = 0; words < size; words++) { /* for each byte */
    if ((next = getc(stream)) == EOF)
      io_error("getc(3)");

    /* just to fool optimizers */
    chars[next]++;
  } /* for each byte */
  if (fclose(stream) == -1)
    io_error("fclose after getc");
  get_delta_t(Getc);
  fprintf(stderr, "done\n");

  /* use the frequency count */
  for (words = 0; words < 256; words++)
    sprintf((char *) buf, "%d", chars[words]);

  /* Now suck it in, Chunk at a time, as fast as we can */
  newfile(name, &fd, &stream, 0);
  if (lseek(fd, (off_t) 0, SEEK_SET) == -1)
    io_error("lseek before read");
  fprintf(stderr, "Reading intelligently...");
  timestamp();
  do { /* per block */
    if ((words = read(fd, (char *) buf, Chunk)) == -1)
      io_error("read(2)");
    chars[buf[abs(buf[0]) % (Chunk / IntSize)] & 0x7f]++;
  } /* per block */
  while (words);
  if (close(fd) == -1)
    io_error("close after read");
  get_delta_t(FastRead);
  fprintf(stderr, "done\n");

  /* use the frequency count */
  for (words = 0; words < 256; words++)
    sprintf((char *) buf, "%d", chars[words]);

  /*
   * Now test random seeks; first, set up for communicating with children.
   * The object of the game is to do "Seeks" lseek() calls as quickly
   *  as possible.  So we'll farm them out among SeekProcCount processes.
   *  We'll control them by writing 1-byte tickets down a pipe which
   *  the children all read.  We write "Seeks" bytes with val 1, whichever
   *  child happens to get them does it and the right number of seeks get
   *  done.
   * The idea is that since the write() of the tickets is probably
   *  atomic, the parent process likely won't get scheduled while the
   *  children are seeking away.  If you draw a picture of the likely
   *  timelines for three children, it seems likely that the seeks will
   *  overlap very nicely with the process scheduling with the effect
   *  that there will *always* be a seek() outstanding on the file.
   * Question: should the file be opened *before* the fork, so that
   *  all the children are lseeking on the same underlying file object?
   */
  if (pipe(seek_feedback) == -1 || pipe(seek_control) == -1)
    io_error("pipe");
  for (next = 0; next < Seeks; next++)
    seek_tickets[next] = 1;
  for ( ; next < (Seeks + SeekProcCount); next++)
    seek_tickets[next] = 0;

  /* launch some parallel seek processes */
  for (next = 0; next < SeekProcCount; next++) { /* for each seek proc */
    if ((child = fork()) == -1)
      io_error("fork");
    else if (child == 0) { /* child process */

      /* set up and wait for the go-ahead */
      close(seek_feedback[0]);
      close(seek_control[1]);
      newfile(name, &fd, &stream, 0);
      srandom(getpid());
      fprintf(stderr, "Seeker %d...", next + 1);

      /* wait for the go-ahead */
      if (read(seek_control[0], seek_tickets, 1) != 1)
	io_error("read ticket");
      timestamp();
      seeker_report[StartTime] = time_so_far();

      /* loop until we read a 0 ticket back from our parent */
      while(seek_tickets[0]) { /* until Mom says stop */
	doseek((long) (random() % (size / Chunk)), fd,
	       ((lseek_count++ % UpdateSeek) == 0));
	if (read(seek_control[0], seek_tickets, 1) != 1)
	  io_error("read ticket");
      } /* until Mom says stop */
      if (close(fd) == -1)
	io_error("close after seek");

      /* report to parent */
      get_delta_t(Lseek);
      seeker_report[EndTime] = time_so_far();
      seeker_report[CPU] = delta[(int) Lseek][CPU];
      if (write(seek_feedback[1], seeker_report, sizeof(seeker_report))
	  != sizeof(seeker_report))
	io_error("pipe write");
      exit(0);
    } /* child process */
  } /* for each seek proc */

  /*
   * Back in the parent; in an effort to ensure the children get an even
   *  start, wait a few seconds for them to get scheduled, open their
   *  files & so on.
   */
  close(seek_feedback[1]);
  close(seek_control[0]);
  sleep(5);
  fprintf(stderr, "start 'em...");
  if (write(seek_control[1], seek_tickets, sizeof(seek_tickets))
      != sizeof(seek_tickets))
    io_error("write tickets");

  /* read back from children */
  for (next = 0; next < SeekProcCount; next++) { /* for each child */
    if (read(seek_feedback[0], (char *) seeker_report, sizeof(seeker_report))
	!= sizeof(seeker_report))
      io_error("pipe read");

    /*
     * each child writes back its CPU, start & end times.  The elapsed time
     *	to do all the seeks is the time the first child started until the
     *	time the last child stopped
     */
    delta[(int) Lseek][CPU] += seeker_report[CPU];
    if (next == 0) { /* first time */
      first_start = seeker_report[StartTime];
      last_stop = seeker_report[EndTime];
    } /* first time */
    else { /* not first time */
      first_start = (first_start < seeker_report[StartTime]) ?
	first_start : seeker_report[StartTime];
      last_stop = (last_stop > seeker_report[EndTime]) ?
	last_stop : seeker_report[EndTime];
    } /* not first time */
    if (wait(&child) == -1)
      io_error("wait");
    fprintf(stderr, "done...");
  } /* for each child */
  fprintf(stderr, "\n");
  delta[(int) Lseek][Elapsed] = last_stop - first_start;

  if (html)
    write_html(machine, size);
  else
    report(machine, size);
  if (!preserve)
    unlink(name);

  return 0;
}

static void write_html_head() {
  printf("<table>\n");
  printf("<tr>\n");
  printf("<td></td><td></td>");
  printf(TABLE_STYLE, 6, 1, "Sequential Output");
  printf(TABLE_STYLE, 4, 1, "Sequential Input");
  printf(TABLE_STYLE, 2, 2, "Random Seeks");
  printf("\n</tr>\n");

  printf("<tr>\n");
  printf("<td></td><td></td>");
  printf(TABLE_STYLE, 2, 1, "Per Char");
  printf(TABLE_STYLE, 2, 1, "Block");
  printf(TABLE_STYLE, 2, 1, "Rewrite");
  printf(TABLE_STYLE, 2, 1, "Per Char");
  printf(TABLE_STYLE, 2, 1, "Block");
  printf("\n</tr>\n");

  printf("<tr>\n");
  printf("<td>Machine</td><td>MB</td>");
  printf("<td>K/sec</td><td>%%CPU</td>");
  printf("<td>K/sec</td><td>%%CPU</td>");
  printf("<td>K/sec</td><td>%%CPU</td>");
  printf("<td>K/sec</td><td>%%CPU</td>");
  printf("<td>K/sec</td><td>%%CPU</td>");
  printf("<td> /sec</td><td>%%CPU</td>");
  printf("\n</tr>\n");
}

static void write_html_tail() {
  printf("</table>\n");
}


static void write_html(char * machine, off_t size) {
  write_html_head();
  printf("<tr>\n<td>%s</td><td>%lld</td>", machine,
	 (long long) (size / (1024 * 1024)));
  printf("<td>%d</td><td>%4.1f</td><td>%d</td><td>%4.1f</td><td>%d</td><td>%4.1f</td>",
	 (int) (((double) size) / (delta[(int) Putc][Elapsed] * 1024.0)),
	 delta[(int) Putc][CPU] / delta[(int) Putc][Elapsed] * 100.0,
	 (int) (((double) size) / (delta[(int) FastWrite][Elapsed] * 1024.0)),
	 delta[(int) FastWrite][CPU] / delta[(int) FastWrite][Elapsed] * 100.0,
	 (int) (((double) size) / (delta[(int) ReWrite][Elapsed] * 1024.0)),
	 delta[(int) ReWrite][CPU] / delta[(int) ReWrite][Elapsed] * 100.0);
  printf("<td>%d</td><td>%4.1f</td><td>%d</td><td>%4.1f</td>",
	 (int) (((double) size) / (delta[(int) Getc][Elapsed] * 1024.0)),
	 delta[(int) Getc][CPU] / delta[(int) Getc][Elapsed] * 100.0,
	 (int) (((double) size) / (delta[(int) FastRead][Elapsed] * 1024.0)),
	 delta[(int) FastRead][CPU] / delta[(int) FastRead][Elapsed] * 100.0);
  printf("<td>%5.1f</td><td>%4.1f</td>\n</tr>\n",
	 ((double) Seeks) / delta[(int) Lseek][Elapsed],
	 delta[(int) Lseek][CPU] / delta[(int) Lseek][Elapsed] * 100.0);

  write_html_tail();
}

static void report(char * machine, off_t size) {
  printf("		");
  printf(
	 "-------Sequential Output-------- ---Sequential Input-- --Random--\n");
  printf("		");
  printf(
	 "-Per Char- --Block--- -Rewrite-- -Per Char- --Block--- --Seeks---\n");
  printf("Machine    GB ");
  printf("M/sec %%CPU M/sec %%CPU M/sec %%CPU M/sec %%CPU M/sec ");
  printf("%%CPU	 /sec %%CPU\n");

  printf("%-8.8s %4lld ", machine, (long long) (size / (1024 * 1024 * 1000)));
  printf("%5.1f %4.1f %5.1f %4.1f %5.1f %4.1f ",
	 ((double) size) / (delta[(int) Putc][Elapsed] * 1024.0 * 1024.0),
	 delta[(int) Putc][CPU] / delta[(int) Putc][Elapsed] * 100.0,
	 ((double) size) / (delta[(int) FastWrite][Elapsed] * 1024.0 * 1024.0),
	 delta[(int) FastWrite][CPU] / delta[(int) FastWrite][Elapsed] * 100.0,
	 ((double) size) / (delta[(int) ReWrite][Elapsed] * 1024.0 * 1024.0),
	 delta[(int) ReWrite][CPU] / delta[(int) ReWrite][Elapsed] * 100.0);
  printf("%5.1f %4.1f %5.1f %4.1f ",
	 ((double) size) / (delta[(int) Getc][Elapsed] * 1024.0 * 1024.0),
	 delta[(int) Getc][CPU] / delta[(int) Getc][Elapsed] * 100.0,
	 ((double) size) / (delta[(int) FastRead][Elapsed] * 1024.0 * 1024.0),
	 delta[(int) FastRead][CPU] / delta[(int) FastRead][Elapsed] * 100.0);
  printf("%5d %4.1f\n",
	 (int) (((double) Seeks) / delta[(int) Lseek][Elapsed]),
	 delta[(int) Lseek][CPU] / delta[(int) Lseek][Elapsed] * 100.0);
}

static void newfile(char * name, int * fd, FILE * * stream, int create) {
  if (create) { /* create from scratch */
    if (unlink(name) == -1 && *fd != -1)
      io_error("unlink");
    *fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0777);
  } /* create from scratch */
  else
    *fd = open(name, O_RDWR, 0777);

  if (*fd == -1)
    io_error(name);
  *stream = fdopen(*fd, "r+");
  if (*stream == NULL)
    io_error("fdopen");
}

static void usage() {
  fprintf(stderr,
	  "usage: Bonnie [-d scratch-dir] [-s size-in-Mb] [-m machine-label] [-f txt/html]\n");
  exit(1);
}

static void timestamp() {
  last_timestamp = time_so_far();
  last_cpustamp = cpu_so_far();
}

static void get_delta_t(tests_t test) {
  int which = (int) test;

  delta[which][Elapsed] = time_so_far() - last_timestamp;
  delta[which][CPU] = cpu_so_far() - last_cpustamp;
}

static double cpu_so_far() {
#if defined(SysV)
  struct tms tms;

  if (times(&tms) == -1)
    io_error("times");
  return ((double) tms.tms_utime) / ((double) sysconf(_SC_CLK_TCK)) +
    ((double) tms.tms_stime) / ((double) sysconf(_SC_CLK_TCK));

#else
  struct rusage rusage;

  getrusage(RUSAGE_SELF, &rusage);
  return
    ((double) rusage.ru_utime.tv_sec) +
    (((double) rusage.ru_utime.tv_usec) / 1000000.0) +
    ((double) rusage.ru_stime.tv_sec) +
    (((double) rusage.ru_stime.tv_usec) / 1000000.0);
#endif
}

static double time_so_far() {
#if defined(SysV)
  int	     val;
  struct tms tms;

  if ((val = times(&tms)) == -1)
    io_error("times");

  return ((double) val) / ((double) sysconf(_SC_CLK_TCK));

#else
  struct timeval tp;

  if (gettimeofday(&tp, (struct timezone *) NULL) == -1)
    io_error("gettimeofday");
  return ((double) (tp.tv_sec - basetime)) +
    (((double) tp.tv_usec) / 1000000.0);
#endif
}

static void io_error(char * message) {
  char buf[Chunk];

  if((errno == EOVERFLOW) || (errno == EFBIG))
    sprintf(buf, "Bonnie: drastic I/O error (%s): %s", message, strerror(errno));
  else
    sprintf(buf, "Bonnie: drastic I/O error (%s)", message);
  perror(buf);
  exit(1);
}

/*
 * Do a typical-of-something random I/O.  Any serious application that
 *  has a random I/O bottleneck is going to be smart enough to operate
 *  in a page mode, and not stupidly pull individual words out at
 *  odd offsets.
 * The 'where' argument is used as a chunk number
 * To keep the cache from getting too clever, some pages must be updated.
 *  However an application that updated each of many random pages that
 *  it looked at is hard to imagine.
 * However, it would be wrong to put the update percentage in as a
 *  parameter - the effect is too nonlinear.
 */
static void
doseek(off_t where, int fd, int update) {
  int	buf[Chunk / IntSize];
  off_t probe;
  off_t size;

  probe = where * Chunk;
  if (lseek(fd, (off_t)probe, SEEK_SET) != probe)
    io_error("lseek in doseek");
  if ((size = read(fd, (char *) buf, Chunk)) == -1)
    io_error("read in doseek");

  /* every so often, update a block */
  if (update) { /* update this block */

    /* touch a word */
    buf[((int) random() % (size/IntSize - 2)) + 1]--;
    if (lseek(fd, (off_t)probe, SEEK_SET) != probe)
      io_error("lseek in doseek update");
    if (write(fd, (char *) buf, size) == -1)
      io_error("write in doseek");
  } /* update this block */
}

static void dirty_buffer(int * buf) {
  static int todirty = 0;
  buf[todirty++] = (int) random();
  if (todirty == Chunk/IntSize)
    todirty = 0;
}

static void fill_buffer(int * buf) {
  int i;

  srandom(getpid());
  for (i = 0; i < Chunk/IntSize; i++)
    buf[i] = (int) random();
}

#if defined(SysV)
static char randseed[32];

static void srandom(int seed) {
  sprintf(randseed, "%06d", seed);
}

static long random() {
  return nrand48(randseed);
}
#endif

