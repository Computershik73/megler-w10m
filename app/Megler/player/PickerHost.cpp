#include "pch.h"

#include "player/PickerHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Media.Capture.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>

namespace megler::app::pickerHost {

using namespace winrt;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Pickers;
using winrt::Windows::Media::Capture::CameraCaptureUI;
using winrt::Windows::Media::Capture::CameraCaptureUIMaxVideoResolution;
using winrt::Windows::Media::Capture::CameraCaptureUIMode;
using winrt::Windows::Media::Capture::CameraCaptureUIPhotoFormat;
using winrt::Windows::Media::Capture::CameraCaptureUIVideoFormat;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

namespace {

std::mutex gMutex; // the two below
CoreDispatcher gDispatcher{nullptr};
std::wstring gFlagSource;
// A picker or the camera is up: Windows refuses a second one ("a picker is already showing", E_ILLEGAL_METHOD_CALL).
std::atomic<bool> gBusy{false};

/// What the shell keeps of a request: the call's strings are the core's only while it lasts.
struct Request {
    unsigned id = 0;
    int kind = MEGLER_PICK_FILES;
    std::string types;
    bool multiple = false;
    int max = 0;
    std::wstring folder;
    int durationS = 0, quality = -1, start = MEGLER_PICK_START_DOCUMENTS;
};

/// The extensions of names, for the log: never the names.
std::string extensionsOf(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        const size_t dot = n.rfind('.');
        const bool ok = dot != std::string::npos && n.size() - dot <= 8 && n.find(' ', dot) == std::string::npos;
        if (!out.empty()) out += ' ';
        out += ok ? n.substr(dot) : "(no extension)";
    }
    return out;
}

/// The request's types on one line, for the log.
std::string typesOf(const std::string& types) {
    std::string out;
    for (char c : types) out += c == '\n' ? ' ' : c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? "any type" : out;
}

const char* what(int kind) { return kind == MEGLER_PICK_PHOTO ? "a photo" : kind == MEGLER_PICK_VIDEO ? "a video" : "files"; }

void answer(unsigned id, int status, const std::vector<std::string>& names) {
    std::vector<const char*> list;
    for (const std::string& n : names) list.push_back(n.c_str());
    megler_pick_done(id, status, int(list.size()), list.empty() ? nullptr : list.data());
}

/// A folder's path as Windows.Storage takes it: backslashes (the core's are a mix), no trailing one.
std::wstring windowsPath(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    return p;
}

/// A name not yet in `folder`: "IMG_1.jpg", else "IMG_1 (2).jpg" and on, as Windows' GenerateUniqueName makes it.
std::wstring uniqueIn(const std::wstring& folder, const std::wstring& name) {
    if (!files::exists(folder + L"\\" + name)) return name;
    const size_t dot = name.rfind(L'.');
    const std::wstring stem = dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot), ext = stem.size() == name.size() ? L"" : name.substr(dot);
    for (int k = 2; k < 1000; ++k) {
        const std::wstring candidate = stem + L" (" + std::to_wstring(k) + L")" + ext;
        if (!files::exists(folder + L"\\" + candidate)) return candidate;
    }
    return name;
}

/// A file Windows handed over, copied into the request's folder under `name` (made unique there): the copy's name, or empty
/// with `why` said. Windows' own copy first (CopyAsync, which brings a file of OneDrive's down as it goes); where Windows will
/// not give the folder by its path, the file read through its stream and written by the shell. Off the UI thread.
std::wstring copyInto(StorageFile const& file, const std::wstring& folder, const std::wstring& name, std::string& why) {
    const std::wstring target = uniqueIn(folder, name);
    try {
        StorageFolder to = StorageFolder::GetFolderFromPathAsync(folder).get();
        StorageFile copy = file.CopyAsync(to, target, NameCollisionOption::GenerateUniqueName).get();
        return std::wstring(copy.Name());
    } catch (const hresult_error& e) {
        why = format("Windows' copy failed (0x%08X)", unsigned(e.code()));
    }
    // Read through its stream: the file is the picker's to read, the folder the app's own.
    try {
        auto stream = file.OpenReadAsync().get();
        const std::wstring path = folder + L"\\" + target;
        winrt::file_handle out{CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr)};
        if (!out) {
            why += format("; the copy could not be made (error %lu)", GetLastError());
            return L"";
        }
        winrt::Windows::Storage::Streams::Buffer buffer(1u << 20);
        for (;;) {
            auto got = stream.ReadAsync(buffer, buffer.Capacity(), winrt::Windows::Storage::Streams::InputStreamOptions::None).get();
            if (got.Length() == 0) break;
            DWORD written = 0;
            if (!WriteFile(out.get(), got.data(), got.Length(), &written, nullptr) || written != got.Length()) {
                why += format("; writing the copy failed (error %lu)", GetLastError());
                out.close();
                files::remove(path);
                return L"";
            }
        }
        why.clear();
        return target;
    } catch (const hresult_error& e) {
        why += format("; reading it failed (0x%08X)", unsigned(e.code()));
        return L"";
    }
}

