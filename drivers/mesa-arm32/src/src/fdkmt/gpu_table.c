/*
 * gpu_table.c — bang SoC -> GPU cho loi fd_kmt.
 *
 * Khoa tra cuu = cac chu so trong ten UMD cua driver (KMTQAITYPE_UMDRIVERNAME),
 * vd "qcdx11um8996.dll" -> "8996". Ep bang bien moi truong FDKMT_SOC=8996.
 *
 * Them GPU: xem README.md. Dong moi de verified = false cho den khi
 * fdkmt_probe + egltest PASS tren may that.
 */

#include <string.h>

#include "fd_kmt_priv.h"

static const struct fdkmt_gpu gpus[] = {
   {
      .soc = "8996",
      .name = "Snapdragon 820 / Adreno 530",
      .devices = "HP Pro x3",
      .gpu_id = 530,
      .gmem_base = 0x100000,
      .gmem_size = 1024 * 1024,
      .max_freq_mhz = 624,
      .kmd = &fdkmt_kmd_qcom_v1,
      .verified = true,
   },
   {
      .soc = "8994",
      .name = "Snapdragon 810 / Adreno 430",
      .devices = "Lumia 950 XL",
      .gpu_id = 430,
      .gmem_base = 0x100000,
      .gmem_size = 1536 * 1024,
      .max_freq_mhz = 600,
      .kmd = &fdkmt_kmd_qcom_v1,
      .verified = false,
      .note = "chua kiem chung: ABI KMD qcdxkm8994, GMEM, tan so",
   },
   {
      .soc = "8992",
      .name = "Snapdragon 808 / Adreno 418",
      .devices = "Lumia 950",
      .gpu_id = 418,
      .gmem_base = 0x100000,
      .gmem_size = 512 * 1024,
      .max_freq_mhz = 600,
      .kmd = &fdkmt_kmd_qcom_v1,
      .verified = false,
      .note = "chua kiem chung; Mesa CHUA co GPUId(418) - can them vao "
              "nhom a4xx trong freedreno_devices.py",
   },
};

const struct fdkmt_gpu *
fdkmt_gpu_lookup(const char *soc)
{
   for (unsigned i = 0; i < sizeof(gpus) / sizeof(gpus[0]); i++)
      if (soc && !strcmp(gpus[i].soc, soc))
         return &gpus[i];
   return NULL;
}

const struct fdkmt_gpu *
fdkmt_gpu_table(unsigned *count)
{
   *count = sizeof(gpus) / sizeof(gpus[0]);
   return gpus;
}
