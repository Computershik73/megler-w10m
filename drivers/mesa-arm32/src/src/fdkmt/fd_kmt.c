/*
 * fd_kmt.c — loi D3DKMT cho winsys freedreno (Mesa) tren Windows 10 Mobile ARM32,
 * KMD Qualcomm qcdxkm8996.sys (Adreno 530). Cai dat giao dien
 * third_party/mesa/src/freedreno/drm/d3dkmt/fd_kmt.h.
 *
 * Chi dung API D3DKMT cong khai (WDDM 2.0, GpuMmu):
 *   open:   EnumAdapters2 -> CreateDevice -> CreateContextVirtual(node 0)
 *           -> CreatePagingQueue -> monitored fence (CreateSynchronizationObject2)
 *   alloc:  CreateAllocation2 (private data Qualcomm 0x130 + CRC) -> MakeResident
 *           -> MapGpuVirtualAddress
 *   map:    Lock2 (giu lock suot doi BO)
 *   submit: SubmitCommand(GPU VA cua IB) -> SignalSynchronizationObjectFromGpu2
 *   wait:   doc gia tri monitored fence (CPU VA)
 *
 * Layout private data cap phat: D:\Git\Driver\PHAN-TICH-SAU-RENDER-PATCH.md muc 5.
 * Da kiem chung tren may (kmtalloc -priv, 2026-11-18): CreateAllocation2/MakeResident/
 * MapGpuVirtualAddress/Lock2 deu OK.
 *
 * Bien moi truong (thu nghiem):
 *   FDKMT_DEBUG=1           in log ra stderr
 *   FDKMT_PRIV_SIZE=N       kich thuoc private data submit (mac dinh 0; 0x20 va 948 bi tu choi)
 *   FDKMT_NODE=N            node cua context (mac dinh 0 = 3D)
 */

#include <windows.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <share.h>
#include <string.h>
#include <d3dkmthk.h>

#include "fd_kmt.h"
#include "fd_kmt_priv.h"

#ifndef STATUS_PENDING
#define STATUS_PENDING ((NTSTATUS)0x00000103L)
#endif

/* ------------------------------------------------------------------ */
/* Nap dong cac ham D3DKMT tu gdi32.dll                               */
/* ------------------------------------------------------------------ */

#define KMT_FUNCS(X)                                   \
   X(D3DKMTEnumAdapters2)                              \
   X(D3DKMTQueryAdapterInfo)                           \
   X(D3DKMTCloseAdapter)                               \
   X(D3DKMTCreateDevice)                               \
   X(D3DKMTDestroyDevice)                              \
   X(D3DKMTCreateContextVirtual)                       \
   X(D3DKMTDestroyContext)                             \
   X(D3DKMTCreatePagingQueue)                          \
   X(D3DKMTDestroyPagingQueue)                         \
   X(D3DKMTCreateAllocation2)                          \
   X(D3DKMTDestroyAllocation2)                         \
   X(D3DKMTMakeResident)                               \
   X(D3DKMTEvict)                                      \
   X(D3DKMTMapGpuVirtualAddress)                       \
   X(D3DKMTFreeGpuVirtualAddress)                      \
   X(D3DKMTLock2)                                      \
   X(D3DKMTUnlock2)                                    \
   X(D3DKMTSubmitCommand)                              \
   X(D3DKMTCreateSynchronizationObject2)               \
   X(D3DKMTDestroySynchronizationObject)               \
   X(D3DKMTSignalSynchronizationObjectFromGpu2)        \
   X(D3DKMTWaitForSynchronizationObjectFromCpu)      \
   X(D3DKMTQueryResourceInfo)                         \
   X(D3DKMTOpenResource)                              \
   X(D3DKMTQueryResourceInfoFromNtHandle)             \
   X(D3DKMTOpenResourceFromNtHandle)

#define KMT_DECL(name) static NTSTATUS(APIENTRY *p##name)(void *);
KMT_FUNCS(KMT_DECL)
#undef KMT_DECL

static int g_debug = -1;

