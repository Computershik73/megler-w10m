/* ELF32 for ARM, as much of it as an Android shared object uses. */
#ifndef HK_ELF_H
#define HK_ELF_H

#include "../hk_base.h"

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

#define ET_DYN 3
#define EM_ARM 40

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_GNU_RELRO 0x6474e552u
#define PT_ARM_EXIDX 0x70000001u

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_STRSZ 10
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

#endif
