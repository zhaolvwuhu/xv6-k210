
#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/spinlock.h"
#include "include/proc.h"
#include "include/sbi.h"
#include "include/defs.h"
#include "include/plic.h"
#include "include/dmac.h"


struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();
void trapframedump(struct trapframe *tf);

void
trapinit(void)
{
  initlock(&tickslock, "time");
  #ifdef DEBUG
  printf("trapinit\n");
  #endif
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
  w_sstatus(r_sstatus() | SSTATUS_SIE);
  w_sie(r_sie() | SIE_SEIE | SIE_SSIE);
  #ifdef DEBUG
  printf("trapinithart\n");
  #endif
}

// static struct trapframe tf;

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  // printf("run in usertrap\n");
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  // tf = *(p->trapframe);
  
  if(r_scause() == 8){
    // system call
    if(p->killed)
      exit(-1);
    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;
    // tf.epc += 4;
    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();
    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    trapframedump(p->trapframe);
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  // printf("[usertrapret]p->pagetable: %p\n", p->pagetable);
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  // printf("run in kerneltrap...\n");
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p hart=%d\n", r_sepc(), r_stval(), r_tp());
    panic("kerneltrap");
  }
  // printf("which_dev: %d\n", which_dev);
  
  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING) {
    yield();
  }
  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    #ifdef QEMU
    int irq = plic_claim();
    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }
    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);
    #endif

    return 1;
  } 
  else if(scause == 0x8000000000000005L)
  {
    // software interrupt from a supervisor-mode timer interrupt,
    // if(cpuid() == 0){
      // clockintr();
    // }
    timer_tick();
    return 2;
  }
  #ifndef QEMU
  else if (scause == 0x8000000000000001L && r_stval() == 9) {
    int irq = plic_claim();
    switch (irq)
    {
      case IRQN_DMA0_INTERRUPT:
        dmac_intr(DMAC_CHANNEL0);
        break;
      case IRQN_UARTHS_INTERRUPT:
        uartintr();
        break;
    }
    if (irq) {
      plic_complete(irq);
    }
    w_sip(r_sip() & ~2);
    sbi_set_mie();
    return 3;
  }
  #endif
  else {
    return 0;
  }
}

#ifndef QEMU
void
supervisor_external_handler() {
  int irq = plic_claim();
  switch (irq)
  {
    case IRQN_DMA0_INTERRUPT:
      dmac_intr(DMAC_CHANNEL0);
      break;
    case IRQN_UARTHS_INTERRUPT:
      uartintr();
      break;
  }
  plic_complete(irq);
}
#endif

void device_init(unsigned long pa, uint64 hartid) {
  #ifndef QEMU
  // after RustSBI, txen = rxen = 1, rxie = 1, rxcnt = 0
  // start UART interrupt configuration
  // disable external interrupt on hart1 by setting threshold
  uint32 *hart0_m_threshold = (uint32*)PLIC;
  uint32 *hart1_m_threshold = (uint32*)PLIC_MENABLE(hartid);
  *(hart0_m_threshold) = 0;
  *(hart1_m_threshold) = 1;
  // *(uint32*)0x0c200000 = 0;
  // *(uint32*)0x0c202000 = 1;

  // now using UARTHS whose IRQID = 33
  // assure that its priority equals 1
  // if(*(uint32*)(0x0c000000 + 33 * 4) != 1) panic("uarhs's priority is not 1\n");
  // printf("uart priority: %p\n", *(uint32*)(0x0c000000 + 33 * 4));
  // *(uint32*)(0x0c000000 + 33 * 4) = 0x1;
  uint32 *hart0_m_int_enable_hi = (uint32*)(PLIC_MENABLE(hartid) + 0x04);
  *(hart0_m_int_enable_hi) = (1 << 0x1);
  // *(uint32*)0x0c002004 = (1 << 0x1);
  // sbi_set_extern_interrupt((uint64)supervisor_external_handler);
  #else
  *((uint32*)0x0c002080) = (1 << 10);
  *((uint8*)0x10000004) = 0x0b;
  *((uint8*)0x10000001) = 0x01;
  *((uint32*)0x0c000028) = 0x7;
  *((uint32*)0x0c201000) = 0x0;
  #endif
  #ifdef DEBUG
  printf("device init\n");
  #endif
}

void trapframedump(struct trapframe *tf)
{
  printf("a0: %p\t", tf->a0);
  printf("a1: %p\n", tf->a1);
  printf("a2: %p\t", tf->a2);
  printf("a3: %p\n", tf->a3);
  printf("a4: %p\t", tf->a4);
  printf("a5: %p\n", tf->a5);
  printf("a6: %p\t", tf->a6);
  printf("a7: %p\n", tf->a7);
  printf("t0: %p\t", tf->t0);
  printf("t1: %p\n", tf->t1);
  printf("t2: %p\t", tf->t2);
  printf("t3: %p\n", tf->t3);
  printf("t4: %p\t", tf->t4);
  printf("t5: %p\n", tf->t5);
  printf("t6: %p\t", tf->t6);
  printf("s0: %p\n", tf->s0);
  printf("s1: %p\t", tf->s1);
  printf("s2: %p\n", tf->s2);
  printf("s3: %p\t", tf->s3);
  printf("s4: %p\n", tf->s4);
  printf("s5: %p\t", tf->s5);
  printf("s6: %p\n", tf->s6);
  printf("s7: %p\t", tf->s7);
  printf("s8: %p\n", tf->s8);
  printf("s9: %p\t", tf->s9);
  printf("s10: %p\n", tf->s10);
  printf("s11: %p\t", tf->s11);
  printf("ra: %p\n", tf->ra);
  printf("sp: %p\t", tf->sp);
  printf("gp: %p\n", tf->gp);
  printf("tp: %p\t", tf->tp);
  printf("epc: %p\n", tf->epc);
}