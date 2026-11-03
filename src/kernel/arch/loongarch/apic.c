#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "loongarch.h"
#include "defs.h"

void
apic_init(void)
{
  uint64 mask = (0x1UL << UART0_IRQ) | PCI_IRQ_MASK;

  *(volatile uint64*)(LS7A_INT_MASK_REG) = ~mask;

  *(volatile uint64*)(LS7A_INT_EDGE_REG) = (0x1UL << UART0_IRQ);

  *(volatile uint8*)(LS7A_INT_HTMSI_VEC_REG + UART0_IRQ) = UART0_IRQ;
  for(int i = 16; i < 20; i++)
    *(volatile uint8*)(LS7A_INT_HTMSI_VEC_REG + i) = i;

  *(volatile uint64*)(LS7A_INT_POL_REG) = 0x0UL;

}

void
apicinit(void)
{
  apic_init();
}

void
apic_complete(uint64 irq)
{
  *(volatile uint64*)(LS7A_INT_CLEAR_REG) = (irq);
}
