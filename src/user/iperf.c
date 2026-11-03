// iperf: TCP/UDP bandwidth test (a small rewrite of iperf3 for xv6).
//   iperf -s [-p port] [-1]                 server (-1: exit after one test)
//   iperf -c host [-p port] [-t sec | -n bytes] [-l len] [-i sec] [-u [-b Mbit/s]]
#include "kernel/types.h"
#include "user/user.h"
#include "user/netutil.h"

#define DEFPORT 5201
#define SEC 1000000000UL
#define UDPFIN 0xffffffffU

static int uflag;

static void
report(int id, uint64 t0, uint64 t1, uint64 bytes, char *what)
{
  printf("[");
  padd(id, 3);
  printf("]  ");
  pr_secs(t0); printf("-"); pr_secs(t1);
  printf("  sec  ");
  pr_size(bytes); printf("  ");
  pr_rate(bytes, t1 - t0);
  if(what) printf("  %s", what);
  printf("\n");
}

static int
client(char *host, int port, int secs, uint64 nbytes, int len, int ival, int mbit)
{
  uint ip = parse_ip(host);
  char *buf = malloc(len > 4096 ? len : 4096);
  memset(buf, 'x', len);
  int s = socket(uflag ? SOCK_DGRAM : SOCK_STREAM);
  if(s < 0 || connect(s, ip, port) < 0){
    printf("iperf: unable to connect to server %s:%d\n", host, port);
    return 1;
  }
  printf("Connecting to host %s, port %d\n", host, port);
  uint64 start = nstime(), last = start, lastbytes = 0, bytes = 0;
  uint seq = 0;
  while(1){
    uint64 now = nstime();
    if(nbytes ? bytes >= nbytes : now - start >= (uint64)secs * SEC)
      break;
    if(uflag){
      if(mbit > 0){                  // pace to mbit Mbits/sec, evenly spread
        while(bytes * 8 * 1000 > (nstime() - start) * mbit)
          yield();
      }
      memmove(buf, &seq, 4);
      seq++;
      if(mbit == 0 && seq % 8 == 0)
        yield();                 // let the receiver drain its queue
      if(write(s, buf, len) < 0)
        break;
      bytes += len;
    } else {
      int n = nbytes && nbytes - bytes < (uint64)len ? nbytes - bytes : len;
      int r = write(s, buf, n);
      if(r <= 0)
        break;
      bytes += r;
    }
    if(ival > 0 && now - last >= (uint64)ival * SEC){
      report(5, last - start, now - start, bytes - lastbytes, 0);
      last = now; lastbytes = bytes;
    }
  }
  uint64 end = nstime();
  if(uflag){
    uint fin = UDPFIN;
    memmove(buf, &fin, 4);
    for(int i = 0; i < 3; i++){
      write(s, buf, 8);
      pause(1);
    }
  } else {
    shutdown(s);
    close(s);
  }
  printf("- - - - - - - - - - - - - - - - - - - - - - - - -\n");
  printf("[ ID] Interval           Transfer     Bitrate\n");
  report(5, 0, end - start, bytes, "sender");
  if(uflag)
    printf("\nSent %u datagrams\n", seq);
  printf("\niperf Done.\n");
  return 0;
}

static void
tcp_serve(int c, uint ip, int port)
{
  char *buf = malloc(32768);
  uint64 start = 0, bytes = 0;
  printf("Accepted connection from %d.%d.%d.%d, port %d\n",
         ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
  int r;
  while((r = read(c, buf, 32768)) > 0){
    if(start == 0) start = nstime();
    bytes += r;
  }
  uint64 end = nstime();
  if(start == 0) start = end;
  printf("[ ID] Interval           Transfer     Bitrate\n");
  report(5, 0, end - start, bytes, "receiver");
  close(c);
  free(buf);
}

static int
udp_serve(int s)
{
  char *buf = malloc(2048);
  uint64 start = 0, bytes = 0;
  uint expect = 0, lost = 0, total = 0;
  while(1){
    int r = recvfrom(s, buf, 2048, 0, 0);
    if(r < 4)
      continue;
    uint seq;
    memmove(&seq, buf, 4);
    if(seq == UDPFIN){
      if(start == 0)
        continue;
      break;
    }
    if(start == 0) start = nstime();
    if(seq > expect) lost += seq - expect;
    expect = seq + 1;
    total++;
    bytes += r;
  }
  uint64 end = nstime();
  printf("[ ID] Interval           Transfer     Bitrate         Lost/Total Datagrams\n");
  printf("[  5]  ");
  pr_secs(0); printf("-"); pr_secs(end - start); printf("  sec  ");
  pr_size(bytes); printf("  "); pr_rate(bytes, end - start);
  printf("  %u/%u (%u%%)  receiver\n", lost, total + lost, total + lost ? lost * 100 / (total + lost) : 0);
  free(buf);
  return 0;
}

static int
server(int port, int once)
{
  int s = socket(uflag ? SOCK_DGRAM : SOCK_STREAM);
  if(s < 0 || bind(s, port) < 0 || (!uflag && listen(s) < 0)){
    printf("iperf: unable to start server on port %d\n", port);
    return 1;
  }
  printf("-----------------------------------------------------------\n");
  printf("Server listening on %d\n", port);
  printf("-----------------------------------------------------------\n");
  while(1){
    if(uflag){
      udp_serve(s);
    } else {
      uint ip; int rport;
      int c = accept(s, &ip, &rport);
      if(c < 0)
        continue;
      tcp_serve(c, ip, rport);
    }
    if(once)
      break;
    printf("\n");
  }
  close(s);
  return 0;
}

int
main(int argc, char *argv[])
{
  char *host = 0;
  int port = DEFPORT, secs = 10, len = 0, ival = 1, mbit = 1, once = 0, srv = 0;
  uint64 nbytes = 0;

  for(int i = 1; i < argc; i++){
    char *a = argv[i];
    if(!strcmp(a, "-s")) srv = 1;
    else if(!strcmp(a, "-c") && i + 1 < argc) host = argv[++i];
    else if(!strcmp(a, "-p") && i + 1 < argc) port = atoi(argv[++i]);
    else if(!strcmp(a, "-t") && i + 1 < argc) secs = atoi(argv[++i]);
    else if(!strcmp(a, "-n") && i + 1 < argc) nbytes = atoi(argv[++i]);
    else if(!strcmp(a, "-l") && i + 1 < argc) len = atoi(argv[++i]);
    else if(!strcmp(a, "-i") && i + 1 < argc) ival = atoi(argv[++i]);
    else if(!strcmp(a, "-b") && i + 1 < argc) mbit = atoi(argv[++i]);
    else if(!strcmp(a, "-u")) uflag = 1;
    else if(!strcmp(a, "-1")) once = 1;
    else {
      fprintf(2, "usage: iperf -s [-p port] [-u] [-1] | -c host [-p port] [-t sec|-n bytes] [-l len] [-i sec] [-u [-b Mbit/s]]\n");
      exit(1);
    }
  }
  if(len == 0) len = uflag ? 1470 : 16384;
  if(uflag && (len < 8 || len > 1472)) len = 1470;
  if(srv == (host != 0)){
    fprintf(2, "iperf: specify exactly one of -s or -c host\n");
    exit(1);
  }
  exit(srv ? server(port, once) : client(host, port, secs, nbytes, len, ival, mbit));
}
