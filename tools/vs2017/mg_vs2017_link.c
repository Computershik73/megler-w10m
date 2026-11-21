/* Linked into Megler.exe when the toolset is VS 2017's MSVC 14.16 (tools/build-appx.sh). Its static C++ library builds
   std::mutex and friends on the Concurrency Runtime, whose resource manager asks for the process's processor affinity:
   GetProcessAffinityMask and SetProcessAffinityMask, which an app's libraries (WindowsApp.lib, OneCoreUAP.lib) do not have.
   Every processor is the process's, as for an app it is; nothing is changed. */
#include <windows.h>

static BOOL WINAPI get_affinity(HANDLE process, PDWORD_PTR mine, PDWORD_PTR system)
{
    SYSTEM_INFO info;
    DWORD_PTR all;
    (void)process;
    GetNativeSystemInfo(&info);
    all = info.dwNumberOfProcessors >= sizeof(DWORD_PTR) * 8 ? ~(DWORD_PTR)0 : (((DWORD_PTR)1 << info.dwNumberOfProcessors) - 1);
    if (mine) *mine = all;
    if (system) *system = all;
    return TRUE;
}

static BOOL WINAPI set_affinity(HANDLE process, DWORD_PTR mask)
{
    (void)process;
    (void)mask;
    return TRUE;
}

void *__imp_GetProcessAffinityMask = (void *)get_affinity;
void *__imp_SetProcessAffinityMask = (void *)set_affinity;
