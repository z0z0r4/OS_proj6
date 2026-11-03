// helpers shared by iperf.c, netperf.c and netserver.c
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

static __attribute__((unused)) uint
parse_ip(char *s)
{
  uint ip = 0;
  for(int i = 0; i < 4; i++){
    ip = (ip << 8) | (atoi(s) & 255);
    while(*s && *s != '.') s++;
    if(*s) s++;
  }
  return ip;
}

// print v/100 with two decimals
static __attribute__((unused)) void
pr100(uint64 v)
{
  printf("%lu.%d%d", v / 100, (int)(v % 100) / 10, (int)(v % 10));
}

// "<n> bits/sec" with the same unit scaling iperf uses
static __attribute__((unused)) void
pr_rate(uint64 bytes, uint64 ns)
{
  if(ns == 0) ns = 1;
  uint64 m = bytes * 8 * 100000 / ns;              // Mbits/sec * 100
  if(m >= 100000){ pr100(m / 1000); printf(" Gbits/sec"); }
  else if(m >= 100){ pr100(m); printf(" Mbits/sec"); }
  else { pr100(bytes * 8 * 100000000 / ns); printf(" Kbits/sec"); }
}

static __attribute__((unused)) void
pr_size(uint64 bytes)
{
  if(bytes >= 1024 * 1024){ pr100(bytes * 100 / (1024 * 1024)); printf(" MBytes"); }
  else { pr100(bytes * 100 / 1024); printf(" KBytes"); }
}

// "sec.hundredths"
static __attribute__((unused)) void
pr_secs(uint64 ns)
{
  pr100(ns / 10000000);
}

static __attribute__((unused)) int
readn(int fd, char *buf, int n)
{
  int got = 0;
  while(got < n){
    int r = read(fd, buf + got, n - got);
    if(r <= 0)
      return got ? got : r;
    got += r;
  }
  return got;
}

static __attribute__((unused)) int
writen(int fd, char *buf, int n)
{
  int put = 0;
  while(put < n){
    int r = write(fd, buf + put, n - put);
    if(r <= 0)
      return -1;
    put += r;
  }
  return put;
}

// integer right-aligned in a field of w columns (xv6 printf has no widths)
static __attribute__((unused)) void
padd(int v, int w)
{
  int n = 1;
  for(int t = v; t >= 10; t /= 10) n++;
  for(; n < w; n++) printf(" ");
  printf("%d", v);
}

// string left-aligned in a field of w columns
static __attribute__((unused)) void
pads(char *str, int w)
{
  int n = strlen(str);
  printf("%s", str);
  for(; n < w; n++) printf(" ");
}
