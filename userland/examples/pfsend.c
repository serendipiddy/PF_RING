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
#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/ethernet.h>     /* the L2 protocols */
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pcap.h>

#include <stdlib.h>
#include <math.h> /* add -lm in Makefile */

#include "pfring.h"
#include "pfutils.c"


struct packet { /* struct for unit of the linked list structure of the packets to send */
  u_int16_t len;
  u_int64_t ticks_from_beginning; /* the time (in ticks) of this packet from the beginning of the list */
  u_char *pkt; /* packet contents */
  struct packet *next;
};

struct ip_header {
#if BYTE_ORDER == LITTLE_ENDIAN
  u_int32_t        ihl:4,                /* header length */
    version:4;                        /* version */
#else
  u_int32_t        version:4,                        /* version */
    ihl:4;                /* header length */
#endif
  u_int8_t        tos;                        /* type of service */
  u_int16_t        tot_len;                        /* total length */
  u_int16_t        id;                        /* identification */
  u_int16_t        frag_off;                        /* fragment offset field */
  u_int8_t        ttl;                        /* time to live */
  u_int8_t        protocol;                        /* protocol */
  u_int16_t        check;                        /* checksum */
  u_int32_t saddr, daddr;        /* source and dest address */
} __attribute__((packed));

struct udp_header {
  u_int16_t        source;                /* source port */
  u_int16_t        dest;                /* destination port */
  u_int16_t        len;                /* udp length */
  u_int16_t        check;                /* udp checksum */
} __attribute__((packed));

struct tcp_header {
  u_int16_t source;
  u_int16_t dest;
  u_int32_t seq;
  u_int32_t ack_seq;
  u_int16_t flags;
  u_int16_t window;
  u_int16_t check;
  u_int16_t urg_ptr;
} __attribute__((packed));

struct packet *pkt_head = NULL; /* head of the linked list of packets which will be populated and sent */
pfring  *pd;
pfring_stat pfringStats;
char *device = NULL;
u_int8_t wait_for_packet = 1, do_shutdown = 0;
u_int64_t num_pkt_good_sent = 0, last_num_pkt_good_sent = 0;
u_int64_t num_bytes_good_sent = 0, last_num_bytes_good_sent = 0;
struct timeval lastTime, startTime;
int reforge_mac = 0, reforge_ip = 0, on_the_fly_reforging = 0;
struct in_addr srcaddr, dstaddr;
char mac_address[6];
int send_len = 60;
int if_index = -1;
int daemon_mode = 0;

#define DEFAULT_DEVICE     "eth0"

/**
 * Pull observation of exponential random variable, given mean.
 * Calculated using inversion of CDF.
 */
double get_exponential_val(double mean) {
    double runif, rexp;

    do { runif = drand48(); } /* or just use rand() if not-found */
    while (runif == 0);

    rexp = -log(runif) * mean; /* X = -ln(u)*(1/lam) = -ln(u)*mean */

    return(rexp);
}

/* *************************************** */

/* Check file descriptor is ready for reading */
int is_fd_ready(int fd) {
  struct timeval timeout = {0};
  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(fd, &fdset);
  return (select(fd+1, &fdset, NULL, NULL, &timeout) == 1);
}

/* Read stdin contents, putting this into buf */
int read_packet_hex(u_char *buf, int buf_len) {
  int i = 0, d, bytes = 0;
  char c;
  char s[3] = {0};

  if (!is_fd_ready(fileno(stdin)))
    return 0;

  while ((d = fgetc(stdin)) != EOF) {
    if (d < 0) break;
    c = (u_char) d;
    if ((c >= '0' && c <= '9') 
     || (c >= 'a' && c <= 'f')
     || (c >= 'A' && c <= 'F')) {
      s[i&0x1] = c;
      if (i&0x1) {
        bytes = (i+1)/2;
        sscanf(s, "%2hhx", &buf[bytes-1]);
        if (bytes == buf_len) break;
      }
      i++;
    }
  }

  return bytes;
}