/// The files copied into the request's folder and the core answered: OK with the copies' names, or FAILED. Off the UI thread.
Answer deliver(const Request& r, const std::vector<StorageFile>& picked, const char* who) {
    Answer a;
    const auto began = std::chrono::steady_clock::now();
    std::string why;
    for (const StorageFile& f : picked) {
        const std::wstring copy = copyInto(f, r.folder, std::wstring(f.Name()), why);
        if (copy.empty()) break;
        a.names.push_back(narrow(copy));
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    if (!why.empty() || a.names.size() != picked.size()) {
        shellLog::write(format("picker: request %u: %zu file%s %s, but the copy into the app's folder failed: %s", r.id, picked.size(),
                               picked.size() == 1 ? "" : "s", who, why.c_str()));
        a.names.clear();
        a.status = MEGLER_PICK_FAILED;
    } else {
        shellLog::write(format("picker: request %u: %zu file%s %s (%s), copied for the app in %lld ms", r.id, a.names.size(),
                               a.names.size() == 1 ? "" : "s", who, extensionsOf(a.names).c_str(), ms));
        a.status = MEGLER_PICK_OK;
    }
    answer(r.id, a.status, a.names);
    return a;
}

/// Android's camera's name for what it took now: IMG_20261111_120000.jpg, VID_20261111_120000.mp4.
std::wstring cameraName(int kind) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t at[32];
    wcsftime(at, 32, L"%Y%m%d_%H%M%S", &local);
    return (kind == MEGLER_PICK_VIDEO ? L"VID_" : L"IMG_") + std::wstring(at) + (kind == MEGLER_PICK_VIDEO ? L".mp4" : L".jpg");
}

/// Windows' picker, on the UI thread; what the user picked copied off it.
fire_and_forget showPicker(Request r) {
    std::vector<StorageFile> picked;
    std::string error;
    const Filter filter = filterFor(r.types, r.start);
    try {
        FileOpenPicker picker;
        picker.ViewMode(filter.thumbnails ? PickerViewMode::Thumbnail : PickerViewMode::List);
        picker.SuggestedStartLocation(filter.start == MEGLER_PICK_START_PICTURES  ? PickerLocationId::PicturesLibrary
                                      : filter.start == MEGLER_PICK_START_VIDEOS  ? PickerLocationId::VideosLibrary
                                      : filter.start == MEGLER_PICK_START_MUSIC   ? PickerLocationId::MusicLibrary
                                                                                  : PickerLocationId::DocumentsLibrary);
        for (const std::wstring& e : filter.extensions) picker.FileTypeFilter().Append(e);
        if (r.multiple) {
            auto files = co_await picker.PickMultipleFilesAsync();
            for (StorageFile const& f : files) picked.push_back(f);
        } else if (StorageFile f = co_await picker.PickSingleFileAsync()) {
            picked.push_back(f);
        }
    } catch (const hresult_error& e) {
        error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
    }
    gBusy = false;
    co_await resume_background();
    if (!error.empty()) {
        shellLog::write(format("picker: request %u: Windows' picker failed: %s", r.id, error.c_str()));
        answer(r.id, MEGLER_PICK_FAILED, {});
        co_return;
    }
    if (picked.empty()) {
        shellLog::write(format("picker: request %u: the user backed out", r.id));
        answer(r.id, MEGLER_PICK_CANCELED, {});
        co_return;
    }
    if (r.multiple && r.max > 0 && int(picked.size()) > r.max) {
        shellLog::write(format("picker: request %u: %zu files picked, of which the app takes the first %d", r.id, picked.size(), r.max));
        picked.erase(picked.begin() + r.max, picked.end());
    }
    deliver(r, picked, "picked by the user");
}

