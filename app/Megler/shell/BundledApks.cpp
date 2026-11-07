#include "pch.h"

#include "shell/BundledApks.h"

#include "megler.h"
#include "library/Library.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <bcrypt.h>

#include <algorithm>

namespace megler::app::bundledApks {

namespace {

constexpr uint32_t kEnd = 65536;
constexpr uint32_t kPage = 4096;

bool endsWith(const std::wstring& s, const wchar_t* tail) {
    const size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

/// The package's Google APKs, bitgapps\*.apk.
std::vector<std::wstring> originals() {
    const std::wstring folder = platform::installDirectory() + L"\\bitgapps";
    std::vector<std::wstring> out;
    for (const auto& name : files::list(folder, L"*.apk"))
        if (endsWith(name, L".apk")) out.push_back(folder + L"\\" + name);
    return out;
}

/// How many of the last 64 KB of two files of one size differ, as the disk holds them; -1 when they are not of one size or
/// cannot be read.
int differingAtEnd(const std::wstring& a, const std::wstring& b) {
    files::DiskFile x(a), y(b);
    if (!x.isOpen() || !y.isOpen() || x.size() != y.size() || x.size() == 0) return -1;
    // (From a page's start: the disk is read in whole sectors.)
    const uint64_t size = x.size(), from = size > kEnd ? (size - kEnd) & ~uint64_t(kPage - 1) : 0;
    const uint32_t n = uint32_t(size - from);
    std::vector<char> p(n), q(n);
    if (!x.read(from, p.data(), n) || !y.read(from, q.data(), n)) return -1;
    int differ = 0;
    for (uint32_t i = n > kEnd ? n - kEnd : 0; i < n; i++) differ += p[i] != q[i];
    return differ;
}

std::wstring originalAmong(const std::vector<std::wstring>& originals, const std::wstring& path) {
    const uint64_t size = files::size(path);
    if (!size) return {};
    for (const auto& original : originals) {
        if (files::size(original) != size) continue;
        const int differ = differingAtEnd(original, path);
        if (differ >= 0 && differ <= 64) return original;
    }
    return {};
}

/// A path as the log says it: the shared folder as "shared", LocalState as "LocalState", the package's folder as "package".
std::string shown(const std::wstring& path, const std::wstring& shared) {
    const std::pair<std::wstring, const char*> roots[] = {
        {shared, "shared"}, {platform::localStateDirectory(), "LocalState"}, {platform::installDirectory(), "package"}};
    for (const auto& [root, name] : roots)
        if (!root.empty() && path.size() > root.size() && _wcsnicmp(path.c_str(), root.c_str(), root.size()) == 0 && path[root.size()] == L'\\')
            return name + narrow(path.substr(root.size()));
    return narrow(path);
}

std::string base64url(const unsigned char* p, size_t n) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? uint32_t(p[i + 2]) : 0);
        out += digits[v >> 18 & 63];
        out += digits[v >> 12 & 63];
        if (i + 1 < n) out += digits[v >> 6 & 63];
        if (i + 2 < n) out += digits[v & 63];
    }
    return out;
}

std::string hex(const unsigned char* p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; i++) out += format("%02x", unsigned(p[i]));
    return out;
}

/// A file's SHA-1 as Google Play Store gives its own APK's (URL-safe Base64, unpadded: "SU: Cannot patch ..., bad base file
/// hash, expect ... actual ..."), and its SHA-256 in hex, of its bytes as the disk holds them; "?" for what could not be had.
std::pair<std::string, std::string> digestsOf(const std::wstring& path) {
    files::DiskFile file(path);
    BCRYPT_ALG_HANDLE algorithms[2] = {};
    BCRYPT_HASH_HANDLE hashes[2] = {};
    const wchar_t* const names[2] = {BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM};
    bool ok = file.isOpen();
    for (int i = 0; i < 2 && ok; i++)
        ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithms[i], names[i], nullptr, 0)) &&
             BCRYPT_SUCCESS(BCryptCreateHash(algorithms[i], &hashes[i], nullptr, 0, nullptr, 0, 0));
    std::vector<char> buffer(files::DiskFile::chunk);
    for (uint64_t at = 0; ok && at < file.size(); at += files::DiskFile::chunk) {
        const uint32_t n = uint32_t(std::min<uint64_t>(files::DiskFile::chunk, file.size() - at));
        ok = file.read(at, buffer.data(), n);
        for (int i = 0; i < 2 && ok; i++) ok = BCRYPT_SUCCESS(BCryptHashData(hashes[i], reinterpret_cast<PUCHAR>(buffer.data()), n, 0));
    }
    unsigned char sha1[20] = {}, sha256[32] = {};
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hashes[0], sha1, sizeof(sha1), 0)) && BCRYPT_SUCCESS(BCryptFinishHash(hashes[1], sha256, sizeof(sha256), 0));
    for (int i = 0; i < 2; i++) {
        if (hashes[i]) BCryptDestroyHash(hashes[i]);
        if (algorithms[i]) BCryptCloseAlgorithmProvider(algorithms[i], 0);
    }
    if (!ok) return {"?", "?"};
    return {base64url(sha1, sizeof(sha1)), hex(sha256, sizeof(sha256))};
}