/* *************************************** */

void print_stats() {
  double deltaMillisec, currentThpt, avgThpt, currentThptBits, currentThptBytes, avgThptBits, avgThptBytes;
  struct timeval now;
  char buf1[64], buf2[64], buf3[64], buf4[64], buf5[64], statsBuf[512], timebuf[128];
  u_int64_t deltaMillisecStart;

  gettimeofday(&now, NULL);
  deltaMillisec = delta_time(&now, &lastTime);
  currentThpt = (double)((num_pkt_good_sent-last_num_pkt_good_sent) * 1000)/deltaMillisec;
  currentThptBytes = (double)((num_bytes_good_sent-last_num_bytes_good_sent) * 1000)/deltaMillisec;
  currentThptBits = currentThptBytes * 8;

  deltaMillisec = delta_time(&now, &startTime);
  avgThpt = (double)(num_pkt_good_sent * 1000)/deltaMillisec;
  avgThptBytes = (double)(num_bytes_good_sent * 1000)/deltaMillisec;
  avgThptBits = avgThptBytes * 8;

  if (!daemon_mode) {
    snprintf(statsBuf, sizeof(statsBuf),
             "TX rate: [current %s pps/%s Gbps][average %s pps/%s Gbps][total %s pkts]",
             pfring_format_numbers(currentThpt, buf1, sizeof(buf1), 1),
             pfring_format_numbers(currentThptBits/(1000*1000*1000), buf2, sizeof(buf2), 1),
             pfring_format_numbers(avgThpt, buf3, sizeof(buf3), 1),
             pfring_format_numbers(avgThptBits/(1000*1000*1000),  buf4, sizeof(buf4), 1),
             pfring_format_numbers(num_pkt_good_sent, buf5, sizeof(buf5), 1));
 
    fprintf(stdout, "%s\n", statsBuf);
  }

  deltaMillisecStart = delta_time(&now, &startTime);
  snprintf(statsBuf, sizeof(statsBuf),
           "Duration:          %s\n"
           "SentPackets:       %lu\n"
           "SentBytes:         %lu\n"
           "CurrentSentPps:    %lu\n"
           "CurrentSentBitps:  %lu\n",
           msec2dhmsm(deltaMillisecStart, timebuf, sizeof(timebuf)),
           (long unsigned int) num_pkt_good_sent,
           (long unsigned int) num_bytes_good_sent,
           (long unsigned int) currentThpt,
           (long unsigned int) currentThptBits);
  pfring_set_application_stats(pd, statsBuf);

  memcpy(&lastTime, &now, sizeof(now));
  last_num_pkt_good_sent = num_pkt_good_sent, last_num_bytes_good_sent = num_bytes_good_sent;
}

/* ******************************** */

void my_sigalarm(int sig) {
  if(do_shutdown) return;
  print_stats();
  alarm(1);
  signal(SIGALRM, my_sigalarm);
}

/* ******************************** */

void sigproc(int sig) {
  if(do_shutdown) return;
  fprintf(stdout, "Leaving...\n");
  do_shutdown = 1;
}

/* *************************************** */

