"""A reader for the parts of an ELF shared library report.py looks at: 32- or 64-bit, little-endian."""
import struct

PT_LOAD, PT_DYNAMIC, PT_TLS, PT_GNU_RELRO, PT_ARM_EXIDX = 1, 2, 7, 0x6474E552, 0x70000001
SHN_UNDEF = 0
STB = {0: 'LOCAL', 1: 'GLOBAL', 2: 'WEAK'}
STT = {0: 'NOTYPE', 1: 'OBJECT', 2: 'FUNC', 3: 'SECTION', 4: 'FILE', 5: 'COMMON', 6: 'TLS', 10: 'IFUNC'}
STV = {0: 'DEFAULT', 1: 'INTERNAL', 2: 'HIDDEN', 3: 'PROTECTED'}

DT_NAMES = {
    1: 'NEEDED', 2: 'PLTRELSZ', 3: 'PLTGOT', 4: 'HASH', 5: 'STRTAB', 6: 'SYMTAB', 7: 'RELA', 8: 'RELASZ', 9: 'RELAENT',
    10: 'STRSZ', 11: 'SYMENT', 12: 'INIT', 13: 'FINI', 14: 'SONAME', 15: 'RPATH', 16: 'SYMBOLIC', 17: 'REL', 18: 'RELSZ',
    19: 'RELENT', 20: 'PLTREL', 21: 'DEBUG', 22: 'TEXTREL', 23: 'JMPREL', 24: 'BIND_NOW', 25: 'INIT_ARRAY', 26: 'FINI_ARRAY',
    27: 'INIT_ARRAYSZ', 28: 'FINI_ARRAYSZ', 29: 'RUNPATH', 30: 'FLAGS', 32: 'PREINIT_ARRAY', 33: 'PREINIT_ARRAYSZ',
    35: 'RELRSZ', 36: 'RELR', 37: 'RELRENT',
    0x6000000F: 'ANDROID_REL', 0x60000010: 'ANDROID_RELSZ', 0x60000011: 'ANDROID_RELA', 0x60000012: 'ANDROID_RELASZ',
    0x6FFFE000: 'ANDROID_RELR', 0x6FFFE001: 'ANDROID_RELRSZ', 0x6FFFE003: 'ANDROID_RELRENT',
    0x6FFFFEF5: 'GNU_HASH', 0x6FFFFFF0: 'VERSYM', 0x6FFFFFF9: 'RELACOUNT', 0x6FFFFFFA: 'RELCOUNT', 0x6FFFFFFB: 'FLAGS_1',
    0x6FFFFFFC: 'VERDEF', 0x6FFFFFFD: 'VERDEFNUM', 0x6FFFFFFE: 'VERNEED', 0x6FFFFFFF: 'VERNEEDNUM',
}

R_ARM = {
    0: 'R_ARM_NONE', 2: 'R_ARM_ABS32', 3: 'R_ARM_REL32', 17: 'R_ARM_TLS_DTPMOD32', 18: 'R_ARM_TLS_DTPOFF32',
    19: 'R_ARM_TLS_TPOFF32', 20: 'R_ARM_COPY', 21: 'R_ARM_GLOB_DAT', 22: 'R_ARM_JUMP_SLOT', 23: 'R_ARM_RELATIVE',
    13: 'R_ARM_TLS_DESC', 160: 'R_ARM_IRELATIVE',
}


class Sym:
    __slots__ = ('name', 'value', 'size', 'bind', 'type', 'vis', 'shndx')

    @property
    def defined(self):
        return self.shndx != SHN_UNDEF

    @property
    def exported(self):
        return self.defined and self.bind in ('GLOBAL', 'WEAK') and self.vis in ('DEFAULT', 'PROTECTED')


