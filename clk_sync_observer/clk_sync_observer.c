/**
 * capture NTP packets and extract contained timestamps
 *
 * Authors:
 *   first code by Jan Pacner <xpacne00@stud.fit.vutbr.cz>
 *   idea by Marek Peca <hefaistos@gmail.com>
 *   supported by the wisdom of Pavel Pisa <pisa@cmp.felk.cvut.cz>
 *
 * Date: 2013-05-23 14:48:07 CEST
 * License:
 *   "THE BEER-WARE LICENSE" (Revision 42):
 *   Jan Pacner wrote this file. As long as you retain this notice you
 *   can do whatever you want with this stuff. If we meet some day and
 *   you think this stuff is worth it, you can buy me a beer in return.
 */

// useful links
//   fxr.watson.org
//   http://dpdk.org/ (extremely fast packet processing on x86)
// not implemented
//   support for any L3+ tunneling (including IPSec etc.)
//   support for fragmented packets (neither IPv4 nor IPv6)
//   NTP control digests checking
//   NTP message authentication code support
//   NTP port choice
//   NTP leap detection

//#include <stdint.h>  // HACK for pcap missing u_* types (needed under Linux)
//#define __USE_BSD    // ...see the preceeding line
#include <inttypes.h>
#include <pcap/pcap.h>

#include <stdint.h>  // u_* types
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>  // open()
#include <fcntl.h>  // open()
#include <errno.h>
#include <netinet/in.h>  // in_addr in6_addr
#include <arpa/inet.h>  // inet_ntop()
#include <signal.h>  // sigaction()
#include <getopt.h>
#include <string.h>  // strerror()
#include <stdbool.h>

#include "clxync_obsv.h"

#define RING_BUF_SIZE 8192  /* for 1 packet */
#define READ_TIMEOUT 300  /* ms */

#define IP_VERSION_4 4  /* content of the version field in IP header */
#define IP_VERSION_6 6  /* - || - */

/* ethernet frame */
typedef struct {
  /* preamble and frame delimiter are not part of pcap frame */
  uint8_t mac_addr_dst[6];
  uint8_t mac_addr_src[6];
  /* 802.1Q tag is removed by libpcap */
  uint16_t len_or_ethertype;  /* <1500 payload len
                                 >=1536 EtherType values
                                 rest is undefined */
  /* checksum is removed by libpcap */
} __packed eth_hdr_t;

/* IPv4 header (according to RFC 791), partially adopted from tutorial
   http://www.tcpdump.org/pcap.html and
   http://systhread.net/texts/200805lpcap1.php) */
typedef struct {
  uint8_t        ver_hdrlen;      /* 4b version; 4b header length (in multiples of 4B) */
#define IPv4_version(x) ((x) >> 4)  /* should be IPPROTO_IP */
#define IPv4_hdrlen(x) (((x) & 0x0f) * 4)
  uint8_t        dscp;            /* differentiated services code point */
  uint16_t       totallen;        /* len of fragment (header + data) in bytes */
  uint16_t       id;              /* identification */
  uint16_t       flags_foff;      /* flags & fragment offset field */
#define IPv4_DF       0x4000  /* dont fragment flag */
#define IPv4_FOF_MASK 0x1fff  /* mask for fragmenting bits */
  uint8_t        ttl;
  uint8_t        proto;           /* protocol
                                     IPPROTO_IP (could be more than once,
				     but we do not support IP in IP)
                                     IPPROTO_TCP
                                     IPPROTO_UDP */
  uint16_t       checksum;
  struct in_addr src;
  struct in_addr dst;
} __packed ipv4_hdr_t;

/* IPv6 header (according to RFC 2460) */
typedef struct {
  uint32_t ver_class_label;  /* 4b version; 8b traffic class; 20b flow label */
#define IPv6_version(x) ((x) >> (8 + 20))  /* should be IPPROTO_IPV6 */
  uint16_t payloadlen;  /* len of the data after current header in bytes */
  uint8_t nexthdr;  /* same as IPv4 protocol field
                       netinet/in.h:
		       IPPROTO_NONE no next header
		       IPPROTO_IPV6 ipv6 header (can be more than once)
		       IPPROTO_FRAGMENT */
  uint8_t hoplimit;
  struct in6_addr src;
  struct in6_addr dst;
} __packed ipv6_hdr_t;

