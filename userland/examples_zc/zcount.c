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
#include <byteswap.h>
u_char use_lock_buffer = 0; 
struct lock_buffer * lb_buffer;
int pps = -1;
int use_hardware = 0;

#include "openflow.h"
#include <net/ethernet.h>

// struct ofp_header {
    // u_int8_t version;
    // u_int8_t type;
    // u_int16_t length;
    // u_int32_t xid;
// };

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
	  pfring_format_numbers((double)nBytes, buf2, sizeof(buf2), 0));

  if(print_all && (lastTime.tv_sec > 0)) {
    char buf[256];

    deltaMillisec = delta_time(&endTime, &lastTime);
    pktsDiff = nPkts-lastPkts;
    dropsDiff = nDrops-lastDrops;
    bytesDiff = nBytes - lastBytes;
    bytesDiff /= (1000*1000*1000)/8;

    if (time_pulse)
      fprintf(stderr, "Thresholds: %ju pkts <%.3fusec %ju pkts >%.3fusec\n", 
        threshold_min_count, (double) threshold_min/1000, 
        threshold_max_count, (double) threshold_max/1000);

    snprintf(buf, sizeof(buf),
	     "Actual Stats: %s pps (%s drops) - %s Gbps",
	     pfring_format_numbers(((double)pktsDiff/(double)(deltaMillisec/1000)),  buf1, sizeof(buf1), 1),
	     pfring_format_numbers(((double)dropsDiff/(double)(deltaMillisec/1000)),  buf2, sizeof(buf2), 1),
	     pfring_format_numbers(((double)bytesDiff/(double)(deltaMillisec/1000)),  buf3, sizeof(buf3), 1));
    fprintf(stderr, "%s\n", buf);
  }
    
  fprintf(stderr, "=========================\n\n");

  lastPkts = nPkts, lastDrops = nDrops, lastBytes = nBytes;
  lastTime.tv_sec = endTime.tv_sec, lastTime.tv_usec = endTime.tv_usec;
}

/* ******************************** */

void sigproc(int sig) {
  static int called = 0;
  fprintf(stderr, "Leaving...\n");
  if(called) return; else called = 1;

  do_shutdown = 1;
  lock_buffer_finish(lb_buffer);

  print_stats();
  
  pfring_zc_queue_breakloop(zq);
}

/* *************************************** */

void printHelp(void) {
  printf("zcount - (C) 2014 ntop.org\n");
  printf("Using PFRING_ZC v.%s\n", pfring_zc_version());
  printf("A simple packet counter application.\n\n");
  printf("Usage:   zcount -i <device> -c <cluster id>\n"
	 "                [-h] [-g <core id>] [-R] [-H] [-S <core id>] [-v] [-a]\n\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name\n");
  printf("-c <cluster id> Cluster id\n");
  printf("-g <core id>    Bind this app to a core\n");
  printf("-a              Active packet wait\n");
  printf("-R              Test hw filters adding a rule (Intel 82599)\n");
  printf("-H              High stats refresh rate (workaround for drop counter on 1G Intel cards)\n");
  printf("-S <core id>    Pulse-time thread for inter-packet time check\n");
  printf("-s              Use hardware timestamps\n");
  printf("-C              Check license\n");
  printf("-v              Verbose\n");
  printf("-X <filename>   Log file name for timestamps of packets captured\n");
}

/* *************************************** */

void print_packet(pfring_zc_pkt_buff *buffer) {
  u_char *pkt_data = pfring_zc_pkt_buff_data(buffer, zq);
  char bigbuf[4096];

  if (buffer->ts.tv_nsec)
    printf("[%u.%u] [hash=%08X] ", buffer->ts.tv_sec, buffer->ts.tv_nsec, buffer->hash);

#if 1
  pfring_print_pkt(bigbuf, sizeof(bigbuf), pkt_data, buffer->len, buffer->len);
  fputs(bigbuf, stdout);
#else
  int i;
  for(i = 0; i < buffer->len; i++)
    printf("%02X ", pkt_data[i]);
  printf("\n");
#endif
}

/* *************************************** */
struct ether_header* ofpEth;
struct ofp_packet_in* ofpPin;
struct ofp_packet_out* ofpPout;
struct ofp_match* ofpMatch;
/* read the openflow header and extract 6 bytes of identifiable info from it */
void process_ofp(struct ofp_header * ofp, char * output) {
    // printf("OFP: type(%u) xid(%u) - ", ofp->type, ofp->xid);
    
    switch (ofp->type) {
        
        case OFPT_PACKET_IN:
            ofpPin = (struct ofp_packet_in*) ofp;
            ofpEth = (struct ether_header*) (((char*)ofpPin) + (ntohs(ofp->length) - ntohs(ofpPin->total_len)));
            memcpy(output, &ofpEth->ether_dhost, 6);
            
            // printf("PKT_IN: Encapsulated MAC DST: %02X:%02X:%02X:%02X:%02X:%02X"); // SRC: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                // eth->ether_dhost[0], eth->ether_dhost[1], eth->ether_dh