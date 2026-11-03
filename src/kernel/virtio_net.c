//
// Driver for qemu's virtio-net device (modern interface, no offloads).
//   riscv:     -device virtio-net-device,netdev=n0           (mmio)
//   loongarch: -device virtio-net-pci,netdev=n0,disable-legacy=on
//
// Every function here runs with the network lock (net.c) held, so the
// driver needs no lock of its own.  The 12-byte virtio_net_hdr in front
// of each frame is all zero: no checksum offload, no segmentation.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "virtio.h"
#include "net.h"

#define QSZ      32            // queue entries (power of two)
#define BUFSZ    2048          // one rx/tx buffer (header + frame)
#define NTX      32
#define VNHDR    12
#define VNET_F_MAC 5

#ifdef ARCH_LOONGARCH
int  virtio_pci_net_init(void);
uint8 *virtio_pci_net_cfg(void);
void vnet_pci_set_status(uint8);
uint8 vnet_pci_get_status(void);
uint64 vnet_pci_get_features(void);
void vnet_pci_set_features(uint64);
void vnet_pci_setup_queue(uint16, uint16, uint64, uint64, uint64);
void vnet_pci_notify(uint16);
void vnet_pci_intr_ack(void);
#define DMA(p) (((uint64)(p)) & ~DMWIN_MASK)
#else
#define DMA(p) ((uint64)(p))
static uint64 vbase;
#define R(r) ((volatile uint32 *)(vbase + (r)))
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#endif

struct vq {
  struct virtq_desc *desc;
  uint16 *avail;        // [0]=flags [1]=idx [2..]=ring
  uint8  *used;         // flags(2) idx(2) ring{id(4) len(4)}
  uint16 last;          // used entries consumed so far
};

static struct vq rxq, txq;
static uint8 *rxbuf[QSZ];
static uint8 *txbuf[NTX];
static char txfree[NTX];
uint8 vnet_mac[6];
int vnet_up;

static void
setstatus(uint8 s)
{
#ifdef ARCH_LOONGARCH
  vnet_pci_set_status(s);
#else
  *R(VIRTIO_MMIO_STATUS) = s;
#endif
}

static uint8
getstatus(void)
{
#ifdef ARCH_LOONGARCH
  return vnet_pci_get_status();
#else
  return *R(VIRTIO_MMIO_STATUS);
#endif
}

static void
notify(int q)
{
  __sync_synchronize();
#ifdef ARCH_LOONGARCH
  vnet_pci_notify(q);
#else
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = q;
#endif
}

static int
setupq(int n, struct vq *q)
{
  uint8 *pg = kalloc();
  if(pg == 0)
    return -1;
  memset(pg, 0, PGSIZE);
  q->desc = (struct virtq_desc*)pg;
  q->avail = (uint16*)(pg + 512);
  q->used = pg + 1024;
  q->last = 0;
#ifdef ARCH_LOONGARCH
  vnet_pci_setup_queue(n, QSZ, DMA(q->desc), DMA(q->avail), DMA(q->used));
#else
  *R(VIRTIO_MMIO_QUEUE_SEL) = n;
  if(*R(VIRTIO_MMIO_QUEUE_READY) || *R(VIRTIO_MMIO_QUEUE_NUM_MAX) < QSZ)
    return -1;
  *R(VIRTIO_MMIO_QUEUE_NUM) = QSZ;
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = DMA(q->desc);
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = DMA(q->desc) >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = DMA(q->avail);
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = DMA(q->avail) >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = DMA(q->used);
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = DMA(q->used) >> 32;
  *R(VIRTIO_MMIO_QUEUE_READY) = 1;
#endif
  return 0;
}

static void
post(struct vq *q, int id)
{
  uint16 idx = q->avail[1];
  q->avail[2 + idx % QSZ] = id;
  __sync_synchronize();
  q->avail[1] = idx + 1;
}

