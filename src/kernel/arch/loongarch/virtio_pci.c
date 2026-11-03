#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "loongarch.h"
#include "defs.h"
#include "virtio.h"

#define PCI_VENDOR_ID          0x00
#define PCI_DEVICE_ID          0x02
#define PCI_COMMAND            0x04
#define PCI_CAPABILITY_LIST    0x34
#define PCI_BAR0               0x10

#define PCI_COMMAND_IO         0x1
#define PCI_COMMAND_MEMORY     0x2
#define PCI_COMMAND_MASTER     0x4

#define PCI_VENDOR_VIRTIO      0x1af4
#define PCI_DEVICE_VIRTIO_BLK  0x1042
#define PCI_DEVICE_VIRTIO_NET  0x1041
#define PCI_INTERRUPT_PIN      0x3d

#define PCI_CAP_ID_VNDR        0x09
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

struct virtio_pci_common_cfg {
  uint32 device_feature_select;
  uint32 device_feature;
  uint32 driver_feature_select;
  uint32 driver_feature;
  uint16 msix_config;
  uint16 num_queues;
  uint8 device_status;
  uint8 config_generation;
  uint16 queue_select;
  uint16 queue_size;
  uint16 queue_msix_vector;
  uint16 queue_enable;
  uint16 queue_notify_off;
  uint64 queue_desc;
  uint64 queue_driver;
  uint64 queue_device;
};

struct vpci {
  volatile struct virtio_pci_common_cfg *common_cfg;
  volatile uint8 *isr_cfg;
  volatile uint16 *notify_cfg;
  volatile uint8 *dev_cfg;
  uint32 notify_off_multiplier;
  uint busno, devno, funcno;
  uint64 bar_addr[6];
  int irq;
};

static struct vpci blk, net;
static struct vpci *cur;   // device used by the virtio_pci_* wrappers
static uint64 next_mmio = PCI_MMIO_BASE & ~DMWIN_MASK;

static uint64
pci_conf_addr(uint bus, uint dev, uint func, uint off)
{
  return PCI_ECAM + (bus << 20) + (dev << 15) + (func << 12) + off;
}

static uint8
pci_read8(uint bus, uint dev, uint func, uint off)
{
  return *(volatile uint8*)pci_conf_addr(bus, dev, func, off);
}

static uint16
pci_read16(uint bus, uint dev, uint func, uint off)
{
  return *(volatile uint16*)pci_conf_addr(bus, dev, func, off);
}

static uint32
pci_read32(uint bus, uint dev, uint func, uint off)
{
  return *(volatile uint32*)pci_conf_addr(bus, dev, func, off);
}

static void
pci_write16(uint bus, uint dev, uint func, uint off, uint16 val)
{
  *(volatile uint16*)pci_conf_addr(bus, dev, func, off) = val;
}

static void
pci_write32(uint bus, uint dev, uint func, uint off, uint32 val)
{
  *(volatile uint32*)pci_conf_addr(bus, dev, func, off) = val;
}

static uint64
align_up(uint64 value, uint64 align)
{
  return (value + align - 1) & ~(align - 1);
}

static uint64
alloc_mmio(uint64 size)
{
  uint64 addr;

  size = align_up(size, PGSIZE);
  addr = align_up(next_mmio, size);
  if(addr + size > (PCI_MMIO_BASE & ~DMWIN_MASK) + PCI_MMIO_SIZE)
    panic("virtio pci mmio");
  next_mmio = addr + size;
  return addr;
}