void sayDigests(const char* what, const std::wstring& path, const std::wstring& shared) {
    const auto [sha1, sha256] = digestsOf(path);
    shellLog::write(format("bundled apks: %s %s, %llu bytes: SHA-1 %s, SHA-256 %s", what, shown(path, shared).c_str(),
                           (unsigned long long)files::size(path), sha1.c_str(), sha256.c_str()));
}

/// Where `copy`, as the disk holds it, differs from `original`, said in the log under `label`: its first 16 differing bytes
/// one by one (where, the package's byte, the copy's, the bits that differ), then how many bytes and bits in all, how many of
/// those bytes are zeros in the copy, in how many pages of 4 KB, and whether each chunk that differed read alike three times
/// (a copy that reads otherwise each time is a reading's fault, not the file's). The copy's pages that differ go to `pages`
/// (each an 8-byte offset, then the page), 256 at most, for a PC to look at. The number of bytes that differ; -1 when either
/// file could not be read.
int64_t describe(const std::wstring& original, const std::wstring& copy, const std::string& label, const std::wstring& pages,
                 const std::wstring& shared) {
    files::DiskFile a(original), b(copy);
    if (!a.isOpen() || !b.isOpen() || a.size() != b.size()) return -1;
    const uint32_t chunk = files::DiskFile::chunk;
    std::vector<char> x(chunk), y(chunk), z(chunk);
    uint64_t bytes = 0, bits = 0, zeros = 0, pagesDiffering = 0, chunks = 0, unsteady = 0;
    std::string kept;
    unsigned said = 0, saved = 0;
    for (uint64_t at = 0; at < a.size(); at += chunk) {
        const uint32_t n = uint32_t(std::min<uint64_t>(chunk, a.size() - at));
        if (!a.read(at, x.data(), n) || !b.read(at, y.data(), n)) {
            shellLog::write(format("bundled apks: %s could not be read at %llu", label.c_str(), (unsigned long long)at));
            return -1;
        }
        if (!memcmp(x.data(), y.data(), n)) continue;
        chunks++;
        for (int again = 0; again < 2; again++) {
            if (!b.read(at, z.data(), n) || memcmp(y.data(), z.data(), n)) {
                unsteady++;
                break;
            }
        }
        for (uint32_t page = 0; page < n; page += kPage) {
            const uint32_t m = std::min(kPage, n - page);
            if (!memcmp(x.data() + page, y.data() + page, m)) continue;
            pagesDiffering++;
            if (saved < 256) {
                const uint64_t where = at + page;
                kept.append(reinterpret_cast<const char*>(&where), sizeof(where));
                kept.append(y.data() + page, m);
                kept.append(kPage - m, '\0');
                saved++;
            }
            for (uint32_t i = page; i < page + m; i++) {
                const unsigned diff = uint8_t(x[i] ^ y[i]);
                if (!diff) continue;
                bytes++;
                for (unsigned v = diff; v; v &= v - 1) bits++;
                zeros += y[i] == 0;
                if (said++ < 16)
                    shellLog::write(format("bundled apks: %s at %llu (0x%llx): %02x in the package's, %02x in the copy (bits %02x)", label.c_str(),
                                           (unsigned long long)(at + i), (unsigned long long)(at + i), unsigned(uint8_t(x[i])),
                                           unsigned(uint8_t(y[i])), diff));
            }
        }
    }
    if (bytes) {
        const bool written = files::write(pages, kept);
        const std::string steady = unsteady ? format("%llu of those chunks read otherwise when read again", (unsigned long long)unsteady)
                                            : std::string("each of those chunks read alike three times");
        shellLog::write(format("bundled apks: %s: %llu bytes differ (%llu bits; %llu of those bytes zeros in the copy) in %llu pages of 4 KB, "
                               "%llu chunks of 1 MB; %s, from the %s; the copy's pages that differ: %s",
                               label.c_str(), (unsigned long long)bytes, (unsigned long long)bits, (unsigned long long)zeros,
                               (unsigned long long)pagesDiffering, (unsigned long long)chunks, steady.c_str(),
                               b.uncached() ? "disk" : "cache (the disk could not be read past it)",
                               written ? shown(pages, shared).c_str() : "not written"));
    }
    return int64_t(bytes);
}