void printHelp(void) {
  printf("pfsend - (C) 2011-15 ntop.org\n");
  printf("Replay synthetic traffic, or a pcap, or a packet in hex format from standard input.\n\n"); 
  printf("pfsend -i out_dev [-a] [-f <.pcap file>] [-g <core_id>] [-h]\n"
         "       [-l <length>] [-n <num>] "
#if !(defined(__arm__) || defined(__mips__))
         "[-r <rate>] [-p <rate>] "
#endif
         "[-m <dst MAC>]\n"
         "       [-w <TX watermark>] [-v]\n\n");
  printf("-a              Active send retry\n");
#if 0
  printf("-b <cpu %%>     CPU percentage priority (0-99)\n");
#endif
  printf("-f <.pcap file> Send packets as read from a pcap file, looping through them\n");
  printf("-g <core_id>    Bind this app to a core\n");
  printf("-h              Print this help\n");
  printf("-i <device>     Device name. Use device\n");
  printf("-l <length>     Packet length to send. Ignored with -f\n");
  printf("-n <num>        Num pkts to send (use 0 for infinite)\n");
#if !(defined(__arm__) || defined(__mips__))
  printf("-r <Gbps rate>  Rate to send Gbit per second (example -r 2.5 sends 2.5 Gbit/sec, -r -1 pcap capture rate)\n");
  printf("-p <pps rate>   Rate to send packets per second (example -p 100 send 100 pps)\n");
#endif
  printf("-m <dst MAC>    Reforge destination MAC (format AA:BB:CC:DD:EE:FF)\n");
  printf("-b <num>        Reforge source IP with <num> different IPs ('balanced traffic')\n");
  printf("-S <ip>         Use <ip> as base source IP -b\n");
  printf("-D <ip>         Use <ip> as destination IP in -b\n");
  printf("-O              On the fly reforging instead of preprocessing (-b)\n");
  printf("-z              Randomize generated IPs sequence\n");
  printf("-o <num>        Offset for generated IPs (-b) or packets in pcap (-f)\n");
  printf("-w <watermark>  TX watermark (low value=low latency) [not effective on ZC]\n");
  printf("-x <if index>   Send to the selected interface, if supported\n");
  printf("-d              Daemon mode\n");
  printf("-P <pid file>   Write pid to the specified file (daemon mode only)\n");
  printf("-e <time>       Enables exponentially distributed inter-packet delay, with mean time (s)\n");
  printf("-E <time>       Sets a linear delta to inter-packet mean time (occurs each second) (requires -e)\n");
  printf("-v              Verbose\n");
  exit(0);
}

/* ******************************************* */

/*
 * Checksum routine for Internet Protocol family headers (C Version)
 *
 * Borrowed from DHCPd
 */

static u_int32_t in_cksum(unsigned char *buf, unsigned nbytes, u_int32_t sum) {
  uint i;

  /* Checksum all the pairs of bytes first... */
  for (i = 0; i < (nbytes & ~1U); i += 2) {
    sum += (u_int16_t) ntohs(*((u_int16_t *)(buf + i)));
    /* Add carry. */
    if(sum > 0xFFFF)
      sum -= 0xFFFF;
  }

  /* If there's a single byte left over, checksum it, too.   Network
     byte order is big-endian, so the remaining byte is the high byte. */
  if(i < nbytes) {
    sum += buf [i] << 8;
    /* Add carry. */
    if(sum > 0xFFFF)
      sum -= 0xFFFF;
  }

  return sum;
}

/* ******************************************* */

static u_int32_t wrapsum (u_int32_t sum) {
  sum = ~sum & 0xFFFF;
  return htons(sum);
}

/* ******************************************* */

/**
 * Forges a UDP packet
 * @param buffer      destination buffer of UDP packet
 * @param buffer_len  size of complete packet (including payload)
 * @param idx         class D section of ip_src address
 */
