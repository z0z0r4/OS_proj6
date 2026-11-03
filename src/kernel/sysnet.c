//
// Socket system calls.  Stream sockets are also read and written with
// read()/write(); the network stack itself lives in net.c.
//

#include "types.h"
#include "riscv.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "defs.h"
#include "fs.h"
#include "file.h"
#include "proc.h"
#include "net.h"

static struct sock *
argsock(int n)
{
  int fd;
  argint(n, &fd);
  if(fd < 0 || fd >= NOFILE)
    return 0;
  struct file *f = myproc()->ofile[fd];
  return (f && f->type == FD_SOCK) ? f->sock : 0;
}

static int
newfd(struct file *f)
{
  struct proc *p = myproc();
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

// wrap a socket in a file and install it; closes the socket on failure
static int
sockfd(struct sock *s)
{
  struct file *f = filealloc();
  int fd;

  if(f == 0 || (fd = newfd(f)) < 0){
    if(f) fileclose(f);
    sockclose(s);
    return -1;
  }
  f->type = FD_SOCK;
  f->readable = f->writable = 1;
  f->sock = s;
  return fd;
}

uint64
sys_socket(void)
{
  int type;
  argint(0, &type);
  int id = sock_open(type);
  if(id < 0)
    return -1;
  return sockfd(sockbyid(id));
}

uint64
sys_bind(void)
{
  struct sock *s = argsock(0);
  int port;
  argint(1, &port);
  return s ? sock_bind(s, port) : -1;
}

uint64
sys_listen(void)
{
  struct sock *s = argsock(0);
  return s ? sock_listen(s) : -1;
}

uint64
sys_accept(void)
{
  struct sock *s = argsock(0), *c;
  uint64 ipp, portp;
  uint32 ip;
  int port;

  argaddr(1, &ipp);
  argaddr(2, &portp);
  if(s == 0 || (c = sock_accept(s, &ip, &port)) == 0)
    return -1;
  struct proc *p = myproc();
  if((ipp && copyout(p->pagetable, ipp, (char*)&ip, sizeof(ip)) < 0) ||
     (portp && copyout(p->pagetable, portp, (char*)&port, sizeof(port)) < 0)){
    sockclose(c);
    return -1;
  }
  return sockfd(c);
}

uint64
sys_connect(void)
{
  struct sock *s = argsock(0);
  uint64 ip, port;
  argaddr(1, &ip);
  argaddr(2, &port);
  return s ? sock_connect(s, ip, port) : -1;
}

uint64
sys_sendto(void)
{
  struct sock *s = argsock(0);
  uint64 buf, ip, port;
  int n;
  argaddr(1, &buf);
  argint(2, &n);
  argaddr(3, &ip);
  argaddr(4, &port);
  return s ? sock_sendto(s, buf, n, ip, port) : -1;
}

uint64
sys_recvfrom(void)
{
  struct sock *s = argsock(0);
  uint64 buf, ipp, portp;
  int n;
  argaddr(1, &buf);
  argint(2, &n);
  argaddr(3, &ipp);
  argaddr(4, &portp);
  return s ? sock_recvfrom(s, buf, n, ipp, portp) : -1;
}

uint64
sys_shutdown(void)
{
  struct sock *s = argsock(0);
  return s ? sock_shutdown(s) : -1;
}

uint64
sys_rcvtimeo(void)
{
  struct sock *s = argsock(0);
  int t;
  argint(1, &t);
  return s ? sock_rcvtimeo(s, t) : -1;
}

// give up the CPU until the scheduler comes back to us
uint64
sys_yield(void)
{
  yield();
  return 0;
}

// monotonic clock in nanoseconds
uint64
sys_nstime(void)
{
  return hal_time_ns();
}
