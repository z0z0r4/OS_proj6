// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian

// File header
struct elfhdr {
  uint magic;  // must equal ELF_MAGIC
  uchar elf[12];
  ushort type;
  ushort machine;
  uint version;
  uint64 entry;
  uint64 phoff;
  uint64 shoff;
  uint flags;
  ushort ehsize;
  ushort phentsize;
  ushort phnum;
  ushort shentsize;
  ushort shnum;
  ushort shstrndx;
};

// Program section header
struct proghdr {
  uint32 type;
  uint32 flags;
  uint64 off;
  uint64 vaddr;
  uint64 paddr;
  uint64 filesz;
  uint64 memsz;
  uint64 align;
};

// Values for Proghdr type
#define ELF_PROG_LOAD           1
#define ELF_PROG_DYNAMIC        2
#define ELF_PROG_INTERP         3

// Values for elfhdr type
#define ELF_ET_EXEC             2
#define ELF_ET_DYN              3

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4

// ---- dynamic linking (used by the kernel loader and by user/ldso.c)

// Entries of the .dynamic section
struct dynent {
  long tag;
  uint64 val;
};
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_INIT         12
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_INIT_ARRAYSZ 27

// Symbol table entry
struct elfsym {
  uint32 name;
  uchar info;     // binding in the high nibble
  uchar other;
  ushort shndx;   // 0: undefined
  uint64 value;
  uint64 size;
};
#define ELF_SHN_ABS     0xfff1
#define ELF_STB_LOCAL   0
#define ELF_STB_WEAK    2

// Relocation entry with addend
struct elfrela {
  uint64 offset;
  uint64 info;    // symbol index << 32 | type
  long addend;
};
// The same numbers on riscv64 and loongarch64
#define R_NONE          0
#define R_64            2
#define R_RELATIVE      3
#define R_JUMP_SLOT     5

// What exec() hands to the dynamic linker (address passed in a2)
struct dyninfo {
  uint64 entry;   // the program's own entry point
  uint64 phdr;    // its program headers, in memory
  uint64 phnum;
  uint64 base;    // where the dynamic linker itself was loaded
};
