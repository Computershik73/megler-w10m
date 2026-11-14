// The shell's start and its copies, tested on a PC: tools/build-tests.sh win builds this program (mg_shell_tests.exe)
// from the very files the phone's package is built from -- shell/Files.cpp, ShellLog.cpp, Startup.cpp, BundledApks.cpp,
// Placement.cpp, SharedApks.cpp, Settings.cpp, Strings.cpp and player/PermissionRules.cpp -- with the few stand-ins below
// for the rest of the shell, and runs it on a scratch folder of its own:
//
//   - paths that are not absolute (made from a folder Windows gave none of: a package on a memory card may have no shared
//     folder) refused by every function of shell/Files.cpp, nothing made, written, listed or removed at a drive's root, and
//     no drive's or volume's root removed whole;
//   - files::same; files::copyWhole through Windows's cache and past it (COPY_FILE_NO_BUFFERING, from 8 MB), with what
//     its progress is told; files::mend of a copy with a byte wrong; files::treeSize of a folder with folders in it;
//   - the log: the core's word on a process about to end ("host: ..." at the error level) and writeNow's line are in the
//     file the moment they are said, not at the writer's next turn; a write of the file made to stand holds up no other
//     thread's line, and what was said meanwhile follows it in the file;
//   - the start's steps and its watch (shell/Startup.h): a step said at once, the watch's lines while the window is not
//     shown, and none after it is;
//   - bundledApks::plan on a library and a shared folder in each state a phone's can be in (a first start's, all copied,
//     the stamp missing, a copy that is not the package's, microG's of another size, an installed app whose package
//     is gone), and bundledApks::start's copies made on their thread, each said in the log before it begins and after,
//     then the once-a-build check, which leaves out the copies just made;
//   - which folders of the shared folder's apks an installed package reads and which go (shell/SharedApks.h): a package
//     installed by Windows's own installer (Open the Package) or by Windows after Megler gave up on it, one waiting for
//     Windows's installer, Megler's own installs, Update All, Google Play services from Megler's newer APK, packages made
//     before Megler wrote down which folder a package reads; and on the disk, make's records, a listing read, decided and
//     carried out, a folder whose APK an app has open left whole, each decision said once;
//   - an app's permissions (player/PermissionRules.cpp): the user's answers kept in its settings and forgotten, what the app
//     is told of each kind of group -- a refused group Megler has nothing behind, Megler's no over Windows' yes, a group
//     asked every time --, the groups a manifest asks for, and Google Play services' own location in Megler's settings;
//   - the versions of the apps' own packages (shell/PackageVersions.h): which Megler made one, the next one a build makes
//     of an app (Aurora Store's update of an app made its package again with the same version, 0x80073CFB), Windows' order;
//   - the phone's picker and camera for an app (player/PickerHost.cpp): Windows' picker's filter, start and view for the
//     MIME types an app asks for, the flags that answer a request in place of the user (pick-cancel, pick-file-<name>), and
//     files handed over copied into the request's folder -- a name already there made unique, one missing a failure -- with
//     the core told (megler_pick_done, stood in for below), and the thunk answering from the flags with no window to show a
//     picker over. The pickers themselves need a user and an app's window: not here.
//
// A Lumia 630's Megler 0.1.0.671 was ended at every start while it made those copies on its UI thread before its window
// was shown (library/Library.cpp); what is tested here is the way they are made since.
//
// Usage: mg_shell_tests <scratch folder>. Each check is a line; the last line counts them, and the exit code is 1 when one
// failed. Not an app container: what Windows's broker refuses an app is not found out here.
#include "pch.h"

#include "library/Library.h"
#include "player/PermissionHost.h"
#include "player/PickerHost.h"
#include "shell/AppPackage.h"
#include "shell/BundledApks.h"
#include "shell/Files.h"
#include "shell/PackageVersions.h"
#include "shell/Placement.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/SharedApks.h"
#include "shell/ShellLog.h"
#include "shell/Startup.h"
#include "shell/Strings.h"

#include "megler.h"

#include <atomic>
#include <cstdio>
#include <thread>

namespace megler::app {

namespace {
std::wstring gLocal, gInstall, gShared;
} // namespace

// What the copies' code asks of the rest of the shell: the folders, the tests' own; no app's own package; no report of an
// APK (the core is not in this program), and what a report says of a permission as library/Library.cpp says it.
ApkReport ApkReport::parse(const std::string&) { return ApkReport(); }
bool ApkReport::asks(const std::string& permission) const {
    if (!permissionsKnown) return true;
    for (size_t i = 0; i < permissions.size(); ++i)
        if (permissions[i] == permission) return i >= permissionMaxSdk.size() || permissionMaxSdk[i] == 0 || permissionMaxSdk[i] >= 34;
    return false;
}

namespace platform {
const std::wstring& localStateDirectory() { return gLocal; }
const std::wstring& installDirectory() { return gInstall; }
std::string packageVersion() { return "0.1.0.0"; }
} // namespace platform

namespace appPackage {
std::wstring sharedFolder() { return gShared; }
const SingleApp& single() {
    static const SingleApp none;
    return none;
}
} // namespace appPackage

} // namespace megler::app

extern "C" char* megler_apk_report(const char*) { return nullptr; }
extern "C" void megler_free(void*) {}

// The core's side of the picker (player/PickerHost.cpp answers it): each answer kept for the tests, from whatever thread.
namespace {
struct PickDone {
    unsigned id = 0;
    int status = -1;
    std::vector<std::string> names;
};
std::mutex gPickMutex;
std::vector<PickDone> gPicksDone;
} // namespace
extern "C" void megler_pick_done(unsigned id, int status, int count, const char* const* names) {
    PickDone d;
    d.id = id;
    d.status = status;
    for (int i = 0; i < count; ++i) d.names.push_back(names[i]);
    std::lock_guard<std::mutex> lock(gPickMutex);
    gPicksDone.push_back(d);
}

using namespace megler::app;

