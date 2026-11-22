/*
 * kmd_qcom.c — ABI KMD Qualcomm WDDM (qcdxkm<SoC>.sys), phien ban 1.
 *
 * Kiem chung tren qcdxkm8996.sys (HP Pro x3, W10M 15254, 2026-11-18):
 *  - Private data CreateAllocation2: header 0x130 byte + (N-1) x 0x4C descriptor;
 *    [0]=0x130, [4]=ByteSize (lam tron 4K), [0x0E]=N, [0x0F]=AllocFlags,
 *    [0x11]=height, [0x12]=width, [0x14]=pitch, [1]=CRC (seed 1, poly
 *    0x04C11DB7, cac dword runtime {1, 8..0xD, 0x2E..0x31} tinh nhu 0).
 *    Nguon layout: D:\Git\Driver\PHAN-TICH-SAU-RENDER-PATCH.md muc 5.
 *  - AllocFlags 0x80: Lock2 tra PAGE_READWRITE (co cache) thay vi WRITECOMBINE.
 *  - SubmitCommand: private data 0 byte (0x20 / 948 -> STATUS_INVALID_PARAMETER).
 *  - GPU page 64 KiB (BO lam tron 64 KiB).
 *
 * KMD Qualcomm doi khac (8994/8992...) co the khac: kiem bang
 * `kmtalloc -priv` + `fdkmt_probe` truoc khi danh dau verified trong gpu_table.c.
 */

#include <string.h>

#include "fd_kmt.h"
#include "fd_kmt_priv.h"

#define QC_ALLOC_HEADER_SIZE 0x130
#define QC_POLY              0x04C11DB7u
#define QC_AF_CPU_CACHED     0x80

static uint32_t
qc_alloc_crc(const uint32_t *blob, unsigned dwords)
{
   static const unsigned runtime[] = { 1, 8, 9, 0xA, 0xB, 0xC, 0xD,
                                       0x2E, 0x2F, 0x30, 0x31 };
   uint32_t c = 1;
   for (unsigned k = 0; k < dwords; k++) {
      uint32_t v = blob[k];
      for (unsigned r = 0; r < sizeof(runtime) / sizeof(runtime[0]); r++)
         if (runtime[r] == k)
            v = 0;
      c = ((c & 0x80000000u) ? QC_POLY : 0) ^ (c << 1) ^ v;
   }
   return c;
}

/* BO tuyen tinh: 1 subresource, width = so byte, height 1. */
static unsigned
qc_alloc_privdata(void *buf, uint64_t bytes, uint32_t kmt_flags)
{
   uint32_t *blob = buf;
   memset(blob, 0, QC_ALLOC_HEADER_SIZE);
   blob[0x00] = QC_ALLOC_HEADER_SIZE;
   blob[0x04] = (uint32_t)((bytes + 0xFFF) & ~0xFFFull);
   blob[0x0E] = 1;
   blob[0x0F] = (kmt_flags & FD_KMT_BO_CACHED) ? QC_AF_CPU_CACHED : 0;
   blob[0x11] = 1;
   blob[0x12] = (uint32_t)bytes;
   blob[0x14] = (uint32_t)bytes;
   blob[0x01] = qc_alloc_crc(blob, QC_ALLOC_HEADER_SIZE / 4);
   return QC_ALLOC_HEADER_SIZE;
}

/* Private data allocation khi OpenResource (vd texture D3D11 1080x1920 RGBA8):
 * [0]=0x130, [4]=0x7f8040 (pitch*height + 0x40?), [0xC]=0x7f9000 (kich thuoc
 * da lam tron 4K), [0xE]=1, [0xF]=0x10, [0x11]=dinh dang DXGI.
 */
static uint64_t
qc_alloc_size_from_privdata(const void *buf, unsigned bytes)
{
   const uint32_t *w = buf;
   if (bytes < 0x34 || w[0] != QC_ALLOC_HEADER_SIZE)
      return 0;
   return w[0x0C];
}

const struct fdkmt_kmd_ops fdkmt_kmd_qcom_v1 = {
   .alloc_size_from_privdata = qc_alloc_size_from_privdata,
   .name = "qcom-v1",
   .alloc_privdata_max = QC_ALLOC_HEADER_SIZE,
   .alloc_privdata = qc_alloc_privdata,
   .submit_privdata_size = 0,
   .bo_align = 0x10000,
   .has_cpu_cached = true,
};
