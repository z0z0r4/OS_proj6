// wire format between netperf and netserver (everything is little-endian
// 32-bit words in a fixed header that starts every test)
#define NETPORT 12865
#define NP_MAGIC 0x4e504552

enum { TCP_STREAM = 1, UDP_STREAM, TCP_RR, UDP_RR, TCP_CRR };

struct nphdr {
  uint magic;
  uint type;
  uint req;       // request / message size
  uint rsp;       // response size (RR tests)
};

struct npresult {  // server -> client, TCP_STREAM and UDP_STREAM
  uint64 bytes;
  uint64 ns;
  uint64 msgs;
};
