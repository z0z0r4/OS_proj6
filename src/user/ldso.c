//
// ldso: the dynamic linker.  The kernel's exec() loads this program (the
// PT_INTERP of a dynamically linked executable) above the executable and
// starts it with a0 = argc, a1 = argv, a2 = &struct dyninfo.  It
//
//   1. relocates itself,
//   2. maps every DT_NEEDED shared object with the dlmap() system call,
//   3. resolves symbols and applies the relocations (binding is eager),
//   4. runs the objects' constructors, and
//   5. calls the program's main().
//
// It also implements dlopen()/dlsym() for libxv6.so (see dl.c).  Built as
// a position-independent executable with no relocations needed before
// step 1, so keep it free of anything that needs a GOT or absolute
// addresses before then.
//
#include "kernel/types.h"
#include "kernel/elf.h"
#include "user/user.h"

struct dl_hooks {     // filled in here, called from libxv6.so
  void *(*open)(const char *);
  void *(*sym)(void *, const char *);
  int (*close)(void *);
  const char *(*error)(void);
};

#define NOBJ 16
#define NAMELEN 32
#define MAXNEEDED 8

struct obj {
  char name[NAMELEN];        // what DT_NEEDED / dlopen called it
  uint64 base;
  struct dynent *dyn;
  struct elfsym *symtab;
  char *strtab;
  uint32 *hash;
  struct elfrela *rela, *jmprel;
  uint64 relasz, pltrelsz;
  uint64 init, *init_array, init_arraysz;
};

static struct obj objs[NOBJ];  // objs[0] is the executable
static int nobj;
static const char *dlerr;

static int
slen(const char *s)
{
  int n = 0;
  while(s[n])
    n++;
  return n;
}

static void
scopy(char *d, const char *s)
{
  while((*d++ = *s++) != 0)
    ;
}

static void
puts2(const char *s)
{
  write(2, s, slen(s));
}

static void
fatal(const char *what, const char *name)
{
  puts2("ldso: ");
  puts2(what);
  puts2(": ");
  puts2(name);
  puts2("\n");
  exit(127);
}

static int
streq(const char *a, const char *b)
{
  while(*a && *a == *b)
    a++, b++;
  return *a == *b;
}

// ---- step 1: self-relocation (only R_RELATIVE entries can exist)

extern struct dynent _DYNAMIC[] __attribute__((visibility("hidden")));

static void
self_relocate(uint64 base)
{
  struct elfrela *r = 0;
  uint64 n = 0;

  for(struct dynent *d = _DYNAMIC; d->tag != DT_NULL; d++){
    if(d->tag == DT_RELA)
      r = (struct elfrela *)(base + d->val);
    else if(d->tag == DT_RELASZ)
      n = d->val / sizeof(*r);
  }
  for(uint64 i = 0; i < n; i++){
    if((r[i].info & 0xffffffff) == R_RELATIVE)
      *(uint64 *)(base + r[i].offset) = base + r[i].addend;
  }
}

// ---- symbols

