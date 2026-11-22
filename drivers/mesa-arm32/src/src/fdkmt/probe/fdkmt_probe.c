/*
 * fdkmt_probe — kiem tra loi fd_kmt khong can Mesa:
 *   1. fd_kmt_open
 *   2. 2 BO (lenh + du lieu), map, ghi/doc CPU
 *   3. IB PM4: CP_MEM_WRITE(data, 0xC0FFEE01); submit; cho fence
 *   4. doc data[0]
 * Exit 0 = GPU da chay PM4; 10+N = hong buoc N.
 *
 *   fdkmt_probe [nop]    -- "nop": IB chi co CP_NOP (thu submit an toan nhat)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fd_kmt.h"

int fd_kmt_bo_cpu_sync(struct fd_kmt *k, uint32_t handle, void **out_ptr);

#define CP_NOP        16
#define CP_MEM_WRITE  61
#define MAGIC         0xC0FFEE01u

static unsigned
odd_parity(unsigned v)
{
   return (~0x6996u >> ((v ^ (v >> 16) ^ (v >> 8) ^ (v >> 4)) & 0xf)) & 1;
}

/* Header goi type-7 (a5xx+), giong pm4_pkt7_hdr cua Mesa. */
static unsigned
pkt7(unsigned opcode, unsigned cnt)
{
   return 0x70000000u | (cnt & 0x3fff) | (odd_parity(cnt) << 15) |
          ((opcode & 0x7f) << 16) | (odd_parity(opcode) << 23);
}

static int
fail(int step, const char *what, int err)
{
   printf("fdkmt_probe: FAIL buoc %d: %s (%d)\n", step, what, err);
   return 10 + step;
}

