/*
 * fdkmt_share.c — tham do allocation dung chung D3D11 <-> D3DKMT (buoc 1 cua
 * present khong copy).
 *
 * Tao texture D3D11 (render target, MISC_SHARED) -> lay shared handle (KMT) ->
 * mo bang D3DKMTQueryResourceInfo + D3DKMTOpenResource tren device KMT rieng ->
 * in kich thuoc va dump private data cua resource + tung allocation ra
 * share_res.bin / share_alloc<N>.bin (de so voi layout private data cua
 * kmd_qcom.c: pitch, tiling, UBWC...).
 *
 *   fdkmt_share.exe [width height [dxgi_format]]   (mac dinh 1080 1920 28=RGBA8)
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


typedef NTSTATUS(APIENTRY *kmt_fn)(void *);
static kmt_fn
kmt(const char *name)
{
   kmt_fn f = (kmt_fn)GetProcAddress(LoadLibraryA("gdi32.dll"), name);
   if (!f)
      printf("thieu %s\n", name);
   return f;
}
#define K(name) kmt(#name)

static void
dump(const char *name, const void *p, unsigned n)
{
   FILE *f = fopen(name, "wb");
   if (f) {
      fwrite(p, 1, n, f);
      fclose(f);
   }
   const unsigned *w = (const unsigned *)p;
   printf("  %s (%u byte):", name, n);
   for (unsigned i = 0; i < n / 4 && i < 128; i++)
      printf("%s%08x", i % 8 ? " " : "\n    ", w[i]);
   printf("\n");
}

/* ---- Doc noi dung allocation dung chung bang GPU qua loi fd_kmt ---- */
#include "fd_kmt.h"

#define CP_NOP 16
#define CP_MEM_TO_MEM 0x73

static unsigned
odd_parity(unsigned v)
{
   return (~0x6996u >> ((v ^ (v >> 16) ^ (v >> 8) ^ (v >> 4)) & 0xf)) & 1;
}

static unsigned
pkt7(unsigned opcode, unsigned cnt)
{
   return 0x70000000u | (cnt & 0x3fff) | (odd_parity(cnt) << 15) |
          ((opcode & 0x7f) << 16) | (odd_parity(opcode) << 23);
}

/* Cac vung (byte offset) can doc: dau allocation va quanh cac pitch du doan. */
static const unsigned regions[][2] = {
   { 0, 2048 }, { 4320, 256 }, { 4352, 256 }, { 4608, 256 }, { 8192, 256 },
   { 16384, 256 },
};