/* UDP header (according to RFC 768) */
typedef struct {
  uint16_t src;  /* port */
  uint16_t dst;  /* port */
  uint16_t len;  /* len of (header + data) in bytes */
  uint16_t checksum;
} __packed udp_hdr_t;

/* ??
typedef struct {
  uint32_t key_id;
  uint8_t dgst[128];
} __packed ntp_ftr_t;
*/

struct global_vars_s {
  pcap_t *pcap_handle;
} global_vars;

struct args_s {
  char *d;  // eth device
  FILE *o;  // output
  char *tstamp_type; // prefered timestamp type
  int promisc;
  char buf[1024];// = {0};
  int buf_end;// = 0;
  struct timeval sysclk;
} args = {
  .tstamp_type = NULL,
  .promisc = 0,
  .buf_end = 0,
};

char *ipaddr2str(char *s, const uint8_t *addr, const bool is_ipv6) {
  static char buf[INET6_ADDRSTRLEN+1];
  if (s == NULL)
    s = buf;
  inet_ntop(is_ipv6 ? AF_INET6 : AF_INET, (void*)addr, s, INET6_ADDRSTRLEN);
  return s;
}

int ntp2txt(char *s, ntp_pkt_t *ntp) {
  uint8_t li, vn, mode;
  li = ntp->li_vn_mode >> 6;
  vn = (ntp->li_vn_mode >> 3) & 0x7;
  mode = (ntp->li_vn_mode >> 0) & 0x7;

  return
    sprintf(s,
	    " li=%u vn=%u mode=%u stratum=%u poll=%u precision=%u\n"
	    " root_delay=%u root_disp=%u ref_id=0x%08x\n"
	    " ref_tstamp=%016llX org_tstamp=%016llX\n"
	    " rec_tstamp=%016llX xmt_tstamp=%016llX\n",
	    li, vn, mode,
	    ntp->stratum, ntp->poll, ntp->precision,
	    ntp->root_delay, ntp->root_disp, ntp->ref_id,
	    ntp->ref_tstamp, ntp->org_tstamp, ntp->rec_tstamp, ntp->xmt_tstamp);
}

void output_ntp_packet(FILE *out,
		       bool ipv6,
		       const uint8_t *src_ip, uint16_t src_port,
		       const uint8_t *dst_ip, uint16_t dst_port,
		       const struct timeval *tstamp, /* actually is in nsec */
		       ntp_pkt_t *ntp) {
  static char a1[INET6_ADDRSTRLEN+1], a2[INET6_ADDRSTRLEN+1];
  static char s[1024];
  ntp2txt(s, ntp);
  fprintf(out,
	  "t=%lu.%09lu\tsrc=%s[%d] dst=%s[%d]\n%s",
	  tstamp->tv_sec, tstamp->tv_usec,
	  ipaddr2str(a1, src_ip, ipv6), src_port,
	  ipaddr2str(a2, dst_ip, ipv6), dst_port,
	  s);
}

static inline uint8_t get_u8(uint8_t **p) {
  uint8_t u = **p;
  ++*p;
  return u;
}

static inline uint32_t get_u32be(uint8_t **p) {
  uint32_t u = get_u8(p);
  u <<= 8;  u |= get_u8(p);
  u <<= 8;  u |= get_u8(p);
  u <<= 8;  u |= get_u8(p);
  return u;
}

static inline uint64_t get_u64be(uint8_t **p) {
  uint64_t u = get_u32be(p);
  u <<= 32;  u |= get_u32be(p); 
  return u;
}

