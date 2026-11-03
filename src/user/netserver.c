// netserver: the remote end of netperf (TCP and UDP on one port).
#include "kernel/types.h"
#include "user/user.h"
#include "user/netutil.h"
#include "user/netproto.h"

static void
tcp_test(int c, struct nphdr *h)
{
  uint n = h->req > 65536 ? 65536 : h->req;
  uint r = h->rsp > 65536 ? 65536 : h->rsp;
  char *buf = malloc(n > r ? (n > 4096 ? n : 4096) : (r > 4096 ? r : 4096));

  if(h->type == TCP_STREAM){
    struct npresult res;
    uint64 start = 0;
    res.bytes = res.msgs = 0;
    int k;
    while((k = read(c, buf, 4096)) > 0){
      if(start == 0) start = nstime();
      res.bytes += k;
      res.msgs++;
    }
    res.ns = nstime() - (start ? start : nstime());
    write(c, &res, sizeof(res));
  } else {                              // TCP_RR / TCP_CRR: echo fixed-size replies
    do {
      if(readn(c, buf, n) < (int)n)
        break;
      if(writen(c, buf, r) < 0)
        break;
    } while(h->type == TCP_RR);
  }
  free(buf);
}

static void
tcp_loop(int s)
{
  while(1){
    uint ip; int port;
    int c = accept(s, &ip, &port);
    if(c < 0)
      continue;
    struct nphdr h;
    if(readn(c, (char*)&h, sizeof(h)) < (int)sizeof(h) || h.magic != NP_MAGIC){
      close(c);
      continue;
    }
    if(h.type == TCP_CRR){              // short: serve inline
      tcp_test(c, &h);
      close(c);
    } else if(fork() == 0){
      close(s);
      tcp_test(c, &h);
      close(c);
      exit(0);
    } else {
      close(c);
      while(wait(0) > 0 && 0)           // children are reaped lazily below
        ;
    }
  }
}

static void
udp_loop(int s)
{
  char *buf = malloc(2048);
  uint64 start = 0, bytes = 0, msgs = 0;
  while(1){
    uint ip; int port;
    int r = recvfrom(s, buf, 2048, &ip, &port);
    if(r < (int)sizeof(struct nphdr))
      continue;
    struct nphdr *h = (struct nphdr*)buf;
    if(h->magic != NP_MAGIC)
      continue;
    if(h->type == UDP_RR){
      uint rsp = h->rsp > 1472 ? 1472 : h->rsp;
      sendto(s, buf, rsp < sizeof(*h) ? sizeof(*h) : rsp, ip, port);
    } else if(h->type == UDP_STREAM){
      if(h->rsp == 1){                  // end marker: report back
        struct npresult res;
        res.bytes = bytes; res.msgs = msgs;
        res.ns = start ? nstime() - start : 0;
        sendto(s, &res, sizeof(res), ip, port);
        start = bytes = msgs = 0;
      } else {
        if(start == 0) start = nstime();
        bytes += r; msgs++;
      }
    }
  }
}

int
main(int argc, char *argv[])
{
  int port = NETPORT;
  for(int i = 1; i + 1 < argc; i++)
    if(!strcmp(argv[i], "-p"))
      port = atoi(argv[i + 1]);

  int t = socket(SOCK_STREAM), u = socket(SOCK_DGRAM);
  if(t < 0 || u < 0 || bind(t, port) < 0 || bind(u, port) < 0 || listen(t) < 0){
    fprintf(2, "netserver: cannot bind port %d\n", port);
    exit(1);
  }
  printf("Starting netserver with host 'IN(6)ANY' port '%d' and family AF_UNSPEC\n", port);
  if(fork() == 0){
    close(t);
    udp_loop(u);
  }
  close(u);
  tcp_loop(t);
  exit(0);
}