/// Where Megler keeps APKs: the library's copies, the apps' own (each folder of <shared>\apks\<identity>), and the services'.
std::vector<std::wstring> copiesIn(const std::wstring& library, const std::wstring& shared) {
    std::vector<std::wstring> out;
    const auto apks = [&out](const std::wstring& folder) {
        for (const auto& name : files::list(folder, L"*.apk"))
            if (endsWith(name, L".apk")) out.push_back(folder + L"\\" + name);
    };
    apks(library);
    if (shared.empty()) return out;
    for (const auto& identity : files::listFolders(shared + L"\\apks", L"*")) {
        for (const auto& token : files::listFolders(shared + L"\\apks\\" + identity, L"*")) {
            // (One being made, keepInShared's, is not one yet.)
            if (endsWith(token, L".part")) continue;
            const std::wstring apk = shared + L"\\apks\\" + identity + L"\\" + token + L"\\app.apk";
            if (files::exists(apk)) out.push_back(apk);
        }
    }
    for (const auto& folder : files::listFolders(shared + L"\\services", L"*")) apks(shared + L"\\services\\" + folder);
    return out;
}

/// The flag test-copies: how this phone copies a file. The package's largest Google APK is copied three times as Megler made
/// every copy before (files::copy and files::flush alone) to LocalState\test-copies.apk, and each copy compared with it as
/// the disk holds it (describe), then removed. A Lumia 650 got copies of Google Play services wrong twice, a bit each time
/// (625, 632), and Google Play Store's once.
void testCopies(const std::vector<std::wstring>& bundle, const std::wstring& shared) {
    std::wstring largest;
    for (const auto& original : bundle)
        if (largest.empty() || files::size(original) > files::size(largest)) largest = original;
    const std::wstring local = platform::localStateDirectory(), copy = local + L"\\test-copies.apk";
    ULARGE_INTEGER room = {};
    if (largest.empty() || !GetDiskFreeSpaceExW(local.c_str(), &room, nullptr, nullptr) || room.QuadPart < files::size(largest) + (300ull << 20)) {
        shellLog::write("bundled apks: test-copies: no room for a copy of " + (largest.empty() ? std::string("an APK") : shown(largest, shared)) + " (" +
                        formattedByteCount(room.QuadPart) + " free)");
        return;
    }
    files::makeDirectory(local + L"\\bundled-apks");
    unsigned wrong = 0, made = 0;
    for (int i = 1; i <= 3; i++) {
        const ULONGLONG began = GetTickCount64();
        if (!files::copy(largest, copy) || !files::flush(copy)) {
            shellLog::write(format("bundled apks: test-copies: copy %d of %s could not be made (error %lu)", i, shown(largest, shared).c_str(), GetLastError()));
            break;
        }
        const ULONGLONG copied = GetTickCount64();
        const int64_t differ = describe(largest, copy, format("test copy %d", i), local + L"\\bundled-apks\\test-" + widen(std::to_string(i)) + L".pages", shared);
        shellLog::write(format("bundled apks: test-copies: copy %d of %s made in %.1f s and compared in %.1f s: %s", i, shown(largest, shared).c_str(),
                               double(copied - began) / 1000.0, double(GetTickCount64() - copied) / 1000.0,
                               differ < 0 ? "could not be read" : differ ? "differs" : "the same byte for byte"));
        made++;
        wrong += differ != 0;
    }
    files::remove(copy);
    shellLog::write(format("bundled apks: test-copies: %u of %u copies differ", wrong, made));
}