static void
alloc_bars(struct vpci *d)
{
  uint bus = d->busno, dev = d->devno, func = d->funcno;
  uint64 *bar_addr = d->bar_addr;
  for(int i = 0; i < 6; i++){
    uint off = PCI_BAR0 + i * 4;
    uint32 old = pci_read32(bus, dev, func, off);

    pci_write32(bus, dev, func, off, 0xffffffff);
    uint32 low = pci_read32(bus, dev, func, off);
    pci_write32(bus, dev, func, off, old);

    if(low == 0 || low == 0xffffffff)
      continue;
    if(low & 0x1){
      pci_write32(bus, dev, func, off, old);
      continue;
    }

    uint64 mask;
    uint64 size;
    uint64 addr;
    int type = (low >> 1) & 0x3;

    if(type == 0x2 && i + 1 < 6){
      uint32 oldhi = pci_read32(bus, dev, func, off + 4);
      pci_write32(bus, dev, func, off + 4, 0xffffffff);
      uint32 high = pci_read32(bus, dev, func, off + 4);
      pci_write32(bus, dev, func, off + 4, oldhi);
      mask = ((uint64)high << 32) | (low & ~0xf);
      size = -mask;
      addr = alloc_mmio(size);
      pci_write32(bus, dev, func, off, (uint32)addr);
      pci_write32(bus, dev, func, off + 4, (uint32)(addr >> 32));
      bar_addr[i] = addr | DMWIN_MASK;
      i++;
    } else {
      mask = low & ~0xf;
      size = (uint32)(-mask);
      addr = alloc_mmio(size);
      pci_write32(bus, dev, func, off, (uint32)addr);
      bar_addr[i] = addr | DMWIN_MASK;
    }
  }
}

static int
find_device(struct vpci *d, uint16 id)
{
  for(uint dev = 0; dev < 32; dev++){
    uint16 vendor = pci_read16(0, dev, 0, PCI_VENDOR_ID);
    uint16 device = pci_read16(0, dev, 0, PCI_DEVICE_ID);
    if(vendor == PCI_VENDOR_VIRTIO && device == id){
      d->busno = 0;
      d->devno = dev;
      d->funcno = 0;
      // QEMU swizzles INTA of slot N onto PCH-PIC line 16 + N % 4.
      d->irq = 16 + ((dev + pci_read8(0, dev, 0, PCI_INTERRUPT_PIN) - 1) & 3);
      return 0;
    }
  }
  return -1;
}

static uint64
cap_addr(struct vpci *d, uint8 bar, uint32 off)
{
  if(bar >= 6 || d->bar_addr[bar] == 0)
    panic("virtio pci cap bar");
  return d->bar_addr[bar] + off;
}

static void
parse_caps(struct vpci *d)
{
  uint busno = d->busno, devno = d->devno, funcno = d->funcno;
  uint8 cap = pci_read8(busno, devno, funcno, PCI_CAPABILITY_LIST);

  while(cap){
    uint8 id = pci_read8(busno, devno, funcno, cap);
    uint8 next = pci_read8(busno, devno, funcno, cap + 1);

    if(id == PCI_CAP_ID_VNDR){
      uint8 cfg_type = pci_read8(busno, devno, funcno, cap + 3);
      uint8 bar = pci_read8(busno, devno, funcno, cap + 4);
      uint32 offset = pci_read32(busno, devno, funcno, cap + 8);

      if(cfg_type == VIRTIO_PCI_CAP_COMMON_CFG){
        d->common_cfg = (volatile struct virtio_pci_common_cfg*)cap_addr(d, bar, offset);
      } else if(cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG){
        d->notify_cfg = (volatile uint16*)cap_addr(d, bar, offset);
        d->notify_off_multiplier = pci_read32(busno, devno, funcno, cap + 16);
      } else if(cfg_type == VIRTIO_PCI_CAP_ISR_CFG){
        d->isr_cfg = (volatile uint8*)cap_addr(d, bar, offset);
      } else if(cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG){
        d->dev_cfg = (volatile uint8*)cap_addr(d, bar, offset);
      }
    }
    cap = next;
  }

  if(d->common_cfg == 0 || d->notify_cfg == 0 || d->isr_cfg == 0)
    panic("virtio pci caps");
}

