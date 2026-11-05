/*
 * System calls by number from guest code of Android's other ABIs: x86_64, ARM64 and x86.
 *
 * bionic's syscall() here (g_syscall, mg_libc_misc.c) is written in 32-bit ARM's EABI numbers, which the phone's guest code
 * passes, and so is what ARM64's svc traps hand on (arm64/mg_patch_arm64.c, mg_core.c). Code of another ABI passes its own:
 * BoringSSL asks for getrandom as 318 on x86_64, 278 on ARM64, 355 on x86, 384 on ARM; gettid is 186 on x86_64, which on ARM
 * is sigaltstack. Each call g_syscall handles is looked up by its number in the guest's ABI (mg_syscall_abi.inc, made by
 * tools/syscall-numbers.py from the NDK's headers) and given ARM's, and what that ABI passes differently is made ARM's:
 *
 *   - the 64-bit ABIs pass a 64-bit offset or length in one register, where ARM's has it in two after a pad: pread64,
 *     pwrite64, ftruncate, fallocate;
 *   - their mmap takes its offset in bytes, ARM's mmap2 in pages;
 *   - their statfs and fstatfs take no size of the buffer, which ARM's statfs64 and fstatfs64 have before it;
 *   - x86 passes a 64-bit offset in the two words after the count, with no pad: pread64, pwrite64, ftruncate64.
 */
#include "../mg_core.h"
#include "mg_bionic.h"

#if MG_GUEST_CODE && !MG_ARCH_ARM32

#include "mg_syscall_abi.inc"

/*
 * 32-bit ARM's number for the guest's call `nr`, its six arguments `a` made as ARM's would be passed. -1 for a call g_syscall
 * does not handle; -2 for arguments ARM's call cannot take (an mmap at an offset that is not a page's).
 */
long mg_syscall_abi_to_arm(long nr, long a[6])
{
    long arm = -1;
    size_t i;
    for (i = 0; i < sizeof(g_abi_nr) / sizeof(g_abi_nr[0]); i++) {
        if (g_abi_nr[i][0] == nr) {
            arm = g_abi_nr[i][1];
            break;
        }
    }
    switch (arm) {
#if MG_GUEST_LP64
    case 192: /* mmap */
        if (a[5] & 4095) return -2;
        a[5] = (long)((u64)a[5] >> 12);
        break;
    case 180: case 181: /* pread64, pwrite64: fd, buffer, count, offset */
        a[4] = (long)(u32)a[3];
        a[5] = (long)((u64)a[3] >> 32);
        a[3] = 0;
        break;
    case 194: /* ftruncate: fd, length */
        a[2] = (long)(u32)a[1];
        a[3] = (long)((u64)a[1] >> 32);
        a[1] = 0;
        break;
    case 352: { /* fallocate: fd, mode, offset, length */
        u64 offset = (u64)a[2], length = (u64)a[3];
        a[2] = (long)(u32)offset;
        a[3] = (long)(offset >> 32);
        a[4] = (long)(u32)length;
        a[5] = (long)(length >> 32);
        break;
    }
    case 266: case 267: /* statfs, fstatfs: path or fd, buffer */
        a[2] = a[1];
        a[1] = 0;
        break;
#else
    case 180: case 181: /* pread64, pwrite64: fd, buffer, count, low, high */
        a[5] = a[4];
        a[4] = a[3];
        a[3] = 0;
        break;
    case 194: /* ftruncate64: fd, low, high */
        a[3] = a[2];
        a[2] = a[1];
        a[1] = 0;
        break;
#endif
    default:
        break;
    }
    return arm;
}

#endif
