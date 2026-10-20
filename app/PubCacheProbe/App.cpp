// A probe of the publisher cache folder (tools/build-pubcache-probe.sh, tools/pubcache-probe.py): whether two packages of
// one publisher, each its own process, share a folder that Win32 calls reach, as a package made for each Android app would
// need for /sdcard. Built twice, as MeglerProbeA and MeglerProbeB; each says what it found in LocalState\probe.log.
//
// What each start does, in the folder "shared" both manifests declare:
//  - says the folder's path, and the app's memory limit and use (MemoryManager);
//  - keeps heartbeat-<letter>.txt counting every half second, so that the other can tell whether this one runs on in the
//    background or is suspended;
//  - writes who-<letter>.txt, lists the folder, and reads the other's who- and heartbeat files (twice, three seconds apart);
//  - makes sdcard\Android\data\com.example.<letter> level by level and a file in it;
//  - opens lock.dat shared and takes a byte-range lock on it without waiting (as SQLite does), and opens excl.dat with no
//    sharing; both are held while the process lives, so the second app to start finds them taken by the first.
#include <windows.h>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Storage;
using namespace Windows::UI::Core;

namespace {

FILE *g_log;
std::wstring g_dir;
wchar_t g_me, g_peer;
HANDLE g_lock = INVALID_HANDLE_VALUE, g_excl = INVALID_HANDLE_VALUE;

void say(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

std::string narrow(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

HANDLE open_file(const std::wstring &path, DWORD access, DWORD share, DWORD disposition)
{
    CREATEFILE2_EXTENDED_PARAMETERS p = {};
    p.dwSize = sizeof(p);
    p.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    return CreateFile2(path.c_str(), access, share, disposition, &p);
}

bool write_text(const std::wstring &path, const std::string &text)
{
    HANDLE h = open_file(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD done = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &done, nullptr);
    CloseHandle(h);
    return ok && done == text.size();
}

// The file's text, or "(error N)".
std::string read_text(const std::wstring &path)
{
    HANDLE h = open_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return "(error " + std::to_string(GetLastError()) + ")";
    char buf[512];
    DWORD done = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &done, nullptr);
    CloseHandle(h);
    buf[done] = 0;
    return buf;
}

void heartbeat()
{
    std::wstring path = g_dir + L"\\heartbeat-" + g_me + L".txt";
    for (unsigned n = 0;; n++) {
        write_text(path, std::to_string(n) + " at " + std::to_string(GetTickCount64()) + " ms since boot");
        Sleep(500);
    }
}

void list()
{
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW((g_dir + L"\\*").c_str(), FindExInfoBasic, &f, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        say("list: error %lu", GetLastError());
        return;
    }
    do {
        say("list: %s%s, %lu bytes", narrow(f.cFileName).c_str(), (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "\\" : "",
            f.nFileSizeLow);
    } while (FindNextFileW(h, &f));
    FindClose(h);
}

void probe()
{
    say("=== %c, process %lu ===", (char)g_me, GetCurrentProcessId());
    try {
        say("memory: limit %llu MB, in use %llu MB", Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20,
            Windows::System::MemoryManager::AppMemoryUsage() >> 20);
    } catch (hresult_error const &e) {
        say("memory: 0x%08x", (unsigned)e.code());
    }
    try {
        StorageFolder f = ApplicationData::Current().GetPublisherCacheFolder(L"shared");
        g_dir = f.Path().c_str();
        say("publisher cache folder \"shared\": %s", narrow(g_dir).c_str());
    } catch (hresult_error const &e) {
        say("publisher cache folder \"shared\": 0x%08x %s", (unsigned)e.code(), narrow(e.message().c_str()).c_str());
        return;
    }
    std::thread(heartbeat).detach();
    std::wstring who = g_dir + L"\\who-" + g_me + L".txt";
    char text[160];
    snprintf(text, sizeof(text), "%c, process %lu, %llu ms since boot", (char)g_me, GetCurrentProcessId(), GetTickCount64());
    say("write who-%c.txt: %s", (char)g_me, write_text(who, text) ? "ok" : ("error " + std::to_string(GetLastError())).c_str());
    list();
    say("read who-%c.txt: %s", (char)g_peer, read_text(g_dir + L"\\who-" + g_peer + L".txt").c_str());
    std::wstring beat = g_dir + L"\\heartbeat-" + g_peer + L".txt";
    say("read heartbeat-%c.txt: %s", (char)g_peer, read_text(beat).c_str());
    Sleep(3000);
    say("read heartbeat-%c.txt 3 s later: %s", (char)g_peer, read_text(beat).c_str());

    std::wstring dir = g_dir;
    const wchar_t *levels[] = { L"sdcard", L"Android", L"data" };
    for (const wchar_t *l : levels) {
        dir += L"\\";
        dir += l;
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) say("mkdir %s: error %lu", narrow(dir).c_str(), GetLastError());
    }
    dir += std::wstring(L"\\com.example.") + g_me;
    BOOL made = CreateDirectoryW(dir.c_str(), nullptr);
    DWORD made_err = made ? 0 : GetLastError();
    say("mkdir sdcard\\Android\\data\\com.example.%c: %s", (char)g_me, made ? "made" : made_err == ERROR_ALREADY_EXISTS ? "there already" : ("error " + std::to_string(made_err)).c_str());
    say("write a file there: %s", write_text(dir + L"\\note.txt", text) ? "ok" : ("error " + std::to_string(GetLastError())).c_str());
    say("read com.example.%c\\note.txt: %s", (char)g_peer, read_text(g_dir + L"\\sdcard\\Android\\data\\com.example." + g_peer + L"\\note.txt").c_str());

    g_lock = open_file(g_dir + L"\\lock.dat", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS);
    if (g_lock == INVALID_HANDLE_VALUE) {
        say("open lock.dat shared: error %lu", GetLastError());
    } else {
        OVERLAPPED o = {};
        BOOL locked = LockFileEx(g_lock, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 4096, 0, &o);
        say("open lock.dat shared: ok; lock its first 4096 bytes: %s", locked ? "locked (held while this process lives)" : ("error " + std::to_string(GetLastError())).c_str());
        if (!locked) {
            char buf[16];
            DWORD done = 0;
            BOOL r = ReadFile(g_lock, buf, sizeof(buf), &done, nullptr);
            say("read lock.dat's locked bytes: %s", r ? ("ok, " + std::to_string(done) + " bytes").c_str() : ("error " + std::to_string(GetLastError())).c_str());
        }
    }
    g_excl = open_file(g_dir + L"\\excl.dat", GENERIC_READ | GENERIC_WRITE, 0, OPEN_ALWAYS);
    say("open excl.dat with no sharing: %s", g_excl != INVALID_HANDLE_VALUE ? "ok (held while this process lives)" : ("error " + std::to_string(GetLastError())).c_str());
    say("done");
}

struct View : implements<View, IFrameworkView> {
    void Initialize(CoreApplicationView const &view) { view.Activated({ this, &View::OnActivated }); }
    void SetWindow(CoreWindow const &) {}
    void Load(hstring const &) {}
    void Run()
    {
        std::thread(probe).detach();
        CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    }
    void Uninitialize() {}
    void OnActivated(CoreApplicationView const &, IActivatedEventArgs const &) { CoreWindow::GetForCurrentThread().Activate(); }
};

struct Source : implements<Source, IFrameworkViewSource> {
    IFrameworkView CreateView() { return make<View>(); }
};

} // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    init_apartment();
    std::wstring name = Package::Current().Id().Name().c_str();   // MeglerProbeA or MeglerProbeB
    g_me = name.empty() ? L'?' : name.back();
    g_peer = g_me == L'A' ? L'B' : L'A';
    std::wstring log = std::wstring(ApplicationData::Current().LocalFolder().Path().c_str()) + L"\\probe.log";
    g_log = _wfopen(log.c_str(), L"a");
    CoreApplication::Run(make<Source>());
    return 0;
}