static int
vpci_init(struct vpci *d, uint16 id)
{
  if(find_device(d, id) < 0)
    return -1;
  alloc_bars(d);
  parse_caps(d);
  pci_write16(d->busno, d->devno, d->funcno, PCI_COMMAND,
              PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
  return 0;
}

void
virtio_pci_init(void)
{
  if(vpci_init(&blk, PCI_DEVICE_VIRTIO_BLK) < 0)
    panic("virtio pci disk");
  cur = &blk;
}

// The network device: returns the PCH-PIC line, or -1 when absent.
int
virtio_pci_net_init(void)
{
  if(vpci_init(&net, PCI_DEVICE_VIRTIO_NET) < 0)
    return -1;
  return net.irq;
}

uint8 *virtio_pci_net_cfg(void) { return (uint8*)net.dev_cfg; }
uint8 virtio_pci_net_status(void) { return net.common_cfg->device_status; }

static uint8 vp_get_status(struct vpci *d) { return d->common_cfg->device_status; }
static void vp_set_status(struct vpci *d, uint8 v) { d->common_cfg->device_status = v; }
static uint64 vp_get_features(struct vpci *d)
{
  uint64 f;
  d->common_cfg->device_feature_select = 0;
  f = d->common_cfg->device_feature;
  d->common_cfg->device_feature_select = 1;
  f |= ((uint64)d->common_cfg->device_feature) << 32;
  return f;
}
static void vp_set_features(struct vpci *d, uint64 f)
{
  d->common_cfg->driver_feature_select = 0;
  d->common_cfg->driver_feature = f;
  d->common_cfg->driver_feature_select = 1;
  d->common_cfg->driver_feature = f >> 32;
}
static void vp_notify(struct vpci *d, uint16 q)
{
  d->common_cfg->queue_select = q;
  uint16 off = d->common_cfg->queue_notify_off;
  *(volatile uint16*)((uint64)d->notify_cfg + off * d->notify_off_multiplier) = q;
}

uint8 virtio_pci_get_status(void) { return vp_get_status(cur); }
void virtio_pci_set_status(uint8 s) { vp_set_status(cur, s); }
uint64 virtio_pci_get_features(void) { return vp_get_features(cur); }
void virtio_pci_set_features(uint64 f) { vp_set_features(cur, f); }
void virtio_pci_select_queue(uint16 q) { cur->common_cfg->queue_select = q; }
uint16 virtio_pci_queue_max(void) { return cur->common_cfg->queue_size; }
uint16 virtio_pci_queue_ready(void) { return cur->common_cfg->queue_enable; }
void virtio_pci_set_queue_num(uint16 n) { cur->common_cfg->queue_size = n; }
void virtio_pci_set_queue_desc(uint64 a) { cur->common_cfg->queue_desc = a; }
void virtio_pci_set_queue_avail(uint64 a) { cur->common_cfg->queue_driver = a; }
void virtio_pci_set_queue_used(uint64 a) { cur->common_cfg->queue_device = a; }
void virtio_pci_set_queue_ready(uint16 r)
{
  cur->common_cfg->queue_msix_vector = 0xffff;
  cur->common_cfg->queue_enable = r;
}
void virtio_pci_notify(uint16 q) { vp_notify(cur, q); }
void virtio_pci_intr_ack(void) { (void)*cur->isr_cfg; }

// Network device accessors (explicit device, never touching `cur`).
void vnet_pci_set_status(uint8 s) { vp_set_status(&net, s); }
uint8 vnet_pci_get_status(void) { return vp_get_status(&net); }
uint64 vnet_pci_get_features(void) { return vp_get_features(&net); }
void vnet_pci_set_features(uint64 f) { vp_set_features(&net, f); }
void vnet_pci_setup_queue(uint16 q, uint16 n, uint64 desc, uint64 avail, uint64 used)
{
  volatile struct virtio_pci_common_cfg *c = net.common_cfg;
  c->queue_select = q;
  c->queue_size = n;
  c->queue_desc = desc;
  c->queue_driver = avail;
  c->queue_device = used;
  c->queue_msix_vector = 0xffff;
  c->queue_enable = 1;
}
void vnet_pci_notify(uint16 q) { vp_notify(&net, q); }
void vnet_pci_intr_ack(void) { (void)*net.isr_cfg; }