class Elf:
    def __init__(self, path):
        self.path = path
        self.d = d = open(path, 'rb').read()
        if d[:4] != b'\x7fELF' or d[5] != 1:
            raise ValueError('%s: not a little-endian ELF file' % path)
        self.is64 = d[4] == 2
        if self.is64:
            (self.e_type, self.e_machine, _, _, phoff, shoff, self.e_flags, _, phentsize, phnum, shentsize, shnum,
             shstrndx) = struct.unpack_from('<HHIQQQIHHHHHH', d, 16)
        else:
            (self.e_type, self.e_machine, _, _, phoff, shoff, self.e_flags, _, phentsize, phnum, shentsize, shnum,
             shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
        self.phdrs = []
        for i in range(phnum):
            o = phoff + i * phentsize
            if self.is64:
                t, flags, off, vaddr, _, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', d, o)
            else:
                t, off, vaddr, _, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', d, o)
            self.phdrs.append({'type': t, 'offset': off, 'vaddr': vaddr, 'filesz': filesz, 'memsz': memsz, 'flags': flags, 'align': align})
        self.sections = []
        for i in range(shnum):
            o = shoff + i * shentsize
            if self.is64:
                name, t, flags, addr, off, size, link, info, _, entsize = struct.unpack_from('<IIQQQQIIQQ', d, o)
            else:
                name, t, flags, addr, off, size, link, info, _, entsize = struct.unpack_from('<IIIIIIIIII', d, o)
            self.sections.append({'name_off': name, 'type': t, 'flags': flags, 'addr': addr, 'offset': off, 'size': size,
                                  'link': link, 'info': info, 'entsize': entsize})
        if self.sections and shstrndx < len(self.sections):
            st = self.sections[shstrndx]
            for s in self.sections:
                s['name'] = self.cstr(st['offset'] + s['name_off'])
        self.dynamic = []
        for p in self.phdrs:
            if p['type'] == PT_DYNAMIC:
                size = 16 if self.is64 else 8
                for o in range(p['offset'], p['offset'] + p['filesz'], size):
                    tag, val = struct.unpack_from('<qQ' if self.is64 else '<iI', d, o)
                    if tag == 0:
                        break
                    self.dynamic.append((tag & 0xFFFFFFFF if not self.is64 else tag, val))
        self._dynsyms = None
        self._symtab = None

    def cstr(self, off):
        end = self.d.index(b'\0', off)
        return self.d[off:end].decode('utf-8', 'replace')

    def section(self, name):
        for s in self.sections:
            if s.get('name') == name:
                return s
        return None

    def offset_of(self, vaddr):
        for p in self.phdrs:
            if p['type'] == PT_LOAD and p['vaddr'] <= vaddr < p['vaddr'] + p['filesz']:
                return p['offset'] + vaddr - p['vaddr']
        return None

    def dyn(self, name):
        return [v for t, v in self.dynamic if DT_NAMES.get(t) == name]

    def dyn_tags(self):
        return [DT_NAMES.get(t, '0x%x' % t) for t, v in self.dynamic]

    def _dynstr(self):
        return self.offset_of(self.dyn('STRTAB')[0])

    def needed(self):
        base = self._dynstr()
        return [self.cstr(base + v) for v in self.dyn('NEEDED')]

    def soname(self):
        v = self.dyn('SONAME')
        return self.cstr(self._dynstr() + v[0]) if v else None

    def _read_syms(self, sec, strsec):
        out = []
        ent = 24 if self.is64 else 16
        for o in range(sec['offset'], sec['offset'] + sec['size'], ent):
            s = Sym()
            if self.is64:
                name, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', self.d, o)
            else:
                name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.d, o)
            s.name = self.cstr(strsec['offset'] + name)
            s.value, s.size, s.shndx = value, size, shndx
            s.bind = STB.get(info >> 4, str(info >> 4))
            s.type = STT.get(info & 15, str(info & 15))
            s.vis = STV[other & 3]
            out.append(s)
        return out

    def dynsyms(self):
        if self._dynsyms is None:
            sec = self.section('.dynsym')
            self._dynsyms = self._read_syms(sec, self.sections[sec['link']])[1:] if sec else []
        return self._dynsyms

    def symtab(self):
        if self._symtab is None:
            sec = self.section('.symtab')
            self._symtab = self._read_syms(sec, self.sections[sec['link']])[1:] if sec else []
        return self._symtab

    def exports(self):
        return sorted(set(s.name for s in self.dynsyms() if s.exported))

    def imports(self):
        return sorted(set(s.name for s in self.dynsyms() if not s.defined and s.name))

    def relocs(self):
        """(section name, offset, type number, symbol index) of every REL entry of a 32-bit library."""
        out = []
        for sec in self.sections:
            if sec['type'] == 9:        # SHT_REL
                for o in range(sec['offset'], sec['offset'] + sec['size'], 8):
                    off, info = struct.unpack_from('<II', self.d, o)
                    out.append((sec['name'], off, info & 0xFF, info >> 8))
        return out

    def read(self, vaddr, size):
        o = self.offset_of(vaddr)
        return self.d[o:o + size] if o is not None else None

    def dynamic_relocs(self):
        """(table, offset, type number, symbol index) of every relocation a 32-bit library asks its loader for,
        from the dynamic section as a loader reads it: DT_REL, DT_JMPREL, Android's packed DT_ANDROID_REL ("APS2")
        and DT_RELR (whose entries are all R_ARM_RELATIVE)."""
        out = []

        def table(tag, sztag, name):
            for addr, size in zip(self.dyn(tag), self.dyn(sztag)):
                o = self.offset_of(addr)
                for k in range(o, o + size, 8):
                    off, info = struct.unpack_from('<II', self.d, k)
                    out.append((name, off, info & 0xFF, info >> 8))

        table('REL', 'RELSZ', 'DT_REL')
        table('JMPREL', 'PLTRELSZ', 'DT_JMPREL')
        for addr, size in zip(self.dyn('ANDROID_REL'), self.dyn('ANDROID_RELSZ')):
            o = self.offset_of(addr)
            data = self.d[o:o + size]
            if data[:4] != b'APS2':
                raise ValueError('%s: packed relocations in an unknown format' % self.path)
            pos = [4]

            def sleb():
                v, shift = 0, 0
                while True:
                    b = data[pos[0]]
                    pos[0] += 1
                    v |= (b & 0x7F) << shift
                    shift += 7
                    if not b & 0x80:
                        break
                if b & 0x40:
                    v -= 1 << shift
                return v

            count, offset = sleb(), sleb()
            n = 0
            while n < count:
                group, flags = sleb(), sleb()
                delta = sleb() if flags & 2 else 0
                info = sleb() if flags & 1 else 0
                if flags & 8 and flags & 4:
                    sleb()
                for _ in range(group):
                    if n >= count:
                        break
                    offset += delta if flags & 2 else sleb()
                    if not flags & 1:
                        info = sleb()
                    if flags & 8 and not flags & 4:
                        sleb()
                    out.append(('DT_ANDROID_REL', offset & 0xFFFFFFFF, info & 0xFF, (info & 0xFFFFFFFF) >> 8))
                    n += 1
        for tag, sztag in (('RELR', 'RELRSZ'), ('ANDROID_RELR', 'ANDROID_RELRSZ')):
            for addr, size in zip(self.dyn(tag), self.dyn(sztag)):
                o = self.offset_of(addr)
                where = 0
                for k in range(o, o + size, 4):
                    e = struct.unpack_from('<I', self.d, k)[0]
                    if not e & 1:
                        out.append(('DT_RELR', e, 23, 0))
                        where = e + 4
                    else:
                        for bit in range(31):
                            if (e >> (bit + 1)) & 1:
                                out.append(('DT_RELR', where + bit * 4, 23, 0))
                        where += 31 * 4
        return out

    def verneed(self):
        """{library: [version names]} from DT_VERNEED."""
        out = {}
        v = self.dyn('VERNEED')
        n = self.dyn('VERNEEDNUM')
        if not v:
            return out
        base = self._dynstr()
        o = self.offset_of(v[0])
        for _ in range(n[0]):
            _, cnt, file_off, aux, nxt = struct.unpack_from('<HHIII', self.d, o)
            names = []
            a = o + aux
            for _ in range(cnt):
                _, _, _, name, anext = struct.unpack_from('<IHHII', self.d, a)
                names.append(self.cstr(base + name))
                a += anext
            out[self.cstr(base + file_off)] = names
            if not nxt:
                break
            o += nxt
        return out
