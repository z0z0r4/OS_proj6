//
// A small TCP/IP stack.  One spinlock (netlock) protects everything;
// processes block with sleep(chan, &netlock).  Packets to local addresses
// (127.0.0.0/8 and our own address) go through a loopback queue that is
// drained at the end of every entry point, so the stack never recurses.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "defs.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "net.h"

#define ETH 14
#define IPH 20
#define NSOCK 64
#define NPG 8                       // pages per ring
#define RSZ (NPG * PGSIZE)          // ring size, a power of two
#define MSS 1448                    // a multiple of 8: keeps ring copies word-aligned
#define BACKLOG 8
#define UDPMAX 1472

#define ECONNREFUSED_ 111
#define ECONNRESET_ 104
#define ETIMEDOUT_ 110
#define EINTR_ 4

#define FIN 1
#define SYN 2
#define RST 4
#define PSH 8
#define ACK 16

enum { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTAB, FIN_WAIT1, FIN_WAIT2,
       CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };

struct ring {
  uint8 *pg[NPG];
  uint head, len;
};

struct sock {
  int used, type, state, orphan;
  uint32 laddr, raddr;
  int lport, rport;
  struct ring rcv, snd;
  int err, eof, shut;
  // tcp
  uint32 iss, irs, snd_una, snd_nxt, rcv_nxt, snd_wnd;
  int mss, fin_pending, fin_sent, dupacks;
  int rto, rto_left, rto_on, rexmits, tw_left;
  struct sock *parent;
  struct sock *acc[BACKLOG];
  int nacc;
  int rtimeo;                     // receive timeout in ticks, 0 = block forever
  uint rdeadline;
};

static struct spinlock netlock;
static struct sock socks[NSOCK];
static uint32 myip = (10 << 24) | (2 << 8) | 15;
static uint32 netmask = 0xffffff00;
static uint32 gateway = (10 << 24) | (2 << 8) | 2;
static uint32 issgen = 0x1000;
static int nextport = 49152;
static uint16 ipid;

// The frame under construction.  Offset 2 puts the TCP payload (frame
// offset 54) on an 8-byte boundary so memmove can copy it a word at a time.
static uint8 txstore[2056] __attribute__((aligned(8)));
#define txf (txstore + 2)
static int draining;

// loopback queue of raw IP packets
#define NLO 64
static struct { int len; int pad; uint8 d[1504]; } loq[NLO] __attribute__((aligned(8)));
static int lo_h, lo_n;