/// Windows' camera, on the UI thread; what it took copied off it under the name Android's camera gives, and its own file gone.
fire_and_forget showCamera(Request r) {
    StorageFile taken{nullptr};
    std::string error;
    try {
        CameraCaptureUI camera;
        if (r.kind == MEGLER_PICK_VIDEO) {
            camera.VideoSettings().Format(CameraCaptureUIVideoFormat::Mp4);
            camera.VideoSettings().AllowTrimming(false);
            if (r.durationS > 0) camera.VideoSettings().MaxDurationInSeconds(float(r.durationS));
            camera.VideoSettings().MaxResolution(r.quality == 0 ? CameraCaptureUIMaxVideoResolution::LowDefinition
                                                                : CameraCaptureUIMaxVideoResolution::HighestAvailable);
        } else {
            camera.PhotoSettings().Format(CameraCaptureUIPhotoFormat::Jpeg);
            camera.PhotoSettings().AllowCropping(false);
        }
        taken = co_await camera.CaptureFileAsync(r.kind == MEGLER_PICK_VIDEO ? CameraCaptureUIMode::Video : CameraCaptureUIMode::Photo);
    } catch (const hresult_error& e) {
        error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
    }
    gBusy = false;
    co_await resume_background();
    if (!error.empty()) {
        shellLog::write(format("camera: request %u: Windows' camera failed: %s", r.id, error.c_str()));
        answer(r.id, MEGLER_PICK_FAILED, {});
        co_return;
    }
    if (!taken) {
        shellLog::write(format("camera: request %u: the user backed out", r.id));
        answer(r.id, MEGLER_PICK_CANCELED, {});
        co_return;
    }
    std::string why;
    const std::wstring copy = copyInto(taken, r.folder, cameraName(r.kind), why);
    try {
        taken.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
    } catch (const hresult_error&) {
    }
    if (copy.empty()) {
        shellLog::write(format("camera: request %u: %s taken, but the copy into the app's folder failed: %s", r.id, what(r.kind), why.c_str()));
        answer(r.id, MEGLER_PICK_FAILED, {});
        co_return;
    }
    shellLog::write(format("camera: request %u: %s taken (%s)", r.id, what(r.kind), extensionsOf({narrow(copy)}).c_str()));
    answer(r.id, MEGLER_PICK_OK, {narrow(copy)});
}

/// The Pictures library's folder Megler (where packages are copied over USB), or the tests' own.
std::wstring flagSourcePath(bool& library) {
    std::lock_guard<std::mutex> lock(gMutex);
    library = gFlagSource.empty();
    return gFlagSource;
}

/// A request answered by the flags (takeFlags), off the UI thread, nothing shown.
void answerFromFlags(const Request& r, const FlagAnswer& flags) {
    if (flags.cancel) {
        shellLog::write(format("picker: request %u (%s of %s) answered by the flag pick-cancel: as the user backing out", r.id, what(r.kind), typesOf(r.types).c_str()));
        answer(r.id, MEGLER_PICK_CANCELED, {});
        return;
    }
    bool library = false;
    std::wstring source = flagSourcePath(library);
    std::vector<StorageFile> picked;
    try {
        StorageFolder folder = library ? KnownFolders::PicturesLibrary().GetFolderAsync(L"Megler").get() : StorageFolder::GetFolderFromPathAsync(windowsPath(source)).get();
        for (const std::wstring& n : flags.names) picked.push_back(folder.GetFileAsync(n).get());
    } catch (const hresult_error& e) {
        shellLog::write(format("picker: request %u answered by the flags pick-file-: %s not found in %s (0x%08X)", r.id, extensionsOf({narrow(flags.names.empty() ? L"" : flags.names[0])}).c_str(),
                               library ? "the Pictures library's folder Megler" : "the tests' folder", unsigned(e.code())));
        answer(r.id, MEGLER_PICK_FAILED, {});
        return;
    }
    if (r.kind != MEGLER_PICK_FILES) {
        // As the camera would have it: the one file, under its name for what it took.
        std::string why;
        const std::wstring copy = copyInto(picked[0], r.folder, cameraName(r.kind), why);
        if (copy.empty()) {
            shellLog::write(format("camera: request %u answered by the flag pick-file-: the copy failed: %s", r.id, why.c_str()));
            answer(r.id, MEGLER_PICK_FAILED, {});
            return;
        }
        shellLog::write(format("camera: request %u (%s) answered by the flag pick-file- (%s)", r.id, what(r.kind), extensionsOf({narrow(copy)}).c_str()));
        answer(r.id, MEGLER_PICK_OK, {narrow(copy)});
        return;
    }
    deliver(r, picked, "given by the flags pick-file-");
}

} // namespace

