// Network stack: Ethernet (virtio-net) + loopback, ARP, IPv4, ICMP echo,
// UDP and TCP.  Addresses and ports are host-order integers everywhere
// outside net.c's wire-format helpers.

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

struct sock;

// net.c
void  netinit(void);
void  net_intr(void);                 // virtio-net interrupt / poll
void  net_tick(void);                 // called once per clock tick
void  net_eth_input(uint8*, int);     // from the driver (lock held)
void  sockclose(struct sock*);
int   sockread(struct sock*, uint64, int);
int   sockwrite(struct sock*, uint64, int);
int   sock_open(int);
struct sock *sockbyid(int);
int   net_irq(void);
int   sock_bind(struct sock*, int);
int   sock_listen(struct sock*);
struct sock *sock_accept(struct sock*, uint32*, int*);
int   sock_connect(struct sock*, uint32, int);
int   sock_sendto(struct sock*, uint64, int, uint32, int);
int   sock_recvfrom(struct sock*, uint64, int, uint64, uint64);
int   sock_shutdown(struct sock*);
int   sock_rcvtimeo(struct sock*, int);

// virtio_net.c (all called with the network lock held)
int   vnet_init(void);                // returns irq, or -1 if no device
int   vnet_send(uint8*, int);
void  vnet_poll(void);
int   vnet_pending(void);
extern uint8 vnet_mac[6];
extern int   vnet_up;