static void forge_udp_packet(u_char *buffer, u_int buffer_len, u_int idx) {
  struct ip_header *ip_header;
  struct udp_header *udp_header;
  int i;

  /* Reset packet */
  memset(buffer, 0, buffer_len);

  for(i=0; i<12; i++) buffer[i] = i; /* enter dummy Ethernet dst and src */
  buffer[12] = 0x08, buffer[13] = 0x00; /* set ether type to IP (0x0800)*/
  if(reforge_mac) memcpy(buffer, mac_address, 6); /* explicitly set Ethernet src  */

  /* start making an ipv4 header at point (buffer+ether_length) */
  ip_header = (struct ip_header*) &buffer[sizeof(struct ether_header)]; 
  ip_header->ihl = 5;
  ip_header->version = 4;
  ip_header->tos = 0;
  ip_header->tot_len = htons(buffer_len-sizeof(struct ether_header));
  ip_header->id = htons(2012);
  ip_header->ttl = 64;
  ip_header->frag_off = htons(0);
  ip_header->protocol = IPPROTO_UDP;
  ip_header->daddr = dstaddr.s_addr;
  ip_header->saddr = htonl((ntohl(srcaddr.s_addr) + idx) % 0xFFFFFFFF);
  ip_header->check = wrapsum(in_cksum((unsigned char *)ip_header, sizeof(struct ip_header), 0));

  /* start UDP header in buffer */
  udp_header = (struct udp_header*)(buffer + sizeof(struct ether_header) + sizeof(struct ip_header)); 
  udp_header->source = htons(2012);
  udp_header->dest = htons(3000);
  udp_header->len = htons(buffer_len-sizeof(struct ether_header)-sizeof(struct ip_header));
  udp_header->check = 0; /* It must be 0 to compute the checksum */

  /*
    Computing UDP checksum
    http://www.cs.nyu.edu/courses/fall01/G22.2262-001/class11.htm
    http://www.ietf.org/rfc/rfc0761.txt
    http://www.ietf.org/rfc/rfc0768.txt
  */

  i = sizeof(struct ether_header) + sizeof(struct ip_header) + sizeof(struct udp_header); /* index of payload beginning */
  udp_header->check = wrapsum(in_cksum((unsigned char *)udp_header, sizeof(struct udp_header),
                                in_cksum((unsigned char *)&buffer[i], buffer_len-i, /* calculate across payload */
                                in_cksum((unsigned char *)&ip_header->saddr, 2 * sizeof(ip_header->saddr),
                                IPPROTO_UDP + ntohs(udp_header->len)))));
}

/* ******************************************* */

static struct pfring_pkthdr hdr; /* note: this is static to be (re)used by on the fly reforging */

static int reforge_packet(u_char *buffer, u_int buffer_len, u_int idx, u_int use_prev_hdr) {
  struct ip_header *ip_header;

  if (reforge_mac) memcpy(buffer, mac_address, 6);

  if (reforge_ip) {
    if (!use_prev_hdr) {
      memset(&hdr, 0, sizeof(hdr));
      hdr.len = hdr.caplen = buffer_len;

      if (pfring_parse_pkt(buffer, &hdr, 4, 0, 0) < 3)
        return -1;
      if (hdr.extended_hdr.parsed_pkt.ip_version != 4)
        return -1;
    }

    ip_header = (struct ip_header *) &buffer[hdr.extended_hdr.parsed_pkt.offset.l3_offset];
    ip_header->daddr = dstaddr.s_addr;
    ip_header->saddr = htonl((ntohl(srcaddr.s_addr) + idx) % 0xFFFFFFFF);
    ip_header->check = 0;
    ip_header->check = wrapsum(in_cksum((unsigned char *) ip_header, sizeof(struct ip_header), 0));

    if (hdr.extended_hdr.parsed_pkt.l3_proto == IPPROTO_UDP) {
      struct udp_header *udp_header = (struct udp_header *) &buffer[hdr.extended_hdr.parsed_pkt.offset.l4_offset];
      udp_header->check = 0;
      udp_header->check = wrapsum(in_cksum((unsigned char *) udp_header, sizeof(struct udp_header),
                                    in_cksum((unsigned char *) &buffer[hdr.extended_hdr.parsed_pkt.offset.payload_offset], 
                                      buffer_len - hdr.extended_hdr.parsed_pkt.offset.payload_offset,
                                      in_cksum((unsigned char *) &ip_header->saddr, 2 * sizeof(ip_header->saddr),
                                        IPPROTO_UDP + ntohs(udp_header->len)))));
    } else if (hdr.extended_hdr.parsed_pkt.l3_proto == IPPROTO_TCP) {
      struct tcp_header *tcp_header = (struct tcp_header *) &buffer[hdr.extended_hdr.parsed_pkt.offset.l4_offset];
      int tcp_hdr_len = hdr.extended_hdr.parsed_pkt.offset.payload_offset - hdr.extended_hdr.parsed_pkt.offset.l4_offset;
      int payload_len = buffer_len - hdr.extended_hdr.parsed_pkt.offset.payload_offset;
      tcp_header->check = 0;
      tcp_header->check = wrapsum(in_cksum((unsigned char *) tcp_header, tcp_hdr_len,
                                   in_cksum((unsigned char *) &buffer[hdr.extended_hdr.parsed_pkt.offset.payload_offset],
                                     payload_len,
                                     in_cksum((unsigned char *) &ip_header->saddr, 2 * sizeof(ip_header->saddr),
                                       IPPROTO_TCP + ntohs(htons(tcp_hdr_len + payload_len))))));
    }
  }

  return 0;
}

