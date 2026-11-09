// The folders of the shared folder that hold the apps' APKs (<shared>\apks\<identity>\<token>: shell/AppPackage.h, make;
// docs/PER-APP-PACKAGES.md): which of them an installed app's package reads, which a package made and waiting for Windows's
// own installer will read, and which nothing reads any more and go.
//
// An app's own package reads the folder its MeglerApp.ini names (apk=shared:apks\<identity>\<token>), in its install folder,
// which Megler cannot read on a phone. What Megler knows is what it wrote down itself:
//
//   apks\<identity>\<token>\made.txt         the full name of each package made with that folder, one a line
//                                            ("MeglerApp.<...>_<Megler's version>_arm__7p434m16tbgc6"), written by make
//                                            before the package exists (since 2026-11-09)
//   apks\<identity>\installed.txt            the folder of the package Megler itself last installed (since 0.1.0.645)
//   LocalState\packages\<identity>.appx.txt  the folder and full name of the package made and not installed by Megler
//                                            (<identity>.appx beside it, which the app's page offers to Windows's own
//                                            installer: Open the Package), written once that file is begun (since 2026-11-09)
//   the library's entries                    each reads one folder (Library::adopt: <id>.installed, shared=<folder>)
//
// Up to 0.1.0.675 only installed.txt and the library were gone by, and both are written only when Megler's own install
// succeeds: a folder neither named was removed an hour after it was begun, though a package installed some other way read
// it -- by Windows's own installer (Open the Package, after Windows refused Megler's install: a phone without the capability
// part of its interop unlock, a certificate Windows did not trust), from a file manager, or by an install Windows finished
// after Megler had given up on it. A user's Candy Crush Saga (0.1.0.673) ran three times one morning, and from 17:41 on
// found no APK: its library read none, and its package stood still.
//
// Now a package's full name, which Windows's package manager lists, is matched with the made.txt that names it: that folder
// is the installed package's, whoever installed it; installed.txt is written for it, and the library reads it
// (ui/SeparateApps.cpp). A folder whose package waits for Windows's installer is kept while it waits. Where nothing tells
// which folder an installed package reads (made before made.txt was written), what it may read is kept -- installed.txt's
// folder and any begun after it, or every one where there is no installed.txt -- until the app's next package (Update All
// after an update of Megler) is made and recorded. The decision is a function of what was read (plan), tested on a PC
// (core/tests/mg_shell_windows.cpp); look reads it from the disk and apply carries it out.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace megler::app::sharedApks {

/// An app's own package installed, as Windows's package manager lists it (appPackage::installedApps).
struct Installed {
    std::string identity;  // "MeglerApp.com.king.candycrushsaga.2a041e0408f22f6b"
    std::string fullName;  // "MeglerApp.com.king.candycrushsaga.2a041e0408f22f6b_0.1.0.673_arm__7p434m16tbgc6"
};

/// A folder of apks\<identity>, as it is on the disk.
struct Token {
    std::wstring name;              // "<16 hex digits: the time its copy was begun at>-<process id>"; ".part" after it while copied
    bool recorded = false;          // it has a made.txt, read
    bool unreadable = false;        // it has a made.txt that could not be read: what it says is not known
    std::vector<std::string> made;  // the full names made.txt names
    uint64_t madeTime = 0;          // when made.txt was last written (a FILETIME's 100 ns): the last package made with it
    bool read = false;              // an entry of Megler's library reads it
};

/// apks\<identity>, and what is written down of it.
struct App {
    std::wstring identity;
    std::vector<Token> tokens;
    std::wstring installedToken;       // the folder installed.txt names; empty where there is none
    uint64_t installedTime = 0;        // when installed.txt was written: after Megler's own install
    bool installedUnreadable = false;  // installed.txt is there and could not be read
    bool waiting = false;              // a package made of it waits for Windows's installer (LocalState\packages\<identity>.appx)
    std::wstring waitingToken;         // the folder that package reads, by its record; empty where none says (made before records)
    std::string waitingFullName;       // its full name, by its record
};

/// What becomes of a folder, and why, in words for the log ("kept: ...", "removed: ...").
struct Verdict {
    std::wstring identity, token;
    bool keep = true;
    std::string why;
};

/// An installed package whose folder is known for sure: the one folder whose made.txt names its full name (and that Megler's
/// own later install of the same package from another folder has not superseded). installed.txt is to name it, and the
/// library's entry of the app is to read its APK (ui/SeparateApps.cpp).
struct Known {
    std::wstring identity, token;
    std::string fullName;
    bool waitingDone = false;  // the package that waited for Windows's installer is this one, installed: it goes
};

struct Plan {
    std::vector<Verdict> verdicts;     // each folder's, app by app
    std::vector<std::wstring> forget;  // apps not installed whose installed.txt goes
    std::vector<Known> known;
};

/// The time `token` was begun at, from its name (keepInShared: "%08lx%08lx-%08lx"); 0 where its name says none.
uint64_t begunAt(const std::wstring& token);

/// What becomes of each folder of `apps` with `installed` (the whole list of the apps' packages installed) at `now` (a
/// FILETIME's 100 ns). A folder is kept when:
///   - a package installed now was made with it (made.txt names its full name) -- but not one made before Megler itself
///     installed that same package from another folder (installed.txt, written after that install, which replaced whatever
///     package of the name was there; and the file of a package made before it is gone, each make writing it again);
///   - the library reads it;
///   - the package made and waiting for Windows's installer reads it (its record), or, where that package has no record,
///     it may (as below);
///   - the app is installed and no folder's made.txt names its package (made up to 0.1.0.675), and it may be the one it
///     reads: installed.txt's folder, or one begun (or made) after installed.txt was written, or any where there is no
///     installed.txt;
///   - it was begun within the hour: a package may be being made with it (Megler's jobs, another Megler's);
///   - its made.txt is there and could not be read; or its app is installed, and its installed.txt is there and could not be
///     read.
/// Every other goes, a ".part" left by a copy that never finished among them.
Plan plan(const std::vector<App>& apps, const std::vector<Installed>& installed, uint64_t now);

/// What is on the disk: each folder of `shared`\apks\<identity>, its made.txt and its installed.txt; whether
/// `packages`\<identity>.appx waits, and its record; and which folders `read` (full paths: the library's) names.
std::vector<App> look(const std::wstring& shared, const std::wstring& packages, const std::vector<std::wstring>& read);

/// `plan` carried out on `shared` and `packages`: each folder kept or removed (a folder whose APK an app has open is left
/// whole, for the next time), installed.txt written for each known package where it names another folder, and removed
/// for each app not installed; the package that waited and is installed removed with its record. Each decision said in
/// the log once a run, and the space freed.
void apply(const Plan& plan, const std::wstring& shared, const std::wstring& packages);

/// `folder`\made.txt with `fullName` in it (once), written again whether or not it was there, so that its time is this
/// make's: make calls it before the package exists, and a package is not made where it cannot be written -- a package
/// whose folder no record names would be taken for no one's.
bool recordMade(const std::wstring& folder, const std::string& fullName);
/// The record beside a package made (`appx`, LocalState\packages\<identity>.appx): `appx`.txt, the folder of the shared
/// folder its app reads (`apkFolder`, apks\<identity>\<token>) and its full name. Written once the package's file is begun,
/// so that it says the package that is there.
bool recordWaiting(const std::wstring& appx, const std::wstring& apkFolder, const std::string& fullName);
/// A package made, installed: its file goes, and its record once the file has gone.
void removeWaiting(const std::wstring& appx);

}  // namespace megler::app::sharedApks
