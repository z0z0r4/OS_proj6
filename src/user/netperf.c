// netperf: TCP_STREAM, UDP_STREAM, TCP_RR, UDP_RR and TCP_CRR against netserver.
//   netperf [-H host] [-p port] [-t TEST] [-l secs] [-m msgsize] [-r req,rsp]
#include "kernel/types.h"
#include "user/user.h"
#include "user/netutil.h"
#include "user/netproto.h"

#define SEC 1000000000UL
#define SOCKBUF 32768

static char *name[] = { 0, "TCP STREAM", "UDP UNIDIRECTIONAL SEND", "TCP REQUEST/RESPONSE", "UDP REQUEST/RESPONSE", "TCP Connect/Request/Response" };

static void
banner(int type, char *host, int port)
{
  printf("%s TEST from 0.0.0.0 (0.0.0.0) port 0 AF_INET to %s () port %d AF_INET : demo\n",
         name[type], host, port);
}

static void
stream_header(void)
{
  printf("Recv   Send    Send\n");
  printf("Socket Socket  Message  Elapsed\n");
  printf("Size   Size    Size     Time     Throughput\n");
  printf("bytes  bytes   bytes    secs.    10^6bits/sec\n\n");
}

static void
rr_header(void)
{
  printf("Local /Remote\n");
  printf("Socket Size   Request  Resp.   Elapsed  Trans.\n");
  printf("Send   Recv   Size     Size    Time     Rate\n");
  printf("bytes  Bytes  bytes    bytes   secs.    per sec\n\n");
}

// Mbit/s with two decimals
static void
pr_mbit(uint64 bytes, uint64 ns)
{
  pr100(ns ? bytes * 8 * 100000 / ns : 0);   // Mbit/s * 100
}

static int
tcp_stream(uint ip, int port, int secs, int msg, char *host)
{
  int s = socket(SOCK_STREAM);
  struct nphdr h = { NP_MAGIC, TCP_STREAM, msg, 0 };
  if(s < 0 || connect(s, ip, port) < 0 || writen(s, (char*)&h, sizeof(h)) < 0){
    printf("netperf: connect to %s port %d failed\n", host, port);
    return 1;
  }
  banner(TCP_STREAM, host, port);
  char *buf = malloc(msg);
  memset(buf, 'n', msg);
  uint64 start = nstime(), sent = 0;
  while(nstime() - start < (uint64)secs * SEC){
    if(writen(s, buf, msg) < 0)
      break;
    sent += msg;
  }
  uint64 el = nstime() - start;
  shutdown(s);
  struct npresult res;
  if(readn(s, (char*)&res, sizeof(res)) < (int)sizeof(res)){
    printf("netperf: no result from netserver\n");
    return 1;
  }
  close(s);
  stream_header();
  padd(SOCKBUF, 6); printf(" "); padd(SOCKBUF, 6); printf(" "); padd(msg, 6); printf("    ");
  pr_secs(el); printf("    ");
  pr_mbit(res.bytes, res.ns ? res.ns : el);
  printf("\n");
  return 0;
}

static int
udp_stream(uint ip, int port, int secs, int msg, char *host)
{
  int s = socket(SOCK_DGRAM);
  if(msg < (int)sizeof(struct nphdr)) msg = sizeof(struct nphdr);
  if(msg > 1472) msg = 1472;
  if(s < 0 || connect(s, ip, port) < 0){
    printf("netperf: connect to %s port %d failed\n", host, port);
    return 1;
  }
  banner(UDP_STREAM, host, port);
  char *buf = malloc(msg);
  memset(buf, 'u', msg);
  struct nphdr *h = (struct nphdr*)buf;
  h->magic = NP_MAGIC; h->type = UDP_STREAM; h->req = msg; h->rsp = 0;
  uint64 start = nstime(), sent = 0, bytes = 0;
  while(nstime() - start < (uint64)secs * SEC){
    if(write(s, buf, msg) < 0)
      break;
    sent++; bytes += msg;
    if(sent % 8 == 0)
      yield();                           // let the receiver drain its queue
  }
  uint64 el = nstime() - start;
  h->rsp = 1;
  struct npresult res;
  res.bytes = 0; res.ns = el; res.msgs = 0;
  rcvtimeo(s, 10);                      // the end marker may be dropped: retry
  for(int i = 0; i < 5; i++){
    write(s, buf, msg);
    if(recvfrom(s, &res, sizeof(res), 0, 0) == sizeof(res))
      break;
  }
  stream_header();
  padd(SOCKBUF, 6); printf(" "); padd(SOCKBUF, 6); printf(" "); padd(msg, 6); printf("    ");
  pr_secs(el); printf("    ");
  pr_mbit(bytes, el); printf("   (send)\n");
  padd(SOCKBUF, 6); printf("                ");
  pr_secs(res.ns ? res.ns : el); printf("    ");
  pr_mbit(res.bytes, res.ns ? res.ns : el); printf("   (recv)\n");
  return 0;
}

