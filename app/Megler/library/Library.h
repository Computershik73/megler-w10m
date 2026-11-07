// The user's APKs: copies in LocalState\apks, each with what megler_apk_report
// says about it and its launcher icon cached beside it --
//
//   apks\Game.apk          the copy
//   apks\Game.apk.json     megler_apk_report's JSON, as the core returned it
//   apks\Game.apk.icon     megler_apk_icon's bytes (PNG or WebP), when there is one
//   apks\reports.version   which build made the cached reports
//
// An app that came as a bundle of split APKs (.apks, .xapk, .apkm) is its base APK
// and, beside it, the folder megler_bundle_unpack made of its splits:
//
//   apks\Game.apk          the base
//   apks\Game.apk.splits\  split_config.armeabi_v7a.apk and the other splits
//                          kept, the bundle's icon.png, and bundle.json, the
//                          core's record of what it kept and left out
//
// The core is given both, for the report (megler_apk_report_splits) and to run
// the app (megler_run_config.splits_dir), and they go together.
//
// An app installed as an app of the phone's own (ui/SeparateApps.h) has no
// copy here: its APK is the one its package's app reads, in the shared folder
// since 645 (apks\<identity>\<token>, its splits beside it: appPackage::make),
// before that in the package (app\app.apk in its install folder), and a file
// says which package and folder that is --
//
//   apks\Game.apk.installed   the package's full name; then shared=<folder>
//
// -- with the report and the icon cached here as for a copy.
//
// One package, one entry: an APK added for a package the library already has
// replaces what was there once its report names the package, whatever its
// file's name, its version or its signature -- a debug build over a release
// one and back, as `adb install -r -d` with the signature check left out
// would. The app's own files (LocalState\apps\<package>) are kept, as an
// install over an installed app keeps them.
//
// -- so the list opens at once from the cache. Reading an APK takes a moment
// (its libraries are scanned), so it happens on a worker thread: when an APK
// is added, when its cache is missing, and for every APK after an update of
// the app, since a newer core may run what an older one could not.
#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace megler::app {

struct ApkLibrary {
    std::string name;
    uint64_t size = 0;
    int armFunctions = 0, thumbFunctions = 0, missingImports = 0;
    std::vector<std::string> needs;
};

/// megler_apk_report's JSON (core/include/megler.h), read.
struct ApkReport {
    bool valid = false; // the JSON itself could be read
    bool ok = false;
    std::string error;
    std::string package, label, versionName;
    int64_t versionCode = 0;
    int minSdk = 0, targetSdk = 0;
    std::vector<std::string> abis;
    std::string abi;
    bool runnable = false;
    std::string reason;
    std::string engine;
    /// Google Play services that are microG's (com.google.android.gms with org/microg/ in it), not Google's own.
    bool microg = false;
    /// How the app's first activity asks to be held: "landscape", "portrait" or "unspecified".
    std::string orientation;
    bool supported = false;
    std::string activity, nativeLibrary;
    std::vector<ApkLibrary> libraries;
    std::vector<std::string> notes;
    /// For an app in several APKs: the splits it is read with, and those its bundle's unpacking left out.
    std::vector<std::string> splits, splitsLeftOut;
    /// The permissions its manifest asks for, and the last Android each is asked on (0: every one), place for place; and
    /// whether it has a receiver of Google Play services' SMS Retriever. A report made by an older core has none of these:
    /// permissionsKnown is false then, and what the app may have is not known (everything of Megler's, as before).
    bool permissionsKnown = false;
    std::vector<std::string> permissions;
    std::vector<int> permissionMaxSdk;
    bool smsRetriever = false;

    static ApkReport parse(const std::string& json);
    /// Whether its manifest asks for `permission` on the Android Megler says it is (14: a permission whose maxSdkVersion is
    /// below that is not asked for). Not known: true.
    bool asks(const std::string& permission) const;
    /// What its own package declares to Windows (megler_appx_capabilities: MEGLER_APPX_CAP_*); not known: -1, all of Megler's.
    int capabilities() const;
    /// Whether Play can be offered.
    bool ready() const { return valid && ok && runnable && supported; }
    /// "Ready", or in one line why not.
    std::string status() const;
    /// "Unity", "NativeActivity", ... for the report's engine.
    std::string engineName() const;
    /// Whether the player holds the phone upright for it: an app that says
    /// so, and one that says nothing unless it is a game (an app with an
    /// engine of its own is taken for one: those were all shown lying down
    /// before there were any others, and the one tried says landscape itself).
    bool upright() const { return orientation == "portrait" || (orientation != "landscape" && engine == "java"); }
};