/* *************************************** */

int main(int argc, char* argv[]) {
  char *pcap_in = NULL, path[255] = { 0 };
  int c, i, j, n, verbose = 0, active_poll = 0;
  u_int mac_a, mac_b, mac_c, mac_d, mac_e, mac_f;
  u_char buffer[9000];
  u_int32_t num_to_send = 0;
  int bind_core = -1;
  u_int16_t cpu_percentage = 0;
  double pps = 0;
#if !(defined(__arm__) || defined(__mips__))
  double gbit_s = 0;
  double td; /* inter-packet tick-delta */
  ticks tick_start = 0, tick_delta = 0;
#endif
  ticks hz = 0;
  struct packet *tosend;
  int num_balanced_pkts = 1, pkts_offset = 0, watermark = 0;
  u_int num_pcap_pkts = 0;
  int send_full_pcap_once = 1;
  char *pidFileName = NULL;
  int send_error_once = 1;
  int randomize = 0;
  int reforging_idx;
  int stdin_packet_len = 0;
  
  /* Exponentially distributed inter-packet delay stuff */
  u_int64_t ticks_to_wait = 0; 
  double mean_packet_delay = -1;
  double mean_packet_delay_delta = -1;
  double mean_packet_delay_live = -1;
  double exp_delay = 0;
  /* double actual_delay = 0; */
  

  /* Seed all the RNGs used */
  srandom(time(NULL));
  // srand(); 

  /* Set the src and dst addresses for the packets if generated by this (if no pcap given) */
  srcaddr.s_addr = 0x0000000A /* 10.0.0.0 */;
  dstaddr.s_addr = 0x0100A8C0 /* 192.168.0.1 */;

  while((c = getopt(argc, argv, "b:dD:hi:n:g:l:o:Oaf:r:vm:p:P:S:w:x:ze:E:")) != -1) {
    switch(c) {
    case 'b':
      num_balanced_pkts = atoi(optarg);
      if(num_balanced_pkts > 1000000) {
        printf("WARNING: The -b value is pretty big and this can exhaust the available memory.\n");
        printf("WARNING: To avoid that consider using the -O option.\n");
      }
      reforge_ip = 1;
      break;
    case 'D':
      inet_aton(optarg, &dstaddr);
      break;
    case 'h':
      printHelp();
      break;
    case 'i':
      device = strdup(optarg);
      break;
    case 'f':
      pcap_in = strdup(optarg);
      break;
    case 'n':
      num_to_send = atoi(optarg);
      send_full_pcap_once = 0;
      break;
    case 'o':
      pkts_offset = atoi(optarg);
      break;
    case 'O':
      on_the_fly_reforging = 1;
      break;
    case 'g':
      bind_core = atoi(optarg);
      break;
    case 'l':
      send_len = atoi(optarg);
      break;
    case 'x':
      if_index = atoi(optarg);
      break;
    case 'v':
      verbose = 1;
      break;
    case 'a':
      active_poll = 1;
      break;
#if !(defined(__arm__) || defined(__mips__))
    case 'r':
      sscanf(optarg, "%lf", &gbit_s);
      break;
    case 'p':
      sscanf(optarg, "%lf", &pps);
      break;
#endif
    case 'm':
      if(sscanf(optarg, "%02X:%02X:%02X:%02X:%02X:%02X", &mac_a, &mac_b, &mac_c, &mac_d, &mac_e, &mac_f) != 6) {
        printf("Invalid MAC address format (XX:XX:XX:XX:XX:XX)\n");
        return(0);
      } else {
        reforge_mac = 1;
        mac_address[0] = mac_a, mac_address[1] = mac_b, mac_address[2] = mac_c;
        mac_address[3] = mac_d, mac_address[4] = mac_e, mac_address[5] = mac_f;
      }
      break;
    case 'S':
      inet_aton(optarg, &srcaddr);
      break;
    case 'w':
      watermark = atoi(optarg);
      if(watermark < 1) watermark = 1;
      break;
    case 'd':
      daemon_mode = 1;
      break;
    case 'P':
      pidFileName = strdup(optarg);
      break;
    case 'z':
      randomize = 1;
      break;
    case 'e':
      sscanf(optarg, "%lf", &mean_packet_delay);
      sscanf(optarg, "%lf", &mean_packet_delay_live);
      pps = (double) 1/mean_packet_delay;
      break;
    case 'E':
      sscanf(optarg, "%lf", &mean_packet_delay_delta);
      break;
    default:
      printHelp();
    }
  }

  if((device == NULL) || (num_balanced_pkts < 1)
     || (optind < argc) /* Extra argument */)
    printHelp();

  if (num_balanced_pkts > 1000000 && !on_the_fly_reforging)
    printf("Warning: please use -O to reduce memory preallocation when many IPs are configured with -b\n");

  bind2node(bind_core); /* If using ZC only, binds using NUMA? */

  if (daemon_mode)
    daemonize(pidFileName);

  if (pidFileName)
    create_pid_file(pidFileName);

  printf("Sending packets on %s\n", device);

  pd = pfring_open(device, 1500, 0 /* PF_RING_PROMISC */);
  if(pd == NULL) { /* Print PR_RING version information (Or exit if pd == null) */
    printf("pfring_open error [%s] (pf_ring not loaded or interface %s is down ?)\n", 
           strerror(errno), device);
    return(-1);
  } else { 
    u_int32_t version;

    pfring_set_application_name(pd, "pfsend");
    pfring_version(pd, &version);

    printf("Using PF_RING v.%d.%d.%d\n", (version & 0xFFFF0000) >> 16,
           (version & 0x0000FF00) >> 8, version & 0x000000FF);
  }

  if (!pd->send && pd->send_ifindex && if_index == -1) {
    printf("Please use -x <if index>\n");
    return -1;
  }

  if(watermark > 0) {
    int rc;

    if((rc = pfring_set_tx_watermark(pd, watermark)) < 0)
      printf("pfring_set_tx_watermark() failed [rc=%d]\n", rc);
  }

  signal(SIGINT, sigproc);
  signal(SIGTERM, sigproc);
  signal(SIGINT, sigproc);

  /* make sure the packet is at least long enough to hold a packet */
  if(send_len < 60)
    send_len = 60;

  /* Estimate the CPU clock frequency, setting hz */
#if !(defined(__arm__) || defined(__mips__))
  if(gbit_s != 0 || pps != 0) {
    /* computing usleep delay */
    tick_start = getticks();
    usleep(1);
    tick_delta = getticks() - tick_start;

    /* computing CPU freq */
    tick_start = getticks();
    usleep(1001);
    hz = (getticks() - tick_start - tick_delta) * 1000 /*kHz -> Hz*/;
    printf("Estimated CPU freq: %lu Hz\n", (long unsigned int)hz);
  }
#endif

  /* Populate the linked list of packets to be sent */
  if(pcap_in) { /* if pcap file is defined, use pcap library to open this file */
    char ebuf[256];
    u_char *pkt;
    struct pcap_pkthdr *h;
    pcap_t *pt = pcap_open_offline(pcap_in, ebuf);
    struct timeval beginning = { 0, 0 };
    u_int64_t avg_send_len = 0;
    u_int32_t num_orig_pcap_pkts = 0;

    on_the_fly_reforging = 0;

    if(pt) { /* if pcap file was opened correctly */
      struct packet *last = NULL; /* Initialise the packet linked list tail */
      int datalink = pcap_datalink(pt);

      if (datalink == DLT_LINUX_SLL)
        printf("Linux 'cooked' packets detected, stripping 2 bytes from header..\n");

      while (1) {
        struct packet *p;
        int rc = pcap_next_ex(pt, &h, (const u_char **) &pkt); /* get next pcap packet, place into memory at pkt */

        if(rc <= 0) break;
        
        num_orig_pcap_pkts++;
        if ((num_orig_pcap_pkts-1) < pkts_offset) continue;

        if (num_pcap_pkts == 0) { /* if the first packet, record the current time relative to the beginning */
          beginning.tv_sec = h->ts.tv_sec;
          beginning.tv_usec = h->ts.tv_usec;
        }

        p = (struct packet *) malloc(sizeof(struct packet));
        if(p) { /*  */
          p->len = h->caplen;
          if (datalink == DLT_LINUX_SLL) p->len -= 2;
          p->ticks_from_beginning = (((h->ts.tv_sec - beginning.tv_sec) * 1000000) + (h->ts.tv_usec - beginning.tv_usec)) * hz / 1000000;
          p->next = NULL;
          p->pkt = (u_char *)malloc(p->len);

          if(p->pkt == NULL) {
            printf("Not enough memory\n");
            break;
          } else {
            if (datalink == DLT_LINUX_SLL) {
              memcpy(p->pkt, pkt, 12);
              memcpy(&p->pkt[12], &pkt[14], p->len - 14);
            } else {
              memcpy(p->pkt, pkt, p->len);
            }
            if(reforge_mac || reforge_ip)
              reforge_packet((u_char *) p->pkt, p->len, pkts_offset + num_pcap_pkts, 0); 
          }

          if(last) { /* append to the end of the packet linked list */
            last->next = p;
            last = p;
          } else /* this is the first packet in the linked list so set the head and tail to be p */
            pkt_head = p, last = p;
        } else {
          printf("Not enough memory\n");
          break;
        }

        if(verbose)
          printf("Read %d bytes packet from pcap file %s [%lu.%lu Secs =  %lu ticks@%luhz from beginning]\n",
             p->len, pcap_in, h->ts.tv_sec - beginning.tv_sec, h->ts.tv_usec - beginning.tv_usec,
             (long unsigned int)p->ticks_from_beginning,
             (long unsigned int)hz);

        avg_send_len += p->len;
        num_pcap_pkts++;
      } /* while */

      if (num_pcap_pkts == 0) {
        printf("Pcap file %s is empty\n", pcap_in);
        pfring_close(pd);
        return(-1);
      }

      /* at this point avg_send_len is the sum of the packet lengths, dividing below makes it's name valid */
      avg_send_len /= num_pcap_pkts; 

      pcap_close(pt);
      printf("Read %d packets from pcap file %s\n",
            num_pcap_pkts, pcap_in);
      last->next = pkt_head; /* Loop linked list back on itself */
      send_len = avg_send_len;

      if (send_full_pcap_once)
        num_to_send = num_pcap_pkts;
    } else {
      printf("Unable to open file %s\n", pcap_in);
      pfring_close(pd);
      return(-1);
    } /* end of pcap file processing */
  } 
  else { /* this isn't using a pcap file, so need to generate the packets */
    struct packet *p = NULL, *last = NULL; /* initialise the packet linked list structure */

    /* if user defined the packet contents from stdin, override the packet length to be that length */
    if ((stdin_packet_len = read_packet_hex(buffer, sizeof(buffer))) > 0) {
      send_len = stdin_packet_len;
    }

    for (i = 0; i < num_balanced_pkts; i++) {

      if (stdin_packet_len <= 0) { /* create the packet data */
        forge_udp_packet(buffer, send_len, pkts_offset + i); /* src IP address will increase by 1 each forged packet */
      } else {
        if (reforge_packet(buffer, send_len, pkts_offset + i, 0) != 0) { 
          fprintf(stderr, "Unable to reforge the provided packet\n");
          return -1;
        }
      }

      p = (struct packet *) malloc(sizeof(struct packet));
      if (p == NULL) { 
        fprintf(stderr, "Unable to allocate memory requested (%s)\n", strerror(errno));
        return (-1);
      }

      if (i == 0) pkt_head = p; /* set head of packet linked list */

      p->len = send_len;
      p->ticks_from_beginning = 0;
      p->next = pkt_head;
      p->pkt = (u_char *) malloc(p->len);

      if (p->pkt == NULL) {
        fprintf(stderr, "Unable to allocate memory requested (%s)\n", strerror(errno));
        return (-1);
      }

      /* copy the generated packet into the linked list to send */
      memcpy(p->pkt, buffer, send_len);

      /* move to the next position in the linked list */
      if (last != NULL) last->next = p;
      last = p;

      if (on_the_fly_reforging) {
#if 0
        if (stdin_packet_len <= 0) { /* forge_udp_packet, parsing packet for on the fly reforing */
          memset(&hdr, 0, sizeof(hdr));
          hdr.len = hdr.caplen = p->len;
          if (pfring_parse_pkt(p->pkt, &hdr,  /* level */ 4, /* don't add timestamp */ 0, /* don't add hash */ 0) < 3) { /* returns the level parsed up until */
            fprintf(stderr, "Unable to reforge the packet (unexpected)\n");
            return -1; 
          }
        }
#endif
        break;
      }
    }
  }

  /* Set the packet sent rate (tick_delta) */
#if !(defined(__arm__) || defined(__mips__))
  if(gbit_s > 0) {
    /* computing max rate (convert Gbps to pps) */
    double byte_s = (gbit_s * 1000000000) / 8; /* convert Gbit/s to byte/s */
    pps = ( byte_s ) / (8 /*Preamble*/ + send_len /* the packet */ + 4 /*CRC*/ + 12 /*InterFrameGap*/);
  } else if (gbit_s < 0) {
    /* capture rate */
    pps = -1;
  } /* else use pps */

  if (pps > 0) {
    td = (double) (hz / pps); /* set inter-packet tick delta */
    tick_delta = (ticks)td;

    if (gbit_s > 0)
      printf("Rate set to %.2f Gbit/s, %d-byte packets, %.2f pps\n", gbit_s, (send_len + 4 /*CRC*/), pps);
    else
      printf("Rate set to %.2f pps\n", pps);
  }
  
  if (mean_packet_delay >= 0) {
      printf("Rate set to %.4f Gbit/s, %d-byte packets, %.2f pps\n", ((send_len+4)*pps/1000000000), (send_len + 4), pps);
  }
#endif

  /* Ready PF_RING for sending */
  if(bind_core >= 0)
    bind2core(bind_core);

  if(wait_for_packet && (cpu_percentage > 0)) {
    if(cpu_percentage > 99) cpu_percentage = 99;
    pfring_config(cpu_percentage);
  }

  if(!verbose) {
    signal(SIGALRM, my_sigalarm);
    alarm(1);
  }

  gettimeofday(&startTime, NULL);
  memcpy(&lastTime, &startTime, sizeof(startTime));

  pfring_set_socket_mode(pd, send_only_mode);

  if(pfring_enable_ring(pd) != 0) {
    printf("Unable to enable ring :-(\n");
    pfring_close(pd);
    return(-1);
  }

  tosend = pkt_head;
  i = 0;
  reforging_idx = pkts_offset;

  pfring_set_application_stats(pd, "Statistics not yet computed: please try again...");
  if(pfring_get_appl_