void check(const std::wstring& library, const std::wstring& shared, bool asked, const std::string& version) {
    // (Its reads and writes after everything else's: an app starting, Update All.)
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const ULONGLONG began = GetTickCount64();
    const std::wstring local = platform::localStateDirectory();
    const std::vector<std::wstring> bundle = originals();
    if (asked) {
        shellLog::write("bundled apks: the flag check-bundled-apks: each file's digests, and each byte of a copy that differs");
        for (const auto& original : bundle) sayDigests("the package's", original, shared);
        files::makeDirectory(local + L"\\bundled-apks");
    }
    unsigned compared = 0, mendedCopies = 0, index = 0;
    bool whole = true;
    for (const auto& copy : copiesIn(library, shared)) {
        const std::wstring original = originalAmong(bundle, copy);
        if (original.empty()) continue;
        compared++;
        const std::string label = shown(copy, shared), of = shown(original, shared);
        if (asked) {
            const int64_t differ = describe(original, copy, label, local + L"\\bundled-apks\\" + widen(std::to_string(++index)) + L".pages", shared);
            if (differ == 0) shellLog::write("bundled apks: " + label + " is " + of + " byte for byte");
            if (differ > 0) sayDigests("before it is mended,", copy, shared);
        }
        uint64_t mended = 0;
        std::vector<uint64_t> chunks;
        if (!files::mend(original, copy, mended, &chunks)) {
            const DWORD error = GetLastError();
            // (One removed meanwhile -- an app's older folder, a service's older copy -- is no failure.)
            if (!files::exists(copy)) continue;
            whole = false;
            shellLog::write(format("bundled apks: %s could not be compared with %s, or not made like it (error %lu)", label.c_str(), of.c_str(), error));
            continue;
        }
        if (!mended) continue;
        mendedCopies++;
        std::string where;
        for (size_t i = 0; i < chunks.size() && i < 8; i++) where += format("%s%.1f MB", i ? ", " : "", double(chunks[i]) / 1048576.0);
        if (chunks.size() > 8) where += ", ...";
        shellLog::write(format("bundled apks: %s differed from %s: %s of it written again from there (the chunks at %s)", label.c_str(),
                               of.c_str(), formattedByteCount(mended).c_str(), where.c_str()));
        if (asked) sayDigests("mended,", copy, shared);
    }
    shellLog::write(format("bundled apks: %u copies of the package's %u Google APKs compared with them in %.1f s: %s", compared,
                           unsigned(bundle.size()), double(GetTickCount64() - began) / 1000.0,
                           mendedCopies ? format("%u mended", mendedCopies).c_str() : "each as the package's"));
    // (Not once for the build while a copy that differs could not be mended: again at the next start.)
    if (whole) files::write(local + L"\\bundled-apks-checked.txt", version);
    if (files::exists(local + L"\\flags\\test-copies")) testCopies(bundle, shared);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

}  // namespace

std::wstring originalOf(const std::wstring& path) { return originalAmong(originals(), path); }

Replacement replacementFor(const ApkEntry& entry) {
    const ApkReport& had = entry.report;
    const wchar_t* name = had.package == "com.google.android.gms" && !had.microg ? L"PrebuiltGmsCore.apk"
                          : had.package == "com.android.vending"               ? L"Phonesky.apk"
                                                                                : nullptr;
    if (!name || !had.valid || !had.ok) return {};
    const std::wstring path = platform::installDirectory() + L"\\bitgapps\\" + name;
    // (An entry that is a copy of it -- the library's, an app's in the shared folder -- is the package's APK already: nothing
    // is read.)
    if (!files::exists(path) || originalAmong({path}, entry.path) == path) return {};
    std::string json;
    if (char* report = megler_apk_report(narrow(path).c_str())) {
        json = report;
        megler_free(report);
    }
    const ApkReport ours = ApkReport::parse(json);
    if (!ours.valid || !ours.ok || ours.package != had.package || ours.microg) return {};
    const std::string versions = format("%lld where it had %lld", (long long)ours.versionCode, (long long)had.versionCode);
    if (ours.versionCode > had.versionCode)
        return {path, json, "Megler's package brings a later version (" + versions + ")"};
    if (ours.versionCode / 1000 == had.versionCode / 1000 && !ours.abi.empty() && ours.libraries.size() > had.libraries.size())
        return {path, json,
                format("Megler's package brings the variant of %s of the same release (%s; %u libraries of %s where it had %u)", ours.abi.c_str(),
                       versions.c_str(), unsigned(ours.libraries.size()), ours.abi.c_str(), unsigned(had.libraries.size()))};
    return {};
}

bool copy(const std::wstring& source, const std::wstring& target) {
    uint64_t mended = 0;
    const bool copied = files::copyWhole(source, target + L".part", mended) && files::move(target + L".part", target);
    if (!copied) files::remove(target + L".part");
    if (mended)
        shellLog::write(format("bundled apks: %s of the copy of %s differed from it after the copy, and was written again",
                               formattedByteCount(mended).c_str(), shown(source, appPackage::sharedFolder()).c_str()));
    return copied;
}

void checkLater() {
    const std::wstring local = platform::localStateDirectory(), shared = appPackage::sharedFolder();
    const bool asked = files::exists(local + L"\\flags\\check-bundled-apks");
    if (appPackage::isSingleApp()) {
        if (!asked) return;
        // What this app reads as its own APK (Google Play Store hashes its own base.apk before it patches itself).
        const std::wstring apk = appPackage::single().apkFolder + L"\\app.apk";
        std::thread([apk, shared] { sayDigests("this app's APK,", apk, shared); }).detach();
        return;
    }
    const std::string version = platform::packageVersion();
    std::string checked;
    if (!asked && !files::exists(local + L"\\flags\\test-copies") && files::read(local + L"\\bundled-apks-checked.txt", checked) &&
        checked == version)
        return;
    if (!files::exists(platform::installDirectory() + L"\\bitgapps")) return;
    std::thread([library = local + L"\\apks", shared, asked, version] { check(library, shared, asked, version); }).detach();
}

}  // namespace megler::app::bundledApks
