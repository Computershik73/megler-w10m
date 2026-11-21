/*
 * A trace of what this process asks of the graphics kernel (D3DKMT), for the flag kmt-trace (ndk/mg_egl.c): every call of
 * the kernel's graphics functions by any module of the process -- Qualcomm's own Direct3D driver under ANGLE or the shell,
 * Mesa's D3DKMT core -- with its arguments, its private driver data, and the command buffers it submits, into a file the
 * PC reads (tools/kmt-trace.py). Mesa's freedreno hung the GPU when it rendered in GMEM tiles (drivers/mesa-arm32/src/
 * ISSUES.md, 1), where Qualcomm's driver tiles all the time: what the latter tells the kernel's driver and puts at the head
 * of its command buffers is what Mesa's core can be compared with.
 *
 * Windows 10 Mobile's win32u.dll holds one stub for each of these functions -- `movw r12, #<call>; svc #1; bx lr`, 16 bytes
 * apart -- which every way in comes to (gdi32's D3DKMT functions jump to them through its imports; dxgi.dll looks gdi32's
 * up by name). Each stub found in that form is copied, as it is, to a page of the trace's own, where it does what it did,
 * and the stub itself made `ldr.w pc, [pc]` and the address of the trace's function, which calls the copy. A stub in any
 * other form is left alone and said. On ARM32 alone: a diagnosis of the phone's GPU.
 *
 * The file: "KMTR", a version, the names of the functions; then a record for each call -- "KREC", the function's number,
 * its NTSTATUS, the thread, the time (QueryPerformanceCounter), the caller's address, the arguments as they were after
 * the call, and blobs: private data, allocations' private data, and for a submit the command buffer's dwords and those of
 * the buffers its CP_INDIRECT_BUFFER packets call. A command buffer can be read only where the allocation it is in was
 * mapped for the CPU (Lock2) and for the GPU (MapGpuVirtualAddress, UpdateGpuVirtualAddress) after the trace began.
 */
#if defined(_WIN32) && defined(_M_ARM)

/* (The kernel's graphics structures are in the SDK's desktop partition; this file declares nothing else of it it uses.) */
#include <winapifamily.h>
#undef WINAPI_FAMILY
#define WINAPI_FAMILY WINAPI_FAMILY_DESKTOP_APP
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mg_host.h"

#pragma intrinsic(_ReturnAddress)

typedef NTSTATUS(APIENTRY *kmt_fn)(void *);

/* The functions traced, by their name in win32u.dll without "NtGdiDdDDI". */
#define KMT_TRACED(X)                       \
    X(CreateDevice)                         \
    X(DestroyDevice)                        \
    X(CreateContext)                        \
    X(CreateContextVirtual)                 \
    X(DestroyContext)                       \
    X(CreatePagingQueue)                    \
    X(CreateAllocation)                     \
    X(DestroyAllocation)                    \
    X(DestroyAllocation2)                   \
    X(OpenResource)                         \
    X(OpenResourceFromNtHandle)             \
    X(Lock)                                 \
    X(Unlock)                               \
    X(Lock2)                                \
    X(Unlock2)                              \
    X(MakeResident)                         \
    X(Evict)                                \
    X(ReserveGpuVirtualAddress)             \
    X(MapGpuVirtualAddress)                 \
    X(UpdateGpuVirtualAddress)              \
    X(FreeGpuVirtualAddress)                \
    X(SubmitCommand)                        \
    X(Render)                               \
    X(Escape)                               \
    X(QueryAdapterInfo)                     \
    X(CreateSynchronizationObject)          \
    X(SignalSynchronizationObjectFromGpu)   \
    X(SignalSynchronizationObjectFromGpu2)  \
    X(WaitForSynchronizationObjectFromGpu)  \
    X(SetContextSchedulingPriority)         \
    X(SetContextInProcessSchedulingPriority)\
    X(SetAllocationPriority)                \
    X(Present)                              \
    X(GetDeviceState)

enum {
#define KMT_ENUM(name) K_##name,
    KMT_TRACED(KMT_ENUM)
#undef KMT_ENUM
    K_COUNT
};

static const char *const k_names[K_COUNT] = {
#define KMT_NAME(name) #name,
    KMT_TRACED(KMT_NAME)
#undef KMT_NAME
};

/* The copies of the stubs, which the trace's functions call. */
static kmt_fn g_orig[K_COUNT];