void attach(CoreDispatcher dispatcher) {
    std::lock_guard<std::mutex> lock(gMutex);
    gDispatcher = dispatcher;
}

void setFlagSource(const std::wstring& folder) {
    std::lock_guard<std::mutex> lock(gMutex);
    gFlagSource = folder;
}

Filter filterFor(const std::string& types, int start) {
    // The extensions of each kind and type an app asks for, as Windows' picker filters by them. A type of a kind Windows'
    // picker has no list of -- or "*/*", or none -- is every file.
    struct Kind {
        const char* type;
        const wchar_t* extensions;
    };
    static const Kind known[] = {
        {"image/*", L".jpg .jpeg .png .gif .bmp .webp .heic .heif .tif .tiff .dng .jxr .ico"},
        {"image/jpeg", L".jpg .jpeg .jpe .jfif"},
        {"image/jpg", L".jpg .jpeg"},
        {"image/png", L".png"},
        {"image/gif", L".gif"},
        {"image/bmp", L".bmp"},
        {"image/webp", L".webp"},
        {"image/heic", L".heic"},
        {"image/heif", L".heif"},
        {"image/tiff", L".tif .tiff"},
        {"video/*", L".mp4 .m4v .3gp .3g2 .mov .avi .wmv .mkv .webm .ts .mpg .mpeg"},
        {"video/mp4", L".mp4 .m4v"},
        {"video/3gpp", L".3gp"},
        {"video/quicktime", L".mov"},
        {"video/webm", L".webm"},
        {"video/x-matroska", L".mkv"},
        {"audio/*", L".mp3 .m4a .aac .wav .ogg .oga .opus .flac .wma .amr .mid .midi .3ga"},
        {"audio/mpeg", L".mp3"},
        {"audio/mp4", L".m4a"},
        {"audio/aac", L".aac"},
        {"audio/ogg", L".ogg .oga .opus"},
        {"audio/wav", L".wav"},
        {"audio/x-wav", L".wav"},
        {"audio/flac", L".flac"},
        {"application/pdf", L".pdf"},
        {"text/plain", L".txt .log .text"},
        {"text/*", L".txt .log .csv .htm .html .xml .md .json"},
        {"text/csv", L".csv"},
        {"text/html", L".htm .html"},
        {"application/json", L".json"},
        {"application/zip", L".zip"},
        {"application/vnd.android.package-archive", L".apk"},
        {"application/msword", L".doc"},
        {"application/vnd.openxmlformats-officedocument.wordprocessingml.document", L".docx"},
        {"application/vnd.ms-excel", L".xls"},
        {"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", L".xlsx"},
        {"application/vnd.ms-powerpoint", L".ppt"},
        {"application/vnd.openxmlformats-officedocument.presentationml.presentation", L".pptx"},
        {"application/epub+zip", L".epub"},
        {"application/x-fictionbook+xml", L".fb2"},
    };
    Filter f;
    bool any = false, media = true;
    size_t at = 0;
    while (at < types.size()) {
        size_t end = types.find('\n', at);
        if (end == std::string::npos) end = types.size();
        std::string t = types.substr(at, end - at);
        at = end + 1;
        std::transform(t.begin(), t.end(), t.begin(), [](char c) { return char(tolower((unsigned char)c)); });
        if (t.empty()) continue;
        const Kind* found = nullptr;
        for (const Kind& k : known)
            if (t == k.type) found = &k;
        // A type of a known kind that has no list of its own (image/x-icon): the kind's.
        if (!found && t.find('/') != std::string::npos) {
            const std::string whole = t.substr(0, t.find('/')) + "/*";
            for (const Kind& k : known)
                if (whole == k.type && whole != "text/*") found = &k;
        }
        if (!found) {
            any = true;
            continue;
        }
        if (t.rfind("image/", 0) != 0 && t.rfind("video/", 0) != 0) media = false;
        std::wstring list = found->extensions;
        size_t from = 0;
        while (from < list.size()) {
            size_t space = list.find(L' ', from);
            if (space == std::wstring::npos) space = list.size();
            const std::wstring e = list.substr(from, space - from);
            if (std::find(f.extensions.begin(), f.extensions.end(), e) == f.extensions.end()) f.extensions.push_back(e);
            from = space + 1;
        }
    }
    if (any || f.extensions.empty()) {
        f.extensions = {L"*"};
        media = false;
    }
    f.start = start;
    f.thumbnails = media && (start == MEGLER_PICK_START_PICTURES || start == MEGLER_PICK_START_VIDEOS);
    return f;
}

