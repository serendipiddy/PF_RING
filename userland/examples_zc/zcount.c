/*
 * (C) 2003-15 - ntop 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#include "pfring.h"
#include "pfring_zc.h"

#include "zutils.c"

#define ALARM_SLEEP             1
#define MAX_CARD_SLOTS      32768
#define MIN_BUFFER_LEN       1536
#define CACHE_LINE_LEN         64

#define NBUFF      256 /* pow */
#define NBUFFMASK 0xFF /* 256-1 */

//#define USE_BURST_API
#define BURST_LEN   32

pfring_zc_cluster *zc;
pfring_zc_queue *zq;
pfring_zc_pkt_buff *buffers[NBUFF];
u_int32_t lru = 0;

struct timeval startTime;
unsigned long long numPkts = 0, numBytes = 0;
int bind_core = -1;
int bind_time_pulse_core = -1;
int buffer_len;
u_int8_t wait_for_packet = 1, do_shutdown = 0, verbose = 0, add_filtering_rule = 0;
u_int8_t high_stats_refresh = 0, time_pulse = 0;

u_int64_t prev_ns = 0;
u_int64_t threshold_min = 1500, threshold_max = 2500; /* TODO parameters */
u_int64_t threshold_min_count = 0, threshold_max_count = 0;

volatile u_int64_t *pulse_timestamp_ns;
volatile u_int64_t *pulse_timestamp_ns_n;

/* lock buffer */
#include "lock_buffer.c"
u_char use_lock_buffer = 0; 
struct lock_buffer * lb_buffer;
int pps = -1;

static inline void get_packet_timestamp(struct id_time * it) {
    u_int64_t ts = *pulse_timestamp_ns_n;
    it->sec  = ts >> 32; 
    it->nsec = ts & 0xffffffff;
}

/* ******************************** */

void *time_pulse_thread(void *data) {
  struct timespec tn;
  
  bind2core(bind_time_pulse_core);

  while (likely(!do_shutdown)) {
    /* clock_gettime takes up to 30 nsec to get the time */
    clock_gettime(CLOCK_REALTIME, &tn);
    *pulse_timestamp_ns_n = ((u_int64_t) ((u_int64_t) htonl(tn.tv_sec) << 32) | htonl(tn.tv_nsec));
    *pulse_timestamp_ns = ((u_int64_t) ((u_int64_t) tn.tv_sec * 1000000000) + tn.tv_nsec);
  }

  return NULL;
}

/* ******************************** */

void print_stats() {
  struct timeval endTime;
  double deltaMillisec;
  static u_int8_t print_all;
  static u_int64_t lastPkts = 0;
  static u_int64_t lastDrops = 0;
  static u_int64_t lastBytes = 0;
  double pktsDiff, dropsDiff, bytesDiff;
  static struct timeval lastTime;
  char buf1[64], buf2[64], buf3[64];
  unsigned long long nBytes = 0, nPkts = 0, nDrops = 0;
  pfring_zc_stat stats;

  if(startTime.tv_sec == 0) {
    gettimeofday(&startTime, NULL);
    print_all = 0;
  } else
    print_all = 1;

  gettimeofday(&endTime, NULL);
  deltaMillisec = delta_time(&endTime, &startTime);

  nBytes = numBytes;
  nPkts = numPkts;
  if (pfring_zc_stats(zq, &stats) == 0)
    nDrops = stats.drop;

  fprintf(stderr, "=========================\n"
	  "Absolute Stats: %s pkts (%s drops) - %s bytes\n", 
	  pfring_format_numbers((double)nPkts, buf1, sizeof(buf1), 0),
	  pfring_format_numbers((double)nDrops, buf3, sizeof(buf3), 0),
	  pfring