/* Blob kinds of a record (tools/kmt-trace.py). */
enum {
    B_PRIVATE = 1,        /* the call's private driver data, after it */
    B_PRIVATE_BEFORE = 2, /* the same before the call (an escape's question) */
    B_ALLOC_PRIVATE = 3,  /* an allocation's private driver data (addr: its number in the call) */
    B_RUNTIME = 4,        /* private runtime data */
    B_IB1 = 5,            /* a submitted command buffer (addr: its GPU address) */
    B_IB2 = 6,            /* a buffer it calls */
    B_HANDLES = 7,        /* the allocation handles of a call (make resident, evict, destroy, unlock) */
    B_OPS = 8,            /* UpdateGpuVirtualAddress's operations */
    B_NOTE = 9,           /* text */
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static FILE *g_file;
static size_t g_written, g_limit = 96u << 20;
static LARGE_INTEGER g_freq;
static unsigned g_submits_dumped, g_logged[K_COUNT];

/* What the CPU and the GPU see of each allocation, as far as the trace has seen it mapped. */
typedef struct {
    D3DKMT_HANDLE alloc;
    void *cpu;
} cpu_map;
typedef struct {
    UINT64 va, size, offset;   /* [va, va + size) is the allocation's bytes from `offset` on */
    D3DKMT_HANDLE alloc;
} gpu_map;
static cpu_map *g_cpu;
static size_t g_ncpu, g_capcpu;
static gpu_map *g_gpu;
static size_t g_ngpu, g_capgpu;

static void cpu_set(D3DKMT_HANDLE alloc, void *p)
{
    size_t i;
    for (i = 0; i < g_ncpu; i++)
        if (g_cpu[i].alloc == alloc) {
            if (p) g_cpu[i].cpu = p;
            else g_cpu[i] = g_cpu[--g_ncpu];
            return;
        }
    if (!p) return;
    if (g_ncpu == g_capcpu) {
        size_t n = g_capcpu ? g_capcpu * 2 : 256;
        cpu_map *m = (cpu_map *)realloc(g_cpu, n * sizeof(*m));
        if (!m) return;
        g_cpu = m;
        g_capcpu = n;
    }
    g_cpu[g_ncpu].alloc = alloc;
    g_cpu[g_ncpu].cpu = p;
    g_ncpu++;
}

static void *cpu_of(D3DKMT_HANDLE alloc)
{
    size_t i;
    for (i = 0; i < g_ncpu; i++)
        if (g_cpu[i].alloc == alloc) return g_cpu[i].cpu;
    return NULL;
}

/* A mapping for the GPU: the ranges it overlaps go first. */
static void gpu_set(UINT64 va, UINT64 size, D3DKMT_HANDLE alloc, UINT64 offset)
{
    size_t i;
    for (i = 0; i < g_ngpu;) {
        if (g_gpu[i].va < va + size && va < g_gpu[i].va + g_gpu[i].size) g_gpu[i] = g_gpu[--g_ngpu];
        else i++;
    }
    if (!alloc || !size) return;
    if (g_ngpu == g_capgpu) {
        size_t n = g_capgpu ? g_capgpu * 2 : 256;
        gpu_map *m = (gpu_map *)realloc(g_gpu, n * sizeof(*m));
        if (!m) return;
        g_gpu = m;
        g_capgpu = n;
    }
    g_gpu[g_ngpu].va = va;
    g_gpu[g_ngpu].size = size;
    g_gpu[g_ngpu].alloc = alloc;
    g_gpu[g_ngpu].offset = offset;
    g_ngpu++;
}

/* Where the CPU sees `bytes` at the GPU's address `va`, or NULL. */
static const UINT32 *cpu_at(UINT64 va, UINT64 bytes)
{
    size_t i;
    for (i = 0; i < g_ngpu; i++) {
        const gpu_map *m = &g_gpu[i];
        if (va >= m->va && va + bytes <= m->va + m->size) {
            BYTE *p = (BYTE *)cpu_of(m->alloc);
            return p ? (const UINT32 *)(p + (size_t)(m->offset + (va - m->va))) : NULL;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ records */

static void put(const void *p, size_t n)
{
    if (!g_file || !n) return;
    fwrite(p, 1, n, g_file);
    g_written += n;
}

typedef struct {
    UINT32 kind, size;
    UINT64 addr;
    const void *data;
} blob;

/* A record: under the lock. Nothing once the file has its limit. */
static void record(int fn, NTSTATUS st, void *caller, const void *args, UINT32 args_size, const blob *blobs, unsigned nblobs)
{
    UINT32 head[8];
    LARGE_INTEGER now;
    unsigned i;
    size_t total = sizeof(head) + 8 + args_size;
    if (!g_file) return;
    for (i = 0; i < nblobs; i++) total += 16 + blobs[i].size;
    if (g_written + total > g_limit) {
        if (g_written <= g_limit) {
            g_written = g_limit + 1;
            fflush(g_file);
            mg_warn("kmt: the trace's file has its %u MB: nothing more is written into it", (unsigned)(g_limit >> 20));
        }
        return;
    }
    QueryPerformanceCounter(&now);
    head[0] = 0x4345524Bu; /* "KREC" */
    head[1] = (UINT32)fn;
    head[2] = (UINT32)st;
    head[3] = GetCurrentThreadId();
    head[4] = (UINT32)(UINT_PTR)caller;
    head[5] = args_size;
    head[6] = nblobs;
    head[7] = 0;
    put(head, sizeof(head));
    put(&now.QuadPart, 8);
    put(args, args_size);
    for (i = 0; i < nblobs; i++) {
        UINT32 b[4] = { blobs[i].kind, blobs[i].size, (UINT32)blobs[i].addr, (UINT32)(blobs[i].addr >> 32) };
        put(b, sizeof(b));
        put(blobs[i].data, blobs[i].size);
    }
}

static void hex(char *out, size_t cap, const void *p, size_t n)
{
    const BYTE *b = (const BYTE *)p;
    size_t i, at = 0;
    out[0] = 0;
    for (i = 0; i < n && at + 4 < cap; i += 4) {
        UINT32 v = 0;
        memcpy(&v, b + i, n - i >= 4 ? 4 : n - i);
        at += (size_t)snprintf(out + at, cap - at, "%s%08x", i ? " " : "", (unsigned)v);
    }
}

/* A line in the log for the first calls of each kind that say something of themselves. */
static bool say(int fn, unsigned first)
{
    return g_logged[fn]++ < first;
}

/* ------------------------------------------------------------- the calls */

#define CALL(fn, a) g_orig[fn]((void *)(a))

static NTSTATUS APIENTRY t_CreateContextVirtual(D3DKMT_CREATECONTEXTVIRTUAL *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_CreateContextVirtual, a);
    blob b = { B_PRIVATE, a->PrivateDriverDataSize, 0, a->pPrivateDriverData };
    char h[1200];
    AcquireSRWLockExclusive(&g_lock);
    record(K_CreateContextVirtual, st, caller, a, sizeof(*a), &b, a->pPrivateDriverData ? 1 : 0);
    ReleaseSRWLockExclusive(&g_lock);
    hex(h, sizeof(h), a->pPrivateDriverData, a->pPrivateDriverData ? (a->PrivateDriverDataSize < 256 ? a->PrivateDriverDataSize : 256) : 0);
    mg_info("kmt: CreateContextVirtual device 0x%x node %u affinity 0x%x flags 0x%x hint %d -> context 0x%x, status 0x%08lx, "
            "from %p; its private data %u bytes: %s", a->hDevice, a->NodeOrdinal, a->EngineAffinity, a->Flags.Value,
            (int)a->ClientHint, a->hContext, (unsigned long)st, caller, a->PrivateDriverDataSize, h);
    return st;
}

static NTSTATUS APIENTRY t_CreateContext(D3DKMT_CREATECONTEXT *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_CreateContext, a);
    blob b = { B_PRIVATE, a->PrivateDriverDataSize, 0, a->pPrivateDriverData };
    char h[1200];
    AcquireSRWLockExclusive(&g_lock);
    record(K_CreateContext, st, caller, a, sizeof(*a), &b, a->pPrivateDriverData ? 1 : 0);
    ReleaseSRWLockExclusive(&g_lock);
    hex(h, sizeof(h), a->pPrivateDriverData, a->pPrivateDriverData ? (a->PrivateDriverDataSize < 256 ? a->PrivateDriverDataSize : 256) : 0);
    mg_info("kmt: CreateContext device 0x%x node %u flags 0x%x hint %d -> context 0x%x, status 0x%08lx, from %p; its private "
            "data %u bytes: %s", a->hDevice, a->NodeOrdinal, a->Flags.Value, (int)a->ClientHint, a->hContext, (unsigned long)st,
            caller, a->PrivateDriverDataSize, h);
    return st;
}

static NTSTATUS APIENTRY t_CreateDevice(D3DKMT_CREATEDEVICE *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_CreateDevice, a);
    AcquireSRWLockExclusive(&g_lock);
    record(K_CreateDevice, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    mg_info("kmt: CreateDevice adapter 0x%x flags 0x%x -> device 0x%x, status 0x%08lx, from %p", a->hAdapter,
            *(const UINT *)&a->Flags, a->hDevice, (unsigned long)st, caller);
    return st;
}

static NTSTATUS APIENTRY t_CreatePagingQueue(D3DKMT_CREATEPAGINGQUEUE *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_CreatePagingQueue, a);
    AcquireSRWLockExclusive(&g_lock);
    record(K_CreatePagingQueue, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    mg_info("kmt: CreatePagingQueue device 0x%x priority %d -> queue 0x%x, status 0x%08lx, from %p", a->hDevice, (int)a->Priority,
            a->hPagingQueue, (unsigned long)st, caller);
    return st;
}

static NTSTATUS APIENTRY t_CreateAllocation(D3DKMT_CREATEALLOCATION *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_CreateAllocation, a);
    blob b[18];
    unsigned n = 0, i;
    if (a->pPrivateRuntimeData && a->PrivateRuntimeDataSize) {
        blob r = { B_RUNTIME, a->PrivateRuntimeDataSize, 0, a->pPrivateRuntimeData };
        b[n++] = r;
    }
    if (a->pPrivateDriverData && a->PrivateDriverDataSize) {
        blob r = { B_PRIVATE, a->PrivateDriverDataSize, 0, a->pPrivateDriverData };
        b[n++] = r;
    }
    for (i = 0; a->pAllocationInfo2 && i < a->NumAllocations && n < 18; i++) {
        const D3DDDI_ALLOCATIONINFO2 *ai = &a->pAllocationInfo2[i];
        blob r = { B_ALLOC_PRIVATE, sizeof(*ai), i, ai };
        b[n++] = r;
        if (ai->pPrivateDriverData && ai->PrivateDriverDataSize && n < 18) {
            blob p = { B_ALLOC_PRIVATE, ai->PrivateDriverDataSize, 0x100000000ull | i, ai->pPrivateDriverData };
            b[n++] = p;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    record(K_CreateAllocation, st, caller, a, sizeof(*a), b, n);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

static NTSTATUS APIENTRY t_Lock2(D3DKMT_LOCK2 *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_Lock2, a);
    AcquireSRWLockExclusive(&g_lock);
    if (!st) cpu_set(a->hAllocation, a->pData);
    record(K_Lock2, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

static NTSTATUS APIENTRY t_Unlock2(D3DKMT_UNLOCK2 *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_Unlock2, a);
    AcquireSRWLockExclusive(&g_lock);
    if (!st) cpu_set(a->hAllocation, NULL);
    record(K_Unlock2, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

static NTSTATUS APIENTRY t_Lock(D3DKMT_LOCK *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_Lock, a);
    AcquireSRWLockExclusive(&g_lock);
    if (!st) cpu_set(a->hAllocation, a->pData);
    record(K_Lock, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

static NTSTATUS APIENTRY t_Unlock(D3DKMT_UNLOCK *a)
{
    void *caller = _ReturnAddress();
    blob b = { B_HANDLES, a->phAllocations ? a->NumAllocations * 4 : 0, 0, a->phAllocations };
    NTSTATUS st = CALL(K_Unlock, a);
    UINT i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; !st && a->phAllocations && i < a->NumAllocations; i++) cpu_set(a->phAllocations[i], NULL);
    record(K_Unlock, st, caller, a, sizeof(*a), &b, 1);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

static NTSTATUS APIENTRY t_MapGpuVirtualAddress(D3DDDI_MAPGPUVIRTUALADDRESS *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_MapGpuVirtualAddress, a);
    AcquireSRWLockExclusive(&g_lock);
    if (st >= 0 && a->VirtualAddress) gpu_set(a->VirtualAddress, a->SizeInPages * 4096, a->hAllocation, a->OffsetInPages * 4096);
    record(K_MapGpuVirtualAddress, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

static NTSTATUS APIENTRY t_UpdateGpuVirtualAddress(D3DKMT_UPDATEGPUVIRTUALADDRESS *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_UpdateGpuVirtualAddress, a);
    blob b = { B_OPS, a->Operations ? a->NumOperations * (UINT32)sizeof(*a->Operations) : 0, 0, a->Operations };
    UINT i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; st >= 0 && a->Operations && i < a->NumOperations; i++) {
        const D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION *o = &a->Operations[i];
        if (o->OperationType == D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP)
            gpu_set(o->Map.BaseAddress, o->Map.SizeInBytes, o->Map.hAllocation, o->Map.AllocationOffsetInBytes);
        else if (o->OperationType == D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP)
            gpu_set(o->Unmap.BaseAddress, o->Unmap.SizeInBytes, 0, 0);
    }
    record(K_UpdateGpuVirtualAddress, st, caller, a, sizeof(*a), &b, 1);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

static NTSTATUS APIENTRY t_FreeGpuVirtualAddress(D3DKMT_FREEGPUVIRTUALADDRESS *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_FreeGpuVirtualAddress, a);
    AcquireSRWLockExclusive(&g_lock);
    if (st >= 0) gpu_set(a->BaseAddress, a->Size, 0, 0);
    record(K_FreeGpuVirtualAddress, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

/*
 * The buffers a command buffer calls: CP_INDIRECT_BUFFER_PFE (0x3f), _PFD (0x37), _CHAIN (0x57) -- address, size in
 * dwords -- and CP_SET_DRAW_STATE (0x43), groups of a count and an address. Up to `max` of them, each up to 64 KB.
 */
static unsigned called_buffers(const UINT32 *p, UINT32 dwords, blob *out, unsigned max)
{
    unsigned n = 0;
    UINT32 i = 0;
    while (i < dwords && n < max) {
        UINT32 h = p[i], op, cnt;
        if ((h >> 28) == 7) {
            op = (h >> 16) & 0x7f;
            cnt = h & 0x3fff;
        } else if ((h >> 28) == 4) {
            i += 1 + (h & 0x7f);
            continue;
        } else {
            i++;
            continue;
        }
        if ((op == 0x3f || op == 0x37 || op == 0x57) && cnt >= 3 && i + 3 < dwords) {
            UINT64 va = p[i + 1] | ((UINT64)p[i + 2] << 32);
            UINT32 sz = p[i + 3] & 0xfffff;
            const UINT32 *q;
            if (sz > 16384) sz = 16384;
            q = sz ? cpu_at(va, sz * 4ull) : NULL;
            if (q) {
                blob b = { B_IB2, sz * 4, va, q };
                out[n++] = b;
            }
        } else if (op == 0x43) {
            UINT32 k;
            for (k = 0; k + 2 < cnt && i + 1 + k + 2 < dwords && n < max; k += 3) {
                UINT32 c = p[i + 1 + k] & 0xffff;
                UINT64 va = p[i + 2 + k] | ((UINT64)p[i + 3 + k] << 32);
                const UINT32 *q = c && va ? cpu_at(va, c * 4ull) : NULL;
                if (q) {
                    blob b = { B_IB2, c * 4, va, q };
                    out[n++] = b;
                }
            }
        }
        i += 1 + cnt;
    }
    return n;
}

static NTSTATUS APIENTRY t_SubmitCommand(D3DKMT_SUBMITCOMMAND *a)
{
    void *caller = _ReturnAddress();
    blob b[66];
    unsigned n = 0, i;
    NTSTATUS st;
    UINT32 len = a->CommandLength > 256 * 1024 ? 256 * 1024 : a->CommandLength;
    BYTE *copy = NULL;
    char h[400];
    AcquireSRWLockExclusive(&g_lock);
    {
        /* The command buffer and what it calls, copied before the call: the GPU may be running them after, and the
           driver writing them again. */
        const UINT32 *ib = len ? cpu_at(a->Commands, len) : NULL;
        size_t total = 0, at = 0;
        if (a->pPrivateDriverData && a->PrivateDriverDataSize) {
            blob p = { B_PRIVATE, a->PrivateDriverDataSize, 0, a->pPrivateDriverData };
            b[n++] = p;
        }
        if (ib && (g_submits_dumped < 600 || g_submits_dumped % 8 == 0)) {
            blob p = { B_IB1, len, a->Commands, ib };
            b[n++] = p;
            n += called_buffers(ib, len / 4, b + n, 64);
        } else if (!ib) {
            static const char note[] = "the command buffer is not mapped for the CPU where the trace has seen it";
            blob p = { B_NOTE, sizeof(note) - 1, a->Commands, note };
            b[n++] = p;
        }
        g_submits_dumped++;
        for (i = 0; i < n; i++) total += b[i].size;
        copy = (BYTE *)malloc(total ? total : 1);
        if (copy) {
            for (i = 0; i < n; i++) {
                memcpy(copy + at, b[i].data, b[i].size);
                b[i].data = copy + at;
                at += b[i].size;
            }
        } else {
            n = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    st = CALL(K_SubmitCommand, a);
    AcquireSRWLockExclusive(&g_lock);
    record(K_SubmitCommand, st, caller, a, sizeof(*a), b, n);
    ReleaseSRWLockExclusive(&g_lock);
    free(copy);
    if (say(K_SubmitCommand, 40)) {
        hex(h, sizeof(h), a->pPrivateDriverData, a->pPrivateDriverData ? (a->PrivateDriverDataSize < 64 ? a->PrivateDriverDataSize : 64) : 0);
        mg_info("kmt: SubmitCommand context 0x%x (of %u) commands 0x%llx, %u bytes, flags 0x%x, history %u, primaries %u -> "
                "0x%08lx, from %p; its private data %u bytes: %s", a->BroadcastContext[0], a->BroadcastContextCount,
                (unsigned long long)a->Commands, a->CommandLength, *(const UINT *)&a->Flags, a->NumHistoryBuffers,
                a->NumPrimaries, (unsigned long)st, caller, a->PrivateDriverDataSize, h);
    }
    return st;
}

static NTSTATUS APIENTRY t_Render(D3DKMT_RENDER *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_Render, a);
    blob b = { B_PRIVATE, a->PrivateDriverDataSize, 0, a->pPrivateDriverData };
    AcquireSRWLockExclusive(&g_lock);
    record(K_Render, st, caller, a, sizeof(*a), &b, a->pPrivateDriverData ? 1 : 0);
    ReleaseSRWLockExclusive(&g_lock);
    if (say(K_Render, 10))
        mg_info("kmt: Render context 0x%x offset %u length %u -> 0x%08lx, from %p", a->hContext, a->CommandOffset,
                a->CommandLength, (unsigned long)st, caller);
    return st;
}

static NTSTATUS APIENTRY t_Escape(D3DKMT_ESCAPE *a)
{
    void *caller = _ReturnAddress();
    UINT32 size = a->pPrivateDriverData ? a->PrivateDriverDataSize : 0;
    void *before = size ? malloc(size) : NULL;
    NTSTATUS st;
    blob b[2];
    unsigned n = 0;
    char h1[400], h2[400];
    if (before) memcpy(before, a->pPrivateDriverData, size);
    st = CALL(K_Escape, a);
    if (before) {
        blob p = { B_PRIVATE_BEFORE, size, 0, before };
        blob q = { B_PRIVATE, size, 0, a->pPrivateDriverData };
        b[n++] = p;
        b[n++] = q;
    }
    AcquireSRWLockExclusive(&g_lock);
    record(K_Escape, st, caller, a, sizeof(*a), b, n);
    ReleaseSRWLockExclusive(&g_lock);
    if (say(K_Escape, 60)) {
        hex(h1, sizeof(h1), before, size < 48 ? size : 48);
        hex(h2, sizeof(h2), a->pPrivateDriverData, size < 48 ? size : 48);
        mg_info("kmt: Escape adapter 0x%x device 0x%x context 0x%x type %d flags 0x%x, %u bytes -> 0x%08lx, from %p; asked %s; "
                "answered %s", a->hAdapter, a->hDevice, a->hContext, (int)a->Type, a->Flags.Value, size, (unsigned long)st,
                caller, h1, h2);
    }
    free(before);
    return st;
}

static NTSTATUS APIENTRY t_QueryAdapterInfo(D3DKMT_QUERYADAPTERINFO *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_QueryAdapterInfo, a);
    blob b = { B_PRIVATE, a->PrivateDriverDataSize, 0, a->pPrivateDriverData };
    char h[1200];
    AcquireSRWLockExclusive(&g_lock);
    record(K_QueryAdapterInfo, st, caller, a, sizeof(*a), &b, a->pPrivateDriverData ? 1 : 0);
    ReleaseSRWLockExclusive(&g_lock);
    if (say(K_QueryAdapterInfo, 80)) {
        hex(h, sizeof(h), a->pPrivateDriverData, a->pPrivateDriverData ? (a->PrivateDriverDataSize < 160 ? a->PrivateDriverDataSize : 160) : 0);
        mg_info("kmt: QueryAdapterInfo adapter 0x%x type %d, %u bytes -> 0x%08lx, from %p: %s", a->hAdapter, (int)a->Type,
                a->PrivateDriverDataSize, (unsigned long)st, caller, h);
    }
    return st;
}

static NTSTATUS APIENTRY t_SetContextSchedulingPriority(D3DKMT_SETCONTEXTSCHEDULINGPRIORITY *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_SetContextSchedulingPriority, a);
    AcquireSRWLockExclusive(&g_lock);
    record(K_SetContextSchedulingPriority, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    mg_info("kmt: SetContextSchedulingPriority context 0x%x priority %d -> 0x%08lx, from %p", a->hContext, a->Priority,
            (unsigned long)st, caller);
    return st;
}

static NTSTATUS APIENTRY t_SetContextInProcessSchedulingPriority(D3DKMT_SETCONTEXTINPROCESSSCHEDULINGPRIORITY *a)
{
    void *caller = _ReturnAddress();
    NTSTATUS st = CALL(K_SetContextInProcessSchedulingPriority, a);
    AcquireSRWLockExclusive(&g_lock);
    record(K_SetContextInProcessSchedulingPriority, st, caller, a, sizeof(*a), NULL, 0);
    ReleaseSRWLockExclusive(&g_lock);
    mg_info("kmt: SetContextInProcessSchedulingPriority context 0x%x priority %d -> 0x%08lx, from %p", a->hContext, a->Priority,
            (unsigned long)st, caller);
    return st;
}

/* The handles of a call that takes a list of allocations. */
#define HANDLES_CALL(name, type, list, count)                                                \
    static NTSTATUS APIENTRY t_##name(type *a)                                               \
    {                                                                                        \
        void *caller = _ReturnAddress();                                                     \
        blob b = { B_HANDLES, a->list ? (UINT32)a->count * 4 : 0, 0, (const void *)a->list }; \
        NTSTATUS st = CALL(K_##name, a);                                                     \
        AcquireSRWLockExclusive(&g_lock);                                                    \
        record(K_##name, st, caller, a, sizeof(*a), &b, 1);                                  \
        ReleaseSRWLockExclusive(&g_lock);                                                    \
        return st;                                                                           \
    }
HANDLES_CALL(MakeResident, D3DDDI_MAKERESIDENT, AllocationList, NumAllocations)
HANDLES_CALL(Evict, D3DKMT_EVICT, AllocationList, NumAllocations)
HANDLES_CALL(DestroyAllocation, D3DKMT_DESTROYALLOCATION, phAllocationList, AllocationCount)
HANDLES_CALL(DestroyAllocation2, D3DKMT_DESTROYALLOCATION2, phAllocationList, AllocationCount)

/* The rest: the arguments alone. */
#define PLAIN_CALL(name, type)                                \
    static NTSTATUS APIENTRY t_##name(type *a)                \
    {                                                         \
        void *caller = _ReturnAddress();                      \
        NTSTATUS st = CALL(K_##name, a);                      \
        AcquireSRWLockExclusive(&g_lock);                     \
        record(K_##name, st, caller, a, sizeof(*a), NULL, 0); \
        ReleaseSRWLockExclusive(&g_lock);                     \
        return st;                                            \
    }
PLAIN_CALL(DestroyDevice, D3DKMT_DESTROYDEVICE)
PLAIN_CALL(DestroyContext, D3DKMT_DESTROYCONTEXT)
PLAIN_CALL(OpenResource, D3DKMT_OPENRESOURCE)
PLAIN_CALL(OpenResourceFromNtHandle, D3DKMT_OPENRESOURCEFROMNTHANDLE)
PLAIN_CALL(ReserveGpuVirtualAddress, D3DDDI_RESERVEGPUVIRTUALADDRESS)
PLAIN_CALL(CreateSynchronizationObject, D3DKMT_CREATESYNCHRONIZATIONOBJECT2)
PLAIN_CALL(SignalSynchronizationObjectFromGpu, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU)
PLAIN_CALL(SignalSynchronizationObjectFromGpu2, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2)
PLAIN_CALL(WaitForSynchronizationObjectFromGpu, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU)
PLAIN_CALL(SetAllocationPriority, D3DKMT_SETALLOCATIONPRIORITY)
PLAIN_CALL(Present, D3DKMT_PRESENT)
PLAIN_CALL(GetDeviceState, D3DKMT_GETDEVICESTATE)

static void *const k_hooks[K_COUNT] = {
#define KMT_HOOK(name) (void *)t_##name,
    KMT_TRACED(KMT_HOOK)
#undef KMT_HOOK
};

/* ------------------------------------------------------------ the stubs */

/* Whether the 8 bytes are `movw/mov.w r12, #n; svc #1; bx lr`. */
static bool is_stub(const BYTE *p)
{
    UINT16 h0 = (UINT16)(p[0] | p[1] << 8), h1 = (UINT16)(p[2] | p[3] << 8);
    bool movw = (h0 & 0xFBF0) == 0xF240, movi = (h0 & 0xFBEF) == 0xF04F;
    return (movw || movi) && ((h1 >> 8) & 0xF) == 12 && !(h1 & 0x8000) && p[4] == 0x01 && p[5] == 0xDF && p[6] == 0x70 &&
           p[7] == 0x47;
}

int mg_host_kmt_trace_start(const char *path, char *why, size_t size)
{
    static int started = -1;
    HMODULE w32u;
    BYTE *page;
    int i, hooked = 0;
    char skipped[600] = "";
    size_t at = 0;
    if (started >= 0) return started;
    started = 0;
    QueryPerformanceFrequency(&g_freq);
    w32u = GetModuleHandleW(L"win32u.dll");
    if (!w32u) {
        snprintf(why, size, "win32u.dll is not loaded in this process");
        return 0;
    }
    if (!mg_mem_rwx()) {
        snprintf(why, size, "this process cannot have pages writable and executable at once, which the stubs are to be made");
        return 0;
    }
    page = (BYTE *)mg_mem_reserve(4096);
    if (!page || !mg_mem_commit(page, 4096, MG_PROT_RW)) {
        snprintf(why, size, "no page for the copies of the stubs");
        return 0;
    }
    g_file = fopen(path, "wb");
    if (!g_file) {
        snprintf(why, size, "%s could not be made", path);
        return 0;
    }
    {
        UINT32 head[4] = { 0x524D544Bu /* "KMTR" */, 1, (UINT32)K_COUNT, (UINT32)g_freq.QuadPart };
        put(head, sizeof(head));
        for (i = 0; i < K_COUNT; i++) {
            char name[48] = { 0 };
            strncpy(name, k_names[i], sizeof(name) - 1);
            put(name, sizeof(name));
        }
        fflush(g_file);
    }
    for (i = 0; i < K_COUNT; i++) {
        char name[64];
        BYTE *stub;
        snprintf(name, sizeof(name), "NtGdiDdDDI%s", k_names[i]);
        stub = (BYTE *)(UINT_PTR)GetProcAddress(w32u, name);
        if (!stub) continue;
        stub = (BYTE *)((UINT_PTR)stub & ~(UINT_PTR)1);
        if (((UINT_PTR)stub & 3) || !is_stub(stub)) {
            at += (size_t)snprintf(skipped + at, at < sizeof(skipped) ? sizeof(skipped) - at : 0, "%s%s (%02x%02x %02x%02x %02x%02x %02x%02x)",
                                   at ? ", " : "", k_names[i], stub[0], stub[1], stub[2], stub[3], stub[4], stub[5], stub[6], stub[7]);
            continue;
        }
        memcpy(page + i * 16, stub, 8);
        g_orig[i] = (kmt_fn)(void *)((UINT_PTR)(page + i * 16) | 1);
    }
    if (!mg_mem_protect(page, 4096, MG_PROT_RX)) {
        snprintf(why, size, "the copies of the stubs could not be made executable");
        return 0;
    }
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    for (i = 0; i < K_COUNT; i++) {
        char name[64];
        BYTE *stub;
        UINT32 code[2];
        if (!g_orig[i]) continue;
        snprintf(name, sizeof(name), "NtGdiDdDDI%s", k_names[i]);
        stub = (BYTE *)((UINT_PTR)GetProcAddress(w32u, name) & ~(UINT_PTR)1);
        code[0] = 0xF000F8DFu; /* ldr.w pc, [pc, #0] */
        code[1] = (UINT32)(UINT_PTR)k_hooks[i] | 1;
        if (!mg_mem_protect(stub, 8, MG_PROT_RWX)) {
            g_orig[i] = NULL;
            continue;
        }
        memcpy(stub, code, 8);
        mg_mem_protect(stub, 8, MG_PROT_RX);
        FlushInstructionCache(GetCurrentProcess(), stub, 8);
        hooked++;
    }
    started = hooked;
    snprintf(why, size, "%d of win32u's %d functions traced%s%s", hooked, (int)K_COUNT, skipped[0] ? "; not in the stub's form: " : "",
             skipped);
    return hooked;
}

#else

#include <stdio.h>

#include "mg_host.h"

int mg_host_kmt_trace_start(const char *path, char *why, size_t size)
{
    (void)path;
    snprintf(why, size, "the trace of the graphics kernel's calls is for Windows 10 Mobile's ARM32 alone");
    return 0;
}

#endif