namespace {

int gChecks = 0, gFailed = 0;

void check(bool ok, const std::string& what) {
    gChecks++;
    if (!ok) gFailed++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
}

/// `n` bytes of a sequence of its own (`seed`), into `path`.
bool makeFile(const std::wstring& path, uint64_t n, uint32_t seed) {
    std::string data(size_t(n), '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (char& c : data) {
        x = x * 1664525u + 1013904223u;
        c = char(x >> 24);
    }
    return files::write(path, data);
}

/// The byte at `at` of `path` made another.
bool spoil(const std::wstring& path, uint64_t at) {
    HANDLE h = CreateFile2(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos = {};
    pos.QuadPart = LONGLONG(at);
    char c = 0;
    DWORD n = 0;
    bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, &c, 1, &n, nullptr) && n == 1;
    c = char(c ^ 0x5A);
    ok = ok && SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && WriteFile(h, &c, 1, &n, nullptr) && n == 1 && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

/// The log's file as it is: what is in it without the writer's next turn.
std::string logText() {
    std::string text;
    files::read(gLocal + L"\\megler.log", text);
    return text;
}

/// The log's file once every line said is written out.
std::string writtenLog() {
    shellLog::flush();
    return logText();
}

size_t count(const std::string& text, const std::string& what) {
    size_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) n++;
    return n;
}

/// What a copy's progress was told: the stages in the order they came, the last of each.
struct Told {
    std::vector<files::Stage> stages;
    uint64_t lastDone = 0, lastTotal = 0;
    files::Progress progress() {
        return [this](files::Stage stage, uint64_t done, uint64_t total) {
            if (stages.empty() || stages.back() != stage) stages.push_back(stage);
            lastDone = done;
            lastTotal = total;
        };
    }
};

/// Paths that are not absolute -- what is made from a folder Windows gave none of, as a package on a memory card may have no
/// shared folder ("" + L"\\apks\\...") -- are refused, and nothing is made, written, listed or removed at a drive's root.
void testPaths(const std::wstring& root) {
    printf("paths that are not absolute\n");
    const std::wstring atRoot = L"\\megler-shell-tests-" + std::to_wstring(GetCurrentProcessId());
    check(!files::makeDirectories(atRoot + L"\\apks\\x") && GetLastError() == ERROR_BAD_PATHNAME,
          "makeDirectories refuses a folder at the drive's root (as \"\" + \"\\apks\\...\" would be)");
    check(!files::makeDirectory(atRoot) && GetLastError() == ERROR_BAD_PATHNAME, "makeDirectory refuses one");
    check(!files::write(atRoot + L".txt", "x") && GetLastError() == ERROR_BAD_PATHNAME, "write refuses a file at the drive's root");
    check(!files::append(atRoot + L".txt", "x") && GetLastError() == ERROR_BAD_PATHNAME, "append refuses one");
    files::write(gLocal + L"\\paths.txt", "x");
    check(!files::copy(gLocal + L"\\paths.txt", atRoot + L".txt") && GetLastError() == ERROR_BAD_PATHNAME, "copy refuses one");
    uint64_t mended = 0;
    check(!files::copyWhole(gLocal + L"\\paths.txt", atRoot + L".txt", mended) && GetLastError() == ERROR_BAD_PATHNAME, "copyWhole refuses one");
    check(!files::move(gLocal + L"\\paths.txt", atRoot + L".txt") && GetLastError() == ERROR_BAD_PATHNAME && files::exists(gLocal + L"\\paths.txt"),
          "move refuses one, and the file stays where it was");
    check(GetFileAttributesW(atRoot.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW((atRoot + L".txt").c_str()) == INVALID_FILE_ATTRIBUTES,
          "nothing was made at the drive's root");
    // (Should any of it have been made after all, it goes.)
    DeleteFileW((atRoot + L".txt").c_str());
    RemoveDirectoryW((atRoot + L"\\apks\\x").c_str());
    RemoveDirectoryW((atRoot + L"\\apks").c_str());
    RemoveDirectoryW(atRoot.c_str());
    // What is at the drive's root is not looked at either: Windows's own folder, there on this PC's drive.
    std::string text;
    check(!files::exists(L"\\Windows") && files::size(L"\\Windows\\win.ini") == 0 && !files::read(L"\\Windows\\win.ini", text),
          "exists, size and read do not look at the drive's root (\\Windows)");
    check(files::list(L"", L"*").empty() && files::listFolders(L"", L"*").empty() && files::listFolders(L"\\", L"*").empty() &&
              files::list(L"\\Windows", L"*.ini").empty(),
          "the lists of no folder and of the drive's root are empty");
    check(!files::removeTree(L"") && GetLastError() == ERROR_BAD_PATHNAME && !files::removeTree(atRoot) && GetLastError() == ERROR_BAD_PATHNAME,
          "removeTree refuses no folder and a folder at the drive's root");
    // A drive's root is not removed whole: tried on a letter no drive has, so that nothing could go were it not refused.
    wchar_t letter = 0;
    const DWORD drives = GetLogicalDrives();
    for (wchar_t c = L'Z'; c >= L'D' && !letter; c--)
        if (!(drives & (1u << (c - L'A')))) letter = c;
    if (letter) {
        const std::wstring drive = std::wstring(1, letter) + L":\\";
        bool refused = true;
        for (const std::wstring& r : {drive, drive.substr(0, 2), L"\\\\?\\" + drive, std::wstring(L"\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\"),
                                      std::wstring(L"\\\\no-such-server\\share")}) {
            SetLastError(0);
            refused = !files::removeTree(r) && GetLastError() == ERROR_BAD_PATHNAME && refused;
        }
        check(refused, "removeTree refuses a drive's root, a volume's and a share's (" + narrow(drive) + ", " + narrow(drive.substr(0, 2)) +
                           ", \\\\?\\" + narrow(drive) + ", \\\\?\\Volume{...}\\, \\\\server\\share)");
    }
    check(count(writtenLog(), "files: a path that is not absolute was refused") == 1 &&
              writtenLog().find("\"" + narrow(atRoot) + "\\apks\\x\"") != std::string::npos,
          "the log says the first path refused, once");
    // An absolute path is as it was.
    check(files::makeDirectories(root + L"\\paths\\a\\b") && files::write(root + L"\\paths\\a\\b\\c.txt", "c") && files::removeTree(root + L"\\paths") &&
              !files::exists(root + L"\\paths"),
          "an absolute path is made, written and removed as before");
}

/// The package volume every package Megler installs goes to: the one its own install folder is on (shell/Placement.h,
/// volumeHolding), as Windows's package manager would list them on a PC, on a phone (the data volume mounted at C:\Data,
/// perhaps also called U:), and with a memory card.
void testPlacement(const std::wstring& root) {
    printf("placement\n");
    using placement::Volume;
    using placement::volumeHolding;
    const std::vector<Volume> pc = {{L"\\\\?\\Volume{11111111-0000-0000-0000-000000000000}", L"C:\\Program Files\\WindowsApps", L"C:\\"},
                                    {L"\\\\?\\Volume{22222222-0000-0000-0000-000000000000}", L"D:\\WindowsApps", L"D:\\"}};
    check(volumeHolding(pc, L"C:\\Program Files\\WindowsApps\\Megler_0.1.0.673_x64__7p434m16tbgc6") == 0, "a PC: Megler on the system's volume");
    check(volumeHolding(pc, L"D:\\WindowsApps\\Megler_0.1.0.673_x64__7p434m16tbgc6") == 1, "a PC: Megler on another drive's");
    check(volumeHolding(pc, L"d:\\windowsapps\\MEGLER_0.1.0.673_x64__7p434m16tbgc6\\") == 1 &&
              volumeHolding(pc, L"\\\\?\\D:\\WindowsApps\\Megler_0.1.0.673_x64__7p434m16tbgc6") == 1 &&
              volumeHolding(pc, L"D:/WindowsApps/Megler_0.1.0.673_x64__7p434m16tbgc6") == 1,
          "names compared without regard to case, \"\\\\?\\\", '/' or a separator at the end");
    // A phone: its packages in C:\Data\Programs\WindowsApps (C:\data, as Windows gives Megler its install folder), a card's on D:.
    const std::vector<Volume> phone = {{L"\\\\?\\Volume{aaaaaaaa-0000-0000-0000-000000000000}", L"C:\\Data\\Programs\\WindowsApps", L"C:\\Data\\"},
                                       {L"\\\\?\\Volume{bbbbbbbb-0000-0000-0000-000000000000}", L"D:\\WPSystem\\Apps", L"D:\\"}};
    check(volumeHolding(phone, L"C:\\data\\Programs\\WindowsApps\\Megler_0.1.0.673_arm__7p434m16tbgc6") == 0, "a phone: Megler in its memory");
    check(volumeHolding(phone, L"D:\\WPSystem\\Apps\\Megler_0.1.0.673_arm__7p434m16tbgc6") == 1, "a phone: Megler on its memory card");
    // A store path is a folder: one whose name begins as another's is not in it; the mount point is then what holds it.
    check(volumeHolding(phone, L"D:\\WPSystem\\AppsOld\\Megler_0.1.0.673_arm__7p434m16tbgc6") == 1, "a folder named as a store path begins is not in it");
    // The store path named by another drive than the install folder's (a phone's data volume as U:): its volume's name, as the
    // folder's own handle gives it, says which; without it, the longest mount point the folder is under.
    const std::vector<Volume> lettered = {{L"\\\\?\\Volume{aaaaaaaa-0000-0000-0000-000000000000}\\", L"U:\\Programs\\WindowsApps", L"U:\\"},
                                          {L"\\\\?\\Volume{cccccccc-0000-0000-0000-000000000000}", L"C:\\Programs\\WindowsApps", L"C:\\"},
                                          {L"\\\\?\\Volume{bbbbbbbb-0000-0000-0000-000000000000}", L"D:\\WPSystem\\Apps", L"D:\\"}};
    check(volumeHolding(lettered, L"C:\\data\\Programs\\WindowsApps\\Megler_0.1.0.673_arm__7p434m16tbgc6",
                        L"\\\\?\\Volume{AAAAAAAA-0000-0000-0000-000000000000}\\") == 0,
          "a store path named by another drive: the volume the folder's handle names");
    check(volumeHolding(lettered, L"C:\\data\\Programs\\WindowsApps\\Megler_0.1.0.673_arm__7p434m16tbgc6") == 1,
          "without the folder's volume, the mount point it is under");
    const std::vector<Volume> mounted = {{L"", L"", L"C:\\"}, {L"", L"", L"C:\\Data\\"}};
    check(volumeHolding(mounted, L"C:\\Data\\Programs\\WindowsApps\\Megler_0.1.0.673_arm__7p434m16tbgc6") == 1, "the longest mount point it is under");
    check(volumeHolding(phone, L"E:\\Apps\\Megler_0.1.0.673_arm__7p434m16tbgc6") == -1 && volumeHolding({}, L"C:\\Program Files\\WindowsApps\\Megler") == -1 &&
              volumeHolding(pc, L"") == -1 && volumeHolding(pc, L"WindowsApps\\Megler") == -1 && volumeHolding({{L"", L"", L""}}, L"C:\\x") == -1,
          "none where no volume holds it, there are none, or the folder is none or not absolute");
    // Where the apps are kept: the volume the user chose, by its name; Megler's own until then, and the question where there
    // is more than one to choose of.
    using placement::appsPlace;
    using placement::volumeNamed;
    check(volumeNamed(phone, L"\\\\?\\Volume{BBBBBBBB-0000-0000-0000-000000000000}\\") == 1 && volumeNamed(phone, L"") == -1 &&
              volumeNamed(phone, L"\\\\?\\Volume{dddddddd-0000-0000-0000-000000000000}") == -1 && volumeNamed(mounted, L"") == -1,
          "a volume by its name, without regard to case or a separator at the end; none for no name");
    placement::AppsPlace place = appsPlace(phone, L"", 1);
    check(place.volume == 1 && !place.chosen && place.ask, "nothing chosen, a phone with a card: Megler's own volume, and the user is asked");
    place = appsPlace(phone, L"\\\\?\\Volume{aaaaaaaa-0000-0000-0000-000000000000}", 1);
    check(place.volume == 0 && place.chosen && !place.ask, "the phone's memory chosen, Megler on the card: the phone's memory, no question");
    place = appsPlace(phone, L"\\\\?\\Volume{bbbbbbbb-0000-0000-0000-000000000000}", 0);
    check(place.volume == 1 && place.chosen && !place.ask, "the card chosen, Megler in the phone's memory: the card");
    const std::vector<Volume> alone = {phone[0]};
    place = appsPlace(alone, L"", 0);
    check(place.volume == 0 && !place.chosen && !place.ask, "one volume: nothing to ask");
    place = appsPlace(alone, L"\\\\?\\Volume{bbbbbbbb-0000-0000-0000-000000000000}", 0);
    check(place.volume == 0 && !place.chosen && !place.ask, "the card chosen and taken out for good: Megler's own, nothing to ask with one left");
    std::vector<Volume> cardOut = phone;
    cardOut[1].offline = true;
    place = appsPlace(cardOut, L"\\\\?\\Volume{bbbbbbbb-0000-0000-0000-000000000000}", 0);
    check(place.volume == 0 && !place.chosen && !place.ask, "the card chosen and not there now: Megler's own, and no question while one is left");
    std::vector<Volume> otherCard = phone;
    otherCard[1].name = L"\\\\?\\Volume{eeeeeeee-0000-0000-0000-000000000000}";
    place = appsPlace(otherCard, L"\\\\?\\Volume{bbbbbbbb-0000-0000-0000-000000000000}", 0);
    check(place.volume == 0 && !place.chosen && place.ask, "another card than the one chosen: the user is asked again");
    place = appsPlace(phone, L"", -1);
    check(place.volume == -1 && place.ask, "Megler's own volume not known: Windows is told none, and the user is asked");
    place = appsPlace(mounted, L"", 1);
    check(place.volume == 1 && !place.ask, "volumes without names, by which no choice could be kept: no question");
    place = appsPlace({}, L"\\\\?\\Volume{bbbbbbbb-0000-0000-0000-000000000000}", -1);
    check(place.volume == -1 && !place.chosen && !place.ask, "no volumes listed: none, no question");
    std::vector<Volume> titled = phone;
    titled[0].system = true;
    check(placement::volumeTitle(titled[0], true) == "The phone's memory" && placement::volumeTitle(titled[1], true) == "The memory card (D:)" &&
              placement::volumeTitle({L"\\\\?\\Volume{x}", L"", L""}, true) == "The memory card",
          "a phone's volumes as Megler's screens call them");
    check(placement::volumeTitle({pc[0].name, pc[0].storePath, pc[0].mountPoint, true}, false) == "Drive C: (Windows's)" &&
              placement::volumeTitle(pc[1], false) == "Drive D:" && placement::volumeTitle({L"\\\\?\\Volume{x}", L"", L""}, false) == "\\\\?\\Volume{x}",
          "a PC's: " + placement::volumeTitle(pc[1], false));
    // The drives the log says.
    check(placement::driveOf(L"C:\\Data\\Users") == L"C:" && placement::driveOf(L"d:/WPSystem") == L"D:" && placement::driveOf(L"\\\\?\\e:\\x") == L"E:" &&
              placement::driveOf(L"\\\\?\\Volume{abc}\\x") == L"\\\\?\\Volume{abc}" && placement::driveOf(L"\\\\server\\share\\x") == L"\\\\server\\share" &&
              placement::driveOf(L"\\\\?\\UNC\\server\\share\\x") == L"\\\\server\\share" && placement::driveOf(L"\\apks\\x").empty() &&
              placement::driveOf(L"").empty() && placement::driveOf(L"C:").empty(),
          "the drive of a path, of a volume's name, of a share; none of a path that is not absolute");
    // This PC's own: the scratch folder's volume as its handle names it, and the system's drive.
    const std::wstring volume = placement::volumeOf(root);
    check(volume.rfind(L"\\\\?\\Volume{", 0) == 0 && volume.back() == L'}', "the scratch folder's volume, from its handle: " + narrow(volume));
    check(placement::volumeOf(L"").empty() && placement::volumeOf(root + L"\\none").empty(), "no volume for no folder, nor for one that is not there");
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    check(placement::onSystemDrive(system) && placement::onSystemDrive(L"") && !placement::onSystemDrive(system[0] == L'Q' ? L"R:\\x" : L"Q:\\x"),
          "the system's drive is told from another");
    check(placement::summary(root, gLocal, std::wstring()).find(", no shared folder") != std::string::npos &&
              placement::summary(root, gLocal, gShared).find("the shared folder on " + narrow(placement::driveOf(gShared))) != std::string::npos,
          "the log's summary: " + placement::summary(root, gLocal, gShared));
}

/// What an app's own package lacks of what Megler put in the shared folder for it (shell/Placement.h, missingFor): none, an
/// APK or Android's files not there -- a shared folder of another storage's has nothing of Megler's -- and what its screen says.
void testMissing(const std::wstring& root) {
    printf("an app's own package's files in the shared folder\n");
    using placement::Missing;
    const std::wstring shared = root + L"\\missing\\shared", folder = shared + L"\\apks\\MeglerApp.x\\0123", apk = folder + L"\\app.apk",
                       android = shared + L"\\android\\0.1.0.0";
    check(files::makeDirectories(folder) && files::makeDirectories(android) && files::write(apk, std::string("PK\3\4", 4) + "the rest") &&
              files::write(android + L"\\complete", "0.1.0.0"),
          "a shared folder as Megler fills it for an app");
    check(placement::missingFor(shared, apk, android) == Missing::nothing, "all there: nothing missing");
    check(placement::missingFor(std::wstring(), apk, android) == Missing::sharedFolder, "no shared folder");
    check(placement::missingFor(shared, std::wstring(), android) == Missing::apk, "no APK to look for");
    check(placement::missingFor(shared, apk, std::wstring()) == Missing::nothing, "no Android files named: not looked for");
    files::remove(android + L"\\complete");
    check(placement::missingFor(shared, apk, android) == Missing::androidFiles, "Android's files without their file complete");
    files::write(android + L"\\complete", "0.1.0.0");
    files::write(apk, "not a ZIP");
    check(placement::missingFor(shared, apk, android) == Missing::apk, "an APK that is no ZIP");
    files::remove(apk);
    check(placement::missingFor(shared, apk, android) == Missing::apk, "an APK that is not there");
    const std::wstring card = root + L"\\missing\\card-shared";
    files::makeDirectories(card);
    check(placement::missingFor(card, card + L"\\apks\\MeglerApp.x\\0123\\app.apk", card + L"\\android\\0.1.0.0") == Missing::apk,
          "another storage's shared folder, with nothing of Megler's in it");
    for (const Missing missing : {Missing::sharedFolder, Missing::apk, Missing::androidFiles}) {
        const std::string text = placement::missingText(missing, "Telegram", shared, true);
        check(text.rfind("Telegram can't start: ", 0) == 0 && text.find("Settings › System › Storage") != std::string::npos &&
                  text.find("install the app again from Megler's Library") != std::string::npos,
              "its screen says why and what to do: " + text);
    }
    check(placement::missingText(Missing::nothing, "Telegram", shared, true).empty(), "nothing missing, nothing said");
    check(placement::missingText(Missing::apk, "Telegram", shared, true).find("Megler's Settings, Apps' Storage") != std::string::npos,
          "and that Megler's Settings move its apps to where Megler is");
    check(placement::missingText(Missing::apk, "Telegram", shared, false).find("Settings › Apps") != std::string::npos,
          "on desktop Windows, the system drive and Settings › Apps");
    // What Megler says of itself at its start: nothing with a shared folder, wherever it is -- its apps find the one shared
    // folder from either storage -- and that no app can be installed where Windows gave it none.
    check(placement::meglerOnCardText(false, false, true).empty() && placement::meglerOnCardText(true, false, false).empty() &&
              placement::meglerOnCardText(true, false, true).empty(),
          "Megler with a shared folder says nothing: in the phone's memory, on a PC's other drive, on the memory card");
    const std::string none = placement::meglerOnCardText(true, true, true), pc = placement::meglerOnCardText(false, true, false),
                      inMemory = placement::meglerOnCardText(false, true, true);
    check(none.find("Megler is installed on the memory card") != std::string::npos && none.find("no shared folder there") != std::string::npos &&
              none.find("Settings › System › Storage") != std::string::npos && pc.find("no shared folder") != std::string::npos &&
              pc.find("system drive") != std::string::npos && inMemory.find("no shared folder") != std::string::npos &&
              inMemory.find("is installed on the memory card") == std::string::npos,
          "with no shared folder, on a phone's card, in its memory and on a PC: " + none);
}

void testFiles(const std::wstring& root) {
    printf("files\n");
    const std::wstring dir = root + L"\\files";
    files::makeDirectories(dir);
    const std::wstring a = dir + L"\\a.bin", b = dir + L"\\b.bin";
    check(makeFile(a, 300 * 1024 + 7, 1) && makeFile(b, 300 * 1024 + 7, 1), "two files of one sequence made");
    check(files::same(a, b), "same: alike files are the same");
    spoil(b, 200 * 1024);
    check(!files::same(a, b), "same: a byte changed in the middle is seen");
    makeFile(b, 300 * 1024 + 8, 1);
    check(!files::same(a, b), "same: files of two sizes are not the same");
    check(!files::same(a, dir + L"\\none.bin"), "same: a file that is not there is not the same");

    // Through Windows's cache: under unbufferedFrom.
    {
        Told told;
        uint64_t mended = 99;
        const bool ok = files::copyWhole(a, dir + L"\\small.copy", mended, told.progress());
        check(ok && mended == 0 && files::same(a, dir + L"\\small.copy"), "copyWhole: a small file copied whole, nothing mended");
        check(told.stages == std::vector<files::Stage>{files::Stage::copying, files::Stage::flushing, files::Stage::comparing},
              format("copyWhole: its progress told copying, putting on the disk, comparing, in that order (%u stages)", unsigned(told.stages.size())));
        check(told.lastDone == files::size(a) && told.lastTotal == files::size(a), "copyWhole: the comparing told of every byte");
    }
    // Past the cache: unbufferedFrom and more, of a size no whole number of sectors.
    const std::wstring big = dir + L"\\big.bin", bigCopy = dir + L"\\big.copy";
    const uint64_t bigSize = files::unbufferedFrom + 5 * 1024 * 1024 + 123;
    check(makeFile(big, bigSize, 2), format("a file of %llu bytes made", (unsigned long long)bigSize));
    {
        Told told;
        uint64_t mended = 99;
        const ULONGLONG began = GetTickCount64();
        const bool ok = files::copyWhole(big, bigCopy, mended, told.progress());
        check(ok && mended == 0 && files::same(big, bigCopy),
              format("copyWhole: a large file copied past the cache, whole, in %llu ms", (unsigned long long)(GetTickCount64() - began)));
        check(!told.stages.empty() && told.stages.front() == files::Stage::copying && told.stages.back() == files::Stage::comparing &&
                  told.lastDone == bigSize,
              "copyWhole: its progress told of the copy and of every byte compared");
    }
    // A byte wrong in the copy: its chunk written again from the source.
    {
        const uint64_t at = 5 * 1024 * 1024 + 4097;
        check(spoil(bigCopy, at) && !files::same(big, bigCopy), "a byte of the large copy changed");
        uint64_t mended = 0;
        std::vector<uint64_t> chunks;
        Told told;
        const bool ok = files::mend(big, bigCopy, mended, &chunks, told.progress());
        check(ok && mended == files::DiskFile::chunk && chunks.size() == 1 && chunks[0] == (at & ~uint64_t(files::DiskFile::chunk - 1)),
              format("mend: the chunk with the byte written again (%llu bytes mended, %u chunks)", (unsigned long long)mended, unsigned(chunks.size())));
        check(files::same(big, bigCopy), "mend: the copy is the source's byte for byte again");
        check(told.lastDone == bigSize, "mend: its progress told of every byte compared");
    }
    uint64_t none = 0;
    check(!files::copyWhole(dir + L"\\none.bin", dir + L"\\none.copy", none) && !files::exists(dir + L"\\none.copy"),
          "copyWhole: a file that is not there is not copied, and nothing is made");

    // A folder's APKs copied beside a copy of an APK, as Google Play services' splits are beside its shared copy
    // (appPackage::registerServiceApk): each whole through a .part of its own, those there of the same size kept, nothing else of
    // the folder taken (a cut resource table the core keeps beside a split, an .apk.part of a copy that never ended).
    {
        const std::wstring splits = dir + L"\\app.apk.splits", beside = dir + L"\\GmsCore.apk.splits";
        files::makeDirectories(splits);
        check(makeFile(splits + L"\\split_config.xhdpi.apk", 70 * 1024 + 3, 3) && makeFile(splits + L"\\split_config.ru.apk", 9 * 1024, 4) &&
                  files::write(splits + L"\\split_config.ru.apk.0a1b2c3d-ru-en.arsc", "cut") && files::write(splits + L"\\split_config.en.apk.part", "torn"),
              "a folder of two splits, a cut table and a torn copy made");
        int copied = -1;
        check(files::copyFilesWhole(splits, beside, L".apk", copied) && copied == 2 && files::same(splits + L"\\split_config.xhdpi.apk", beside + L"\\split_config.xhdpi.apk") &&
                  files::same(splits + L"\\split_config.ru.apk", beside + L"\\split_config.ru.apk"),
              "copyFilesWhole: the two APKs copied whole");
        check(files::list(beside, L"*").size() == 2, "copyFilesWhole: and nothing else of the folder, no .part left");
        check(files::copyFilesWhole(splits, beside, L".apk", copied) && copied == 0, "copyFilesWhole: again, those there of the same size kept");
        check(makeFile(splits + L"\\split_config.ru.apk", 9 * 1024 + 1, 5) && files::copyFilesWhole(splits, beside, L".apk", copied) && copied == 1 &&
                  files::same(splits + L"\\split_config.ru.apk", beside + L"\\split_config.ru.apk"),
              "copyFilesWhole: one of another size copied again");
        check(files::copyFilesWhole(dir + L"\\no-splits", dir + L"\\no-splits-beside", L".apk", copied) && copied == 0 && !files::exists(dir + L"\\no-splits-beside"),
              "copyFilesWhole: no folder, nothing to copy: no failure, and no folder made");
    }

    // A memory card's file system, as CopyFile2 is refused there for what it copies besides the bytes (ERROR_NOT_SUPPORTED,
    // ERROR_ENCRYPTION_FAILED): the bytes read and written, the time of writing kept. Not where reading and writing cannot help.
    {
        files::testing::refuseCopyFile2(ERROR_NOT_SUPPORTED);
        const std::wstring streamed = dir + L"\\streamed.copy";
        check(files::copy(a, streamed) && files::same(a, streamed) && files::lastWrite(a) == files::lastWrite(streamed),
              "copy: CopyFile2 refused, the bytes read and written, the time of writing kept");
        Told told;
        uint64_t mended = 99;
        const std::wstring bigStreamed = dir + L"\\big-streamed.copy";
        check(files::copyWhole(big, bigStreamed, mended, told.progress()) && mended == 0 && files::same(big, bigStreamed) &&
                  told.stages == std::vector<files::Stage>{files::Stage::copying, files::Stage::flushing, files::Stage::comparing} &&
                  told.lastDone == bigSize,
              "copyWhole: likewise, CopyFile2 past the cache and through it refused; then put on the disk and compared");
        files::testing::refuseCopyFile2(ERROR_ENCRYPTION_FAILED);
        check(files::copy(a, dir + L"\\decrypted.copy") && files::same(a, dir + L"\\decrypted.copy"), "copy: CopyFile2 refused for encryption, likewise");
        files::testing::refuseCopyFile2(ERROR_DISK_FULL);
        check(!files::copy(a, dir + L"\\full.copy") && GetLastError() == ERROR_DISK_FULL && !files::exists(dir + L"\\full.copy"),
              "copy: a full disk is not gone round, and says so");
        files::testing::refuseCopyFile2(0);
        check(count(writtenLog(), "files: CopyFile2 refused to copy") == 1, "the way round CopyFile2 is said once");
    }
    // Reads past the cache refused (a memory card's file system whose sectors are larger than a page, or that has no such
    // reads): read through the cache.
    {
        files::testing::refuseUncachedReads(ERROR_INVALID_PARAMETER);
        files::DiskFile disk(big);
        const bool wasUncached = disk.uncached();
        char head[4096];
        check(wasUncached && disk.read(0, head, sizeof(head)) && !disk.uncached(), "DiskFile: a read refused past the cache is made through it");
        uint64_t mended = 0;
        check(spoil(bigCopy, 3 * 1024 * 1024 + 5) && files::mend(big, bigCopy, mended) && mended == files::DiskFile::chunk && files::same(big, bigCopy),
              "mend: so, a byte wrong found and written again");
        files::testing::refuseUncachedReads(0);
        check(count(writtenLog(), "could not be read past Windows's cache") == 1, "the way round is said once");
    }
}

void testLog() {
    printf("log\n");
    shellLog::core(3, "host: abort() was called on thread 1 (the shell's tests say it); Windows ends the process now", nullptr);
    check(logText().find("host: abort() was called on thread 1 (the shell's tests say it)") != std::string::npos,
          "the core's word on a process about to end is in the file at once");
    shellLog::writeNow("a line the shell's tests write out at once");
    check(logText().find("shell: a line the shell's tests write out at once") != std::string::npos, "writeNow's line is in the file at once");
    shellLog::core(1, "an ordinary line of the shell's tests", nullptr);
    Sleep(600);
    check(logText().find("an ordinary line of the shell's tests") != std::string::npos, "an ordinary line is in the file at the writer's turn");

    // A write of the file that stands holds up no other thread's line: lines go into the buffer under a lock of their own,
    // and the file is written with that one let go. A fault's report wrote its trace lines one by one so, each under the
    // log's locks, while Tuya Smart's main thread waited in the core's mg_logv (0.1.0.671).
    static std::atomic<int> stage{0};
    shellLog::setWriteHook([] {
        if (stage.load() != 1) return;
        stage.store(2);
        for (int i = 0; i < 300 && stage.load() == 2; i++) Sleep(10);
    });
    stage.store(1);
    std::thread reporter([] { shellLog::core(3, "fault: a fault's report of the shell's tests, written out at once", nullptr); });
    for (int i = 0; i < 300 && stage.load() != 2; i++) Sleep(10);
    check(stage.load() == 2, "a fault's line is being written out, its write made to stand");
    const ULONGLONG began = GetTickCount64();
    shellLog::core(1, "a line said while a write of the file stands", nullptr);
    std::vector<std::string> ring;
    shellLog::take(0, ring);
    const ULONGLONG took = GetTickCount64() - began;
    check(took < 1000 && stage.load() == 2 && !ring.empty() && ring.back().find("a line said while a write of the file stands") != std::string::npos,
          format("a line said meanwhile, and the ring read, at once (%llu ms), the write still standing", (unsigned long long)took));
    stage.store(3);
    reporter.join();
    shellLog::setWriteHook(nullptr);
    check(logText().find("fault: a fault's report of the shell's tests, written out at once") != std::string::npos,
          "the fault's line is in the file as its thread goes on");
    const std::string written = writtenLog();
    const size_t fault = written.find("fault: a fault's report of the shell's tests"), meanwhile = written.find("a line said while a write of the file stands");
    check(fault != std::string::npos && meanwhile != std::string::npos && fault < meanwhile, "the line said meanwhile is in the file after it");
}

void testStartup() {
    printf("start\n");
    const char* const notShown = "the window is not shown yet: still testing the start; the UI thread: a place of the tests'";
    startup::launched([](unsigned) { return std::string("a place of the tests'"); }, {150, 300, 2500});
    startup::step("testing the start");
    check(logText().find("start: testing the start") != std::string::npos, "a step is in the file the moment it is said");
    check(startup::current() == "testing the start", "the step is the one said");
    // (Its two first marks passed, the third not: given a PC busy with other builds the time it needs.)
    for (int i = 0; i < 40 && count(logText(), notShown) < 2; i++) Sleep(50);
    check(count(logText(), notShown) == 2, format("the watch spoke at its two marks before the window was shown (%u times)", unsigned(count(logText(), notShown))));
    startup::shown();
    check(startup::isShown() && logText().find("start: the window is shown, ") != std::string::npos, "the window shown is said");
    Sleep(2700);
    check(count(logText(), notShown) == 2, "the watch says nothing once the window is shown, at its third mark either");
    check(startup::processAge() > 0, format("the process's age is known (%llu ms)", (unsigned long long)startup::processAge()));
}

namespace shared {

using sharedApks::App;
using sharedApks::Installed;
using sharedApks::Plan;
using sharedApks::Token;

constexpr uint64_t kMinute = 600000000ull, kHour = 60 * kMinute, kDay = 24 * kHour;
// A time of 2026's, as GetSystemTimeAsFileTime gives one.
constexpr uint64_t kNow = 0x01dd50e000000000ull;

const std::wstring kCandy = L"MeglerApp.com.king.candycrushsaga.2a041e0408f22f6b";
std::string full(const std::wstring& identity, const char* version) { return narrow(identity) + "_" + version + "_arm__7p434m16tbgc6"; }

/// A folder's name as keepInShared makes it: the time its copy was begun at, then a process's id.
std::wstring tokenAt(uint64_t time) {
    return widen(format("%08lx%08lx-%08lx", (unsigned long)(time >> 32), (unsigned long)(time & 0xffffffffu), 0x1a2bul));
}

/// A folder begun at `begun`, whose made.txt names `made` and was written at `madeTime`.
Token recorded(uint64_t begun, std::vector<std::string> made, uint64_t madeTime) {
    Token t;
    t.name = tokenAt(begun);
    t.recorded = true;
    t.made = std::move(made);
    t.madeTime = madeTime;
    return t;
}

/// A folder made up to 0.1.0.675, with no made.txt.
Token unrecorded(uint64_t begun) {
    Token t;
    t.name = tokenAt(begun);
    return t;
}

const sharedApks::Verdict* verdictOf(const Plan& p, const std::wstring& token) {
    for (const auto& v : p.verdicts)
        if (v.token == token) return &v;
    return nullptr;
}
bool kept(const Plan& p, const std::wstring& token) { return verdictOf(p, token) && verdictOf(p, token)->keep; }
bool removed(const Plan& p, const std::wstring& token) { return verdictOf(p, token) && !verdictOf(p, token)->keep; }
std::string why(const Plan& p, const std::wstring& token) { return verdictOf(p, token) ? verdictOf(p, token)->why : std::string("(no verdict)"); }
bool knownAs(const Plan& p, const std::wstring& token, const std::string& fullName, bool waitingDone) {
    return p.known.size() == 1 && p.known[0].token == token && p.known[0].fullName == fullName && p.known[0].waitingDone == waitingDone;
}

} // namespace shared

/*
 * Which folders of the shared folder's apks an installed package reads, and which go (shell/SharedApks.h). Up to 0.1.0.675
 * a folder installed.txt and the library did not name was removed an hour after it was begun, though a package Windows's
 * own installer installed read it: a user's Candy Crush Saga (673), installed through Open the Package after Windows refused
 * Megler's install, found no APK from 17:41 on. That case first; then each other way a package comes to be installed, and
 * what still goes.
 */
void testSharedApks(const std::wstring& root) {
    printf("the apps' APKs in the shared folder\n");
    using namespace shared;
    const std::string f676 = full(kCandy, "0.1.0.676"), f677 = full(kCandy, "0.1.0.677"), f673 = full(kCandy, "0.1.0.673");

    check(sharedApks::begunAt(tokenAt(kNow - 3 * kHour)) == kNow - 3 * kHour && sharedApks::begunAt(L"01dd50e0") == 0 &&
              sharedApks::begunAt(L"01dd50e00000000g-00001a2b") == 0 && sharedApks::begunAt(L"") == 0,
          "a folder's time from its name; none from a name that has none");

    // Open the Package: Megler made the package (the folder's made.txt names it) and Windows refused Megler's install; the
    // library's entry is still Megler's own copy, and installed.txt names nothing. The package waits in LocalState\packages.
    App candy;
    candy.identity = kCandy;
    candy.tokens = {recorded(kNow - 8 * kHour, {f676}, kNow - 8 * kHour)};
    candy.waiting = true;
    candy.waitingToken = candy.tokens[0].name;
    candy.waitingFullName = f676;
    const std::wstring opened = candy.tokens[0].name;
    Plan p = sharedApks::plan({candy}, {}, kNow);
    check(kept(p, opened) && why(p, opened).find("waits in LocalState\\packages for Windows's own installer") != std::string::npos && p.known.empty(),
          "refused, not yet opened: the folder of the package waiting for Windows's installer is kept, eight hours on: " + why(p, opened));
    // Windows's own installer installed it.
    p = sharedApks::plan({candy}, {{narrow(kCandy), f676}}, kNow);
    check(kept(p, opened) && why(p, opened) == "the installed " + f676 + " was made with it",
          "Open the Package: the folder its made.txt names for the installed package is kept, eight hours on (" + why(p, opened) + ")");
    check(knownAs(p, opened, f676, true) && p.forget.empty(),
          "Open the Package: the installed package's folder is known (installed.txt to name it, the library to read it), and the package that "
          "waited is the one installed (it goes)");
    // Once installed.txt names it and the package made has gone, as apply leaves them.
    App settled = candy;
    settled.waiting = false;
    settled.waitingToken.clear();
    settled.waitingFullName.clear();
    settled.installedToken = opened;
    settled.installedTime = kNow;
    p = sharedApks::plan({settled}, {{narrow(kCandy), f676}}, kNow + kDay);
    check(kept(p, opened) && knownAs(p, opened, f676, false), "Open the Package, a day on: still the installed package's");
    // The same without any record, as 673 left it: the installed package is kept whatever it reads (the code before removed it).
    App old;
    old.identity = kCandy;
    old.tokens = {unrecorded(kNow - 8 * kHour)};
    p = sharedApks::plan({old}, {{narrow(kCandy), f673}}, kNow);
    check(kept(p, old.tokens[0].name) && why(p, old.tokens[0].name).find("may read this one") != std::string::npos && p.known.empty(),
          "a package made up to 675, installed by another than Megler: the folder it may read is kept (" + why(p, old.tokens[0].name) + ")");
    // An install Windows finished after Megler had given up on it (no record waiting: Megler's run ended), or one from a file
    // manager: the same, by made.txt.
    App late = candy;
    late.waiting = false;
    late.waitingToken.clear();
    late.waitingFullName.clear();
    p = sharedApks::plan({late}, {{narrow(kCandy), f676}}, kNow);
    check(kept(p, opened) && knownAs(p, opened, f676, false), "installed after Megler gave up on it: known by made.txt alone");

    // Megler's own install, then a newer APK of the app installed by Megler with the same Megler: the first folder's package
    // was replaced by the second's (installed.txt written after it), so the first goes, though its made.txt names the same
    // full name.
    {
        App app;
        app.identity = kCandy;
        app.tokens = {recorded(kNow - 10 * kHour, {f676}, kNow - 10 * kHour), recorded(kNow - 2 * kHour, {f676}, kNow - 2 * kHour)};
        app.tokens[1].read = true;
        app.installedToken = app.tokens[1].name;
        app.installedTime = kNow - 2 * kHour + 5 * kMinute;
        p = sharedApks::plan({app}, {{narrow(kCandy), f676}}, kNow);
        check(removed(p, app.tokens[0].name) && why(p, app.tokens[0].name).find("before Megler installed that package from") != std::string::npos &&
                  kept(p, app.tokens[1].name) && knownAs(p, app.tokens[1].name, f676, false),
              "a newer APK installed by Megler: the older folder of the same full name goes (" + why(p, app.tokens[0].name) + ")");
        // And one made after Megler's install of the same full name, which Windows's installer may have installed since, stays.
        app.tokens.push_back(recorded(kNow - 90 * kMinute, {f676}, kNow - 90 * kMinute));
        p = sharedApks::plan({app}, {{narrow(kCandy), f676}}, kNow);
        check(kept(p, app.tokens[1].name) && kept(p, app.tokens[2].name) && p.known.empty() &&
                  why(p, app.tokens[2].name).find("which of them it reads cannot be told") != std::string::npos,
              "one made of the same full name after Megler's install: both kept, neither known for sure");
    }

    // Update All refused: the package of before is still installed and reads the same folder, which the new package waits with.
    {
        App app;
        app.identity = kCandy;
        app.tokens = {recorded(kNow - 5 * kDay, {f676, f677}, kNow - kHour)};
        app.tokens[0].read = true;
        app.installedToken = app.tokens[0].name;
        app.installedTime = kNow - 5 * kDay + kMinute;
        app.waiting = true;
        app.waitingToken = app.tokens[0].name;
        app.waitingFullName = f677;
        p = sharedApks::plan({app}, {{narrow(kCandy), f676}}, kNow);
        check(kept(p, app.tokens[0].name) && knownAs(p, app.tokens[0].name, f676, false), "Update All refused: the folder is the installed package's");
        p = sharedApks::plan({app}, {{narrow(kCandy), f677}}, kNow);
        check(kept(p, app.tokens[0].name) && knownAs(p, app.tokens[0].name, f677, true),
              "then opened in Windows's installer: still that folder, and the package that waited is the one installed");
    }

    // Google Play services' package made from Megler's newer APK (a folder of its own), refused, then installed by Windows's
    // installer: its folder is the installed package's; the one of before stays while the library reads it, and goes after.
    {
        const std::wstring gms = L"MeglerApp.com.google.android.gms.d1e2f3a4b5c6d7e8";
        const std::string oldGms = full(gms, "0.1.0.676"), newGms = full(gms, "0.1.0.677");
        App app;
        app.identity = gms;
        app.tokens = {recorded(kNow - 3 * kDay, {oldGms}, kNow - 3 * kDay), recorded(kNow - 3 * kHour, {newGms}, kNow - 3 * kHour)};
        app.tokens[0].read = true;
        app.installedToken = app.tokens[0].name;
        app.installedTime = kNow - 3 * kDay + 3 * kMinute;
        app.waiting = true;
        app.waitingToken = app.tokens[1].name;
        app.waitingFullName = newGms;
        p = sharedApks::plan({app}, {{narrow(gms), newGms}}, kNow);
        check(kept(p, app.tokens[1].name) && knownAs(p, app.tokens[1].name, newGms, true) && kept(p, app.tokens[0].name) &&
                  why(p, app.tokens[0].name) == "the library reads it",
              "Google Play services from Megler's newer APK, installed by Windows's installer: its folder known, the old one kept while the "
              "library reads it");
        app.tokens[0].read = false;
        p = sharedApks::plan({app}, {{narrow(gms), newGms}}, kNow);
        check(removed(p, app.tokens[0].name), "the old one goes once the library reads the new: " + why(p, app.tokens[0].name));
    }

    // Two folders made for the same full name, neither installed by Megler: which one the package reads cannot be told, and
    // both stay.
    {
        App app;
        app.identity = kCandy;
        app.tokens = {recorded(kNow - 6 * kHour, {f676}, kNow - 6 * kHour), recorded(kNow - 4 * kHour, {f676}, kNow - 4 * kHour)};
        p = sharedApks::plan({app}, {{narrow(kCandy), f676}}, kNow);
        check(kept(p, app.tokens[0].name) && kept(p, app.tokens[1].name) && p.known.empty(), "two folders of the same full name: both kept, neither known");
    }

    // Packages made up to 0.1.0.675 (no made.txt): installed.txt's folder and any begun after it stay, one begun before it goes;
    // with no installed.txt, all stay.
    {
        App app;
        app.identity = kCandy;
        app.tokens = {unrecorded(kNow - 3 * kDay), unrecorded(kNow - 2 * kDay), unrecorded(kNow - kDay)};
        app.installedToken = app.tokens[1].name;
        app.installedTime = kNow - 2 * kDay + 5 * kMinute;
        p = sharedApks::plan({app}, {{narrow(kCandy), f673}}, kNow);
        check(removed(p, app.tokens[0].name) && kept(p, app.tokens[1].name) && kept(p, app.tokens[2].name) && p.known.empty(),
              "made up to 675, installed by Megler: the folder of before its install goes (" + why(p, app.tokens[0].name) +
                  "), installed.txt's and the later one stay");
        app.installedToken.clear();
        app.installedTime = 0;
        p = sharedApks::plan({app}, {{narrow(kCandy), f673}}, kNow);
        check(kept(p, app.tokens[0].name) && kept(p, app.tokens[1].name) && kept(p, app.tokens[2].name),
              "made up to 675, no installed.txt: every folder stays");
        // A package Megler made since and Windows refused, waiting: kept too; and the old ones still, the installed one not
        // being known.
        app.tokens.push_back(recorded(kNow - 2 * kHour, {f677}, kNow - 2 * kHour));
        app.waiting = true;
        app.waitingToken = app.tokens[3].name;
        app.waitingFullName = f677;
        p = sharedApks::plan({app}, {{narrow(kCandy), f673}}, kNow);
        check(kept(p, app.tokens[0].name) && kept(p, app.tokens[3].name), "made up to 675, a newer package refused since: all stay");
        // Once the newer one is installed, it is known, and the old ones go.
        p = sharedApks::plan({app}, {{narrow(kCandy), f677}}, kNow);
        check(knownAs(p, app.tokens[3].name, f677, true) && removed(p, app.tokens[0].name) && removed(p, app.tokens[1].name) &&
                  removed(p, app.tokens[2].name),
              "and once it is installed, the old ones go");
        // A package made up to 675 waits (no record beside it), its app not installed: what it may read stays.
        App waiting;
        waiting.identity = kCandy;
        waiting.tokens = {unrecorded(kNow - 3 * kDay), unrecorded(kNow - kDay)};
        waiting.waiting = true;
        p = sharedApks::plan({waiting}, {}, kNow);
        check(kept(p, waiting.tokens[0].name) && kept(p, waiting.tokens[1].name), "a package made up to 675 waiting, with no record: every folder stays");
    }

    // What goes: an app not installed, its installed.txt with it; but the library's, one begun within the hour, and one whose
    // made.txt cannot be read stay. A copy that never finished goes.
    {
        App app;
        app.identity = kCandy;
        app.tokens = {recorded(kNow - 2 * kDay, {f676}, kNow - 2 * kDay), recorded(kNow - 30 * kMinute, {}, 0), unrecorded(kNow - 3 * kDay),
                      unrecorded(kNow - 5 * kHour), recorded(kNow - 4 * kDay, {f676}, kNow - 4 * kDay)};
        app.tokens[2].read = true;
        app.tokens[3].name += L".part";
        app.tokens[4].recorded = false;
        app.tokens[4].made.clear();
        app.tokens[4].unreadable = true;
        app.installedToken = app.tokens[0].name;
        app.installedTime = kNow - 2 * kDay;
        p = sharedApks::plan({app}, {}, kNow);
        check(removed(p, app.tokens[0].name) && why(p, app.tokens[0].name).find("is not installed") != std::string::npos && p.forget.size() == 1 &&
                  p.known.empty(),
              "not installed: its folder goes, and its installed.txt (" + why(p, app.tokens[0].name) + ")");
        check(kept(p, app.tokens[1].name) && why(p, app.tokens[1].name).find("within the hour") != std::string::npos, "one begun within the hour stays");
        check(kept(p, app.tokens[2].name) && why(p, app.tokens[2].name) == "the library reads it", "the library's stays");
        check(removed(p, app.tokens[3].name) && why(p, app.tokens[3].name) == "a copy that never finished", "a copy that never finished goes");
        check(kept(p, app.tokens[4].name) && why(p, app.tokens[4].name).find("could not be read") != std::string::npos,
              "one whose made.txt cannot be read stays");
        // Installed now as another package than any folder was made for (a package made by another Megler): the installed
        // one is recognised nowhere, so what it may read stays.
        p = sharedApks::plan({app}, {{narrow(kCandy), full(kCandy, "0.1.0.680")}}, kNow);
        check(kept(p, app.tokens[0].name) && p.forget.empty(), "installed as a package no folder names: installed.txt's folder stays");
    }

    // On the disk: the records make writes, then a listing read, decided and carried out (look, plan, apply).
    {
        const std::wstring disk = root + L"\\shared-apks", apks = disk + L"\\apks", packages = root + L"\\shared-apks-packages";
        files::removeTree(disk);
        files::removeTree(packages);
        files::makeDirectories(packages);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const uint64_t now = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const std::wstring other = L"MeglerApp.com.example.gone.0123456789abcdef";
        const std::wstring opened = tokenAt(now - 8 * kHour), older = tokenAt(now - 9 * kHour), open = tokenAt(now - 7 * kHour),
                           gone = tokenAt(now - 2 * kDay), left = tokenAt(now - 3 * kHour) + L".part";
        // (A folder of Candy Crush's, and one of the other app's.)
        const auto ofCandy = [&](const std::wstring& token) { return apks + L"\\" + kCandy + L"\\" + token; };
        const auto ofOther = [&](const std::wstring& token) { return apks + L"\\" + other + L"\\" + token; };
        bool madeAll = true;
        for (const std::wstring& folder : {ofCandy(opened), ofCandy(older), ofCandy(open), ofOther(gone), ofOther(left)})
            madeAll = files::makeDirectories(folder) && makeFile(folder + L"\\app.apk", 4096, 7) && madeAll;
        const std::wstring split = ofCandy(opened) + L"\\app.apk.splits\\split_config.armeabi_v7a.apk";
        madeAll = files::makeDirectories(ofCandy(opened) + L"\\app.apk.splits") && makeFile(split, 1024, 8) && madeAll;
        check(madeAll, "the folders made");
        // make's records: made.txt, with a second package made with the same folder (Update All), and the record beside the
        // package made.
        const bool recordedAll = sharedApks::recordMade(ofCandy(opened), f676) && sharedApks::recordMade(ofCandy(opened), f677) &&
                                 sharedApks::recordMade(ofCandy(opened), f676) && sharedApks::recordMade(ofCandy(older), f673) &&
                                 sharedApks::recordMade(ofCandy(open), full(kCandy, "0.1.0.670")) &&
                                 sharedApks::recordMade(ofOther(gone), full(other, "0.1.0.676"));
        std::string text;
        check(recordedAll && files::read(ofCandy(opened) + L"\\made.txt", text) && text == f676 + "\r\n" + f677 + "\r\n" &&
                  !files::exists(ofCandy(opened) + L"\\made.txt.part"),
              "recordMade: each full name once, a line each, written whole");
        const std::wstring appx = packages + L"\\" + kCandy + L".appx";
        makeFile(appx, 2048, 9);
        check(sharedApks::recordWaiting(appx, L"apks\\" + kCandy + L"\\" + opened, f677) && files::read(appx + L".txt", text) &&
                  text == "apk=apks\\" + narrow(kCandy) + "\\" + narrow(opened) + "\r\npackage=" + f677 + "\r\n",
              "recordWaiting: the folder and the full name beside the package made");
        // The library reads nothing here (its entries are Megler's own copies), and installed.txt names nothing: Windows's own
        // installer installed 677's package. One folder's APK is open in an app.
        const HANDLE held = CreateFile2((ofCandy(open) + L"\\app.apk").c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        std::vector<sharedApks::App> apps = sharedApks::look(disk, packages, {});
        const auto found = std::find_if(apps.begin(), apps.end(), [&](const App& a) { return a.identity == kCandy; });
        check(apps.size() == 2 && found != apps.end() && found->tokens.size() == 3 && found->waiting && found->waitingToken == opened &&
                  found->waitingFullName == f677 && found->installedToken.empty(),
              "look: both apps' folders, their records, and the package that waits");
        // (The open folder was made for 670's package, which is not installed: it is to go, and stays whole while its APK is open.)
        const Plan plan = sharedApks::plan(apps, {{narrow(kCandy), f677}}, now);
        check(removed(plan, open) && removed(plan, older) && kept(plan, opened), "plan: the open folder and the older one go, the installed one stays");
        sharedApks::apply(plan, disk, packages);
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
        check(files::exists(ofCandy(opened) + L"\\app.apk") && files::exists(split), "apply: the installed package's folder is there, its splits with it");
        check(!files::exists(ofCandy(older)), "apply: a folder made for a package no longer installed is gone");
        check(held != INVALID_HANDLE_VALUE && files::exists(ofCandy(open) + L"\\app.apk") && files::exists(ofCandy(open) + L"\\made.txt"),
              "apply: a folder whose APK an app has open stays whole, made.txt with it, for the next time");
        check(!files::exists(apks + L"\\" + other), "apply: an app not installed has nothing left, its folder neither");
        check(files::read(apks + L"\\" + kCandy + L"\\installed.txt", text) && text == narrow(opened),
              "apply: installed.txt names the installed package's folder");
        check(!files::exists(appx) && !files::exists(appx + L".txt"), "apply: the package that waited, installed now, is gone with its record");
        const std::string log = writtenLog(), at = "app package: apks\\" + narrow(kCandy) + "\\";
        check(log.find(at + narrow(opened) + " kept: the installed " + f677 + " was made with it") != std::string::npos &&
                  log.find(at + narrow(older) + " removed (4 KB): the installed " + f677 + " was not made with it") != std::string::npos &&
                  log.find(at + "installed.txt names " + narrow(opened) + " now") != std::string::npos &&
                  log.find("its APK being open in an app") != std::string::npos &&
                  log.find("folders of apps' APKs that nothing reads removed") != std::string::npos,
              "the log says each folder kept or removed and why, and installed.txt written");
        // A listing again: the open folder goes now, nothing else, and what was said is not said again.
        apps = sharedApks::look(disk, packages, {});
        sharedApks::apply(sharedApks::plan(apps, {{narrow(kCandy), f677}}, now), disk, packages);
        const std::string again = writtenLog();
        check(!files::exists(ofCandy(open)) && files::exists(ofCandy(opened) + L"\\app.apk"), "a listing again: the folder let go of goes, the installed one stays");
        check(count(again, at + narrow(opened) + " kept: ") == 1 && count(again, "\\installed.txt names ") == 1, "a listing again: a decision said once a run");
        files::removeTree(disk);
        files::removeTree(packages);
    }
}

using Kind = bundledApks::Task::Kind;

std::string describe(const std::vector<bundledApks::Task>& tasks) {
    std::string text;
    for (const auto& t : tasks) {
        const char* kind = t.kind == Kind::service ? "service" : t.kind == Kind::offer ? "offer" : t.kind == Kind::again ? "again" : "recover";
        text += (text.empty() ? "" : ", ") + std::string(kind) + " " + (t.id.empty() ? narrow(t.target.substr(t.target.find_last_of(L'\\') + 1)) : t.id);
    }
    return text.empty() ? "none" : text;
}

void testBundled() {
    printf("bundled apks\n");
    const std::wstring bitgapps = gInstall + L"\\bitgapps", library = gLocal + L"\\apks";
    files::makeDirectories(bitgapps);
    files::makeDirectories(library);
    files::makeDirectories(gShared);
    const struct {
        const wchar_t* name;
        uint64_t size;
    } apks[] = {{L"GoogleServicesFramework.apk", 200 * 1024 + 3},
                {L"PrebuiltGmsCore.apk", files::unbufferedFrom + 2 * 1024 * 1024 + 4567},
                {L"Phonesky.apk", 3 * 1024 * 1024 + 89},
                {L"MicroGGmsCore.apk", files::unbufferedFrom + 123}};
    bool made = true;
    uint32_t seed = 10;
    for (const auto& apk : apks) made = makeFile(bitgapps + L"\\" + apk.name, apk.size, seed++) && made;
    check(made, "the package's four Google APKs made");
    const auto never = [](const std::string&) { return false; };

    // A first start: everything to copy, Google Services Framework first and microG's last.
    std::vector<bundledApks::Task> tasks = bundledApks::plan(library, gShared, never);
    check(describe(tasks) == "service GoogleServicesFramework.apk, offer bitgapps-PrebuiltGmsCore.apk, offer bitgapps-Phonesky.apk, service GmsCore.apk",
          "plan at a first start: " + describe(tasks));
    check(!files::exists(library + L"\\bitgapps.added"), "plan at a first start: the offers' stamp is not written before they are made");

    // Made on their own thread, each said before it begins and after it ends.
    std::vector<std::pair<std::string, bool>> done;
    std::mutex doneMutex;
    bundledApks::start(tasks, [&](const bundledApks::Task& task, bool copied) {
        std::lock_guard<std::mutex> lock(doneMutex);
        done.emplace_back(task.what, copied);
    });
    check(bundledApks::copying(), "start: the copies are being made when it returns");
    bool sawProgress = false;
    for (int i = 0; i < 1200 && bundledApks::copying(); i++) {
        sawProgress = sawProgress || bundledApks::progress().active;
        Sleep(50);
    }
    check(!bundledApks::copying(), "start: the copies are made within a minute");
    {
        std::lock_guard<std::mutex> lock(doneMutex);
        bool all = done.size() == 4;
        for (const auto& d : done) all = all && d.second;
        check(all && done.front().first == "Google Services Framework" && done.back().first == "microG's Google Play services",
              format("start: each copy told, in order, made (%u told)", unsigned(done.size())));
    }
    bool alike = true;
    for (const auto& task : tasks) alike = alike && files::same(task.source, task.target) && !files::exists(task.target + L".part");
    check(alike, "start: each copy is its source byte for byte, and no .part is left");
    check(!bundledApks::progress().active, "start: no copy is in progress once they are made");
    const std::string log = writtenLog();
    const size_t before = log.find("bundled apks: copying Google Play services, package\\bitgapps\\PrebuiltGmsCore.apk"),
                 after = log.find("bundled apks: Google Play services copied to LocalState\\apks\\bitgapps-PrebuiltGmsCore.apk in ");
    check(before != std::string::npos && after != std::string::npos && before < after,
          "the log says a copy before it begins and after it ends, with its times");
    check(log.find("bundled apks: 4 of 4 copies made in ") != std::string::npos, "the log says all four were made");
    // Then the once-a-build check, which leaves out the copies just made.
    bool checked = false;
    for (int i = 0; i < 600 && !checked; i++) {
        std::string version;
        checked = files::read(gLocal + L"\\bundled-apks-checked.txt", version) && version == "0.1.0.0";
        if (!checked) Sleep(50);
    }
    check(checked, "the once-a-build check ran after the copies, and wrote its stamp");
    check(writtenLog().find("(4 made at this start, and compared then, left out)") != std::string::npos, "the check left out the four copies just made");

    // All there, the stamp not: nothing to copy, and the stamp written.
    tasks = bundledApks::plan(library, gShared, never);
    check(tasks.empty() && files::exists(library + L"\\bitgapps.added"), "plan with every copy there: " + describe(tasks) + ", and the offers' stamp written");
    tasks = bundledApks::plan(library, gShared, never);
    check(tasks.empty(), "plan again: " + describe(tasks));

    // A library's copy that is not the package's (its end differs), and microG's of another size.
    spoil(library + L"\\bitgapps-Phonesky.apk", files::size(library + L"\\bitgapps-Phonesky.apk") - 10);
    files::append(gShared + L"\\services\\microg\\GmsCore.apk", "x");
    tasks = bundledApks::plan(library, gShared, never);
    check(describe(tasks) == "again bitgapps-Phonesky.apk, service GmsCore.apk", "plan with a copy not the package's and microG's of another size: " + describe(tasks));

    // An app installed from the library's copy whose package is gone, and one whose package is there.
    files::remove(library + L"\\bitgapps-PrebuiltGmsCore.apk");
    files::write(library + L"\\bitgapps-PrebuiltGmsCore.apk.installed", "MeglerApp.com.google.android.gms_1.0.0.0_arm__x\r\n");
    tasks = bundledApks::plan(library, gShared, [](const std::string& marker) { return marker.find("MeglerApp.com.google.android.gms") == 0; });
    check(describe(tasks) == "again bitgapps-Phonesky.apk, recover bitgapps-PrebuiltGmsCore.apk, service GmsCore.apk",
          "plan with an installed app's package gone: " + describe(tasks));
    tasks = bundledApks::plan(library, gShared, never);
    check(describe(tasks) == "again bitgapps-Phonesky.apk, service GmsCore.apk", "plan with that package there: " + describe(tasks));

    // No shared folder (Windows gives a Megler on a memory card none): the library's offers alone, no service copied anywhere.
    {
        const std::wstring lone = gLocal + L"\\apks-no-shared";
        files::makeDirectories(lone);
        tasks = bundledApks::plan(lone, std::wstring(), never);
        check(describe(tasks) == "offer bitgapps-PrebuiltGmsCore.apk, offer bitgapps-Phonesky.apk", "plan with no shared folder: " + describe(tasks));
    }

    // A copy of what is not there: refused, said, and nothing left.
    check(!bundledApks::copy(bitgapps + L"\\None.apk", library + L"\\bitgapps-None.apk", "an APK the tests have not") &&
              !files::exists(library + L"\\bitgapps-None.apk") && !files::exists(library + L"\\bitgapps-None.apk.part"),
          "copy: an APK that is not there is not copied, and nothing is left");
    check(writtenLog().find("bundled apks: an APK the tests have not could not be copied to") != std::string::npos, "copy: the log says why not");
}

/// The versions of an app's own packages (shell/PackageVersions.h): what made one, the next one a build makes, Windows' order.
void testPackageVersions() {
    printf("package versions\n");
    namespace pv = packageVersions;
    check(pv::madeBy("0.1.0.681") == "0.1.0.681", "a package made before 682 was made by its own version");
    check(pv::madeBy("0.1.682.0") == "0.1.0.682" && pv::madeBy("0.1.682.7") == "0.1.0.682", "0.1.682.n was made by 0.1.0.682");
    check(pv::next("0.1.0.682", "") == "0.1.682.0", "a first package of an app: 0.1.682.0");
    check(pv::next("0.1.0.682", "0.1.0.681") == "0.1.682.0", "over a package 681 made: 0.1.682.0");
    check(pv::next("0.1.0.682", "0.1.682.0") == "0.1.682.1", "made again by the same build: 0.1.682.1 (Aurora Store's update, 0x80073CFB before)");
    check(pv::next("0.1.0.682", "0.1.682.4") == "0.1.682.5", "made again once more: 0.1.682.5");
    check(pv::next("0.1.0.683", "0.1.682.4") == "0.1.683.0", "the next build's first: 0.1.683.0");
    check(pv::next("1.2.3.4", "1.2.4.0") == "1.2.3.4", "a Megler whose third place is taken makes its own version");
    check(pv::older("0.1.0.681", "0.1.682.0") && pv::older("0.1.682.9", "0.1.683.0") && pv::older("0.1.682.0", "0.1.682.1"),
          "each build's packages are newer than the last build's, and a package made again newer than the one before");
    check(!pv::older("0.1.682.1", "0.1.682.1") && pv::older("0.1.0.682", "0.1.682.0"),
          "Windows' order: the same is not older; Megler 0.1.0.682's own version is older than its apps' packages");
}

/// What a folder's files take, its folders' too (files::treeSize: an app's data and cache in its own package's Settings).
void testTreeSize(const std::wstring& root) {
    printf("a folder's size with its folders' (files::treeSize)\n");
    const std::wstring dir = root + L"\\tree";
    files::makeDirectories(dir + L"\\cache\\images\\0");
    files::makeDirectories(dir + L"\\files\\empty");
    check(makeFile(dir + L"\\top.dat", 1000, 1) && makeFile(dir + L"\\cache\\http.dat", 70000, 2) && makeFile(dir + L"\\cache\\images\\0\\a.jpg", 5, 3) &&
              makeFile(dir + L"\\files\\save.dat", 300, 4),
          "a tree of files made");
    check(files::treeSize(dir) == 71305 && files::treeSize(dir + L"\\cache") == 70005 && files::treeSize(dir + L"\\files\\empty") == 0,
          "its size, a folder's in it, and an empty folder's");
    check(files::treeSize(dir + L"\\nothing") == 0 && files::folderSize(dir) == 1000, "none for no folder; folderSize counts its own files alone");
}

/// An app's permissions as the shell keeps and decides them (player/PermissionRules.cpp), in the app's settings in the shared
/// folder (shell/Settings.cpp): what the user said kept and forgotten; what the app is told of each kind of group -- one
/// Megler has nothing behind (granted with no one asked) refused by the user, Megler's own no over Windows' yes, a group
/// asked every time by this run's answer; the groups an app's manifest asks for; and Google Play services' own use of the
/// location, one answer for every app in Megler's settings, read as the file has it now.
void testPermissions() {
    using namespace permissionHost;
    printf("an app's permissions: kept, decided, and Google Play services' own location (player/PermissionRules.cpp)\n");
    files::makeDirectories(gShared);
    settings::load(gLocal, gShared, "");
    const std::string app = "org.megler.test.perms";
    const std::wstring appFile = gShared + L"\\apps\\org.megler.test.perms\\settings.ini";
    std::string text;

    // Kept in the app's settings, as megler.permission.<group>; UNDECIDED takes it out.
    check(kept(app, "camera") == MEGLER_PERMISSION_UNDECIDED, "nothing kept at first");
    keep(app, "camera", MEGLER_PERMISSION_DENIED_FIXED);
    keep(app, "contacts", EVERY_TIME);
    keep(app, "sms", MEGLER_PERMISSION_GRANTED);
    check(kept(app, "camera") == MEGLER_PERMISSION_DENIED_FIXED && kept(app, "contacts") == EVERY_TIME && kept(app, "sms") == MEGLER_PERMISSION_GRANTED,
          "Don't allow, Ask every time and Allow kept");
    check(files::read(appFile, text) && count(text, "megler.permission.camera=denied-fixed") == 1 && count(text, "megler.permission.contacts=ask") == 1,
          "kept in the app's settings.ini in the shared folder");
    keep(app, "sms", MEGLER_PERMISSION_UNDECIDED);
    check(kept(app, "sms") == MEGLER_PERMISSION_UNDECIDED && files::read(appFile, text) && count(text, "megler.permission.sms") == 0,
          "UNDECIDED takes the answer out");

    // What the app is told.
    const int un = MEGLER_PERMISSION_UNDECIDED, yes = MEGLER_PERMISSION_GRANTED, no = MEGLER_PERMISSION_DENIED, never = MEGLER_PERMISSION_DENIED_FIXED;
    check(decide(Decider::none, un, un, un) == yes && decide(Decider::none, yes, un, un) == yes, "a group Megler has nothing behind: granted, no one asked");
    check(decide(Decider::none, never, un, un) == never && decide(Decider::none, no, un, un) == no, "... refused by the user: denied");
    check(decide(Decider::megler, un, un, un) == un && decide(Decider::megler, yes, un, un) == yes && decide(Decider::megler, no, un, un) == no &&
              decide(Decider::megler, never, un, un) == never,
          "a group Megler asks about: undecided until asked, then as answered");
    check(decide(Decider::windows, un, yes, un) == yes && decide(Decider::windows, un, never, un) == never && decide(Decider::windows, un, un, un) == un &&
              decide(Decider::windows, yes, never, un) == never,
          "a group Windows decides: Windows' answer, Megler's yes leaving it to Windows");
    check(decide(Decider::windows, never, yes, un) == never && decide(Decider::windows, no, yes, un) == no, "Megler's own no over Windows' yes");
    check(decide(Decider::megler, EVERY_TIME, un, un) == un && decide(Decider::megler, EVERY_TIME, un, yes) == yes && decide(Decider::megler, EVERY_TIME, un, no) == no &&
              decide(Decider::none, EVERY_TIME, un, un) == un && decide(Decider::none, EVERY_TIME, un, never) == never,
          "asked every time: undecided until this run's answer, then that");
    check(decide(Decider::windows, EVERY_TIME, yes, yes) == yes && decide(Decider::windows, EVERY_TIME, never, yes) == never && decide(Decider::windows, EVERY_TIME, yes, un) == un,
          "... and for a group Windows decides, Windows' answer after this run's yes");

    // This run's answers, kept in the process; a new choice drops them.
    check(thisRun(app, "contacts") == un && !refusedByUser(app, "contacts"), "no answer this run at first");
    rememberThisRun(app, "contacts", no);
    check(thisRun(app, "contacts") == no && refusedByUser(app, "contacts") && refusedByUser(app, "camera") && !refusedByUser(app, "sms"),
          "this run's no and Don't allow are the user's refusals; nothing said is none");
    keep(app, "contacts", EVERY_TIME);
    check(thisRun(app, "contacts") == un, "a choice made again drops this run's answer");
    rememberThisRun(app, "contacts", yes);
    forgetAll(app);
    check(kept(app, "camera") == un && kept(app, "contacts") == un && thisRun(app, "contacts") == un && files::read(appFile, text) &&
              count(text, "megler.permission.") == 0,
          "Clear Data forgets every answer, this run's too");

    // The groups an app's manifest asks for, in Android's order; one asked for only up to Android 9 is none; ACCESS_MEDIA_LOCATION
    // with the storage where that is asked for.
    ApkReport r;
    r.permissionsKnown = true;
    r.permissions = {"android.permission.READ_CONTACTS", "android.permission.CAMERA", "android.permission.WRITE_EXTERNAL_STORAGE", "android.permission.INTERNET",
                     "android.permission.ACCESS_MEDIA_LOCATION"};
    r.permissionMaxSdk = {0, 0, 28, 0, 0};
    std::string names;
    for (const auto& g : groupsOf(r)) names += g.name + "/" + g.title + (g.decider == Decider::windows ? "/windows;" : g.decider == Decider::none ? "/none;" : "/megler;");
    check(names == "camera/Camera/windows;read_media_visual/Photos and videos/megler;contacts/Contacts/none;", "the groups asked for: " + names);
    r.permissions.push_back("android.permission.READ_EXTERNAL_STORAGE");
    r.permissionMaxSdk.push_back(0);
    names.clear();
    for (const auto& g : groupsOf(r)) names += g.name + ";";
    check(names == "camera;storage;contacts;", "ACCESS_MEDIA_LOCATION with the storage: " + names);
    check(groupOfPermission("android.permission.CAMERA") == "camera" && groupOfPermission("android.permission.INTERNET").empty() &&
              groupOfPermission("com.example.permission.CAMERA").empty(),
          "a dangerous permission's group, and none for another");
    check(questionOf("camera", "Test") == "Allow Test to take pictures and record video?" && !explanationOf("contacts").empty(), "Megler's question");

    // Google Play services' own location: Megler's settings, read as the file has it now.
    const std::wstring meglerFile = gShared + L"\\settings.ini";
    check(gmsLocation() == un, "Google Play services' own location: not asked at first");
    keepGmsLocation(yes);
    check(gmsLocation() == yes && files::read(meglerFile, text) && count(text, "megler.gmsLocation=granted") == 1, "allowed, kept in Megler's settings.ini");
    // Another package answers (an app's own package, where the question was put): the file says so, and so does gmsLocation.
    files::write(meglerFile, "megler.gmsLocation=denied\r\nmegler.renderCap=960\r\n");
    check(gmsLocation() == never && settings::meglerNow("megler.renderCap").value_or("") == "960", "read as the file has it now");
    check(settings::megler("megler.renderCap").value_or("") != "960", "what this process goes by stays as it was");
    keepGmsLocation(un);
    check(gmsLocation() == un && files::read(meglerFile, text) && count(text, "megler.renderCap=960") == 1, "asked again, Megler's other settings kept");

    // Google Location Accuracy: one setting of Google's for every app, so an app's yes or no is Megler's too, for the apps never
    // asked; Ask every time and a forgotten answer leave it.
    const std::string other = "org.megler.test.perms2";
    check(locationAccuracyElsewhere() == un, "Google Location Accuracy: no app's answer at first");
    keep(other, "location_accuracy", yes);
    check(locationAccuracyElsewhere() == yes && kept(app, "location_accuracy") == un && files::read(meglerFile, text) &&
              count(text, "megler.locationAccuracy=granted") == 1 && count(text, "megler.renderCap=960") == 1,
          "an app's Allow is every app's, kept in Megler's settings.ini; the other app keeps nothing of its own");
    keep(app, "location_accuracy", EVERY_TIME);
    keep(other, "location_accuracy", un);
    check(locationAccuracyElsewhere() == yes, "Ask every time and a forgotten answer leave it");
    keep(app, "location_accuracy", never);
    check(locationAccuracyElsewhere() == never, "an app's Don't allow is every app's too");
    forgetAll(app);
    forgetAll(other);
}

} // namespace

/// The answer the core was given for request `id`, waited for up to five seconds (the flags answer from a thread of their own).
bool pickAnswer(unsigned id, PickDone& out) {
    for (int tries = 0; tries < 500; ++tries) {
        {
            std::lock_guard<std::mutex> lock(gPickMutex);
            for (const PickDone& d : gPicksDone)
                if (d.id == id) {
                    out = d;
                    return true;
                }
        }
        Sleep(10);
    }
    return false;
}

bool has(const std::vector<std::wstring>& list, const wchar_t* e) { return std::find(list.begin(), list.end(), e) != list.end(); }

/// The phone's picker and camera for an app (player/PickerHost.cpp), without a user: what Windows' picker is given, the flags
/// that answer in the user's place, and the files handed over copied into the request's folder.
void testPicker(const std::wstring& root) {
    printf("the phone's picker and camera for an app (player/PickerHost.cpp)\n");
    pickerHost::Filter f = pickerHost::filterFor("image/*\n", MEGLER_PICK_START_PICTURES);
    check(has(f.extensions, L".jpg") && has(f.extensions, L".png") && has(f.extensions, L".heic") && !has(f.extensions, L"*") && f.thumbnails &&
              f.start == MEGLER_PICK_START_PICTURES,
          "image/*: the pictures' extensions, in the pictures library, with thumbnails");
    f = pickerHost::filterFor("image/*\nvideo/*\n", MEGLER_PICK_START_PICTURES);
    check(has(f.extensions, L".jpg") && has(f.extensions, L".mp4") && f.thumbnails, "pictures and videos (the photo picker's): both kinds' extensions, thumbnails");
    f = pickerHost::filterFor("application/pdf\n", MEGLER_PICK_START_DOCUMENTS);
    check(f.extensions.size() == 1 && f.extensions[0] == L".pdf" && !f.thumbnails && f.start == MEGLER_PICK_START_DOCUMENTS, "application/pdf: .pdf alone, a list in the documents");
    f = pickerHost::filterFor("image/*\napplication/pdf\n", MEGLER_PICK_START_DOCUMENTS);
    check(has(f.extensions, L".png") && has(f.extensions, L".pdf") && !f.thumbnails, "pictures and PDFs: both, as a list");
    f = pickerHost::filterFor("IMAGE/X-ICON\n", MEGLER_PICK_START_PICTURES);
    check(has(f.extensions, L".ico") && has(f.extensions, L".jpg"), "a type of a known kind without a list of its own (image/x-icon): the kind's");
    f = pickerHost::filterFor("*/*\n", MEGLER_PICK_START_DOCUMENTS);
    check(f.extensions.size() == 1 && f.extensions[0] == L"*", "*/*: every file");
    f = pickerHost::filterFor("", MEGLER_PICK_START_DOCUMENTS);
    check(f.extensions.size() == 1 && f.extensions[0] == L"*", "no type: every file");
    f = pickerHost::filterFor("application/x-unknown\nimage/png\n", MEGLER_PICK_START_DOCUMENTS);
    check(f.extensions.size() == 1 && f.extensions[0] == L"*", "a type Windows' picker has no extensions for among them: every file");

    // The flags: pick-cancel first; pick-file-<name>, one for a request of one file, every one (at most `max`) for several.
    const std::wstring flags = gLocal + L"\\flags";
    files::makeDirectories(flags);
    files::write(flags + L"\\pick-file-b.pdf", "");
    files::write(flags + L"\\pick-file-a.png", "");
    files::write(flags + L"\\pick-file-c.png", "");
    files::write(flags + L"\\ui-shot-x", "");
    pickerHost::FlagAnswer a = pickerHost::takeFlags(flags, false, 0);
    check(a.found && !a.cancel && a.names.size() == 1 && a.names[0] == L"a.png" && !files::exists(flags + L"\\pick-file-a.png") &&
              files::exists(flags + L"\\pick-file-b.pdf"),
          "a request of one file takes the first pick-file- flag by name, and that flag goes");
    a = pickerHost::takeFlags(flags, true, 1);
    check(a.found && a.names.size() == 1 && a.names[0] == L"b.pdf", "a request of several with at most one takes one");
    files::write(flags + L"\\pick-file-d.png", "");
    a = pickerHost::takeFlags(flags, true, 0);
    check(a.found && a.names.size() == 2 && a.names[0] == L"c.png" && a.names[1] == L"d.png" && files::exists(flags + L"\\ui-shot-x"),
          "a request of several takes every pick-file- flag left, and no other flag");
    a = pickerHost::takeFlags(flags, false, 0);
    check(!a.found, "with none left, the user is asked");
    files::write(flags + L"\\pick-cancel", "");
    files::write(flags + L"\\pick-file-e.png", "");
    a = pickerHost::takeFlags(flags, false, 0);
    check(a.found && a.cancel && !files::exists(flags + L"\\pick-cancel") && files::exists(flags + L"\\pick-file-e.png"), "pick-cancel answers before a file, and goes");
    files::remove(flags + L"\\pick-file-e.png");

    // Files handed over, copied into the request's folder: their names, the bytes, a name already there made unique.
    const std::wstring source = root + L"\\picker-source", folder = root + L"\\LocalState\\data\\app\\megler-picked\\1";
    files::makeDirectories(source);
    files::makeDirectories(folder);
    makeFile(source + L"\\photo one.png", 300000, 7);
    makeFile(source + L"\\doc.pdf", 1000, 8);
    pickerHost::Answer got = pickerHost::answerFromFolder(1, source, {L"photo one.png", L"doc.pdf"}, folder);
    PickDone done;
    check(got.status == MEGLER_PICK_OK && got.names.size() == 2 && got.names[0] == "photo one.png" && got.names[1] == "doc.pdf", "two files handed over: copied under their names");
    check(pickAnswer(1, done) && done.status == MEGLER_PICK_OK && done.names == got.names, "the core told: picked, their names");
    check(files::same(source + L"\\photo one.png", folder + L"\\photo one.png") && files::same(source + L"\\doc.pdf", folder + L"\\doc.pdf"), "the copies are the files");
    got = pickerHost::answerFromFolder(2, source, {L"photo one.png"}, folder);
    check(got.status == MEGLER_PICK_OK && got.names.size() == 1 && got.names[0] != "photo one.png" && got.names[0].find(".png") != std::string::npos &&
              files::same(source + L"\\photo one.png", folder + L"\\" + widen(got.names[0])),
          "a name already in the folder made unique (" + (got.names.empty() ? std::string("none") : got.names[0]) + ")");
    got = pickerHost::answerFromFolder(3, source, {L"missing.png"}, folder);
    check(got.status == MEGLER_PICK_FAILED && pickAnswer(3, done) && done.status == MEGLER_PICK_FAILED && done.names.empty(), "a file that is not there: Windows failing");
    {
        std::string text = writtenLog();
        check(text.find("photo one") == std::string::npos && text.find("(.png .pdf)") != std::string::npos, "the log names the extensions, not the files");
    }

    // The thunk with the flags in place: answered from a thread of its own, no window needed -- a picker's request and the
    // camera's, its copy named as Android's camera names a photo.
    pickerHost::setFlagSource(source);
    files::write(flags + L"\\pick-file-doc.pdf", "");
    const std::string folder8 = narrow(root + L"\\LocalState\\data\\app\\megler-picked\\8");
    files::makeDirectories(widen(folder8));
    megler_pick_request r = {};
    r.id = 8;
    r.kind = MEGLER_PICK_FILES;
    r.types = "application/pdf\n";
    r.folder = folder8.c_str();
    check(pickerHost::thunk(nullptr, &r) && pickAnswer(8, done) && done.status == MEGLER_PICK_OK && done.names.size() == 1 && done.names[0] == "doc.pdf" &&
              files::same(source + L"\\doc.pdf", widen(folder8) + L"\\doc.pdf"),
          "pick-file-doc.pdf answers a request in the user's place");
    files::write(flags + L"\\pick-file-photo one.png", "");
    const std::string folder9 = narrow(root + L"\\LocalState\\data\\app\\megler-picked\\9");
    files::makeDirectories(widen(folder9));
    r.id = 9;
    r.kind = MEGLER_PICK_PHOTO;
    r.types = "image/jpeg\n";
    r.folder = folder9.c_str();
    const bool taken = pickerHost::thunk(nullptr, &r) && pickAnswer(9, done) && done.status == MEGLER_PICK_OK && done.names.size() == 1 &&
                       done.names[0].rfind("IMG_", 0) == 0 && done.names[0].size() == 23 &&
                       files::same(source + L"\\photo one.png", widen(folder9) + L"\\" + widen(done.names[0]));
    check(taken, "and the camera's, as IMG_<date>_<time>.jpg (" + (done.names.empty() ? std::string("none") : done.names[0]) + ")");
    files::write(flags + L"\\pick-cancel", "");
    r.id = 10;
    check(pickerHost::thunk(nullptr, &r) && pickAnswer(10, done) && done.status == MEGLER_PICK_CANCELED, "pick-cancel answers as the user backing out");
    r.id = 11;
    check(!pickerHost::thunk(nullptr, &r), "with no flag and no window to show a picker over, the thunk answers false at once");
    pickerHost::setFlagSource(L"");
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: mg_shell_tests <scratch folder>\n");
        return 2;
    }
    const std::wstring root = argv[1];
    files::removeTree(root);
    gLocal = root + L"\\LocalState";
    gInstall = root + L"\\package";
    gShared = root + L"\\shared";
    if (!files::makeDirectories(gLocal) || !files::makeDirectories(gInstall)) {
        printf("the scratch folder could not be made (error %lu)\n", GetLastError());
        return 2;
    }
    shellLog::initialize(gLocal);
    printf("the shell's start and copies (shell/Files.cpp, ShellLog.cpp, Startup.cpp, BundledApks.cpp, Placement.cpp, SharedApks.cpp)\n");
    testPaths(root);
    testPlacement(root);
    testMissing(root);
    testFiles(root);
    testLog();
    testStartup();
    testBundled();
    testSharedApks(root);
    testPackageVersions();
    testTreeSize(root);
    testPermissions();
    testPicker(root);
    shellLog::flush();
    printf("shell: %d checks, %d failed\n", gChecks, gFailed);
    return gFailed ? 1 : 0;
}