void parse_ntp_packet(ntp_pkt_t *ntp, uint8_t *pkt) {
  ntp->li_vn_mode = get_u8(&pkt);
  ntp->stratum    = get_u8(&pkt);
  ntp->poll       = get_u8(&pkt);
  ntp->precision  = get_u8(&pkt);
  ntp->root_delay = get_u32be(&pkt);
  ntp->root_disp  = get_u32be(&pkt);
  ntp->ref_id     = get_u32be(&pkt);
  ntp->ref_tstamp = get_u64be(&pkt);
  ntp->org_tstamp = get_u64be(&pkt);
  ntp->rec_tstamp = get_u64be(&pkt);
  ntp->xmt_tstamp = get_u64be(&pkt);
}

#define CHECK_PACKET_LEN						\
  do { if (packet > _packet + header->caplen) return; } while (0)

/** remove packet headers (assume only IP) */
void handle_packet(uint8_t *_args, const struct pcap_pkthdr *header,
		   const uint8_t *_packet) {
  struct args_s *args = (struct args_s *)_args;
  uint8_t *packet = (uint8_t *)_packet;
  uint8_t *tmp;

  /* pcap timestamp: header->ts */

  /* jump over ethernet header */
  packet += sizeof(eth_hdr_t);
  CHECK_PACKET_LEN;

  uint8_t
    *src = NULL,  /* in_addr or in6_addr */
    *dst = NULL;  /* in_addr or in6_addr */
  bool ipv6_found = false;

  /* jump over IP header(s) */
  switch (IPv4_version(((ipv4_hdr_t *)packet)->ver_hdrlen)) {
  case IP_VERSION_4:
    /* do not support fragmented packets (but if fragmented, take the
       first fragment and assume, the message is not damaged) */
    if (! (IPv4_DF || (! (IPv4_FOF_MASK &
			  ntohs(((ipv4_hdr_t *)packet)->flags_foff)) )) )
      return;

    /* NTP works only using UDP */
    if (((ipv4_hdr_t *)packet)->proto != IPPROTO_UDP) return;

    tmp = packet;
    packet += IPv4_hdrlen(((ipv4_hdr_t *)packet)->ver_hdrlen);
    CHECK_PACKET_LEN;
    src = (uint8_t *)&((ipv4_hdr_t *)tmp)->src;
    dst = (uint8_t *)&((ipv4_hdr_t *)tmp)->dst;
    break;
  case IP_VERSION_6:
    /* jump over all chained IPv6 headers */
    while (((ipv6_hdr_t *)packet)->nexthdr == IPPROTO_IPV6) {
      packet += sizeof(ipv6_hdr_t);
      CHECK_PACKET_LEN;
    }

    if (((ipv6_hdr_t *)packet)->nexthdr != IPPROTO_UDP) return;

    tmp = packet;
    packet += sizeof(ipv6_hdr_t);
    CHECK_PACKET_LEN;
    src = (uint8_t *)&((ipv6_hdr_t *)tmp)->src;
    dst = (uint8_t *)&((ipv6_hdr_t *)tmp)->dst;
    ipv6_found = true;
    break;
  default:
    return;
  }

  tmp = packet;
  udp_hdr_t *udp_hdr = (udp_hdr_t*)tmp;
  packet += sizeof(udp_hdr_t);  /* jump over UDP header */
  CHECK_PACKET_LEN;

  if (header->caplen - (packet - _packet) != sizeof(ntp_pkt_pkd_t))
    /*FIXME: add broken/unknown packets error reporting */
    return;
  ntp_pkt_t ntp;
  parse_ntp_packet(&ntp, packet);
  uint16_t src_port = htons(udp_hdr->src), dst_port = htons(udp_hdr->dst);
  output_ntp_packet(args->o,
		    ipv6_found, src, src_port, dst, dst_port,
		    &header->ts, &ntp);
#ifdef SQL
  sql_output_ntp_packet(ipv6_found, src, src_port, dst, dst_port,
			&header->ts, &ntp);
#endif
}