int
main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   int nop_only = argc > 1 && !strcmp(argv[1], "nop");
   int cached = argc > 1 && !strcmp(argv[1], "cached");

   struct fd_kmt *k = NULL;
   int ret = fd_kmt_open(&k);
   if (ret)
      return fail(1, "fd_kmt_open", ret);
   printf("fdkmt_probe: [1] open OK\n");

   uint32_t hcmd, hdata;
   uint64_t vcmd, vdata;
   if ((ret = fd_kmt_bo_alloc(k, 65536, FD_KMT_BO_RING, &hcmd, &vcmd)))
      return fail(2, "bo_alloc cmd", ret);
   if ((ret = fd_kmt_bo_alloc(k, 4096, cached ? FD_KMT_BO_CACHED : 0, &hdata,
                              &vdata)))
      return fail(2, "bo_alloc data", ret);
   volatile uint32_t *cmd, *data;
   if ((ret = fd_kmt_bo_map(k, hcmd, (void **)&cmd)))
      return fail(2, "bo_map cmd", ret);
   if ((ret = fd_kmt_bo_map(k, hdata, (void **)&data)))
      return fail(2, "bo_map data", ret);
   data[0] = 0x12345678;
   if (data[0] != 0x12345678)
      return fail(2, "doc lai CPU", 0);
   data[0] = 0;
   /* "stress N": lap N vong: cap phat BO du lieu (kich thuoc thay doi), submit
    * CP_MEM_WRITE vao no, cho fence, kiem tra, giai phong. Dung voi
    * FDKMT_CACHE_MB=0 de moi vong la mot CreateAllocation2/Destroy that.
    */
   if (argc > 2 && !strcmp(argv[1], "stress")) {
      unsigned n = (unsigned)atoi(argv[2]);
      for (unsigned it = 0; it < n; it++) {
         uint32_t h = 0;
         uint64_t va = 0;
         uint64_t sz = 4096ull << (it % 8); /* 4 KiB .. 512 KiB */
         ret = fd_kmt_bo_alloc(k, sz, 0, &h, &va);
         if (ret) {
            printf("fdkmt_probe: stress vong %u: bo_alloc(%llu) = %d\n", it,
                   (unsigned long long)sz, ret);
            return 20;
         }
         volatile uint32_t *d = NULL;
         fd_kmt_bo_map(k, h, (void **)&d);
         d[0] = 0;
         unsigned c = 0;
         cmd[c++] = pkt7(CP_MEM_WRITE, 3);
         cmd[c++] = (uint32_t)va;
         cmd[c++] = (uint32_t)(va >> 32);
         cmd[c++] = MAGIC ^ it;
         while (c & 3)
            cmd[c++] = pkt7(CP_NOP, 0);
         struct fd_kmt_cmd ib = { vcmd, c, hcmd };
         struct fd_kmt_bo_ref r[2] = { { hcmd, FD_KMT_PREP_READ },
                                       { h, FD_KMT_PREP_WRITE } };
         uint32_t f = 0;
         ret = fd_kmt_submit(k, &ib, 1, r, 2, &f);
         if (!ret)
            ret = fd_kmt_fence_wait(k, f, 3000000000ull);
         if (ret || d[0] != (MAGIC ^ it)) {
            printf("fdkmt_probe: stress vong %u (bo %llu): ret=%d d=0x%08x\n", it,
                   (unsigned long long)sz, ret, d[0]);
            return 21;
         }
         fd_kmt_bo_free(k, h);
         if ((it + 1) % 100 == 0)
            printf("fdkmt_probe: stress %u vong OK\n", it + 1);
      }
      printf("fdkmt_probe: stress PASS %u vong\n", n);
      fd_kmt_close(k);
      return 0;
   }

   /* "maxib": tim kich thuoc IB lon nhat KMD nhan (IB toan CP_NOP).
    * KMD tra STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER (0xC01E0200) khi IB qua lon.
    */
   if (argc > 1 && !strcmp(argv[1], "maxib")) {
      static const unsigned sizes[] = { 256, 512, 1024, 1536, 1792, 1900, 1984,
                                        2000, 2016, 2048, 3072, 4096, 8192,
                                        16384, 32768, 65536 };
      for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
         unsigned dw = sizes[s] / 4;
         for (unsigned i = 0; i < dw; i++)
            cmd[i] = pkt7(CP_NOP, 0);
         struct fd_kmt_cmd ib = { vcmd, dw, hcmd };
         struct fd_kmt_bo_ref r = { hcmd, FD_KMT_PREP_READ };
         uint32_t f = 0;
         ret = fd_kmt_submit(k, &ib, 1, &r, 1, &f);
         if (!ret)
            ret = fd_kmt_fence_wait(k, f, 3000000000ull);
         printf("fdkmt_probe: IB %u byte -> %d\n", sizes[s], ret);
      }
      fd_kmt_close(k);
      return 0;
   }

   printf("fdkmt_probe: [2] cmd iova 0x%llx, data iova 0x%llx\n",
          (unsigned long long)vcmd, (unsigned long long)vdata);

   unsigned n = 0;
   if (nop_only) {
      cmd[n++] = pkt7(CP_NOP, 0);
   } else {
      cmd[n++] = pkt7(CP_MEM_WRITE, 3);
      cmd[n++] = (uint32_t)vdata;
      cmd[n++] = (uint32_t)(vdata >> 32);
      cmd[n++] = MAGIC;
   }
   while (n & 3)
      cmd[n++] = pkt7(CP_NOP, 0);

   struct fd_kmt_cmd ib = { vcmd, n, hcmd };
   struct fd_kmt_bo_ref refs[2] = {
      { hcmd, FD_KMT_PREP_READ },
      { hdata, FD_KMT_PREP_WRITE },
   };
   uint32_t fence = 0;
   if ((ret = fd_kmt_submit(k, &ib, 1, refs, 2, &fence)))
      return fail(3, "submit", ret);
   printf("fdkmt_probe: [3] submit %u dword -> fence %u\n", n, fence);

   ret = fd_kmt_fence_wait(k, fence, 3000000000ull);
   printf("fdkmt_probe: [4] fence_wait=%d data[0]=0x%08x\n", ret, data[0]);
   if (ret)
      return fail(4, "fence_wait", ret);
   if (cached) {
      void *p2 = NULL;
      int r2 = fd_kmt_bo_cpu_sync(k, hdata, &p2);
      printf("fdkmt_probe: [5] cached: truoc sync 0x%08x; Unlock2+Lock2=%d "
             "ptr %p -> %p; sau sync 0x%08x\n", data[0], r2, (void *)data, p2,
             p2 ? *(volatile uint32_t *)p2 : 0);
      if (p2)
         data = (volatile uint32_t *)p2;
   }
   if (!nop_only && data[0] != MAGIC)
      return fail(4, "GPU khong ghi gia tri", (int)data[0]);

   fd_kmt_bo_free(k, hcmd);
   fd_kmt_bo_free(k, hdata);
   fd_kmt_close(k);
   printf("fdkmt_probe: PASS%s\n", nop_only ? " (nop)" : " - GPU da chay PM4");
   return 0;
}
