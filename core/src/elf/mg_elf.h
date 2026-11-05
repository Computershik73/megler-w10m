/* ELF for the four processors Android has (32-bit ARM, ARM64, x86, x86_64), as much of it as an Android shared object uses. */
#ifndef MG_ELF_H
#define MG_ELF_H

#include "../mg_base.h"

typedef struct {
    u8 e_ident[16];
    u16 e_type, e_machine;
    u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    u32 p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

typedef struct {
    u32 sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

typedef struct {
    i32 d_tag;
    u32 d_val;
} Elf32_Dyn;

typedef struct {
    u32 st_name, st_value, st_size;
    u8 st_info, st_other;
    u16 st_shndx;
} Elf32_Sym;

typedef struct {
    u32 r_offset, r_info;
} Elf32_Rel;

/* The 64-bit forms (arm64-v8a, x86_64), which have RELA, never REL. */
typedef struct {
    u8 e_ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
    u32 sh_name, sh_type;
    u64 sh_flags, sh_addr, sh_offset, sh_size;
    u32 sh_link, sh_info;
    u64 sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    i64 d_tag;
    u64 d_val;
} Elf64_Dyn;

typedef struct {
    u32 st_name;
    u8 st_info, st_other;
    u16 st_shndx;
    u64 st_value, st_size;
} Elf64_Sym;

typedef struct {
    u64 r_offset, r_info;
    i64 r_addend;
} Elf64_Rela;

#define ELFCLASS32 1
#define ELFCLASS64 2

#define ET_DYN 3
#define EM_386 3
#define EM_ARM 40
#define EM_X86_64 62
#define EM_AARCH64 183

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_GNU_RELRO 0x6474e552u
#define PT_ARM_EXIDX 0x70000001u

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define SHT_PROGBITS 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_REL 17
#define DT_RELSZ 18
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_FLAGS 30
#define DT_PREINIT_ARRAY 32
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_GNU_HASH 0x6ffffef5
#define DT_ANDROID_REL 0x6000000f
#define DT_ANDROID_RELSZ 0x60000010
#define DT_ANDROID_RELA 0x60000011
#define DT_ANDROID_RELASZ 0x60000012
#define DT_ANDROID_RELR 0x6fffe000
#define DT_ANDROID_RELRSZ 0x6fffe001

#define DF_TEXTREL 4

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_TLS 6
#define STT_GNU_IFUNC 10
#define SHN_UNDEF 0
#define ELF_ST_BIND(i) ((i) >> 4)
#define ELF_ST_TYPE(i) ((i) & 15)
#define ELF_R_SYM(i) ((i) >> 8)
#define ELF_R_TYPE(i) ((i) & 255)
#define ELF64_R_SYM(i) ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)(i))

#define R_ARM_NONE 0
#define R_ARM_ABS32 2
#define R_ARM_REL32 3
#define R_ARM_TLS_DESC 13
#define R_ARM_TLS_DTPMOD32 17
#define R_ARM_TLS_DTPOFF32 18
#define R_ARM_TLS_TPOFF32 19
#define R_ARM_COPY 20
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23
#define R_ARM_IRELATIVE 160

#define R_AARCH64_NONE 0
#define R_AARCH64_ABS64 257
#define R_AARCH64_COPY 1024
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLS_TPREL64 1030
#define R_AARCH64_TLSDESC 1031
#define R_AARCH64_IRELATIVE 1032

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_TLSDESC 36
#define R_X86_64_IRELATIVE 37

#define R_386_NONE 0
#define R_386_32 1
#define R_386_PC32 2
#define R_386_COPY 5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define R_386_TLS_TPOFF 14
#define R_386_TLS_DTPMOD32 35
#define R_386_TLS_DTPOFF32 36
#define R_386_TLS_DESC 41
#define R_386_IRELATIVE 42

#endif
