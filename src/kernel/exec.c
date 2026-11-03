#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);
static int loadelf(pde_t *, struct inode *, struct elfhdr *, uint64, uint64 *);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  uint64 entry, phdr_va = 0, ibase = 0;
  char interp[MAXPATH];
  struct dyninfo di;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  interp[0] = 0;
  begin_op();

  // Open the executable file.
  ip = namei(path);
  if(ip == 0 && path[0] != '/'){
    // Bare command names (no '/') fall back to the root directory,
    // so programs installed in "/" work from any working directory.
    int i, slash;
    char rootpath[MAXPATH];

    slash = 0;
    for(i = 0; path[i]; i++)
      if(path[i] == '/')
        slash = 1;
    if(!slash && strlen(path) + 2 <= sizeof(rootpath)){
      rootpath[0] = '/';
      safestrcpy(rootpath + 1, path, sizeof(rootpath) - 1);
      ip = namei(rootpath);
    }
  }
  if(ip == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if(vfs_readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  if(loadelf(pagetable, ip, &elf, 0, &sz) < 0)
    goto bad;

  // A dynamically linked program names its interpreter (the dynamic
  // linker) in a PT_INTERP segment; also find where its program headers
  // ended up in memory, for the interpreter.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(vfs_readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type == ELF_PROG_INTERP){
      if(ph.filesz == 0 || ph.filesz >= sizeof(interp))
        goto bad;
      if(vfs_readi(ip, 0, (uint64)interp, ph.off, ph.filesz) != ph.filesz)
        goto bad;
      interp[ph.filesz] = 0;
    } else if(ph.type == ELF_PROG_LOAD && ph.off <= elf.phoff &&
              elf.phoff < ph.off + ph.filesz){
      phdr_va = ph.vaddr + (elf.phoff - ph.off);
    }
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  entry = elf.entry;
  if(interp[0]){
    // Load the dynamic linker (an ET_DYN image linked at 0) just above
    // the program, and start there instead.
    struct elfhdr ielf;
    struct inode *iip;

    begin_op();
    if((iip = namei(interp)) == 0){
      end_op();
      goto bad;
    }
    ilock(iip);
    ibase = PGROUNDUP(sz);
    if(vfs_readi(iip, 0, (uint64)&ielf, 0, sizeof(ielf)) != sizeof(ielf) ||
       ielf.magic != ELF_MAGIC || ielf.type != ELF_ET_DYN ||
       loadelf(pagetable, iip, &ielf, ibase, &sz) < 0){
      iunlockput(iip);
      end_op();
      goto bad;
    }
    iunlockput(iip);
    end_op();
    di.entry = elf.entry;
    di.phdr = phdr_va;
    di.phnum = elf.phnum;
    di.base = ibase;
    entry = ibase + ielf.entry;
  }

  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate some pages at the next page boundary.
  // Make the first inaccessible as a stack guard.
  // Use the rest as the user stack.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + (USERSTACK+1)*PGSIZE, PTE_W)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-(USERSTACK+1)*PGSIZE);
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;

  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // The dynamic linker also gets a pointer to what it needs to know.
  if(interp[0]){
    sp -= sizeof(di);
    sp -= sp % 16;
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, (char *)&di, sizeof(di)) < 0)
      goto bad;
    p->trapframe->a2 = sp;
  }

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
#ifdef ARCH_LOONGARCH
  p->trapframe->era = entry;  // initial program counter = main
#else
  p->trapframe->epc = entry;  // initial program counter = main
#endif
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Map and fill every PT_LOAD segment of ELF file ip (header already in
// *elf) at base+vaddr.  *szp is the current size of the address space;
// it grows as segments are added, even on failure, so that the caller can
// free what was allocated.  Segments need not start on a page boundary
// (shared objects don't).  Returns 0 on success, -1 on failure.
static int
loadelf(pagetable_t pagetable, struct inode *ip, struct elfhdr *elf,
        uint64 base, uint64 *szp)
{
  struct proghdr ph;
  int i, off;

  for(i=0, off=elf->phoff; i<elf->phnum; i++, off+=sizeof(ph)){
    if(vfs_readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return -1;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      return -1;
    if(base + ph.vaddr + ph.memsz < base + ph.vaddr)
      return -1;
    if(ph.memsz == 0)
      continue;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, *szp, base + ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
      return -1;
    *szp = sz1;
    if(loadseg(pagetable, base + ph.vaddr, ip, ph.off, ph.filesz) < 0)
      return -1;
  }
  return 0;
}

//
// the implementation of the dlmap() system call: map the shared object
// at path (an ET_DYN ELF file) into the calling process, above everything
// that is already there, and return its load address (0 on failure).
// Relocation and symbol resolution are left to the dynamic linker.
//
uint64
kdlmap(char *path)
{
  struct proc *p = myproc();
  struct elfhdr elf;
  struct inode *ip;
  uint64 base, sz, oldsz;

  begin_op();
  if((ip = namei(path)) == 0){
    end_op();
    return 0;
  }
  ilock(ip);
  oldsz = p->sz;
  base = PGROUNDUP(oldsz);
  sz = oldsz;
  if(vfs_readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf) ||
     elf.magic != ELF_MAGIC || elf.type != ELF_ET_DYN ||
     loadelf(p->pagetable, ip, &elf, base, &sz) < 0){
    iunlockput(ip);
    end_op();
    uvmdealloc(p->pagetable, sz, oldsz);
    return 0;
  }
  iunlockput(ip);
  end_op();
  p->sz = sz;
  // the new pages were written through the kernel's view of memory
  // and will be executed: make the instruction stream coherent
#ifdef ARCH_LOONGARCH
  asm volatile("ibar 0");
#else
  asm volatile("fence.i");
#endif
  return base;
}

// Load an ELF program segment into pagetable at virtual address va, which
// need not be page-aligned.  The pages from va to va+sz must already be
// mapped.  Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += n){
    uint64 a = va + i;
    pa = walkaddr(pagetable, PGROUNDDOWN(a));
    if(pa == 0)
      panic("loadseg: address should exist");
    n = PGSIZE - a % PGSIZE;
    if(n > sz - i)
      n = sz - i;
    pa += a % PGSIZE;
#ifdef ARCH_LOONGARCH
    pa |= DMWIN_MASK;
#endif
    if(vfs_readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