// Find and initialise the device.  Returns its interrupt line, or -1
// when the machine has no virtio-net device.
int
vnet_init(void)
{
  int irq = -1;
  uint8 *cfg;
  uint64 want, have;

#ifdef ARCH_LOONGARCH
  if((irq = virtio_pci_net_init()) < 0)
    return -1;
  cfg = virtio_pci_net_cfg();
#else
  for(int k = 0; k < 8; k++){
    vbase = VIRTIO0 + k * 0x1000;
    if(*R(VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976 &&
       *R(VIRTIO_MMIO_VERSION) == 2 && *R(VIRTIO_MMIO_DEVICE_ID) == 1){
      irq = VIRTIO0_IRQ + k;
      break;
    }
  }
  if(irq < 0)
    return -1;
  cfg = (uint8*)(vbase + 0x100);
#endif

  setstatus(0);
  setstatus(VIRTIO_CONFIG_S_ACKNOWLEDGE);
  setstatus(VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

#ifdef ARCH_LOONGARCH
  have = vnet_pci_get_features();
#else
  *R(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 0;
  have = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  *R(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 1;
  have |= (uint64)*R(VIRTIO_MMIO_DEVICE_FEATURES) << 32;
#endif
  if(!(have & (1ULL << VIRTIO_F_VERSION_1)) || !(have & (1 << VNET_F_MAC)))
    return -1;
  want = (1ULL << VIRTIO_F_VERSION_1) | (1 << VNET_F_MAC);
#ifdef ARCH_LOONGARCH
  vnet_pci_set_features(want);
#else
  *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = want;
  *R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 1;
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = want >> 32;
#endif
  setstatus(VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK);
  if(!(getstatus() & VIRTIO_CONFIG_S_FEATURES_OK))
    return -1;

  if(setupq(0, &rxq) < 0 || setupq(1, &txq) < 0)
    return -1;
  for(int i = 0; i < 6; i++)
    vnet_mac[i] = cfg[i];

  for(int i = 0; i < QSZ; i += 2){
    uint8 *pg = kalloc();
    if(pg == 0)
      return -1;
    for(int j = 0; j < 2; j++){
      rxbuf[i + j] = pg + j * BUFSZ + 6;   // frame (after the 12-byte header) at 2 mod 8
      rxq.desc[i + j].addr = DMA(rxbuf[i + j]);
      rxq.desc[i + j].len = BUFSZ - 8;
      rxq.desc[i + j].flags = VRING_DESC_F_WRITE;
      post(&rxq, i + j);
    }
  }
  for(int i = 0; i < NTX; i += 2){
    uint8 *pg = kalloc();
    if(pg == 0)
      return -1;
    for(int j = 0; j < 2; j++){
      txbuf[i + j] = pg + j * BUFSZ + 6;   // same trick as rx
      txfree[i + j] = 1;
    }
  }

  // Transmit buffers are reclaimed lazily by vnet_send(), so completions
  // need not interrupt (VIRTQ_AVAIL_F_NO_INTERRUPT).  An unneeded
  // completion interrupt arriving while the level-triggered line is being
  // cleared is exactly what makes the LoongArch INTx path go quiet.
  txq.avail[0] = 1;

  setstatus(VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER |
            VIRTIO_CONFIG_S_FEATURES_OK | VIRTIO_CONFIG_S_DRIVER_OK);
  notify(0);
  vnet_up = 1;
  return irq;
}

// Transmit one ethernet frame.  Returns -1 if every buffer is busy.
int
vnet_send(uint8 *frame, int len)
{
  if(!vnet_up || len > BUFSZ - 8 - VNHDR)
    return -1;
  while(txq.last != *(volatile uint16*)(txq.used + 2)){
    __sync_synchronize();
    txfree[*(uint32*)(txq.used + 4 + (txq.last % QSZ) * 8)] = 1;
    txq.last++;
  }
  int id;
  for(id = 0; id < NTX && !txfree[id]; id++)
    ;
  if(id == NTX)
    return -1;
  txfree[id] = 0;
  memset(txbuf[id], 0, VNHDR);
  memmove(txbuf[id] + VNHDR, frame, len);
  txq.desc[id].addr = DMA(txbuf[id]);
  txq.desc[id].len = VNHDR + len;
  txq.desc[id].flags = 0;
  post(&txq, id);
  notify(1);
  return 0;
}

// Are there received frames the driver has not looked at yet?  Lock-free
// peek used by the LoongArch interrupt handler (see trap.c).
int
vnet_pending(void)
{
  __sync_synchronize();
  return vnet_up && rxq.last != *(volatile uint16*)(rxq.used + 2);
}

// Acknowledge the interrupt and hand every received frame to the stack.
void
vnet_poll(void)
{
  if(!vnet_up)
    return;
#ifdef ARCH_LOONGARCH
  vnet_pci_intr_ack();
#else
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
#endif
  while(rxq.last != *(volatile uint16*)(rxq.used + 2)){
    __sync_synchronize();
    uint32 id = *(uint32*)(rxq.used + 4 + (rxq.last % QSZ) * 8);
    uint32 len = *(uint32*)(rxq.used + 8 + (rxq.last % QSZ) * 8);
    rxq.last++;
    if(id < QSZ && len > VNHDR && len <= BUFSZ - 8)
      net_eth_input(rxbuf[id] + VNHDR, len - VNHDR);
    post(&rxq, id);
    notify(0);
  }
}
