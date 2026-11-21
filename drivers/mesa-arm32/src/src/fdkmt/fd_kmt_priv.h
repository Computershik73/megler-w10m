/*
 * fd_kmt_priv.h — interface noi bo cua loi fd_kmt, tach phan phu thuoc GPU/KMD:
 *
 *   struct fdkmt_kmd_ops : "ABI" cua mot ho KMD (dinh dang private data cap
 *                          phat/submit, co rieng). Vd kmd_qcom.c = Qualcomm
 *                          qcdxkm*.sys (da kiem chung voi qcdxkm8996).
 *   struct fdkmt_gpu     : mot dong trong bang gpu_table.c: SoC -> GPU Adreno
 *                          (gpu_id cho Mesa, GMEM, tan so) + KMD ops.
 *
 * Them GPU moi: xem README.md (them dong vao gpu_table.c; neu KMD khac dinh
 * dang thi them kmd_<ten>.c).
 */

#ifndef FD_KMT_PRIV_H_
#define FD_KMT_PRIV_H_

#include <stdbool.h>
#include <stdint.h>

struct fdkmt_kmd_ops {
   const char *name;

   /* Do lon toi da cua private data cap phat (byte). */
   unsigned alloc_privdata_max;

   /* Dien private data cho CreateAllocation2 cua mot BO tuyen tinh `bytes`
    * byte; kmt_flags = FD_KMT_BO_* (fd_kmt.h). Tra so byte da dien.
    */
   unsigned (*alloc_privdata)(void *buf, uint64_t bytes, uint32_t kmt_flags);

   /* Private data cua D3DKMTSubmitCommand (byte, noi dung toan 0).
    * Qualcomm 8996: 0 (0x20 / 948 bi tu choi).
    */
   unsigned submit_privdata_size;

   /* Lam tron kich thuoc BO (byte). */
   uint64_t bo_align;

   /* Kich thuoc (byte) cua allocation tu private data KMD tra ve khi
    * OpenResource (resource dung chung). 0 = khong biet.
    */
   uint64_t (*alloc_size_from_privdata)(const void *buf, unsigned bytes);

   /* KMD cap duoc bo nho CPU co cache khi kmt_flags co FD_KMT_BO_CACHED. */
   bool has_cpu_cached;
};

struct fdkmt_gpu {
   const char *soc;          /* so trong ten UMD, vd "8996" (qcdx11um8996.dll) */
   const char *name;         /* vd "Snapdragon 820 / Adreno 530" */
   const char *devices;      /* may W10M dung SoC nay */
   uint32_t gpu_id;          /* Mesa freedreno (freedreno_devices.py) */
   uint64_t chip_id;         /* 0 = Mesa tra theo gpu_id */
   uint64_t gmem_base;
   uint32_t gmem_size;
   uint32_t max_freq_mhz;
   const struct fdkmt_kmd_ops *kmd;
   bool verified;            /* da chay fdkmt_probe/egltest tren may that */
   const char *note;
};

extern const struct fdkmt_kmd_ops fdkmt_kmd_qcom_v1;

const struct fdkmt_gpu *fdkmt_gpu_lookup(const char *soc);
const struct fdkmt_gpu *fdkmt_gpu_table(unsigned *count);

#endif /* FD_KMT_PRIV_H_ */