static void
gpu_readback(HANDLE share, int nt)
{
   struct fd_kmt *k = NULL;
   int ret = fd_kmt_open(&k);
   printf("fd_kmt_open %d\n", ret);
   if (ret)
      return;
   uint32_t hs = 0, hc = 0, hd = 0;
   uint64_t vs = 0, vc = 0, vd = 0, ssz = 0;
   ret = fd_kmt_bo_import(k, share, nt, &hs, &vs, &ssz);
   printf("bo_import %d handle 0x%x iova 0x%llx size 0x%llx\n", ret, hs,
          (unsigned long long)vs, (unsigned long long)ssz);
   if (ret)
      return;
   unsigned total = 0;
   for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++)
      total += regions[r][1];
   fd_kmt_bo_alloc(k, 65536, FD_KMT_BO_RING, &hc, &vc);
   fd_kmt_bo_alloc(k, total, 0, &hd, &vd);
   volatile uint32_t *cmd, *dst;
   fd_kmt_bo_map(k, hc, (void **)&cmd);
   fd_kmt_bo_map(k, hd, (void **)&dst);
   memset((void *)dst, 0xAB, total);

   unsigned c = 0, o = 0;
   for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++)
      for (unsigned b = 0; b < regions[r][1]; b += 4, o += 4) {
         uint64_t src = vs + regions[r][0] + b, d = vd + o;
         cmd[c++] = pkt7(CP_MEM_TO_MEM, 5);
         cmd[c++] = 0;
         cmd[c++] = (uint32_t)d;
         cmd[c++] = (uint32_t)(d >> 32);
         cmd[c++] = (uint32_t)src;
         cmd[c++] = (uint32_t)(src >> 32);
      }
   while (c & 3)
      cmd[c++] = pkt7(CP_NOP, 0);
   printf("IB %u dword\n", c);
   struct fd_kmt_cmd ib = { vc, c, hc };
   struct fd_kmt_bo_ref refs[3] = { { hc, FD_KMT_PREP_READ }, { hs, FD_KMT_PREP_READ },
                                    { hd, FD_KMT_PREP_WRITE } };
   uint32_t f = 0;
   ret = fd_kmt_submit(k, &ib, 1, refs, 3, &f);
   if (!ret)
      ret = fd_kmt_fence_wait(k, f, 3000000000ull);
   printf("submit/wait %d\n", ret);
   if (ret)
      return;

   o = 0;
   for (unsigned r = 0; r < sizeof(regions) / sizeof(regions[0]); r++) {
      printf("byte 0x%x:", regions[r][0]);
      for (unsigned i = 0; i < regions[r][1] / 4; i++, o += 4)
         if (i < 16 || (r == 0 && (i % 64) == 0))
            printf("%s%08x", (i % 8) ? " " : "\n    ", dst[o / 4]);
      printf("\n");
   }
   fd_kmt_bo_free(k, hs);
   fd_kmt_close(k);
}