static void
dbg(const char *fmt, ...)
{
   if (g_debug < 0) {
      const char *e = getenv("FDKMT_DEBUG");
      g_debug = e && *e && *e != '0';
   }
   if (!g_debug)
      return;
   va_list ap;
   va_start(ap, fmt);
   fputs("fdkmt: ", stderr);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static int
load_kmt(void)
{
   static int loaded;
   if (loaded)
      return loaded > 0 ? 0 : -ENOSYS;
   HMODULE gdi = LoadLibraryA("gdi32.dll");
   if (!gdi) {
      loaded = -1;
      return -ENOSYS;
   }
#define KMT_LOAD(name)                                                    \
   p##name = (NTSTATUS(APIENTRY *)(void *))GetProcAddress(gdi, #name);    \
   if (!p##name) {                                                        \
      dbg("thieu export %s\n", #name);                                    \
      loaded = -1;                                                        \
      return -ENOSYS;                                                     \
   }
   KMT_FUNCS(KMT_LOAD)
#undef KMT_LOAD
   loaded = 1;
   return 0;
}

static int
st_to_errno(NTSTATUS st)
{
   switch ((unsigned long)st) {
   case 0xC0000017: return -ENOMEM;     /* NO_MEMORY */
   case 0xC000009A: return -ENOMEM;     /* INSUFFICIENT_RESOURCES */
   case 0xC000000D: return -EINVAL;     /* INVALID_PARAMETER */
   case 0xC0000102: return -EIO;
   default:         return -EIO;
   }
}

/* ------------------------------------------------------------------ */
/* Doi tuong                                                          */
/* ------------------------------------------------------------------ */


/* Thong ke (moi 2 s ghi MESA_LOG_DIR\fdkmt.log): so lan goi + tong thoi gian. */
enum { ST_ALLOC, ST_FREE, ST_MAP, ST_SUBMIT, ST_WAIT, ST_N };
static const char *st_name[ST_N] = { "alloc", "free", "map", "submit", "wait" };
static volatile LONG st_cnt[ST_N];
static volatile LONG64 st_ticks[ST_N];
static LONG64 st_last;
static FILE *st_log;

/* Megler: the statistics are kept and written only where MESA_LOG_DIR names
 * a folder for them, as the driver's other log files are (Megler's patch
 * megler-0025 to Mesa); else the core writes no file at all.
 */
static const char *
st_dir(void)
{
   static int known;
   static const char *dir;
   if (!known) {
      const char *e = getenv("MESA_LOG_DIR");
      dir = e && *e ? e : NULL;
      known = 1;
   }
   return dir;
}

static void
st_add(int i, LARGE_INTEGER t0)
{
   LARGE_INTEGER t1, f;
   const char *dir = st_dir();
   if (!dir)
      return;
   QueryPerformanceCounter(&t1);
   QueryPerformanceFrequency(&f);
   InterlockedIncrement(&st_cnt[i]);
   InterlockedExchangeAdd64(&st_ticks[i], t1.QuadPart - t0.QuadPart);
   if (!st_last)
      st_last = t1.QuadPart;
   if (t1.QuadPart - st_last < 2 * f.QuadPart)
      return;
   st_last = t1.QuadPart;
   if (!st_log) {
      char path[MAX_PATH];
      int n = snprintf(path, sizeof(path), "%s\\fdkmt.log", dir);
      if (n > 0 && n < (int)sizeof(path))
         st_log = _fsopen(path, "w", _SH_DENYNO);
   }
   if (!st_log)
      return;
   fprintf(st_log, "2s:");
   for (int k = 0; k < ST_N; k++) {
      LONG c = InterlockedExchange(&st_cnt[k], 0);
      LONG64 t = InterlockedExchange64(&st_ticks[k], 0);
      fprintf(st_log, "  %s %ld/%.1fms", st_name[k], c, t * 1000.0 / f.QuadPart);
   }
   fprintf(st_log, "\n");
   fflush(st_log);
}

struct kmt_bo {
   D3DKMT_HANDLE handle;
   uint64_t size;          /* da lam tron */
   uint64_t iova;
   void *map;              /* Lock2, NULL neu chua map */
   uint32_t last_fence;    /* fence cua submit gan nhat tham chieu BO */
   uint32_t kflags;        /* FD_KMT_BO_CACHED (anh huong private data) */
   D3DKMT_HANDLE resource; /* != 0: BO nhap tu resource dung chung (khong cache) */
};

struct fd_kmt {
   D3DKMT_HANDLE adapter;
   D3DKMT_HANDLE device;
   D3DKMT_HANDLE context;
   D3DKMT_HANDLE paging_queue;
   volatile UINT64 *paging_fence;
   D3DKMT_HANDLE paging_sync;     /* sync object cua paging queue */

   D3DKMT_HANDLE fence;               /* monitored fence */
   volatile UINT64 *fence_cpu;
   D3DGPU_VIRTUAL_ADDRESS fence_gpu;
   uint32_t seqno;

   CRITICAL_SECTION lock;
   DWORD wait_tls;          /* event cho moi thread */
   struct kmt_bo *bos;
   unsigned nr_bos, max_bos;

   /* Cache BO da giai phong (van resident + map VA) de tai su dung. */
   struct kmt_bo *free_bos;
   unsigned nr_free, max_free;
   uint64_t free_bytes;

   const struct fdkmt_gpu *gpu;   /* tu gpu_table.c */
   char soc[16];
   unsigned priv_size;

   /* Lich su submit (de dump khi GPU treo). */
   struct { uint32_t seq; struct fd_kmt_cmd cmd; } hist[16];
   unsigned hist_pos;
   uint8_t *priv;
};

static struct kmt_bo *
find_bo(struct fd_kmt *k, uint32_t handle)
{
   for (unsigned i = 0; i < k->nr_bos; i++)
      if (k->bos[i].handle == handle)
         return &k->bos[i];
   return NULL;
}

static HANDLE thread_event(struct fd_kmt *k);

static int
wait_paging(struct fd_kmt *k, UINT64 value)
{
   if (*k->paging_fence >= value)
      return 0;
   /* Cho bang event (Sleep(1) tren W10M ~15,6 ms: moi alloc mat ~26 ms). */
   HANDLE ev = thread_event(k);
   if (ev && k->paging_sync) {
      D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU w = { 0 };
      w.hDevice = k->device;
      w.ObjectCount = 1;
      w.ObjectHandleArray = &k->paging_sync;
      w.FenceValueArray = &value;
      w.hAsyncEvent = ev;
      if (pD3DKMTWaitForSynchronizationObjectFromCpu(&w) == 0) {
         if (WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0 ||
             *k->paging_fence >= value)
            return 0;
         return -ETIMEDOUT;
      }
   }
   for (unsigned i = 0; i < 200000; i++) {
      if (*k->paging_fence >= value)
         return 0;
      Sleep(0);
   }
   return -ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/* Phien                                                              */
/* ------------------------------------------------------------------ */

static void kmt_bo_destroy(struct fd_kmt *k, struct kmt_bo *b);

/* Lay so SoC tu ten UMD (vd "qcdx11um8996.dll" -> "8996"), tra gpu_table.c.
 * FDKMT_SOC=xxxx ep SoC (thu GPU chua co trong bang / debug).
 */
static bool
detect_gpu(struct fd_kmt *k)
{
   const char *force = getenv("FDKMT_SOC");
   if (force && *force) {
      strncpy(k->soc, force, sizeof(k->soc) - 1);
   } else {
      D3DKMT_UMDFILENAMEINFO um = { 0 };
      um.Version = KMTUMDVERSION_DX11;
      D3DKMT_QUERYADAPTERINFO q = { 0 };
      q.hAdapter = k->adapter;
      q.Type = KMTQAITYPE_UMDRIVERNAME;
      q.pPrivateDriverData = &um;
      q.PrivateDriverDataSize = sizeof(um);
      NTSTATUS st = pD3DKMTQueryAdapterInfo(&q);
      if (st) {
         dbg("QueryAdapterInfo(UMDRIVERNAME) 0x%08lx\n", (unsigned long)st);
         return false;
      }
      /* lay day chu so cuoi cung trong ten file */
      const WCHAR *name = um.UmdFileName, *p = name, *slash;
      for (slash = name; *p; p++)
         if (*p == L'\\' || *p == L'/')
            slash = p + 1;
      unsigned n = 0;
      for (p = slash; *p && *p != L'.'; p++) {
         if (*p >= L'0' && *p <= L'9') {
            if (n < sizeof(k->soc) - 1)
               k->soc[n++] = (char)*p;
         } else {
            n = 0;
         }
      }
      k->soc[n] = 0;
      dbg("UMD %ls -> SoC '%s'\n", slash, k->soc);
   }
   k->gpu = fdkmt_gpu_lookup(k->soc);
   if (!k->gpu) {
      dbg("SoC '%s' chua co trong gpu_table.c (xem src/fdkmt/README.md)\n", k->soc);
      return false;
   }
   if (!k->gpu->verified)
      dbg("canh bao: %s chua kiem chung (%s)\n", k->gpu->name,
          k->gpu->note ? k->gpu->note : "");
   return true;
}

int
fd_kmt_open(struct fd_kmt **out_kmt)
{
   NTSTATUS st;
   *out_kmt = NULL;

   int ret = load_kmt();
   if (ret)
      return ret;

   struct fd_kmt *k = calloc(1, sizeof(*k));
   if (!k)
      return -ENOMEM;
   InitializeCriticalSection(&k->lock);
   k->wait_tls = TlsAlloc();

   /* Adapter phan cung dau tien (Adreno). */
   D3DKMT_ADAPTERINFO infos[8];
   D3DKMT_ENUMADAPTERS2 ea = { 0 };
   ea.NumAdapters = 8;
   ea.pAdapters = infos;
   st = pD3DKMTEnumAdapters2(&ea);
   if (st) {
      dbg("EnumAdapters2 0x%08lx\n", (unsigned long)st);
      goto fail;
   }
   for (ULONG i = 0; i < ea.NumAdapters; i++) {
      D3DKMT_ADAPTERTYPE at = { 0 };
      D3DKMT_QUERYADAPTERINFO q = { 0 };
      q.hAdapter = infos[i].hAdapter;
      q.Type = KMTQAITYPE_ADAPTERTYPE;
      q.pPrivateDriverData = &at;
      q.PrivateDriverDataSize = sizeof(at);
      pD3DKMTQueryAdapterInfo(&q);
      if (!k->adapter && !at.SoftwareDevice) {
         k->adapter = infos[i].hAdapter;
      } else {
         D3DKMT_CLOSEADAPTER ca = { infos[i].hAdapter };
         pD3DKMTCloseAdapter(&ca);
      }
   }
   if (k->adapter && !detect_gpu(k)) {
      st = 0;
      goto fail;
   }
   if (!k->adapter) {
      dbg("khong co adapter phan cung\n");
      goto fail;
   }

   D3DKMT_CREATEDEVICE cd = { 0 };
   cd.hAdapter = k->adapter;
   st = pD3DKMTCreateDevice(&cd);
   if (st) {
      dbg("CreateDevice 0x%08lx\n", (unsigned long)st);
      goto fail;
   }
   k->device = cd.hDevice;

   const char *node_env = getenv("FDKMT_NODE");
   D3DKMT_CREATECONTEXTVIRTUAL cv = { 0 };
   cv.hDevice = k->device;
   cv.NodeOrdinal = node_env ? (UINT)atoi(node_env) : 0;
   cv.EngineAffinity = 1;
   cv.ClientHint = D3DKMT_CLIENTHINT_OPENGL;
   st = pD3DKMTCreateContextVirtual(&cv);
   if (st) {
      dbg("CreateContextVirtual(OPENGL) 0x%08lx, thu DX11\n", (unsigned long)st);
      cv.ClientHint = D3DKMT_CLIENTHINT_DX11;
      st = pD3DKMTCreateContextVirtual(&cv);
   }
   if (st) {
      dbg("CreateContextVirtual 0x%08lx\n", (unsigned long)st);
      goto fail;
   }
   k->context = cv.hContext;

   /* Thi nghiem treo GMEM: FDKMT_PRIO=<n> (-7..7) dat uu tien lap lich cua context */
   const char *prio_env = getenv("FDKMT_PRIO");
   if (prio_env && *prio_env) {
      NTSTATUS(APIENTRY *setprio)(void *) = (NTSTATUS(APIENTRY *)(void *))
         GetProcAddress(GetModuleHandleA("gdi32.dll"), "D3DKMTSetContextSchedulingPriority");
      D3DKMT_SETCONTEXTSCHEDULINGPRIORITY sp = { 0 };
      sp.hContext = k->context;
      sp.Priority = atoi(prio_env);
      NTSTATUS pst = setprio ? setprio(&sp) : (NTSTATUS)0xC0000002;
      dbg("SetContextSchedulingPriority(%d) 0x%08lx\n", sp.Priority, (unsigned long)pst);
   }

   D3DKMT_CREATEPAGINGQUEUE pq = { 0 };
   pq.hDevice = k->device;
   pq.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
   st = pD3DKMTCreatePagingQueue(&pq);
   if (st) {
      dbg("CreatePagingQueue 0x%08lx\n", (unsigned long)st);
      goto fail;
   }
   k->paging_queue = pq.hPagingQueue;
   k->paging_fence = (volatile UINT64 *)pq.FenceValueCPUVirtualAddress;
   k->paging_sync = pq.hSyncObject;

   D3DKMT_CREATESYNCHRONIZATIONOBJECT2 so = { 0 };
   so.hDevice = k->device;
   so.Info.Type = D3DDDI_MONITORED_FENCE;
   so.Info.MonitoredFence.InitialFenceValue = 0;
   st = pD3DKMTCreateSynchronizationObject2(&so);
   if (st) {
      dbg("CreateSynchronizationObject2(monitored fence) 0x%08lx\n",
          (unsigned long)st);
      goto fail;
   }
   k->fence = so.hSyncObject;
   k->fence_cpu = (volatile UINT64 *)so.Info.MonitoredFence.FenceValueCPUVirtualAddress;
   k->fence_gpu = so.Info.MonitoredFence.FenceValueGPUVirtualAddress;

   const char *ps = getenv("FDKMT_PRIV_SIZE");
   k->priv_size = ps ? (unsigned)strtoul(ps, NULL, 0) : k->gpu->kmd->submit_privdata_size;
   if (k->priv_size)
      k->priv = calloc(1, k->priv_size);

   dbg("open OK: adapter=0x%x device=0x%x context=0x%x pq=0x%x fence=0x%x "
       "fenceGPU=0x%llx priv=%u\n", k->adapter, k->device, k->context,
       k->paging_queue, k->fence, (unsigned long long)k->fence_gpu,
       k->priv_size);

   *out_kmt = k;
   return 0;

fail:
   fd_kmt_close(k);
   return st ? st_to_errno(st) : -ENODEV;
}

void
fd_kmt_close(struct fd_kmt *k)
{
   if (!k)
      return;

   while (k->nr_bos)
      fd_kmt_bo_free(k, k->bos[k->nr_bos - 1].handle);
   for (unsigned i = 0; i < k->nr_free; i++)
      kmt_bo_destroy(k, &k->free_bos[i]);
   free(k->free_bos);
   free(k->bos);
   free(k->priv);

   if (k->fence) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT d = { k->fence };
      pD3DKMTDestroySynchronizationObject(&d);
   }
   if (k->paging_queue) {
      D3DDDI_DESTROYPAGINGQUEUE d = { k->paging_queue };
      pD3DKMTDestroyPagingQueue(&d);
   }
   if (k->context) {
      D3DKMT_DESTROYCONTEXT d = { k->context };
      pD3DKMTDestroyContext(&d);
   }
   if (k->device) {
      D3DKMT_DESTROYDEVICE d = { k->device };
      pD3DKMTDestroyDevice(&d);
   }
   if (k->adapter) {
      D3DKMT_CLOSEADAPTER d = { k->adapter };
      pD3DKMTCloseAdapter(&d);
   }
   DeleteCriticalSection(&k->lock);
   free(k);
}

int
fd_kmt_get_info(struct fd_kmt *k, struct fd_kmt_info *info)
{
   const struct fdkmt_gpu *g = k->gpu;
   memset(info, 0, sizeof(*info));
   info->gpu_id = g->gpu_id;
   info->chip_id = g->chip_id;
   info->gmem_base = g->gmem_base;
   info->gmem_size = g->gmem_size;
   info->va_size = 0xFFFFFFFFull;
   info->nr_priorities = 1;
   info->max_freq = (uint64_t)g->max_freq_mhz * 1000000;
   return 0;
}

int
fd_kmt_get_timestamp(struct fd_kmt *k, uint64_t *ts)
{
   (void)k;
   *ts = 0;
   return -ENOSYS;
}

/* ------------------------------------------------------------------ */
/* BO                                                                 */
/* ------------------------------------------------------------------ */

/* Gioi han cache BO (MiB): FDKMT_CACHE_MB, mac dinh 64; 0 = tat cache. */
static uint64_t
cache_max(void)
{
   static int64_t v = -1;
   if (v < 0) {
      const char *e = getenv("FDKMT_CACHE_MB");
      v = (int64_t)(e ? strtoul(e, NULL, 0) : 64) * 1024 * 1024;
   }
   return (uint64_t)v;
}

static int kmt_bo_map_i(struct fd_kmt *k, uint32_t handle, void **out_ptr);

/* Lay BO cung kich thuoc/co tu cache; xoa 0 noi dung (Mesa co the gia dinh
 * BO moi = 0 nhu GEM). Tra true neu dung lai duoc.
 */
static bool
kmt_bo_reuse(struct fd_kmt *k, uint64_t bytes, uint32_t kflags,
             uint32_t *out_handle, uint64_t *out_iova)
{
   EnterCriticalSection(&k->lock);
   for (unsigned i = 0; i < k->nr_free; i++) {
      struct kmt_bo *f = &k->free_bos[i];
      if (f->size != bytes || f->kflags != kflags)
         continue;
      if (k->nr_bos == k->max_bos) {
         unsigned n = k->max_bos ? k->max_bos * 2 : 64;
         struct kmt_bo *b = realloc(k->bos, n * sizeof(*b));
         if (!b)
            break;
         k->bos = b;
         k->max_bos = n;
      }
      struct kmt_bo bo = *f;
      *f = k->free_bos[--k->nr_free];
      k->free_bytes -= bo.size;
      bo.last_fence = 0;
      k->bos[k->nr_bos++] = bo;
      LeaveCriticalSection(&k->lock);

      void *ptr = NULL;
      if (kmt_bo_map_i(k, bo.handle, &ptr) == 0 && ptr)
         memset(ptr, 0, (size_t)bo.size);
      *out_handle = bo.handle;
      *out_iova = bo.iova;
      return true;
   }
   LeaveCriticalSection(&k->lock);
   return false;
}

static int
kmt_bo_alloc_i(struct fd_kmt *k, uint64_t size, uint32_t flags,
                uint32_t *out_handle, uint64_t *out_iova)
{
   NTSTATUS st;
   *out_handle = 0;
   *out_iova = 0;

   const struct fdkmt_kmd_ops *kmd = k->gpu->kmd;
   uint64_t bytes = (size + kmd->bo_align - 1) & ~(kmd->bo_align - 1);
   uint32_t kflags = kmd->has_cpu_cached ? (flags & FD_KMT_BO_CACHED) : 0;

   if (kmt_bo_reuse(k, bytes, kflags, out_handle, out_iova))
      return 0;

   uint32_t priv[256];
   if (kmd->alloc_privdata_max > sizeof(priv))
      return -EINVAL;
   unsigned priv_bytes = kmd->alloc_privdata(priv, bytes, kflags);

   D3DDDI_ALLOCATIONINFO2 ai = { 0 };
   ai.pPrivateDriverData = priv;
   ai.PrivateDriverDataSize = priv_bytes;

   D3DKMT_CREATEALLOCATION ca = { 0 };
   ca.hDevice = k->device;
   ca.NumAllocations = 1;
   ca.pAllocationInfo2 = &ai;
   st = pD3DKMTCreateAllocation2(&ca);
   if (st) {
      dbg("CreateAllocation2(%llu) 0x%08lx\n", (unsigned long long)bytes,
          (unsigned long)st);
      return st_to_errno(st);
   }

   D3DDDI_MAKERESIDENT mr = { 0 };
   mr.hPagingQueue = k->paging_queue;
   mr.NumAllocations = 1;
   mr.AllocationList = &ai.hAllocation;
   st = pD3DKMTMakeResident(&mr);
   if (st == STATUS_PENDING)
      st = wait_paging(k, mr.PagingFenceValue) ? (NTSTATUS)0xC0000102 : 0;
   if (st) {
      dbg("MakeResident 0x%08lx\n", (unsigned long)st);
      goto fail_alloc;
   }

   D3DDDI_MAPGPUVIRTUALADDRESS map = { 0 };
   map.hPagingQueue = k->paging_queue;
   map.hAllocation = ai.hAllocation;
   map.SizeInPages = (D3DGPU_SIZE_T)(bytes / 4096);
   map.Protection.Write = 1;
   st = pD3DKMTMapGpuVirtualAddress(&map);
   if (st == STATUS_PENDING)
      st = wait_paging(k, map.PagingFenceValue) ? (NTSTATUS)0xC0000102 : 0;
   if (st) {
      dbg("MapGpuVirtualAddress 0x%08lx\n", (unsigned long)st);
      goto fail_alloc;
   }

   EnterCriticalSection(&k->lock);
   if (k->nr_bos == k->max_bos) {
      unsigned n = k->max_bos ? k->max_bos * 2 : 64;
      struct kmt_bo *b = realloc(k->bos, n * sizeof(*b));
      if (!b) {
         LeaveCriticalSection(&k->lock);
         goto fail_alloc;
      }
      k->bos = b;
      k->max_bos = n;
   }
   struct kmt_bo *bo = &k->bos[k->nr_bos++];
   memset(bo, 0, sizeof(*bo));
   bo->handle = ai.hAllocation;
   bo->kflags = kflags;
   bo->size = bytes;
   bo->iova = map.VirtualAddress;
   LeaveCriticalSection(&k->lock);

   dbg("bo_alloc %llu -> handle=0x%x iova=0x%llx\n", (unsigned long long)bytes,
       ai.hAllocation, (unsigned long long)map.VirtualAddress);
   *out_handle = ai.hAllocation;
   *out_iova = map.VirtualAddress;
   return 0;

fail_alloc: {
      D3DKMT_DESTROYALLOCATION2 da = { 0 };
      da.hDevice = k->device;
      da.phAllocationList = &ai.hAllocation;
      da.AllocationCount = 1;
      pD3DKMTDestroyAllocation2(&da);
   }
   return st ? st_to_errno(st) : -ENOMEM;
}

static void
kmt_bo_free_i(struct fd_kmt *k, uint32_t handle)
{
   /* GPU co the con dung: cho fence cuoi cung tham chieu BO. Cho TRUOC khi go
    * BO khoi danh sach (watchdog dump treo can tim thay BO chua IB).
    */
   EnterCriticalSection(&k->lock);
   struct kmt_bo *bo = find_bo(k, handle);
   uint32_t last = bo ? bo->last_fence : 0;
   LeaveCriticalSection(&k->lock);
   if (!bo)
      return;
   if (last)
      fd_kmt_fence_wait(k, last, 5000000000ull);

   EnterCriticalSection(&k->lock);
   bo = find_bo(k, handle);
   if (!bo) {
      LeaveCriticalSection(&k->lock);
      return;
   }
   struct kmt_bo b = *bo;
   *bo = k->bos[--k->nr_bos];
   if (!b.resource && k->free_bytes + b.size <= cache_max()) {
      if (k->nr_free == k->max_free) {
         unsigned n = k->max_free ? k->max_free * 2 : 64;
         struct kmt_bo *f = realloc(k->free_bos, n * sizeof(*f));
         if (f) {
            k->free_bos = f;
            k->max_free = n;
         }
      }
      if (k->nr_free < k->max_free) {
         k->free_bos[k->nr_free++] = b;
         k->free_bytes += b.size;
         LeaveCriticalSection(&k->lock);
         return;
      }
   }
   LeaveCriticalSection(&k->lock);
   kmt_bo_destroy(k, &b);
}

static void
kmt_bo_destroy(struct fd_kmt *k, struct kmt_bo *pb)
{
   struct kmt_bo b = *pb;
   if (b.map) {
      D3DKMT_UNLOCK2 ul = { 0 };
      ul.hDevice = k->device;
      ul.hAllocation = b.handle;
      pD3DKMTUnlock2(&ul);
   }
   if (b.iova) {
      D3DKMT_FREEGPUVIRTUALADDRESS fv = { 0 };
      fv.hAdapter = k->adapter;
      fv.BaseAddress = b.iova;
      fv.Size = b.size;
      pD3DKMTFreeGpuVirtualAddress(&fv);
   }
   D3DKMT_DESTROYALLOCATION2 da = { 0 };
   da.hDevice = k->device;
   if (b.resource) {
      da.hResource = b.resource; /* dong ca resource da mo */
   } else {
      da.phAllocationList = &b.handle;
      da.AllocationCount = 1;
   }
   pD3DKMTDestroyAllocation2(&da);
}

/* Nhap allocation dung chung (vd texture D3D11 MISC_SHARED) vao device cua
 * loi: mo resource (handle NT neu is_nt, khong thi handle KMT toan cuc), lay
 * allocation dau tien, MakeResident + map GPU VA. Kich thuoc lay tu private
 * data cua KMD (kmd->alloc_size_from_privdata).
 */
int
fd_kmt_bo_import(struct fd_kmt *k, void *share, int is_nt, uint32_t *out_handle,
                 uint64_t *out_iova, uint64_t *out_size)
{
   static char rt[4096];
   NTSTATUS st;
   UINT total = 0, rsize = 0, nalloc = 0, rtsize = 0;
   if (is_nt) {
      D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE q = { 0 };
      q.hDevice = k->device;
      q.hNtHandle = share;
      q.pPrivateRuntimeData = rt;
      q.PrivateRuntimeDataSize = sizeof(rt);
      st = pD3DKMTQueryResourceInfoFromNtHandle(&q);
      rtsize = q.PrivateRuntimeDataSize;
      total = q.TotalPrivateDriverDataSize;
      rsize = q.ResourcePrivateDriverDataSize;
      nalloc = q.NumAllocations;
   } else {
      D3DKMT_QUERYRESOURCEINFO q = { 0 };
      q.hDevice = k->device;
      q.hGlobalShare = (D3DKMT_HANDLE)(UINT_PTR)share;
      q.pPrivateRuntimeData = rt;
      q.PrivateRuntimeDataSize = sizeof(rt);
      st = pD3DKMTQueryResourceInfo(&q);
      rtsize = q.PrivateRuntimeDataSize;
      total = q.TotalPrivateDriverDataSize;
      rsize = q.ResourcePrivateDriverDataSize;
      nalloc = q.NumAllocations;
   }
   if (st || nalloc < 1) {
      dbg("QueryResourceInfo 0x%08lx allocs %u\n", (unsigned long)st, nalloc);
      return st ? st_to_errno(st) : -EINVAL;
   }

   char *tot = calloc(1, total + 16);
   char *rp = calloc(1, rsize + 16);
   D3DDDI_OPENALLOCATIONINFO2 *oi = calloc(nalloc, sizeof(*oi));
   D3DKMT_HANDLE hres = 0;
   if (!tot || !rp || !oi) {
      st = (NTSTATUS)0xC0000017;
      goto out;
   }
   if (is_nt) {
      D3DKMT_OPENRESOURCEFROMNTHANDLE o = { 0 };
      o.hDevice = k->device;
      o.hNtHandle = share;
      o.NumAllocations = nalloc;
      o.pOpenAllocationInfo2 = oi;
      o.pPrivateRuntimeData = rt;
      o.PrivateRuntimeDataSize = rtsize;
      o.pResourcePrivateDriverData = rp;
      o.ResourcePrivateDriverDataSize = rsize;
      o.pTotalPrivateDriverDataBuffer = tot;
      o.TotalPrivateDriverDataBufferSize = total;
      st = pD3DKMTOpenResourceFromNtHandle(&o);
      hres = o.hResource;
   } else {
      D3DKMT_OPENRESOURCE o = { 0 };
      o.hDevice = k->device;
      o.hGlobalShare = (D3DKMT_HANDLE)(UINT_PTR)share;
      o.NumAllocations = nalloc;
      o.pOpenAllocationInfo2 = oi;
      o.pPrivateRuntimeData = rt;
      o.PrivateRuntimeDataSize = rtsize;
      o.pResourcePrivateDriverData = rp;
      o.ResourcePrivateDriverDataSize = rsize;
      o.pTotalPrivateDriverDataBuffer = tot;
      o.TotalPrivateDriverDataBufferSize = total;
      st = pD3DKMTOpenResource(&o);
      hres = o.hResource;
   }
   if (st) {
      dbg("OpenResource 0x%08lx\n", (unsigned long)st);
      goto out;
   }

   D3DKMT_HANDLE h = oi[0].hAllocation;
   const struct fdkmt_kmd_ops *kmd = k->gpu->kmd;
   {
      const uint32_t *pw = oi[0].pPrivateDriverData;
      unsigned n = oi[0].PrivateDriverDataSize / 4;
      dbg("import privdata %u B:", oi[0].PrivateDriverDataSize);
      for (unsigned i = 0; pw && i < n && i < 0x20; i++)
         dbg(" [%x]=%x", i, pw[i]);
      dbg("\n");
   }
   uint64_t bytes = kmd->alloc_size_from_privdata
                       ? kmd->alloc_size_from_privdata(oi[0].pPrivateDriverData,
                                                       oi[0].PrivateDriverDataSize)
                       : 0;
   if (!bytes) {
      st = (NTSTATUS)0xC000000D;
      goto fail_res;
   }
   bytes = (bytes + 0xFFF) & ~0xFFFull;

   D3DDDI_MAKERESIDENT mr = { 0 };
   mr.hPagingQueue = k->paging_queue;
   mr.NumAllocations = 1;
   mr.AllocationList = &h;
   st = pD3DKMTMakeResident(&mr);
   if (st == STATUS_PENDING)
      st = wait_paging(k, mr.PagingFenceValue) ? (NTSTATUS)0xC0000102 : 0;
   if (st) {
      dbg("import MakeResident 0x%08lx\n", (unsigned long)st);
      goto fail_res;
   }

   D3DDDI_MAPGPUVIRTUALADDRESS map = { 0 };
   map.hPagingQueue = k->paging_queue;
   map.hAllocation = h;
   map.SizeInPages = (D3DGPU_SIZE_T)(bytes / 4096);
   map.Protection.Write = 1;
   st = pD3DKMTMapGpuVirtualAddress(&map);
   if (st == STATUS_PENDING)
      st = wait_paging(k, map.PagingFenceValue) ? (NTSTATUS)0xC0000102 : 0;
   if (st) {
      dbg("import MapGpuVirtualAddress 0x%08lx\n", (unsigned long)st);
      goto fail_res;
   }

   EnterCriticalSection(&k->lock);
   if (k->nr_bos == k->max_bos) {
      unsigned n = k->max_bos ? k->max_bos * 2 : 64;
      struct kmt_bo *nb = realloc(k->bos, n * sizeof(*nb));
      if (!nb) {
         LeaveCriticalSection(&k->lock);
         st = (NTSTATUS)0xC0000017;
         goto fail_res;
      }
      k->bos = nb;
      k->max_bos = n;
   }
   struct kmt_bo *bo = &k->bos[k->nr_bos++];
   memset(bo, 0, sizeof(*bo));
   bo->handle = h;
   bo->size = bytes;
   bo->iova = map.VirtualAddress;
   bo->resource = hres;
   LeaveCriticalSection(&k->lock);

   dbg("bo_import -> handle=0x%x iova=0x%llx size %llu\n", h,
       (unsigned long long)map.VirtualAddress, (unsigned long long)bytes);
   *out_handle = h;
   *out_iova = map.VirtualAddress;
   *out_size = bytes;
   goto out;

fail_res: {
      D3DKMT_DESTROYALLOCATION2 da = { 0 };
      da.hDevice = k->device;
      da.hResource = hres;
      pD3DKMTDestroyAllocation2(&da);
   }
out:
   free(tot);
   free(rp);
   free(oi);
   return st ? st_to_errno(st) : 0;
}

static int
kmt_bo_map_i(struct fd_kmt *k, uint32_t handle, void **out_ptr)
{
   *out_ptr = NULL;
   EnterCriticalSection(&k->lock);
   struct kmt_bo *bo = find_bo(k, handle);
   if (!bo) {
      LeaveCriticalSection(&k->lock);
      return -EINVAL;
   }
   if (!bo->map) {
      D3DKMT_LOCK2 lk = { 0 };
      lk.hDevice = k->device;
      lk.hAllocation = handle;
      NTSTATUS st = pD3DKMTLock2(&lk);
      if (st) {
         LeaveCriticalSection(&k->lock);
         dbg("Lock2 0x%08lx\n", (unsigned long)st);
         return st_to_errno(st);
      }
      bo->map = lk.pData;
   }
   *out_ptr = bo->map;
   LeaveCriticalSection(&k->lock);
   return 0;
}

/* Thu nghiem: Unlock2 + Lock2 lai (xem KMD co don cache CPU khong).
 * Tra con tro moi (co the khac con tro cu).
 */
int
fd_kmt_bo_cpu_sync(struct fd_kmt *k, uint32_t handle, void **out_ptr)
{
   EnterCriticalSection(&k->lock);
   struct kmt_bo *bo = find_bo(k, handle);
   if (!bo || !bo->map) {
      LeaveCriticalSection(&k->lock);
      return -EINVAL;
   }
   D3DKMT_UNLOCK2 ul = { 0 };
   ul.hDevice = k->device;
   ul.hAllocation = handle;
   NTSTATUS st = pD3DKMTUnlock2(&ul);
   if (st) {
      LeaveCriticalSection(&k->lock);
      return st_to_errno(st);
   }
   D3DKMT_LOCK2 lk = { 0 };
   lk.hDevice = k->device;
   lk.hAllocation = handle;
   st = pD3DKMTLock2(&lk);
   bo->map = st ? NULL : lk.pData;
   *out_ptr = bo->map;
   LeaveCriticalSection(&k->lock);
   return st ? st_to_errno(st) : 0;
}

int
fd_kmt_bo_wait(struct fd_kmt *k, uint32_t handle, uint32_t op,
               uint64_t timeout_ns)
{
   (void)op;
   EnterCriticalSection(&k->lock);
   struct kmt_bo *bo = find_bo(k, handle);
   uint32_t f = bo ? bo->last_fence : 0;
   LeaveCriticalSection(&k->lock);
   if (!f)
      return 0;
   if (timeout_ns == 0)
      return (*k->fence_cpu >= f) ? 0 : -EBUSY;
   return fd_kmt_fence_wait(k, f, timeout_ns);
}

/* ------------------------------------------------------------------ */
/* Submit / fence                                                     */
/* ------------------------------------------------------------------ */

/* FDKMT_DUMP_FAIL=1: ghi IB bi KMD tu choi ra %TEMP%/fdkmt_fail_<n>.bin
 * (header 16 byte: iova lo/hi, so dword, NTSTATUS; sau do la cac dword IB).
 */
static void
dump_failed_ib(struct fd_kmt *k, const struct fd_kmt_cmd *c, NTSTATUS st)
{
   static LONG n;
   const char *e = getenv("FDKMT_DUMP_FAIL");
   if (!e || *e == '0')
      return;
   struct kmt_bo *bo = find_bo(k, c->handle);
   if (!bo || !bo->map || c->iova < bo->iova)
      return;
   const uint8_t *p = (const uint8_t *)bo->map + (size_t)(c->iova - bo->iova);
   char path[MAX_PATH];
   DWORD len = GetTempPathA(MAX_PATH, path);
   if (!len || len > MAX_PATH - 32)
      return;
   sprintf(path + len, "fdkmt_fail_%ld.bin", (long)InterlockedIncrement(&n));
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   uint32_t hdr[4] = { (uint32_t)c->iova, (uint32_t)(c->iova >> 32), c->size_dw,
                       (uint32_t)st };
   fwrite(hdr, 4, 4, f);
   fwrite(p, 4, c->size_dw, f);
   fclose(f);
}

static int
kmt_submit_i(struct fd_kmt *k, const struct fd_kmt_cmd *cmds,
              uint32_t nr_cmds, const struct fd_kmt_bo_ref *bos,
              uint32_t nr_bos, uint32_t *out_fence)
{
   NTSTATUS st;
   *out_fence = 0;

   EnterCriticalSection(&k->lock);

   for (uint32_t i = 0; i < nr_cmds; i++) {
      D3DKMT_SUBMITCOMMAND sc = { 0 };
      sc.Commands = cmds[i].iova;
      sc.CommandLength = cmds[i].size_dw * 4;
      sc.BroadcastContextCount = 1;
      sc.BroadcastContext[0] = k->context;
      sc.pPrivateDriverData = k->priv;
      sc.PrivateDriverDataSize = k->priv_size;
      st = pD3DKMTSubmitCommand(&sc);
      if (st) {
         LeaveCriticalSection(&k->lock);
         dbg("SubmitCommand(iova=0x%llx len=%u priv=%u) 0x%08lx\n",
             (unsigned long long)cmds[i].iova, cmds[i].size_dw * 4,
             k->priv_size, (unsigned long)st);
         dump_failed_ib(k, &cmds[i], st);
         return st_to_errno(st);
      }
   }

   uint32_t seq = ++k->seqno;
   for (uint32_t i = 0; i < nr_cmds; i++) {
      k->hist[k->hist_pos % 16].seq = seq;
      k->hist[k->hist_pos % 16].cmd = cmds[i];
      k->hist_pos++;
   }
   UINT64 value = seq;
   D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 sg = { 0 };
   sg.ObjectCount = 1;
   sg.ObjectHandleArray = &k->fence;
   sg.BroadcastContextCount = 1;
   sg.BroadcastContextArray = &k->context;
   sg.MonitoredFenceValueArray = &value;
   st = pD3DKMTSignalSynchronizationObjectFromGpu2(&sg);
   if (st) {
      LeaveCriticalSection(&k->lock);
      dbg("SignalSynchronizationObjectFromGpu2 0x%08lx\n", (unsigned long)st);
      return st_to_errno(st);
   }

   for (uint32_t i = 0; i < nr_bos; i++) {
      struct kmt_bo *bo = find_bo(k, bos[i].handle);
      if (bo)
         bo->last_fence = seq;
   }
   for (uint32_t i = 0; i < nr_cmds; i++) {
      struct kmt_bo *bo = find_bo(k, cmds[i].handle);
      if (bo)
         bo->last_fence = seq;
   }
   LeaveCriticalSection(&k->lock);

   dbg("submit %u IB -> fence %u\n", nr_cmds, seq);
   *out_fence = seq;
   return 0;
}


/* ---- Dump khi GPU treo (FDKMT_DUMP_HANG=1) ------------------------------
 * File %TEMP%/fdkmt_hang_<n>.bin: chuoi khoi { u32 magic 'IBDP', u32 fence,
 * u64 iova, u32 so dword, u32 cap (1 = IB1, 2 = IB2), dword... }.
 */
static FILE *
hang_open(void)
{
   static LONG n;
   char path[MAX_PATH];
   DWORD len = GetTempPathA(MAX_PATH, path);
   if (!len || len > MAX_PATH - 32)
      return NULL;
   sprintf(path + len, "fdkmt_hang_%ld.bin", (long)InterlockedIncrement(&n));
   return fopen(path, "wb");
}

static const uint32_t *
iova_to_ptr(struct fd_kmt *k, uint64_t iova, uint32_t dw)
{
   for (unsigned i = 0; i < k->nr_bos; i++) {
      struct kmt_bo *b = &k->bos[i];
      if (b->map && iova >= b->iova && iova + dw * 4ull <= b->iova + b->size)
         return (const uint32_t *)((const uint8_t *)b->map + (size_t)(iova - b->iova));
   }
   return NULL;
}

static void
hang_write(FILE *f, uint32_t fence, uint64_t iova, const uint32_t *p, uint32_t dw,
           uint32_t level)
{
   uint32_t hdr[6] = { 0x50444249u, fence, (uint32_t)iova, (uint32_t)(iova >> 32), dw,
                       level };
   fwrite(hdr, 4, 6, f);
   fwrite(p, 4, dw, f);
}

static void
dump_hang(struct fd_kmt *k, uint32_t waiting)
{
   const char *e = getenv("FDKMT_DUMP_HANG");
   if (!e || *e == '0')
      return;
   EnterCriticalSection(&k->lock);
   FILE *f = hang_open();
   if (f) {
      uint64_t done = *k->fence_cpu;
      for (unsigned h = 0; h < 16; h++) {
         const struct fd_kmt_cmd *c = &k->hist[h].cmd;
         if (!k->hist[h].seq || k->hist[h].seq <= done)
            continue;
         const uint32_t *p = iova_to_ptr(k, c->iova, c->size_dw);
         if (!p)
            continue;
         hang_write(f, k->hist[h].seq, c->iova, p, c->size_dw, 1);
         /* IB2: CP_INDIRECT_BUFFER(_PFE/_PFD/_CHAIN) = pkt7 op 0x3f/0x37/0x57 */
         for (uint32_t i = 0; i < c->size_dw;) {
            uint32_t hd = p[i];
            if ((hd >> 28) != 7) {
               i++;
               continue;
            }
            uint32_t op = (hd >> 16) & 0x7f, cnt = hd & 0x3fff;
            if ((op == 0x3f || op == 0x37 || op == 0x57) && cnt >= 3 && i + 3 < c->size_dw) {
               uint64_t ib = p[i + 1] | ((uint64_t)p[i + 2] << 32);
               uint32_t sz = p[i + 3] & 0xfffff;
               const uint32_t *q = iova_to_ptr(k, ib, sz);
               if (q)
                  hang_write(f, k->hist[h].seq, ib, q, sz, 2);
            }
            i += 1 + cnt;
         }
      }
      /* Danh sach BO dang song: khoi 'BOLS' { u32 magic, u32 n, n x {u64 iova,
       * u64 size} } - de doi chieu dia chi trong IB voi BO con ton tai.
       */
      uint32_t bh[2] = { 0x534c4f42u, k->nr_bos };
      fwrite(bh, 4, 2, f);
      for (unsigned i = 0; i < k->nr_bos; i++) {
         uint64_t e2[2] = { k->bos[i].iova, k->bos[i].size };
         fwrite(e2, 8, 2, f);
      }
      fclose(f);
   }
   dbg("GPU treo? cho fence %u, da xong %llu -> da dump IB\n", waiting,
       (unsigned long long)*k->fence_cpu);
   LeaveCriticalSection(&k->lock);
}

static HANDLE
thread_event(struct fd_kmt *k)
{
   HANDLE ev = (HANDLE)TlsGetValue(k->wait_tls);
   if (!ev) {
      ev = CreateEventA(NULL, FALSE, FALSE, NULL);
      TlsSetValue(k->wait_tls, ev);
   }
   return ev;
}

static int
kmt_fence_wait_i(struct fd_kmt *k, uint32_t fence, uint64_t timeout_ns)
{
   if (*k->fence_cpu >= fence)
      return 0;
   if (timeout_ns == 0)
      return -ETIMEDOUT;

   /* Cho bang event cua kernel (Sleep(1) tren W10M ~15,6 ms -> rat cham). */
   HANDLE ev = thread_event(k);
   UINT64 value = fence;
   D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU w = { 0 };
   w.hDevice = k->device;
   w.ObjectCount = 1;
   w.ObjectHandleArray = &k->fence;
   w.FenceValueArray = &value;
   w.hAsyncEvent = ev;
   NTSTATUS st = ev ? pD3DKMTWaitForSynchronizationObjectFromCpu(&w) : -1;
   if (st == 0) {
      DWORD ms = timeout_ns == UINT64_MAX ? INFINITE
                 : (DWORD)((timeout_ns + 999999) / 1000000);
      DWORD r;
      if (ms > 2000) {
         /* sau 2 s chua xong: co the GPU treo -> dump IB roi cho tiep */
         r = WaitForSingleObject(ev, 2000);
         if (r != WAIT_OBJECT_0 && *k->fence_cpu < fence) {
            dump_hang(k, fence);
            r = WaitForSingleObject(ev, ms == INFINITE ? INFINITE : ms - 2000);
         }
      } else {
         r = WaitForSingleObject(ev, ms);
      }
      if (r == WAIT_OBJECT_0 || *k->fence_cpu >= fence)
         return 0;
      dbg("fence_wait %u timeout (hien tai %llu)\n", fence,
          (unsigned long long)*k->fence_cpu);
      return -ETIMEDOUT;
   }

   /* Du phong: poll. */
   dbg("WaitForSynchronizationObjectFromCpu 0x%08lx, poll\n", (unsigned long)st);
   ULONGLONG start = GetTickCount64();
   ULONGLONG limit_ms = timeout_ns == UINT64_MAX ? ~0ull : timeout_ns / 1000000;
   for (unsigned spin = 0;; spin++) {
      if (*k->fence_cpu >= fence)
         return 0;
      if (GetTickCount64() - start >= limit_ms)
         return -ETIMEDOUT;
      Sleep(spin < 1000 ? 0 : 1);
   }
}

/* ------------------------------------------------------------------ */
/* API cong khai: bao lop do thoi gian                               */
/* ------------------------------------------------------------------ */
#define TIMED(i, expr) do { LARGE_INTEGER t0_; QueryPerformanceCounter(&t0_); expr; st_add(i, t0_); } while (0)

int
fd_kmt_bo_alloc(struct fd_kmt *k, uint64_t size, uint32_t flags,
                uint32_t *out_handle, uint64_t *out_iova)
{
   int r;
   TIMED(ST_ALLOC, r = kmt_bo_alloc_i(k, size, flags, out_handle, out_iova));
   return r;
}

void
fd_kmt_bo_free(struct fd_kmt *k, uint32_t handle)
{
   TIMED(ST_FREE, kmt_bo_free_i(k, handle));
}

int
fd_kmt_bo_map(struct fd_kmt *k, uint32_t handle, void **out_ptr)
{
   int r;
   TIMED(ST_MAP, r = kmt_bo_map_i(k, handle, out_ptr));
   return r;
}

int
fd_kmt_submit(struct fd_kmt *k, const struct fd_kmt_cmd *cmds,
              uint32_t nr_cmds, const struct fd_kmt_bo_ref *bos,
              uint32_t nr_bos, uint32_t *out_fence)
{
   int r;
   TIMED(ST_SUBMIT, r = kmt_submit_i(k, cmds, nr_cmds, bos, nr_bos, out_fence));
   return r;
}

int
fd_kmt_fence_wait(struct fd_kmt *k, uint32_t fence, uint64_t timeout_ns)
{
   int r;
   TIMED(ST_WAIT, r = kmt_fence_wait_i(k, fence, timeout_ns));
   return r;
}