static uint16 get16(uint8 *p) { return (p[0] << 8) | p[1]; }
static uint32 get32(uint8 *p) { return ((uint32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void put16(uint8 *p, uint v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8 *p, uint32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int islocal(uint32 a) { return (a >> 24) == 127 || a == myip; }
#define SEQ_LT(a, b) ((int)((a) - (b)) < 0)
#define SEQ_LE(a, b) ((int)((a) - (b)) <= 0)

// ones-complement sum of n bytes, added to `sum`
static uint32
csum_add(uint32 sum, uint8 *p, int n)
{
  for(; n > 1; n -= 2, p += 2)
    sum += (p[0] << 8) | p[1];
  if(n)
    sum += p[0] << 8;
  return sum;
}

static uint16
csum_fold(uint32 sum)
{
  while(sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

// pseudo-header checksum of the transport segment t
static uint16
l4sum(uint32 src, uint32 dst, int proto, uint8 *t, int len)
{
  uint8 ph[12];
  put32(ph, src); put32(ph + 4, dst); ph[8] = 0; ph[9] = proto; put16(ph + 10, len);
  return csum_fold(csum_add(csum_add(0, ph, 12), t, len));
}

// ---------------------------------------------------------------- rings

static int
ring_init(struct ring *r)
{
  r->head = r->len = 0;
  for(int i = 0; i < NPG; i++){
    if((r->pg[i] = kalloc()) == 0){
      while(i-- > 0){ kfree(r->pg[i]); r->pg[i] = 0; }
      return -1;
    }
  }
  return 0;
}

static void
ring_free(struct ring *r)
{
  for(int i = 0; i < NPG; i++){
    if(r->pg[i]){ kfree(r->pg[i]); r->pg[i] = 0; }
  }
  r->head = r->len = 0;
}

// copy n bytes into the ring after its current contents
static void
ring_put(struct ring *r, uint8 *src, int n)
{
  uint pos = (r->head + r->len) % RSZ;
  r->len += n;
  while(n > 0){
    int c = PGSIZE - (pos % PGSIZE);
    if(c > n) c = n;
    memmove(r->pg[pos / PGSIZE] + pos % PGSIZE, src, c);
    pos = (pos + c) % RSZ; src += c; n -= c;
  }
}

// copy n bytes starting `off` bytes after the head, without consuming
static void
ring_peek(struct ring *r, int off, uint8 *dst, int n)
{
  uint pos = (r->head + off) % RSZ;
  while(n > 0){
    int c = PGSIZE - (pos % PGSIZE);
    if(c > n) c = n;
    memmove(dst, r->pg[pos / PGSIZE] + pos % PGSIZE, c);
    pos = (pos + c) % RSZ; dst += c; n -= c;
  }
}

static void
ring_drop(struct ring *r, int n)
{
  r->head = (r->head + n) % RSZ;
  r->len -= n;
}

// ---------------------------------------------------------------- IP out

static struct { uint32 ip; uint8 mac[6]; int valid; } arp[4];
static int arpnext;
static struct { uint32 ip; int len; uint8 d[1536]; } pend;

static void
eth_send(uint8 *mac, int type, int len)
{
  memmove(txf, mac, 6);
  memmove(txf + 6, vnet_mac, 6);
  put16(txf + 12, type);
  if(len < 60){
    memset(txf + len, 0, 60 - len);
    len = 60;
  }
  vnet_send(txf, len);
}

static void
arp_send(int op, uint8 *tmac, uint32 tip)
{
  static uint8 bcast[6] = {255, 255, 255, 255, 255, 255};
  uint8 *a = txf + ETH;
  put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, op);
  memmove(a + 8, vnet_mac, 6); put32(a + 14, myip);
  memmove(a + 18, tmac, 6); put32(a + 24, tip);
  eth_send(op == 1 ? bcast : tmac, 0x0806, ETH + 28);
}

static void
arp_learn(uint32 ip, uint8 *mac)
{
  int i;
  for(i = 0; i < 4 && !(arp[i].valid && arp[i].ip == ip); i++)
    ;
  if(i == 4)
    i = arpnext++ & 3;
  arp[i].ip = ip; arp[i].valid = 1; memmove(arp[i].mac, mac, 6);
  if(pend.len && pend.ip == ip){        // the packet that was waiting for it
    memmove(txf, pend.d, pend.len);
    memmove(txf, mac, 6);
    vnet_send(txf, pend.len);
    pend.len = 0;
  }
}

// txf holds an IP packet at offset ETH with l4len bytes of payload
static void
ip_output(int proto, uint32 src, uint32 dst, int l4len)
{
  uint8 *ip = txf + ETH;
  ip[0] = 0x45; ip[1] = 0; put16(ip + 2, IPH + l4len);
  put16(ip + 4, ipid++); put16(ip + 6, 0x4000);
  ip[8] = 64; ip[9] = proto; put16(ip + 10, 0);
  put32(ip + 12, src); put32(ip + 16, dst);

  if(islocal(dst)){
    if(lo_n < NLO){
      int i = (lo_h + lo_n++) % NLO;
      loq[i].len = IPH + l4len;
      memmove(loq[i].d, ip, IPH + l4len);
    }
    return;
  }
  if(!vnet_up)
    return;
  put16(ip + 10, csum_fold(csum_add(0, ip, IPH)));
  uint32 hop = ((dst ^ myip) & netmask) == 0 ? dst : gateway;
  int len = ETH + IPH + l4len;
  for(int i = 0; i < 4; i++){
    if(arp[i].valid && arp[i].ip == hop){
      eth_send(arp[i].mac, 0x0800, len);
      return;
    }
  }
  if(len > sizeof(pend.d))
    return;
  pend.ip = hop; pend.len = len < 60 ? 60 : len;
  memset(pend.d, 0, pend.len);
  memmove(pend.d, txf, len);
  uint8 zero[6] = {0};
  arp_send(1, zero, hop);
}

// ---------------------------------------------------------------- sockets

static struct sock *
sockalloc(int type)
{
  for(struct sock *s = socks; s < socks + NSOCK; s++){
    if(s->used)
      continue;
    memset(s, 0, sizeof(*s));
    if(ring_init(&s->rcv) < 0)
      return 0;
    if(type == SOCK_STREAM && ring_init(&s->snd) < 0){
      ring_free(&s->rcv);
      return 0;
    }
    s->used = 1; s->type = type; s->mss = MSS;
    return s;
  }
  return 0;
}

static void
sockfree(struct sock *s)
{
  ring_free(&s->rcv);
  ring_free(&s->snd);
  s->used = 0;
}

static int
portbusy(int port, struct sock *self)
{
  for(struct sock *s = socks; s < socks + NSOCK; s++)
    if(s->used && s != self && s->lport == port && s->state != TIME_WAIT && s->state != CLOSED)
      return 1;
  return 0;
}

static int
autoport(struct sock *s)
{
  for(int i = 0; i < 16384; i++){
    int p = nextport++;
    if(nextport > 65535)
      nextport = 49152;
    if(!portbusy(p, s)){
      s->lport = p;
      return 0;
    }
  }
  return -1;
}

// ---------------------------------------------------------------- TCP out

static void
tcp_raw(uint32 src, uint32 dst, int sport, int dport, uint32 seq, uint32 ack,
        int flags, int wnd, int hl, int n)
{
  uint8 *t = txf + ETH + IPH;
  put16(t, sport); put16(t + 2, dport); put32(t + 4, seq); put32(t + 8, ack);
  t[12] = (hl / 4) << 4; t[13] = flags; put16(t + 14, wnd);
  put16(t + 16, 0); put16(t + 18, 0);
  if(hl > 20){
    t[20] = 2; t[21] = 4; put16(t + 22, MSS);
  }
  if(!islocal(dst))
    put16(t + 16, l4sum(src, dst, 6, t, hl + n));
  ip_output(6, src, dst, hl + n);
}

static int
rcvwnd(struct sock *s)
{
  return RSZ - s->rcv.len;
}

// send one segment carrying n bytes of the send buffer starting at `off`
static void
tcp_xmit(struct sock *s, int flags, uint32 seq, int off, int n)
{
  if(n > 0)
    ring_peek(&s->snd, off, txf + ETH + IPH + 20, n);
  tcp_raw(s->laddr, s->raddr, s->lport, s->rport, seq, s->rcv_nxt,
          flags | ((flags & SYN) && s->state == SYN_SENT ? 0 : ACK),
          rcvwnd(s), (flags & SYN) ? 24 : 20, n);
}

static void
rto_arm(struct sock *s)
{
  s->rto_on = 1;
  s->rto_left = s->rto;
}

// send as much data (and a FIN) as the peer's window allows
static void
tcp_output(struct sock *s, int probe)
{
  if(s->state < ESTAB || s->state == TIME_WAIT)
    return;
  int off = s->snd_nxt - s->snd_una - (s->fin_sent ? 1 : 0);
  if(s->fin_sent)
    return;
  uint32 wnd = s->snd_wnd;
  if(probe && wnd == 0)
    wnd = 1;
  while(off < s->snd.len){
    int room = (int)wnd - off;
    int n = s->snd.len - off;
    if(n > s->mss) n = s->mss;
    if(n > room) n = room;
    if(n <= 0)
      break;
    tcp_xmit(s, ACK | (off + n == s->snd.len ? PSH : 0), s->snd_nxt, off, n);
    s->snd_nxt += n; off += n;
    if(!s->rto_on) rto_arm(s);
  }
  if(s->fin_pending && off == s->snd.len){
    tcp_xmit(s, FIN, s->snd_nxt, off, 0);
    s->snd_nxt++; s->fin_sent = 1;
    if(s->state == ESTAB) s->state = FIN_WAIT1;
    else if(s->state == CLOSE_WAIT) s->state = LAST_ACK;
    if(!s->rto_on) rto_arm(s);
  }
}

static void
tcp_ack(struct sock *s)
{
  tcp_xmit(s, ACK, s->snd_nxt, 0, 0);
}

static void
tcp_syn(struct sock *s)
{
  tcp_xmit(s, SYN, s->iss, 0, 0);
  s->snd_nxt = s->iss + 1;
  rto_arm(s);
}

static void
tcp_reset(struct sock *s, int err)
{
  s->err = err;
  s->state = CLOSED;
  s->rto_on = 0;
  wakeup(s); wakeup(&s->rcv); wakeup(&s->snd);
}

// ---------------------------------------------------------------- TCP in

static void
tcp_input(uint8 *ip, int len, uint32 src, uint32 dst, int lo)
{
  if(len < 20) return;
  uint8 *t = ip;
  int hl = (t[12] >> 4) * 4;
  if(hl < 20 || hl > len) return;
  if(!lo && l4sum(src, dst, 6, t, len) != 0) return;
  int sport = get16(t), dport = get16(t + 2);
  uint32 seq = get32(t + 4), ack = get32(t + 8);
  int flags = t[13], win = get16(t + 14);
  uint8 *data = t + hl;
  int dlen = len - hl;

  struct sock *s = 0, *l = 0;
  for(struct sock *c = socks; c < socks + NSOCK; c++){
    if(!c->used || c->type != SOCK_STREAM || c->lport != dport || c->state == CLOSED)
      continue;
    if(c->state == LISTEN){
      if(c->laddr == 0 || c->laddr == dst) l = c;
    } else if(c->rport == sport && c->raddr == src){
      s = c;
      break;
    }
  }

  if(s == 0){
    if(l && (flags & SYN) && !(flags & (ACK | RST))){
      if(l->nacc >= BACKLOG) return;
      struct sock *c = sockalloc(SOCK_STREAM);
      if(c == 0) return;
      c->laddr = dst; c->lport = dport; c->raddr = src; c->rport = sport;
      c->parent = l; c->irs = seq; c->rcv_nxt = seq + 1;
      c->iss = issgen += 0x3d0f1; c->rto = 3;
      c->snd_wnd = win;
      if(hl >= 24 && t[20] == 2 && t[21] == 4){
        int m = get16(t + 22);
        if(m < c->mss) c->mss = m;
      }
      c->state = SYN_RCVD;
      tcp_syn(c);
      return;
    }
    if(!(flags & RST)){                 // nobody home: refuse
      if(flags & ACK)
        tcp_raw(dst, src, dport, sport, ack, 0, RST, 0, 20, 0);
      else
        tcp_raw(dst, src, dport, sport, 0, seq + dlen + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0), RST | ACK, 0, 20, 0);
    }
    return;
  }

  if(flags & RST){
    int was = s->state;
    tcp_reset(s, was == SYN_SENT ? ECONNREFUSED_ : ECONNRESET_);
    if(s->orphan || was == SYN_RCVD)
      sockfree(s);
    return;
  }

  if(s->state == SYN_SENT){
    if((flags & (SYN | ACK)) == (SYN | ACK) && ack == s->iss + 1){
      s->irs = seq; s->rcv_nxt = seq + 1;
      s->snd_una = s->snd_nxt = ack;
      s->snd_wnd = win;
      if(hl >= 24 && t[20] == 2 && t[21] == 4){
        int m = get16(t + 22);
        if(m < s->mss) s->mss = m;
      }
      s->state = ESTAB; s->rto_on = 0; s->rexmits = 0;
      tcp_ack(s);
      wakeup(s);
    }
    return;
  }

  if(s->state == SYN_RCVD){
    if(flags & SYN){                    // duplicate SYN: repeat SYN-ACK
      tcp_xmit(s, SYN, s->iss, 0, 0);
      return;
    }
    if(!(flags & ACK) || ack != s->iss + 1)
      return;
    s->snd_una = s->snd_nxt = ack;
    s->snd_wnd = win;
    s->state = ESTAB; s->rto_on = 0; s->rexmits = 0;
    struct sock *p = s->parent;
    if(p && p->state == LISTEN && p->nacc < BACKLOG){
      p->acc[p->nacc++] = s;
      wakeup(p);
    } else {
      tcp_raw(s->laddr, s->raddr, s->lport, s->rport, s->snd_nxt, 0, RST, 0, 20, 0);
      tcp_reset(s, ECONNRESET_);
      sockfree(s);
      return;
    }
  }

  // acknowledgement processing
  if(flags & ACK){
    if(SEQ_LT(s->snd_una, ack) && SEQ_LE(ack, s->snd_nxt)){
      int n = ack - s->snd_una;
      int finacked = 0;
      if(s->fin_sent && ack == s->snd_nxt){
        finacked = 1;
        n--;
      }
      if(n > s->snd.len) n = s->snd.len;
      ring_drop(&s->snd, n);
      s->snd_una = ack;
      s->dupacks = 0; s->rexmits = 0; s->rto = 3;
      if(s->snd_una == s->snd_nxt) s->rto_on = 0; else rto_arm(s);
      wakeup(&s->snd);
      if(finacked){
        if(s->state == FIN_WAIT1){ s->state = FIN_WAIT2; s->tw_left = 100; }
        else if(s->state == CLOSING){ s->state = TIME_WAIT; s->tw_left = 4; }
        else if(s->state == LAST_ACK){
          s->state = CLOSED;
          wakeup(s); wakeup(&s->rcv);
          if(s->orphan) sockfree(s);
          return;
        }
      }
      s->snd_wnd = win;
    } else if(ack == s->snd_una && dlen == 0 && !(flags & FIN) &&
              win == s->snd_wnd && s->snd_una != s->snd_nxt){
      if(++s->dupacks == 3){            // fast retransmit (go back N)
        s->snd_nxt = s->snd_una; s->fin_sent = 0;
        tcp_output(s, 0);
      }
    } else if(SEQ_LE(s->snd_una, ack) && SEQ_LE(ack, s->snd_nxt)){
      s->snd_wnd = win;
    }
  }

  // incoming data
  int fin = flags & FIN;
  if(dlen > 0 || fin){
    if(s->orphan && dlen > 0 && s->state != FIN_WAIT1 && s->state != FIN_WAIT2){
      tcp_raw(s->laddr, s->raddr, s->lport, s->rport, s->snd_nxt, 0, RST, 0, 20, 0);
      tcp_reset(s, ECONNRESET_);
      sockfree(s);
      return;
    }
    if(SEQ_LT(s->rcv_nxt, seq)){        // a hole: ask for the missing bytes
      tcp_ack(s);
      return;
    }
    uint32 skip = s->rcv_nxt - seq;
    if(skip > (uint32)dlen || (skip == (uint32)dlen && !fin)){
      tcp_ack(s);
      return;
    }
    data += skip; dlen -= skip;
    int take = dlen;
    if(take > rcvwnd(s)) take = rcvwnd(s);
    if(take > 0 && (s->state == ESTAB || s->state == FIN_WAIT1 || s->state == FIN_WAIT2)){
      ring_put(&s->rcv, data, take);
      s->rcv_nxt += take;
      wakeup(&s->rcv);
    } else {
      take = 0;
    }
    if(fin && take == dlen && !s->eof &&
       (s->state == ESTAB || s->state == FIN_WAIT1 || s->state == FIN_WAIT2)){
      s->rcv_nxt++;
      s->eof = 1;
      if(s->state == ESTAB) s->state = CLOSE_WAIT;
      else if(s->state == FIN_WAIT1) s->state = CLOSING;
      else { s->state = TIME_WAIT; s->tw_left = 4; }
      wakeup(&s->rcv); wakeup(s);
    }
    tcp_ack(s);
  }
  tcp_output(s, 0);
}

// ---------------------------------------------------------------- UDP

static void
udp_input(uint8 *u, int len, uint32 src, uint32 dst, int lo)
{
  if(len < 8) return;
  int sport = get16(u), dport = get16(u + 2), ulen = get16(u + 4);
  if(ulen < 8 || ulen > len) return;
  if(!lo && get16(u + 6) != 0 && l4sum(src, dst, 17, u, ulen) != 0) return;
  for(struct sock *s = socks; s < socks + NSOCK; s++){
    if(!s->used || s->type != SOCK_DGRAM || s->lport != dport)
      continue;
    if(s->raddr && (s->raddr != src || s->rport != sport))
      continue;
    int n = ulen - 8;
    if(RSZ - s->rcv.len < 8 + n)
      return;                           // receive queue full: drop
    uint8 hdr[8];
    put16(hdr, n); put16(hdr + 2, sport); put32(hdr + 4, src);
    ring_put(&s->rcv, hdr, 8);
    ring_put(&s->rcv, u + 8, n);
    wakeup(&s->rcv);
    return;
  }
}

// ---------------------------------------------------------------- IP in

static void
ip_input(uint8 *ip, int len, int lo)
{
  if(len < IPH || (ip[0] >> 4) != 4) return;
  int hl = (ip[0] & 15) * 4;
  int total = get16(ip + 2);
  if(hl < IPH || total < hl || total > len) return;
  if(get16(ip + 6) & 0x3fff) return;    // fragments are not reassembled
  if(!lo && csum_fold(csum_add(0, ip, hl)) != 0) return;
  uint32 src = get32(ip + 12), dst = get32(ip + 16);
  if(!lo && dst != myip && dst != 0xffffffff && dst != (myip | ~netmask)) return;
  uint8 *p = ip + hl;
  int plen = total - hl;

  switch(ip[9]){
  case 1:                               // ICMP echo
    if(plen >= 8 && p[0] == 8 && plen <= UDPMAX + 8){
      uint8 *r = txf + ETH + IPH;
      memmove(r, p, plen);
      r[0] = 0; put16(r + 2, 0);
      put16(r + 2, csum_fold(csum_add(0, r, plen)));
      ip_output(1, dst, src, plen);
    }
    break;
  case 6:
    tcp_input(p, plen, src, dst, lo);
    break;
  case 17:
    udp_input(p, plen, src, dst, lo);
    break;
  }
}

void
net_eth_input(uint8 *f, int len)
{
  if(len < ETH) return;
  int type = get16(f + 12);
  if(type == 0x0806 && len >= ETH + 28){
    uint8 *a = f + ETH;
    if(get16(a) != 1 || get16(a + 2) != 0x0800)
      return;
    arp_learn(get32(a + 14), a + 8);
    if(get16(a + 6) == 1 && get32(a + 24) == myip)
      arp_send(2, a + 8, get32(a + 14));
  } else if(type == 0x0800){
    ip_input(f + ETH, len - ETH, 0);
  }
}

// process queued loopback packets; each may queue more
static int
net_drain(void)
{
  static uint8 pkt[1504] __attribute__((aligned(8)));
  int n = 0;
  if(draining)
    return 0;
  draining = 1;
  while(lo_n > 0){
    n++;
    int len = loq[lo_h].len;
    memmove(pkt, loq[lo_h].d, len);
    lo_h = (lo_h + 1) % NLO;
    lo_n--;
    ip_input(pkt, len, 1);
  }
  draining = 0;
  return n;
}

static void
nrelease(void)
{
  net_drain();
  release(&netlock);
}

extern uint ticks;

// has the receive timeout of s run out?  Arms the deadline on first use.
static int
rtimedout(struct sock *s)
{
  if(s->rtimeo == 0)
    return 0;
  if(s->rdeadline == 0)
    s->rdeadline = ticks + s->rtimeo;
  if((int)(ticks - s->rdeadline) >= 0){
    s->rdeadline = 0;
    return 1;
  }
  return 0;
}

static void
nwait(void *chan)
{
  if(net_drain())       // delivering queued packets may have changed the
    return;             // state we are waiting for: let the caller look again
  sleep(chan, &netlock);
}

void
net_intr(void)
{
  if(!vnet_up)
    return;
  acquire(&netlock);
  vnet_poll();
  nrelease();
}

static int netirq = -1;

void
netinit(void)
{
  initlock(&netlock, "net");
  for(int i = 0; i < 4; i++)
    arp[i].valid = 0;
  netirq = vnet_init();
  if(netirq >= 0)
    printf("net: virtio-net %x:%x:%x:%x:%x:%x ip 10.0.2.15\n",
           vnet_mac[0], vnet_mac[1], vnet_mac[2], vnet_mac[3], vnet_mac[4], vnet_mac[5]);
}

int
net_irq(void)
{
  return netirq;
}

// once per clock tick: retransmission and TIME_WAIT timers, and a
// backstop poll of the receive ring in case an interrupt was lost
void
net_tick(void)
{
  acquire(&netlock);
  if(vnet_up)
    vnet_poll();
  for(struct sock *s = socks; s < socks + NSOCK; s++){
    if(s->used && s->rdeadline && (int)(ticks - s->rdeadline) >= 0)
      wakeup(&s->rcv);                  // a receive timeout has expired
    if(!s->used || s->type != SOCK_STREAM)
      continue;
    if((s->state == TIME_WAIT || (s->state == FIN_WAIT2 && s->orphan)) && --s->tw_left <= 0){
      s->state = CLOSED;
      if(s->orphan) sockfree(s);
      continue;
    }
    if(!s->rto_on || --s->rto_left > 0)
      continue;
    if(++s->rexmits > 10){
      int was = s->state;
      tcp_reset(s, ETIMEDOUT_);
      if(s->orphan || was == SYN_RCVD) sockfree(s);
      continue;
    }
    if(s->rto < 48) s->rto *= 2;
    rto_arm(s);
    if(s->state == SYN_SENT){
      tcp_xmit(s, SYN, s->iss, 0, 0);
    } else if(s->state == SYN_RCVD){
      tcp_xmit(s, SYN, s->iss, 0, 0);
    } else {
      s->snd_nxt = s->snd_una; s->fin_sent = 0;
      tcp_output(s, 1);
    }
  }
  nrelease();
}

// ---------------------------------------------------------------- user API

int
sock_open(int type)
{
  if(type != SOCK_STREAM && type != SOCK_DGRAM)
    return -1;
  acquire(&netlock);
  struct sock *s = sockalloc(type);
  release(&netlock);
  return s ? (int)(s - socks) : -1;
}

struct sock *sockbyid(int i) { return &socks[i]; }

int
sock_bind(struct sock *s, int port)
{
  int r = -1;
  acquire(&netlock);
  if(s->lport == 0 && port > 0 && port < 65536 && !portbusy(port, s)){
    s->lport = port;
    r = 0;
  }
  nrelease();
  return r;
}

int
sock_listen(struct sock *s)
{
  int r = -1;
  acquire(&netlock);
  if(s->type == SOCK_STREAM && s->state == CLOSED && s->lport){
    s->state = LISTEN;
    r = 0;
  }
  nrelease();
  return r;
}

struct sock *
sock_accept(struct sock *s, uint32 *ip, int *port)
{
  struct sock *c = 0;
  acquire(&netlock);
  if(s->state != LISTEN){
    release(&netlock);
    return 0;
  }
  while(s->nacc == 0){
    if(killed(myproc()) || s->state != LISTEN){
      release(&netlock);
      return 0;
    }
    nwait(s);
  }
  c = s->acc[0];
  for(int i = 1; i < s->nacc; i++)
    s->acc[i - 1] = s->acc[i];
  s->nacc--;
  c->parent = 0;
  *ip = c->raddr; *port = c->rport;
  nrelease();
  return c;
}

int
sock_connect(struct sock *s, uint32 ip, int port)
{
  int r = -1;
  acquire(&netlock);
  if(ip == 0)
    ip = 0x7f000001;
  if(port <= 0 || port > 65535 || (!islocal(ip) && !vnet_up)){
    release(&netlock);
    return -1;
  }
  if(s->type == SOCK_DGRAM){
    if(s->lport == 0 && autoport(s) < 0){
      release(&netlock);
      return -1;
    }
    s->raddr = ip; s->rport = port;
    nrelease();
    return 0;
  }
  if(s->state != CLOSED || (s->lport == 0 && autoport(s) < 0)){
    release(&netlock);
    return -1;
  }
  s->raddr = ip; s->rport = port;
  s->laddr = (ip >> 24) == 127 ? ip : (islocal(ip) ? ip : myip);
  s->iss = issgen += 0x3d0f1; s->rto = 3; s->err = 0;
  s->state = SYN_SENT;
  tcp_syn(s);
  while(s->state == SYN_SENT){
    if(killed(myproc())){
      tcp_reset(s, EINTR_);
      break;
    }
    nwait(s);
  }
  if(s->state == ESTAB){
    r = 0;
  } else {
    s->raddr = 0; s->rport = 0;
    s->state = CLOSED;
  }
  nrelease();
  return r;
}

int
sockwrite(struct sock *s, uint64 addr, int n)
{
  if(s->type == SOCK_DGRAM)
    return s->raddr ? sock_sendto(s, addr, n, s->raddr, s->rport) : -1;
  char *kb = kalloc();
  if(kb == 0)
    return -1;
  int done = 0, fail = 0;
  while(done < n && !fail){
    int c = n - done < PGSIZE ? n - done : PGSIZE;
    if(copyin(myproc()->pagetable, kb, addr + done, c) < 0){
      fail = 1;
      break;
    }
    int off = 0;
    acquire(&netlock);
    while(off < c){
      if(s->state != ESTAB && s->state != CLOSE_WAIT){
        fail = 1;
        break;
      }
      if(s->shut || killed(myproc())){
        fail = 1;
        break;
      }
      int room = RSZ - s->snd.len;
      if(room == 0){
        nwait(&s->snd);
        continue;
      }
      int k = c - off < room ? c - off : room;
      ring_put(&s->snd, (uint8*)kb + off, k);
      off += k;
      tcp_output(s, 0);
    }
    nrelease();
    done += off;
  }
  kfree(kb);
  return done > 0 ? done : -1;
}

int
sockread(struct sock *s, uint64 addr, int n)
{
  if(s->type == SOCK_DGRAM)
    return sock_recvfrom(s, addr, n, 0, 0);
  char *kb = kalloc();
  if(kb == 0)
    return -1;
  if(n > PGSIZE)
    n = PGSIZE;
  int r;
  acquire(&netlock);
  while(1){
    if(s->rcv.len > 0){
      r = s->rcv.len < n ? s->rcv.len : n;
      int before = rcvwnd(s);
      ring_peek(&s->rcv, 0, (uint8*)kb, r);
      ring_drop(&s->rcv, r);
      s->rdeadline = 0;
      // tell the sender when a closed window has opened up
      if(before < s->mss && rcvwnd(s) >= s->mss && s->state >= ESTAB)
        tcp_ack(s);
      break;
    }
    if(s->eof || s->state == CLOSED || s->state == TIME_WAIT){
      r = s->err ? -1 : 0;
      s->err = 0;
      break;
    }
    if(killed(myproc()) || rtimedout(s)){
      r = -1;
      break;
    }
    nwait(&s->rcv);
  }
  nrelease();
  if(r > 0 && copyout(myproc()->pagetable, addr, kb, r) < 0)
    r = -1;
  kfree(kb);
  return r;
}

int
sock_sendto(struct sock *s, uint64 addr, int n, uint32 ip, int port)
{
  if(s->type != SOCK_DGRAM || n < 0 || n > UDPMAX || port <= 0 || port > 65535)
    return -1;
  if(ip == 0)
    ip = 0x7f000001;
  uint8 *u = txf + ETH + IPH;      // build in place; copyin first
  char *kb = kalloc();
  if(kb == 0 || copyin(myproc()->pagetable, kb, addr, n) < 0){
    if(kb) kfree(kb);
    return -1;
  }
  acquire(&netlock);
  if((!islocal(ip) && !vnet_up) || (s->lport == 0 && autoport(s) < 0)){
    release(&netlock);
    kfree(kb);
    return -1;
  }
  uint32 src = islocal(ip) ? ip : myip;
  put16(u, s->lport); put16(u + 2, port); put16(u + 4, 8 + n); put16(u + 6, 0);
  memmove(u + 8, kb, n);
  if(!islocal(ip)){
    uint16 c = l4sum(src, ip, 17, u, 8 + n);
    put16(u + 6, c ? c : 0xffff);
  }
  ip_output(17, src, ip, 8 + n);
  nrelease();
  kfree(kb);
  return n;
}

int
sock_recvfrom(struct sock *s, uint64 addr, int n, uint64 ipp, uint64 portp)
{
  if(s->type != SOCK_DGRAM)
    return -1;
  char *kb = kalloc();
  if(kb == 0)
    return -1;
  uint8 hdr[8];
  int r = -1, len = 0;
  uint32 ip = 0;
  int port = 0;
  acquire(&netlock);
  while(s->rcv.len == 0){
    if(killed(myproc()) || rtimedout(s)){
      release(&netlock);
      kfree(kb);
      return -1;
    }
    nwait(&s->rcv);
  }
  ring_peek(&s->rcv, 0, hdr, 8);
  len = get16(hdr); port = get16(hdr + 2); ip = get32(hdr + 4);
  r = len < n ? len : n;
  if(r > PGSIZE) r = PGSIZE;
  ring_peek(&s->rcv, 8, (uint8*)kb, r);
  ring_drop(&s->rcv, 8 + len);
  s->rdeadline = 0;
  release(&netlock);
  struct proc *p = myproc();
  if(r > 0 && copyout(p->pagetable, addr, kb, r) < 0)
    r = -1;
  if(ipp && copyout(p->pagetable, ipp, (char*)&ip, sizeof(ip)) < 0)
    r = -1;
  if(portp && copyout(p->pagetable, portp, (char*)&port, sizeof(port)) < 0)
    r = -1;
  kfree(kb);
  return r;
}

int
sock_rcvtimeo(struct sock *s, int t)
{
  acquire(&netlock);
  s->rtimeo = t < 0 ? 0 : t;
  s->rdeadline = 0;
  release(&netlock);
  return 0;
}

int
sock_shutdown(struct sock *s)
{
  acquire(&netlock);
  if(s->type == SOCK_STREAM && (s->state == ESTAB || s->state == CLOSE_WAIT)){
    s->shut = 1;
    s->fin_pending = 1;
    tcp_output(s, 0);
  }
  nrelease();
  return 0;
}

// last reference to the file went away
void
sockclose(struct sock *s)
{
  acquire(&netlock);
  if(s->type == SOCK_DGRAM){
    sockfree(s);
    release(&netlock);
    return;
  }
  s->orphan = 1;
  if(s->state == LISTEN){
    for(struct sock *c = socks; c < socks + NSOCK; c++){
      if(c->used && c->parent == s){
        tcp_raw(c->laddr, c->raddr, c->lport, c->rport, c->snd_nxt, 0, RST, 0, 20, 0);
        c->state = CLOSED;
        sockfree(c);
      }
    }
    s->nacc = 0;
    s->state = CLOSED;
    sockfree(s);
  } else if(s->state == ESTAB || s->state == CLOSE_WAIT){
    s->fin_pending = 1;
    ring_drop(&s->rcv, s->rcv.len);     // nobody will read it
    tcp_output(s, 0);
  } else if(s->state == FIN_WAIT1 || s->state == FIN_WAIT2 || s->state == CLOSING ||
            s->state == LAST_ACK || s->state == TIME_WAIT || s->state == SYN_RCVD){
    ;                                   // already closing; timers finish it
  } else {
    sockfree(s);
  }
  nrelease();
}