int
main(int argc, char **argv)
{
   UINT w = argc > 2 ? (UINT)atoi(argv[1]) : 1080;
   UINT h = argc > 2 ? (UINT)atoi(argv[2]) : 1920;
   DXGI_FORMAT fmt = argc > 3 ? (DXGI_FORMAT)atoi(argv[3]) : DXGI_FORMAT_R8G8B8A8_UNORM;

   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *dctx = NULL;
   HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                  D3D11_SDK_VERSION, &dev, NULL, &dctx);
   printf("D3D11CreateDevice 0x%08lx\n", hr);
   if (FAILED(hr))
      return 1;

   D3D11_TEXTURE2D_DESC td = { 0 };
   td.Width = w;
   td.Height = h;
   td.MipLevels = td.ArraySize = 1;
   td.Format = fmt;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_DEFAULT;
   td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   int nt = getenv("SHARE_NT") != NULL;
   td.MiscFlags = nt ? (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
                     : D3D11_RESOURCE_MISC_SHARED;
   ID3D11Texture2D *tex = NULL;
   hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
   printf("CreateTexture2D %ux%u fmt %d SHARED: 0x%08lx\n", w, h, fmt, hr);
   if (FAILED(hr))
      return 1;

   IDXGIResource *dres = NULL;
   HANDLE share = NULL;
   ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&dres);
   if (nt) {
      IDXGIResource1 *r1 = NULL;
      ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&r1);
      hr = r1 ? IDXGIResource1_CreateSharedHandle(r1, NULL, GENERIC_ALL, NULL, &share) : E_FAIL;
   } else
      hr = dres ? IDXGIResource_GetSharedHandle(dres, &share) : E_FAIL;
   printf("GetSharedHandle 0x%08lx handle %p\n", hr, share);
   if (FAILED(hr))
      return 1;

   ID3D11DeviceContext_Flush(dctx); /* resource dung chung tao tre toi Flush */

   /* device KMT rieng tren cung adapter (LUID cua DXGI) */
   IDXGIDevice *xdev = NULL;
   IDXGIAdapter *ad = NULL;
   DXGI_ADAPTER_DESC ades;
   ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xdev);
   IDXGIDevice_GetAdapter(xdev, &ad);
   IDXGIAdapter_GetDesc(ad, &ades);

   D3DKMT_OPENADAPTERFROMLUID oa = { 0 };
   oa.AdapterLuid = ades.AdapterLuid;
   NTSTATUS st = K(D3DKMTOpenAdapterFromLuid)(&oa);
   printf("OpenAdapterFromLuid 0x%08lx\n", st);
   if (st)
      return 1;
   D3DKMT_CREATEDEVICE cd = { 0 };
   cd.hAdapter = oa.hAdapter;
   st = K(D3DKMTCreateDevice)(&cd);
   printf("CreateDevice 0x%08lx (sizeof %u, offset hDevice %u):", st, (unsigned)sizeof(cd),
          (unsigned)((char *)&cd.hDevice - (char *)&cd));
   for (unsigned i = 0; i < sizeof(cd) / 4; i++)
      printf(" %08x", ((unsigned *)&cd)[i]);
   printf("\n");
   if (st)
      return 1;

   printf("hAdapter 0x%x hDevice 0x%x\n", oa.hAdapter, cd.hDevice);
   D3DKMT_QUERYRESOURCEINFO qi = { 0 };
   qi.hDevice = cd.hDevice;
   qi.hGlobalShare = (D3DKMT_HANDLE)(UINT_PTR)share;
   static char rtbuf[65536];
   int nullrt = getenv("SHARE_NULLRT") != NULL;
   qi.pPrivateRuntimeData = nullrt ? NULL : rtbuf;
   qi.PrivateRuntimeDataSize = nullrt ? 0 : sizeof(rtbuf);
   D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE qn = { 0 };
   if (nt) {
      qn.hDevice = cd.hDevice;
      qn.hNtHandle = share;
      qn.pPrivateRuntimeData = nullrt ? NULL : rtbuf;
      qn.PrivateRuntimeDataSize = nullrt ? 0 : sizeof(rtbuf);
      if (getenv("SHARE_WIN32U")) {
         NTSTATUS(APIENTRY *f)(void *) = (NTSTATUS(APIENTRY *)(void *))GetProcAddress(
            LoadLibraryA("win32u.dll"), "NtGdiDdDDIQueryResourceInfoFromNtHandle");
         printf("win32u fn %p\n", (void *)f);
         st = f ? f(&qn) : 0xC0000002;
      } else
         st = K(D3DKMTQueryResourceInfoFromNtHandle)(&qn);
      qi.PrivateRuntimeDataSize = qn.PrivateRuntimeDataSize;
      qi.TotalPrivateDriverDataSize = qn.TotalPrivateDriverDataSize;
      qi.ResourcePrivateDriverDataSize = qn.ResourcePrivateDriverDataSize;
      qi.NumAllocations = qn.NumAllocations;
   } else {
      if (getenv("SHARE_BOGUS")) qi.hGlobalShare = 0x40000002;
      if (getenv("SHARE_NODEV")) qi.hDevice = 0;
      if (getenv("SHARE_BADDEV")) qi.hDevice = 0x12345678;
      st = K(D3DKMTQueryResourceInfo)(&qi);
   }
   printf("QueryResourceInfo 0x%08lx: priv %u, total priv %u, resource priv %u, allocs %u\n",
          st, qi.PrivateRuntimeDataSize, qi.TotalPrivateDriverDataSize,
          qi.ResourcePrivateDriverDataSize, qi.NumAllocations);
   if (st) {
      if (!getenv("SHARE_FORCE"))
         return 1;
      qi.PrivateRuntimeDataSize = 4096;
      qi.TotalPrivateDriverDataSize = 65536;
      qi.ResourcePrivateDriverDataSize = 4096;
      qi.NumAllocations = 1;
   }

   void *rt = calloc(1, qi.PrivateRuntimeDataSize + 16);
   void *tot = calloc(1, qi.TotalPrivateDriverDataSize + 16);
   void *rp = calloc(1, qi.ResourcePrivateDriverDataSize + 16);
   D3DDDI_OPENALLOCATIONINFO *oi = calloc(qi.NumAllocations, sizeof(*oi));

   D3DKMT_OPENRESOURCE or = { 0 };
   or.hDevice = cd.hDevice;
   or.hGlobalShare = qi.hGlobalShare;
   or.NumAllocations = qi.NumAllocations;
   or.pOpenAllocationInfo = oi;
   or.pPrivateRuntimeData = rt;
   or.PrivateRuntimeDataSize = qi.PrivateRuntimeDataSize;
   or.pResourcePrivateDriverData = rp;
   or.ResourcePrivateDriverDataSize = qi.ResourcePrivateDriverDataSize;
   or.pTotalPrivateDriverDataBuffer = tot;
   or.TotalPrivateDriverDataBufferSize = qi.TotalPrivateDriverDataSize;
   if (nt) {
      D3DKMT_OPENRESOURCEFROMNTHANDLE on = { 0 };
      on.hDevice = cd.hDevice;
      on.hNtHandle = share;
      on.NumAllocations = qi.NumAllocations;
      on.pOpenAllocationInfo2 = (D3DDDI_OPENALLOCATIONINFO2 *)calloc(qi.NumAllocations, sizeof(D3DDDI_OPENALLOCATIONINFO2));
      on.pPrivateRuntimeData = rt;
      on.PrivateRuntimeDataSize = qi.PrivateRuntimeDataSize;
      on.pResourcePrivateDriverData = rp;
      on.ResourcePrivateDriverDataSize = qi.ResourcePrivateDriverDataSize;
      on.pTotalPrivateDriverDataBuffer = tot;
      on.TotalPrivateDriverDataBufferSize = qi.TotalPrivateDriverDataSize;
      st = K(D3DKMTOpenResourceFromNtHandle)(&on);
      or.hResource = on.hResource;
      for (UINT i = 0; i < qi.NumAllocations; i++) {
         oi[i].hAllocation = on.pOpenAllocationInfo2[i].hAllocation;
         oi[i].pPrivateDriverData = on.pOpenAllocationInfo2[i].pPrivateDriverData;
         oi[i].PrivateDriverDataSize = on.pOpenAllocationInfo2[i].PrivateDriverDataSize;
      }
   } else
      st = K(D3DKMTOpenResource)(&or);
   printf("OpenResource 0x%08lx hResource 0x%x\n", st, or.hResource);
   if (st)
      return 1;

   dump("share_runtime.bin", rt, qi.PrivateRuntimeDataSize);
   dump("share_res.bin", rp, qi.ResourcePrivateDriverDataSize);
   for (UINT i = 0; i < qi.NumAllocations; i++) {
      char n[32];
      printf("alloc %u: hAllocation 0x%x priv %u\n", i, oi[i].hAllocation,
             oi[i].PrivateDriverDataSize);
      sprintf(n, "share_alloc%u.bin", i);
      dump(n, oi[i].pPrivateDriverData, oi[i].PrivateDriverDataSize);
   }
   /* Do layout: D3D11 ghi mau pixel = (y << 16) | x vao texture, roi khoa
    * allocation qua device KMT cua minh va tim vi tri cac pixel mau.
    */
   {
      UINT *img = malloc((size_t)w * h * 4);
      for (UINT y = 0; y < h; y++)
         for (UINT x = 0; x < w; x++)
            img[y * w + x] = (y << 16) | x;
      ID3D11DeviceContext_UpdateSubresource(dctx, (ID3D11Resource *)tex, 0, NULL, img, w * 4, 0);
      ID3D11DeviceContext_Flush(dctx);
      /* doi GPU xong: query event */
      D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
      ID3D11Query *q = NULL;
      ID3D11Device_CreateQuery(dev, &qd, &q);
      ID3D11DeviceContext_End(dctx, (ID3D11Asynchronous *)q);
      BOOL done = FALSE;
      while (ID3D11DeviceContext_GetData(dctx, (ID3D11Asynchronous *)q, &done, sizeof(done), 0) != S_OK)
         Sleep(1);

      free(img);
      gpu_readback(share, nt);
   }
   printf("OK\n");
   return 0;
}