void print_pcap_warning(FILE *f, int rc) {
  switch (rc) {
  case PCAP_WARNING_PROMISC_NOTSUP:
    fprintf(f, "WARNING: promiscuous mode not supported\n");
    break;
  case PCAP_WARNING_TSTAMP_TYPE_NOTSUP:
    fprintf(f, "WARNING: timestamp type not supported\n");
    break;
  case PCAP_WARNING:
    fprintf(f, "WARNING: generic(?) pcap_activate() warning\n");
    break;
  default:
    fprintf(f, "WARNING: unknown pcap_activate() warning\n");
    break;
  }
}

int start_capture(struct args_s *args) {
  char errbuf[PCAP_ERRBUF_SIZE];
  int rc = 0;
  errbuf[0] = '\0';

  /* set up capture device and parameters */
  pcap_t *pcap;
  global_vars.pcap_handle = pcap = pcap_create(args->d, errbuf);
  if (pcap == NULL)
    goto pcap_fatal;
  if (pcap_set_snaplen(pcap, RING_BUF_SIZE))
    goto pcap_fatal;
  fprintf(stderr, "setting %spromiscuous mode @%s\n",
	  args->promisc ? "" : "not-", args->d);
  if (pcap_set_promisc(pcap, args->promisc))
    goto pcap_fatal;
  if (pcap_set_timeout(pcap, READ_TIMEOUT))
    goto pcap_fatal;  

  /* set timestamp type and resolution */
  const char *tstamp_type_pref[] = {
    "adapter_unsynced", "adapter", "host",
  };
  int *tstamp_types;
  rc = pcap_list_tstamp_types(pcap, &tstamp_types);
  if (rc == PCAP_ERROR) {
    fprintf(stderr, "ERR: %s \"%s\"\n", pcap_geterr(pcap), args->d);
  }
  else {
    int i, j, pref_idx = length(tstamp_type_pref), pref_type = PCAP_ERROR;
    fprintf(stderr, "%s supported pcap timestamp types: ", args->d);
    for (i = 0; i < rc; i++) {
      const char *name = pcap_tstamp_type_val_to_name(tstamp_types[i]);
      fprintf(stderr, "%s (%s)%s", name,
	      pcap_tstamp_type_val_to_description(tstamp_types[i]),
	      (i == rc-1) ? "\n" : ", ");
      for (j = 0; j < pref_idx; j++) {
	if (args->tstamp_type) {
	  /* prefer user's choice */
	  if (strcmp(name, args->tstamp_type) == 0) {
	    pref_idx = -1;
	    pref_type = tstamp_types[i];
	  }
	}
	if (strcmp(name, tstamp_type_pref[j]) == 0) {
	  pref_idx = j;
	  pref_type = tstamp_types[i];
	}
      }
    }
    if (pref_idx != length(tstamp_type_pref)) {
      fprintf(stderr, "setting timestamp type to \"%s\"\n",
	      pcap_tstamp_type_val_to_name(pref_type));
      if (pcap_set_tstamp_type(pcap, pref_type)) {
	fprintf(stderr, "ERR: %s \"%s\"\n", pcap_geterr(pcap), args->d);
      }
    }
    else {
      fprintf(stderr, "no prefered timestamp type found\n"); 
    }
    pcap_free_tstamp_types(tstamp_types);
  }
  if (pcap_set_tstamp_precision(pcap, PCAP_TSTAMP_PRECISION_NANO))
    goto pcap_fatal;

  /* activate capture device */
  if ((rc = pcap_activate(pcap)) < 0)
    goto pcap_fatal;
  if (rc)
    print_pcap_warning(stderr, rc);

  /* IPv4, IPv6, UDP, port 123
     http://ethereal.cs.pu.edu.tw/lists/ethereal-users/200208/msg00039.html */
  struct bpf_program filter;
  if (pcap_compile(pcap, &filter,
		   "udp && (port 123)", 1, PCAP_NETMASK_UNKNOWN)) {
    fprintf(stderr, "ERR: %s \"%s\"\n",
	    pcap_geterr(pcap), args->d);
    return EXIT_FAILURE;
  }

  /* man pcap-filter */
  if (pcap_setfilter(pcap, &filter)) {
    fprintf(stderr, "ERR: %s \"%s\"\n",
	    pcap_geterr(pcap), args->d);
    return EXIT_FAILURE;
  }

  rc = pcap_loop(pcap, -1, handle_packet, (void *)args);
  pcap_close(pcap);

  if (rc == -1)
    goto pcap_fatal;
  return EXIT_SUCCESS;
  
 pcap_fatal:
  fprintf(stderr, "ERR: pcap @%s: %s\n",
	  args->d, pcap_geterr(pcap));
  return EXIT_FAILURE;
}