struct ApkEntry {
    std::string id;        // the copy's file name: "Game.apk"
    std::wstring path;     // the copy, or the installed package's APK
    std::string installedPackage; // when the APK is the installed package's: its full name; Megler has no copy
    std::wstring splitsDir; // its splits' folder (path + ".splits"), empty for an app in one APK
    uint64_t size = 0;     // the copy's, with its splits'
    bool analysed = false; // false until a report has been read or made
    bool analysing = false;
    bool replaces = false; // just added: once read, the other entries of its package go
    ApkReport report;
    std::wstring iconPath; // empty when the APK has no icon the core finds

    /// The label, else the package, else the file name.
    std::string title() const;
};

class Library {
public:
    static Library& instance();

    /// Lists LocalState\apks from the cache and queues what has to be read.
    /// Once, on the UI thread, after megler_init.
    void load();

    std::vector<ApkEntry> entries() const;
    std::optional<ApkEntry> find(const std::string& id) const;
    /// Goes up whenever entries() would answer differently.
    unsigned generation() const;
    const std::wstring& directory() const { return directory_; }

    /// The file name a picked file's copy gets: its own name, made safe. A
    /// bundle's (.apks, .xapk, .apkm) is its name with .apk for its own extension:
    /// what it is kept as is its base.
    static std::string idFor(const std::string& pickedName);
    /// Whether a picked file is a bundle of split APKs, by its name.
    static bool isBundle(const std::string& pickedName);
    /// The folder a copy's splits are in, whether or not it is there.
    static std::wstring splitsDirFor(const std::wstring& path) { return path + L".splits"; }
    /// A complete copy now lies at directory()\id: lists it and reads it; once
    /// read, the entries of the same package that were there before go (their
    /// copies, splits and caches; the app's own files stay).
    void added(const std::string& id);
    /// The demo the package carries (demo\megler-demo.apk: a picture, touch and
    /// sound to judge by eye and ear), copied into the library. False when
    /// this package has none or the copy failed.
    bool addDemo();
    static bool hasDemo();
    /// The Java test apps a package built with --java carries beside the demo
    /// (demo\hello.apk, demo\demos.apk and demo\gl.apk: tools/build-appx.sh), copied into
    /// the library like the demo. `name` is the file's name. False when this
    /// package has no such file or the copy failed.
    bool addPackaged(const std::string& name);
    static bool hasPackaged(const std::string& name);
    /// Deletes the copy, its splits and its cache. The app's own files
    /// (LocalState\apps, with the expansion files a bundle brought) are the
    /// core's and stay. An installed package's APK stays with the package.
    void remove(const std::string& id);

    /// The app `id`, just installed as the package `fullName`, whose APK Megler
    /// can read in the shared folder's `sharedApk` (apks\<identity>\<token>,
    /// relative to it; appPackage::make), or, without it, in the package (made
    /// before 645): Megler's copy (if any) and its splits go, and the entry is
    /// that APK's. The report and icon are to be in the cache already
    /// (directory()\id.json, .icon). Other entries of its package go.
    void adopt(const std::string& id, const std::string& fullName, const std::wstring& sharedApk = std::wstring());
    /// The other way: the installed app's APK and splits copied back here
    /// (before the package is uninstalled). Empty, or why not.
    std::string restore(const std::string& id);

private:
    void queue(const std::string& id);
    /// Under mutex_: the entry at `it` goes, its copy, splits and cache with it.
    void eraseLocked(std::vector<ApkEntry>::iterator it);
    void work();
    void analyse(const std::string& id);
    /// Under mutex_: an entry whose APK is an installed package's, from the cache: `marker` is its id.apk.installed, the
    /// package's full name, and, for a package made since 645, the folder of the shared folder its APK is in (adopt).
    ApkEntry installedEntry(const std::string& id, const std::string& marker) const;

    mutable std::mutex mutex_;
    std::wstring directory_;
    std::vector<ApkEntry> entries_;
    std::deque<std::string> queue_;
    bool working_ = false;
    bool refreshingAll_ = false;
    unsigned generation_ = 1;
};

} // namespace megler::app