static uint32
elfhash(const char *name)
{
  uint32 h = 0, g;

  while(*name){
    h = (h << 4) + (uchar)*name++;
    g = h & 0xf0000000;
    if(g)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

// value of the symbol `name` defined by o, or 0 with *found = 0
static uint64
lookup_in(struct obj *o, const char *name, uint32 h, int *found)
{
  uint32 nbucket = o->hash[0];
  uint32 *bucket = o->hash + 2;
  uint32 *chain = bucket + nbucket;

  for(uint32 i = bucket[h % nbucket]; i != 0; i = chain[i]){
    struct elfsym *s = &o->symtab[i];
    if(s->shndx == 0 || (s->info >> 4) == ELF_STB_LOCAL)
      continue;
    if(streq(o->strtab + s->name, name)){
      *found = 1;
      return s->shndx == ELF_SHN_ABS ? s->value : o->base + s->value;
    }
  }
  *found = 0;
  return 0;
}

// search every loaded object in load order (the executable first)
static uint64
lookup(const char *name, int *found)
{
  uint32 h = elfhash(name);

  for(int i = 0; i < nobj; i++){
    uint64 v = lookup_in(&objs[i], name, h, found);
    if(*found)
      return v;
  }
  *found = 0;
  return 0;
}

// ---- objects

static void
parse_dynamic(struct obj *o)
{
  for(struct dynent *d = o->dyn; d->tag != DT_NULL; d++){
    uint64 v = d->val;
    if(d->tag == DT_HASH) o->hash = (uint32 *)(o->base + v);
    else if(d->tag == DT_STRTAB) o->strtab = (char *)(o->base + v);
    else if(d->tag == DT_SYMTAB) o->symtab = (struct elfsym *)(o->base + v);
    else if(d->tag == DT_RELA) o->rela = (struct elfrela *)(o->base + v);
    else if(d->tag == DT_RELASZ) o->relasz = v;
    else if(d->tag == DT_JMPREL) o->jmprel = (struct elfrela *)(o->base + v);
    else if(d->tag == DT_PLTRELSZ) o->pltrelsz = v;
    else if(d->tag == DT_INIT) o->init = o->base + v;
    else if(d->tag == DT_INIT_ARRAY) o->init_array = (uint64 *)(o->base + v);
    else if(d->tag == DT_INIT_ARRAYSZ) o->init_arraysz = v;
  }
}

// register the object mapped at base whose program headers are at phdr
static struct obj *
add_object(const char *name, uint64 base, struct proghdr *ph, int phnum)
{
  if(nobj == NOBJ)
    fatal("too many shared objects", name);
  struct obj *o = &objs[nobj++];
  int i;
  for(i = 0; name[i] && i < NAMELEN - 1; i++)
    o->name[i] = name[i];
  o->name[i] = 0;
  o->base = base;
  for(i = 0; i < phnum; i++){
    if(ph[i].type == ELF_PROG_DYNAMIC)
      o->dyn = (struct dynent *)(base + ph[i].vaddr);
  }
  if(o->dyn == 0)
    fatal("not a dynamic object", name);
  parse_dynamic(o);
  if(o->hash == 0 || o->symtab == 0 || o->strtab == 0)
    fatal("object has no symbol table", name);
  return o;
}

static const char *
base_name(const char *path)
{
  const char *b = path;
  for(; *path; path++)
    if(*path == '/')
      b = path + 1;
  return b;
}

static struct obj *
find_object(const char *name)
{
  name = base_name(name);
  for(int i = 1; i < nobj; i++)
    if(streq(objs[i].name, name))
      return &objs[i];
  return 0;
}

// map the shared object `name` (and, recursively, what it needs)
static struct obj *
load_object(const char *name)
{
  char path[NAMELEN + 2];
  struct obj *o;

  name = base_name(name);
  if((o = find_object(name)) != 0)
    return o;
  if(slen(name) >= NAMELEN)
    fatal("name too long", name);
  path[0] = '/';
  scopy(path + 1, name);
  uint64 base = dlmap(path);
  if(base == 0)
    fatal("cannot load shared object", path);
  struct elfhdr *eh = (struct elfhdr *)base;
  o = add_object(name, base, (struct proghdr *)(base + eh->phoff), eh->phnum);
  return o;
}

// load every DT_NEEDED of objs[first..nobj) breadth-first
static void
load_needed(int first)
{
  for(int i = first; i < nobj; i++){
    struct obj *o = &objs[i];
    for(struct dynent *d = o->dyn; d->tag != DT_NULL; d++)
      if(d->tag == DT_NEEDED)
        load_object(o->strtab + d->val);
  }
}

// ---- step 3: relocation

static void
relocate_table(struct obj *o, struct elfrela *r, uint64 bytes)
{
  for(uint64 i = 0; i < bytes / sizeof(*r); i++, r++){
    uint32 type = r->info & 0xffffffff;
    uint32 symi = r->info >> 32;
    uint64 *where = (uint64 *)(o->base + r->offset);

    if(type == R_NONE)
      continue;
    if(type == R_RELATIVE){
      *where = o->base + r->addend;
    } else if(type == R_64 || type == R_JUMP_SLOT){
      struct elfsym *s = &o->symtab[symi];
      const char *name = o->strtab + s->name;
      int found;
      uint64 v = lookup(name, &found);
      if(!found && (s->info >> 4) != ELF_STB_WEAK)
        fatal("undefined symbol", name);
      *where = type == R_64 ? v + r->addend : v;
    } else {
      fatal("unsupported relocation in", o->name);
    }
  }
}

static void
relocate(struct obj *o)
{
  relocate_table(o, o->rela, o->relasz);
  relocate_table(o, o->jmprel, o->pltrelsz);
}

// ---- step 4: constructors

static void
run_init(struct obj *o)
{
  if(o->init)
    ((void (*)(void))o->init)();
  for(uint64 i = 0; i < o->init_arraysz / sizeof(uint64); i++)
    ((void (*)(void))o->init_array[i])();
}

// ---- dlopen() and friends

static void *
dl_open(const char *path)
{
  int first = nobj;
  int fail = 0;

  dlerr = 0;
  struct obj *o = find_object(path);
  if(o)
    return o;
  // fatal() would kill the program; for dlopen just report failure
  char p[NAMELEN + 2];
  const char *name = base_name(path);
  if(slen(name) >= NAMELEN){
    dlerr = "name too long";
    return 0;
  }
  p[0] = '/';
  scopy(p + 1, name);
  int fd = open(p, 0);
  if(fd < 0){
    dlerr = "no such shared object";
    return 0;
  }
  close(fd);
  if(nobj == NOBJ){
    dlerr = "too many shared objects";
    return 0;
  }
  o = load_object(name);
  load_needed(first);
  for(int i = first; i < nobj; i++)
    relocate(&objs[i]);
  for(int i = nobj - 1; i >= first; i--)
    run_init(&objs[i]);
  (void)fail;
  return o;
}

static void *
dl_sym(void *handle, const char *name)
{
  int found;
  uint64 v;

  dlerr = 0;
  if(handle)
    v = lookup_in((struct obj *)handle, name, elfhash(name), &found);
  else
    v = lookup(name, &found);
  if(!found){
    dlerr = "symbol not found";
    return 0;
  }
  return (void *)v;
}

static int
dl_close(void *handle)
{
  (void)handle;           // objects stay mapped for the life of the process
  return 0;
}

static const char *
dl_error(void)
{
  const char *e = dlerr;
  dlerr = 0;
  return e;
}

// ---- entry point

void
ld_start(int argc, char **argv, struct dyninfo *info)
{
  self_relocate(info->base);

  // objs[0]: the executable, which was loaded at 0
  add_object("", 0, (struct proghdr *)info->phdr, info->phnum);
  load_needed(0);
  for(int i = 0; i < nobj; i++)
    relocate(&objs[i]);

  int found;
  struct dl_hooks *h = (struct dl_hooks *)lookup("__dl_hooks", &found);
  if(found){
    h->open = dl_open;
    h->sym = dl_sym;
    h->close = dl_close;
    h->error = dl_error;
  }

  for(int i = nobj - 1; i >= 0; i--)
    run_init(&objs[i]);

  exit(((int (*)(int, char **))info->entry)(argc, argv));
}