/* sigaction handler */
void my_sa_handler(int x) {
  x = x;
  fprintf(stderr, "finishing packet capture\n");
  pcap_breakloop(global_vars.pcap_handle);
}

int main(int argc, char *argv[]) {
  global_vars.pcap_handle = NULL;

  sigset_t sigblock;
  sigfillset(&sigblock);
  struct sigaction signew = {
    .sa_handler    = my_sa_handler,
    //.sa_sigaction  = NULL,  /* may overlap with sa_handler => do not use both */
    .sa_mask       = sigblock,
    .sa_flags      = 0,
  };

  sigaction(SIGTERM, &signew, NULL);  /* termination */
  sigaction(SIGHUP,  &signew, NULL);  /* hangup */
  sigaction(SIGINT,  &signew, NULL);  /* interrupt */

  args.d = NULL;
  args.o = NULL;

  int opt;
  while ((opt = getopt(argc, argv, "+hd:o:t:")) != -1) {
    switch (opt) {
    case 'h':
      printf(
#ifdef SQL
      "USAGE: %s [-h] [-d <eth_device>] [-o <output_file>] [sql_db_file]\n"
#else
      "USAGE: %s [-h] [-d <eth_device>] [-o <output_file>]\n"
#endif
	     "  -d ethernet device to watch on\n"
	     "    if none given, watch on all available devices\n"
	     "  -t timestamp_type\n"
	     "    try to select chosen pcap timestamp type, if possible\n"
	     "  -o output file\n"
	     "    if none given, use stdout\n", argv[0]);
      return EXIT_SUCCESS;
    case 'd':
      if (args.d == NULL)
	args.d = argv[optind -1];
      else
	fprintf(stderr, "ERR: Argument -%c can be given only once!", (char)opt);
      break;
    case 't':
      args.tstamp_type = argv[optind-1];
      break;
    case 'o':
      if (args.o == NULL) {
	int fildes;
	if (
	    // obtain file descriptor
	    ((fildes = open(argv[optind -1], O_WRONLY | O_CREAT | O_EXCL,
			    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)) == -1)
	    ||
	    // use the obtained file descriptor
	    ((args.o = fdopen(fildes, "w")) == NULL)
	    ) {
	  fprintf(stderr, "ERR: Cannot open \"%s\" (%s).\n",
		  argv[optind -1], strerror(errno));
	  return EXIT_FAILURE;
	}
      }
      else {
	fprintf(stderr, "ERR: Argument -%c can be given only once!", (char)opt);
      }
      break;
    default:
      break;
    }
  }

  /* optind points to next argument (after the current one) in argv */
#ifdef SQL
  if (optind != argc) {
    sql_init(argv[optind]);
  }
  else {
    sql_init(DEFAULT_SQL_DB);
  }
  sql_log("init");
#else
  fprintf(stderr, "Unknown argument \"%s\".\n", argv[optind]);
  return EXIT_FAILURE;
#endif

  if (args.d == NULL) {
    fprintf(stderr,
	    "WARN: On some platforms (e.g. Linux) the pcap device \"any\" "
	    "produces\nmalformed packets. See -h for choosing a particular "
	    "device.\n");
    args.d = "any";
  }
  if (args.o == NULL) args.o = stdout;

  fprintf(stderr, "Press Ctrl+C for exit.\n");
  int ret = start_capture(&args);
  fclose(args.o);
#ifdef SQL
  sql_close();
#endif
  return ret;
}