FlagAnswer takeFlags(const std::wstring& flagsFolder, bool multiple, int max) {
    FlagAnswer a;
    if (files::exists(flagsFolder + L"\\pick-cancel")) {
        files::remove(flagsFolder + L"\\pick-cancel");
        a.found = a.cancel = true;
        return a;
    }
    std::vector<std::wstring> flags = files::list(flagsFolder, L"pick-file-*");
    std::sort(flags.begin(), flags.end());
    for (const std::wstring& flag : flags) {
        if (flag.size() <= 10) continue;
        if (!multiple && !a.names.empty()) break;
        if (multiple && max > 0 && int(a.names.size()) >= max) break;
        a.names.push_back(flag.substr(10));
        files::remove(flagsFolder + L"\\" + flag);
    }
    a.found = !a.names.empty();
    return a;
}

Answer answerFromFolder(unsigned id, const std::wstring& source, const std::vector<std::wstring>& names, const std::wstring& folder) {
    Request r;
    r.id = id;
    r.folder = windowsPath(folder);
    std::vector<StorageFile> picked;
    try {
        StorageFolder from = StorageFolder::GetFolderFromPathAsync(windowsPath(source)).get();
        for (const std::wstring& n : names) picked.push_back(from.GetFileAsync(n).get());
    } catch (const hresult_error& e) {
        shellLog::write(format("picker: request %u: a file is not in the folder it was to come from (0x%08X)", id, unsigned(e.code())));
        answer(id, MEGLER_PICK_FAILED, {});
        return Answer();
    }
    return deliver(r, picked, "handed over");
}

bool MEGLER_CB thunk(void*, const megler_pick_request* request) {
    if (!request || !request->folder || !request->folder[0]) return false;
    Request r;
    r.id = request->id;
    r.kind = request->kind;
    r.types = request->types ? request->types : "";
    r.multiple = request->multiple;
    r.max = request->max;
    r.folder = windowsPath(widen(request->folder));
    r.durationS = request->duration_s;
    r.quality = request->quality;
    r.start = request->start;
    // A run no one picks in: the flags answer in place of the user, off the UI thread, nothing shown.
    FlagAnswer flags = takeFlags(platform::localStateDirectory() + L"\\flags", r.multiple, r.max);
    if (flags.found) {
        std::thread([r, flags] { answerFromFlags(r, flags); }).detach();
        return true;
    }
    CoreDispatcher dispatcher{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        dispatcher = gDispatcher;
    }
    if (!dispatcher) {
        shellLog::write(format("picker: request %u (%s): no window to show a picker over", r.id, what(r.kind)));
        return false;
    }
    if (gBusy.exchange(true)) {
        shellLog::write(format("picker: request %u (%s): a picker or the camera is up already, and Windows shows one at a time", r.id, what(r.kind)));
        return false;
    }
    if (r.kind == MEGLER_PICK_FILES) {
        const Filter f = filterFor(r.types, r.start);
        std::string extensions;
        for (const std::wstring& e : f.extensions) extensions += (extensions.empty() ? "" : " ") + narrow(e);
        static const char* const places[] = {"the documents", "the pictures library", "the videos library", "the music library"};
        shellLog::write(format("picker: request %u: Windows' picker for %s (%s), %s, in %s%s", r.id, typesOf(r.types).c_str(), extensions.c_str(),
                               r.multiple ? "several files" : "one file", places[f.start >= 0 && f.start <= 3 ? f.start : 0],
                               f.thumbnails ? " with thumbnails" : ""));
    } else {
        shellLog::write(format("camera: request %u: Windows' camera for %s%s", r.id, what(r.kind),
                               r.kind == MEGLER_PICK_VIDEO && r.durationS > 0 ? format(", at most %d s", r.durationS).c_str() : ""));
    }
    dispatcher.RunAsync(CoreDispatcherPriority::Normal, [r] {
        if (r.kind == MEGLER_PICK_FILES) showPicker(r);
        else showCamera(r);
    });
    return true;
}

} // namespace megler::app::pickerHost