static int
rr(int type, uint ip, int port, int secs, int req, int rsp, char *host)
{
  struct nphdr h = { NP_MAGIC, type, req, rsp };
  int udp = type == UDP_RR;
  char *buf = malloc(2048 > req + 16 ? 2048 : req + 16);
  char *rb = malloc(2048 > rsp + 16 ? 2048 : rsp + 16);
  int s = -1;
  uint64 trans = 0, start;

  if(udp && req < (int)sizeof(h)) req = sizeof(h);
  banner(type, host, port);
  if(udp || type == TCP_RR){
    s = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
    if(s < 0 || connect(s, ip, port) < 0){
      printf("netperf: connect to %s port %d failed\n", host, port);
      return 1;
    }
    if(!udp && writen(s, (char*)&h, sizeof(h)) < 0)
      return 1;
  }
  if(udp)
    rcvtimeo(s, 10);
  memset(buf, 'r', req);
  memmove(buf, &h, sizeof(h) < (uint)req ? sizeof(h) : req);
  start = nstime();
  while(nstime() - start < (uint64)secs * SEC){
    if(type == TCP_CRR){
      s = socket(SOCK_STREAM);
      if(s < 0 || connect(s, ip, port) < 0 || writen(s, (char*)&h, sizeof(h)) < 0)
        break;
    }
    if(udp){
      if(write(s, buf, req) < 0)
        break;
      if(read(s, rb, 2048) < 0)
        continue;                       // reply lost: not a transaction
    } else {
      if(writen(s, buf, req) < 0 || readn(s, rb, rsp) < rsp)
        break;
    }
    trans++;
    if(type == TCP_CRR){
      close(s);
      s = -1;
    }
  }
  uint64 el = nstime() - start;
  if(s >= 0) close(s);
  rr_header();
  padd(SOCKBUF, 6); printf(" "); padd(SOCKBUF, 6); printf(" "); padd(req, 8); printf(" "); padd(rsp, 7); printf(" ");
  pr_secs(el); printf("    ");
  pr100(el ? trans * 100 * SEC / el : 0);
  printf("\n");
  return 0;
}

int
main(int argc, char *argv[])
{
  char *host = "127.0.0.1", *test = "TCP_STREAM";
  int port = NETPORT, secs = 10, msg = 0, req = 1, rsp = 1;

  for(int i = 1; i < argc; i++){
    char *a = argv[i];
    if(!strcmp(a, "-H") && i + 1 < argc) host = argv[++i];
    else if(!strcmp(a, "-p") && i + 1 < argc) port = atoi(argv[++i]);
    else if(!strcmp(a, "-t") && i + 1 < argc) test = argv[++i];
    else if(!strcmp(a, "-l") && i + 1 < argc) secs = atoi(argv[++i]);
    else if(!strcmp(a, "-m") && i + 1 < argc) msg = atoi(argv[++i]);
    else if(!strcmp(a, "-r") && i + 1 < argc){
      char *r = argv[++i];
      req = atoi(r);
      while(*r && *r != ',') r++;
      if(*r) rsp = atoi(r + 1); else rsp = req;
    } else {
      fprintf(2, "usage: netperf [-H host] [-p port] [-t TCP_STREAM|UDP_STREAM|TCP_RR|UDP_RR|TCP_CRR] [-l secs] [-m size] [-r req,rsp]\n");
      exit(1);
    }
  }
  if(req < 1) req = 1;
  if(rsp < 1) rsp = 1;
  if(req > 60000) req = 60000;
  if(rsp > 60000) rsp = 60000;
  uint ip = parse_ip(host);
  int rc;
  if(!strcmp(test, "TCP_STREAM")) rc = tcp_stream(ip, port, secs, msg ? msg : 16384, host);
  else if(!strcmp(test, "UDP_STREAM")) rc = udp_stream(ip, port, secs, msg ? msg : 1024, host);
  else if(!strcmp(test, "TCP_RR")) rc = rr(TCP_RR, ip, port, secs, req, rsp, host);
  else if(!strcmp(test, "UDP_RR")) rc = rr(UDP_RR, ip, port, secs, req, rsp, host);
  else if(!strcmp(test, "TCP_CRR")) rc = rr(TCP_CRR, ip, port, secs, req, rsp, host);
  else {
    fprintf(2, "netperf: unknown test %s\n", test);
    rc = 1;
  }
  exit(rc);
}
