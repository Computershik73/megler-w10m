/*
 * Megler for Windows 10 Mobile: what the app shell sees of the core.
 *
 * The core is plain C; the shell is C++/WinRT. Nothing here names a WinRT or
 * Direct3D type: pointers to such things cross as void *.
 *
 * Strings are UTF-8. Paths use forward or back slashes. Unless it says
 * otherwise a function may be called from any thread.
 */
#ifndef MEGLER_H
#define MEGLER_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * x86 (desktop Windows): the structures here are laid out alike by the core's ELF, which aligns a 64-bit field at 4
 * bytes as Android's x86 ABI does, and by the shell, which MSVC's x86 aligns at 8. Both sides take 4.
 */
#if defined(_M_IX86) || (defined(__i386__) && defined(MG_CORE_ELF))
#pragma pack(push, 4)
#endif

/*
 * On desktop Windows the core is an ELF library of Android's ABI that Megler.exe loads (docs/DESKTOP.md), and the shell
 * calls it the way Windows calls: MEGLER_API on each function the core exports, MEGLER_CB on each callback of the shell's
 * the core calls. On x86_64 that is Windows' calling convention (ms_abi); on x86 a call from Windows realigns the stack
 * to Android's 16 bytes; on ARM64 the two conventions are one. Nothing at all for the phone and the tests. MG_CORE_ELF is
 * defined where the core's ELF is compiled.
 */
#if defined(MG_CORE_ELF) && defined(__x86_64__)
#define MEGLER_API __attribute__((ms_abi, visibility("default")))
#define MEGLER_CB __attribute__((ms_abi))
#elif defined(MG_CORE_ELF) && defined(__i386__)
#define MEGLER_API __attribute__((force_align_arg_pointer, visibility("default")))
#define MEGLER_CB
#elif defined(MG_CORE_ELF)
#define MEGLER_API __attribute__((visibility("default")))
#define MEGLER_CB
#else
#define MEGLER_API
#define MEGLER_CB
#endif

/* ------------------------------------------------------------------ setup */

enum { MEGLER_LOG_DEBUG = 0, MEGLER_LOG_INFO, MEGLER_LOG_WARN, MEGLER_LOG_ERROR };

/*
 * A line of the log. May be called from any thread, and from several at once: it keeps its own order. The layer calls it
 * past its log's lock, so a line it writes out to a file there (a fault's report, which has to be on the disk before the
 * process may end) holds up no other thread's lines.
 */
typedef void (MEGLER_CB *megler_log_fn)(int level, const char *line, void *user);

typedef struct megler_config {
    /* Where everything the layer keeps lives: each app's files, saves and settings (the package's LocalState folder). */
    const char *data_dir;
    /* The installed package's folder: ANGLE's DLLs and the self-test's libraries are read from it. */
    const char *package_dir;
    megler_log_fn log;
    void *log_user;
    /* What the OS allows the app to use and what it uses now (MemoryManager), in bytes; either may be NULL. */
    uint64_t (MEGLER_CB *memory_limit)(void);
    uint64_t (MEGLER_CB *memory_usage)(void);
    /* Where the shared storage is kept: <storage_dir>/sdcard, and each app's own part of it, <storage_dir>/apps/<package>/
       sdcard, which other apps see in /sdcard/Android/data and obb. NULL for data_dir. The package of one Android app
       gives the publisher cache folder that every such package shares, so that what one app keeps in the shared storage
       the others find (docs/PER-APP-PACKAGES.md); an app's private files stay under data_dir. */
    const char *storage_dir;
} megler_config;

/*
 * Once, at start-up. Returns false when this process cannot run Android code
 * at all -- the codeGeneration capability is missing, so no memory can be made
 * executable. The APK functions below still work then.
 */
MEGLER_API bool megler_init(const megler_config *config);
MEGLER_API const char *megler_version(void);
MEGLER_API void megler_free(void *p);

/* ------------------------------------------------------------------- APKs */

/*
 * What an APK is and whether it can run here, as JSON (free with megler_free;
 * NULL only when out of memory):
 *
 *   {
 *     "ok": true,                      false: "error" says why the file could not be read
 *     "package": "com.example.game",
 *     "label": "Example Game",         the application's name, when it is a plain string
 *     "versionName": "1.2.3", "versionCode": 123,
 *     "minSdk": 16, "targetSdk": 28,
 *     "permissions": ["android.permission.INTERNET", "android.permission.CAMERA"],   what the manifest asks for
 *                                      (<uses-permission>, <uses-permission-sdk-23>), each once, in its order
 *     "permissionMaxSdk": [0, 28],     android:maxSdkVersion of each, place for place: the last Android it is asked
 *                                      on, 0 for every one
 *     "smsRetriever": true,            the app uses Google Play services' SMS Retriever: its manifest has a receiver of
 *                                      it (its SMS_RETRIEVED, or the permission only Google Play services holds), or its
 *                                      code names one of those or the class SmsRetriever (an app that registers its
 *                                      receiver at run time); absent when neither
 *     "smsRetrieverInCode": true,      found in its code (the DEX files, read only when the manifest says nothing of it
 *                                      and has Google Play services' com.google.android.gms.version meta-data)
 *     "abis": ["armeabi-v7a", "arm64-v8a"],
 *     "abi": "armeabi-v7a",            the one that would be used; absent when there is none
 *     "runnable": true,                false: "reason" says why not (no 32-bit ARM code, no native code at all, ...)
 *     "reason": "...",
 *     "engine": "native-activity",     what would drive it: "native-activity", "sdl", "unity", "cocos2d-x", "unreal", "godot", "java", "unknown"
 *     "supported": true,               whether that engine has a driver here yet
 *     "activity": "android.app.NativeActivity",   the launcher activity's class
 *     "orientation": "landscape",      how that activity asks to be held (android:screenOrientation): "landscape" for
 *                                      landscape, sensorLandscape, reverseLandscape and userLandscape; "portrait" for
 *                                      portrait and its three like; "unspecified" for everything else and for none,
 *                                      which on a phone means upright unless it is turned
 *     "nativeLibrary": "main",         android.app.lib_name, for a NativeActivity
 *     "hasIcon": true,                 whether megler_apk_icon will find one
 *     "splits": ["config.armeabi_v7a", "config.xxhdpi"],   for an app in several APKs (megler_apk_report_splits): the
 *     "splitsLeftOut": ["config.arm64_v8a"],               splits it was read with, and those the unpacking of its
 *                                                          bundle left out; both absent for an app in one APK
 *     "libraries": [ { "name": "libmain.so", "size": 123456, "needs": ["libc.so", "liblog.so"],
 *                      "armFunctions": 0, "thumbFunctions": 812,     exported functions by mode
 *                      "importsNotInSystem": 3 } ],                  imports this layer's system libraries lack
 *                                                                    (another library of the app may provide them)
 *     "notes": ["..."]                 things worth telling the user: ARM-mode code, Vulkan only, ...
 *   }
 *
 * It reads the APK and nothing else: no code is loaded.
 */
MEGLER_API char *megler_apk_report(const char *apk_path);

/*
 * The APK's launcher icon as the bytes of its image file (free with megler_free),
 * or false when it has none this layer can find. A PNG when the APK has one in
 * any density; otherwise a WebP, which Windows 10 Mobile cannot decode.
 */
MEGLER_API bool megler_apk_icon(const char *apk_path, uint8_t **data, size_t *size);

/*
 * A WebP picture, which the phone's own decoder (WIC) cannot read, decoded by the core (libwebp's decoder, where the build
 * has it: core/src/webp): its size, and its pixels into width x height of `stride` bytes a row, B, G, R, A premultiplied,
 * scaled to that size when it is another. False when it is no WebP file this build decodes. For an icon the shell draws
 * itself (an app package's tiles, shell/AppPackage.cpp).
 */
MEGLER_API bool megler_webp_size(const void *data, size_t size, int *width, int *height);
MEGLER_API bool megler_webp_decode(const void *data, size_t size, void *pixels, int width, int height, int stride);

/*
 * A file the shell reaches only through handles -- one the user picked, which an app container may not open by its
 * path, but Windows' broker hands out handles to (IStorageItemHandleAccess) -- under a path of the shell's choosing:
 * from now on the core opens `path` for reading by calling `open(user)` for a new HANDLE each time, so that
 * megler_apk_report, megler_apk_icon, megler_bundle_unpack and megler_appx_add_file read the file where it is, with no copy.
 * Writing to it is refused. The path is compared without regard to case or to '/' and '\'. NULL `open` takes the path
 * back; the caller keeps `user` alive until then. A few at a time. Windows only: false elsewhere.
 */
typedef void *(MEGLER_CB *megler_handle_opener)(void *user);
MEGLER_API bool megler_file_by_handle(const char *path, megler_handle_opener open, void *user);

/*
 * An app in several APKs: a base and its split APKs, as Google Play installs an app built as an Android App Bundle.
 * Config splits hold what is for one kind of phone -- config.armeabi_v7a.apk the native libraries for 32-bit ARM,
 * config.xxhdpi.apk the pictures for one screen density, config.ru.apk one language's strings -- and feature splits
 * more code and resources. Outside Google Play such an app comes as a bundle of them (megler_bundle_unpack, below) and is
 * kept as its base APK and a folder of its splits, the folder megler_bundle_unpack makes.
 *
 * megler_apk_report_splits is megler_apk_report of the app as one: what the base's manifest says, the native libraries and
 * the engine looked for in every APK, "splits" and "splitsLeftOut" added. `splits_dir` is the folder: its .apk files are
 * the splits (none, or NULL: the base alone). What the unpacking left out of the bundle counts too: the ABIs of the
 * splits for other processors are among "abis", and an app whose bundle has native code for other processors only is
 * not runnable, with a reason that says so. A split that is another app's, or no split, is left out and noted.
 * megler_apk_icon_splits finds the icon in every APK (a density split often has the only pictures of it) and else takes the
 * icon the bundle came with.
 */
MEGLER_API char *megler_apk_report_splits(const char *apk_path, const char *splits_dir);
MEGLER_API bool megler_apk_icon_splits(const char *apk_path, const char *splits_dir, uint8_t **data, size_t *size);

/*
 * An app's activities as another app's launcher sees them (megler.PhoneApps; the shell writes them beside the list of the
 * phone's apps, library/PhoneAppsList.cpp): `out_dir`, made if it is not there, is given activities.json --
 *
 *   { "package": "org.example", "label": "Example", "icon": 2131558400,
 *     "activities": [ { "name": "org.example.Main", "label": "Example", "icon": 2131558401, "enabled": false,
 *                       "exported": true, "filters": [ { "id": 1, "actions": ["android.intent.action.MAIN"],
 *                       "categories": ["android.intent.category.LAUNCHER"], "schemes": [], "hosts": [], "types": [] } ] },
 *                     { "name": "org.example.Light", "target": "org.example.Main", ... } ],
 *     "certificates": [ "3082..." ] }
 *
 * -- an activity-alias with the activity it stands for ("target"), a label in the phone's language (a resource's, through
 * the splits' tables too), "enabled" only when the manifest says, "exported" as the manifest says or else whether it has an
 * intent filter -- and a picture of each icon the app and its activities name, <id as 8 hex digits>.png: a PNG, a WebP made
 * one, an XML one (adaptive, vector) drawn. "certificates" are those the APK is signed with, each its DER in hex digits: the
 * APK Signing Block's v3 signer's, else its v2 one's, else the v1 PKCS#7 file's under META-INF (megler.PhoneApps gives them
 * as the app's signatures; Google Play compares them with its own record). `splits_dir` as megler_apk_report_splits has it.
 * False when the APK's manifest cannot be read or the description cannot be written.
 */
MEGLER_API bool megler_apk_describe_activities(const char *apk_path, const char *splits_dir, const char *out_dir);

/*
 * A bundle of split APKs as they go about outside Google Play: an .apks file (SAI's, bundletool's, APKPure's: the base
 * APK and its splits in a ZIP archive, perhaps with APKPure's manifest.json and an icon) or an .xapk (APKPure's:
 * manifest.json, the APK or the base and its splits, an icon, and perhaps expansion files under Android/obb/<package>/)
 * or an .apkm (APKMirror's: base.apk and split_*.apk, an info.json and an icon.png).
 * It is unpacked into what this phone can use, as an installer picks what to install:
 *
 *   apk_path      where the base APK is written (replaced if it is there)
 *   splits_dir    the folder the splits kept are written into, as split_<name>.apk (Android's names for installed splits),
 *                 with the icon the bundle came with (icon.png) and a record of what was done (bundle.json), which
 *                 megler_apk_report_splits reads. What was in it before is removed: a split of an older version is not
 *                 read with a newer base.
 *   density_dpi   the density Android will be told the screen has: of the splits for densities, the nearest at or above
 *                 it and the nearest below it are kept (a bundle with one keeps that one)
 *   languages     the languages whose splits are kept, as their codes: "en,ru". NULL or "": English
 *   progress      called now and then with the bytes written so far and how many there will be; may be NULL
 *
 * Kept: the base; the split for 32-bit ARM (armeabi-v7a, else armeabi), never one for another processor; the density
 * and language splits above; every other split (feature splits, asset packs). At most ten splits. Which APK is which is
 * read from their manifests (the base names no split); the splits not wanted are left out by their file names, which
 * keep bundletool's (config.arm64_v8a.apk, split_config.arm64_v8a.apk, splits/base-arm64_v8a.apk), without unpacking
 * them first. The expansion files are written where the app finds them when it runs, its own part of the shared
 * storage (/storage/emulated/0/Android/obb/<package>/ is <data_dir>/apps/<package>/sdcard/Android/obb/<package>/),
 * as manifest.json says or as the bundle has them; one meant for anywhere else is left out.
 *
 * Returns JSON (free with megler_free; NULL only when out of memory), the same as the record:
 *
 *   { "ok": true, "bundle": "BiP.apks", "package": "com.turkcell.bip", "versionCode": 4450, "versionName": "4.2.42",
 *     "base": "com.turkcell.bip.apk",                                 its name in the bundle
 *     "splits": [ { "name": "config.armeabi_v7a", "from": "config.armeabi_v7a.apk", "file": "split_config.armeabi_v7a.apk" } ],
 *     "leftOut": [ { "name": "config.arm64_v8a", "from": "config.arm64_v8a.apk", "abi": "arm64-v8a", "why": "..." } ],
 *     "expansions": [ { "from": "Android/obb/...", "size": 123, "to": "/storage/emulated/0/Android/obb/..." } ],
 *     "icon": true }
 *
 * or { "ok": false, "error": "..." }, a sentence, and then nothing it wrote is left. It reads the bundle and writes;
 * it takes as long as the files take to unpack (seconds for hundreds of megabytes on a phone): not on the UI thread.
 * The base and the splits it writes are what megler_apk_report_splits and megler_run_config.splits_dir take.
 */
MEGLER_API char *megler_bundle_unpack(const char *bundle_path, const char *apk_path, const char *splits_dir, int density_dpi, const char *languages,
                         void (MEGLER_CB *progress)(void *user, uint64_t done, uint64_t total), void *user);

/* ---------------------------------------------------------------- running
 *
 * One app runs per process: Android libraries cannot be unloaded, so after
 * megler_stop (or the app's own exit) the process must end before another app
 * -- or the same one again -- can start.
 *
 * The screen. The app draws with OpenGL ES, which ANGLE turns into Direct3D
 * 11. ANGLE makes the Direct3D device; the shell makes a swap chain on that
 * same device and shows it. Each frame the app renders into a texture the
 * shell provided, and the shell copies that texture to its back buffer and
 * presents:
 *
 *   attach_device    once, when ANGLE has its device (an ID3D11Device *; the
 *                    shell takes its own reference). Make the swap chain here.
 *   acquire_target   when the app creates its window surface: return an
 *                    ID3D11Texture2D * of exactly width x height,
 *                    DXGI_FORMAT_B8G8R8A8_UNORM, bindable as a render target
 *                    and a shader resource, made on that device. The layer
 *                    takes no reference; it stays valid until release_target.
 *   present          a frame is complete in the target. Copy it to the back
 *                    buffer (CopyResource -- nothing that changes the device
 *                    context's pipeline state, which is ANGLE's) and Present.
 *                    The image is upside down by Direct3D's convention (row 0
 *                    is the bottom of the picture): flip it when showing it.
 *   release_target   the window surface is gone.
 *
 * All four are called on the app's own render thread, never on the UI thread.
 *
 * An app whose user interface is Java and that draws no OpenGL does none of
 * that: its views are drawn in memory (megler_gfx_host below) and each finished
 * picture of the whole screen comes through one more call:
 *
 *   present_pixels   `pixels` is the picture: width x height pixels of four
 *                    bytes, in memory B, G, R, A, colour already multiplied by
 *                    alpha, the top row first, `stride` bytes from one row to
 *                    the next (DXGI_FORMAT_B8G8R8A8_UNORM, row 0 the top: no
 *                    flip). Width and height are the surface's: megler_run_config's,
 *                    and after megler_set_surface its (a picture drawn before it may
 *                    still come at the old size).
 *                    COPY IT BEFORE RETURNING: the memory is the app's own
 *                    bitmap and is drawn into again as soon as this returns.
 *                    Then show the copy, on the shell's own thread or this
 *                    one. The picture is the whole window with nothing behind
 *                    it: take it as opaque and ignore the alpha byte (where
 *                    the app drew nothing the bytes are zero, which is black).
 *                    It replaces present for such an app: attach_device and
 *                    acquire_target are never called for it, so the device
 *                    and swap chain are the shell's own, made when the first
 *                    picture comes. Called when something on screen changed,
 *                    not at a steady rate, on the app's main thread (the one
 *                    its Activity runs on) -- and, once the app draws with
 *                    OpenGL ES on a view (a GLSurfaceView, EGL on a
 *                    SurfaceView), also on the thread that swapped a frame:
 *                    the core then composes the window itself, the views
 *                    over the frames read back from OpenGL ES, and hands the
 *                    result here, the GL view's rectangle as the part for a
 *                    new frame. One call at a time, never on the UI thread.
 *                    `pixels` NULL would mean the app has nothing
 *                    to show any more; the layer does not pass that on today,
 *                    and a shell that is handed it leaves the last picture.
 *                    May be NULL: such an app then runs unseen. A picture the
 *                    shell's drawing shows itself (megler_gfx_host.present)
 *                    does not come here; pictures that do come after such
 *                    ones are whole and right as ever, the part given as what
 *                    changed since the last picture shown either way.
 *   present_pixels_part  the same picture, whole as present_pixels hands it,
 *                    of which only the rectangle [x, x + part_width) x [y, y +
 *                    part_height) differs from the picture handed before: the
 *                    app drew that part of the screen again and left the rest
 *                    as it was. A shell that keeps its last picture copies that
 *                    rectangle only. Optional: when NULL the layer calls
 *                    present_pixels for such a picture as well. Same thread
 *                    and same rule (copy before returning) as present_pixels;
 *                    the first picture always comes through present_pixels.
 */
typedef struct megler_window_host {
    void *user;
    bool (MEGLER_CB *attach_device)(void *user, void *d3d11_device);
    void *(MEGLER_CB *acquire_target)(void *user, int width, int height);
    void (MEGLER_CB *present)(void *user);
    void (MEGLER_CB *release_target)(void *user);
    void (MEGLER_CB *present_pixels)(void *user, const void *pixels, int width, int height, int stride);
    void (MEGLER_CB *present_pixels_part)(void *user, const void *pixels, int width, int height, int stride, int x, int y, int part_width,
                                int part_height);
} megler_window_host;

/*
 * Sound. The layer mixes everything the app plays into one stream of 16-bit
 * stereo and hands it over ten milliseconds at a time:
 *
 *   open    once, when the app first makes a sound: start an output at `rate`
 *           Hz (44100). Return false if there is none; the app then runs silent.
 *   write   `frames` stereo frames (2 * frames samples) to play next. It should
 *           return when they are queued, blocking while more than about 50 ms
 *           is already waiting: the layer's mixer runs as fast as this lets it.
 *           Called on the mixer's thread only.
 *
 * All of it may be left NULL.
 */
/*
 *   route   where the sound is to go, as a Java app's AudioManager has it: MEGLER_AUDIO_ROUTE_DEFAULT where it always goes;
 *           for a call (MODE_IN_COMMUNICATION, MODE_IN_CALL, a communication device), MEGLER_AUDIO_ROUTE_EARPIECE, or
 *           MEGLER_AUDIO_ROUTE_SPEAKER on speaker (setSpeakerphoneOn, setCommunicationDevice). Called on the app's threads
 *           when it changes. May be left NULL.
 */
enum { MEGLER_AUDIO_ROUTE_DEFAULT = 0, MEGLER_AUDIO_ROUTE_EARPIECE = 1, MEGLER_AUDIO_ROUTE_SPEAKER = 2 };
typedef struct megler_audio_host {
    void *user;
    bool (MEGLER_CB *open)(void *user, int rate);
    void (MEGLER_CB *write)(void *user, const int16_t *samples, int frames);
    void (MEGLER_CB *route)(void *user, int route);
} megler_audio_host;

/*
 * Films. An app that plays a video file hands it to Android's decoders; here
 * the layer copies the file out to one of its own and the shell decodes it
 * with whatever the system has (Media Foundation's source reader on a phone):
 *
 *   open         `path` is an MP4 (or whatever the app shipped) in the data
 *                folder. Fill in what it holds and return a handle, or NULL
 *                when it cannot be played.
 *   read_video   the next picture into `rgba`: width * height * 4 bytes, red
 *                first, top row first. 1, 0 at the end of the file, -1 when
 *                it cannot be decoded. `time_us` is when it is to be shown.
 *   read_audio   up to `max_frames` frames of 16-bit sound, in the file's own
 *                rate and channels, interleaved. How many, 0 at the end, -1.
 *   seek         both go on from this time.
 *   close        the handle is done with.
 *
 * Called from the app's threads, one call at a time for a handle. All of it
 * may be left NULL: the app is then told its file cannot be opened.
 */
typedef struct megler_media_info {
    int width, height;                 /* 0: no picture */
    int frame_rate_num, frame_rate_den;
    int sample_rate, channels;         /* 0: no sound */
    int64_t duration_us;
} megler_media_info;

typedef struct megler_media_host {
    void *user;
    void *(MEGLER_CB *open)(void *user, const char *path, megler_media_info *info);
    int (MEGLER_CB *read_video)(void *user, void *media, uint8_t *rgba, int64_t *time_us);
    int (MEGLER_CB *read_audio)(void *user, void *media, int16_t *samples, int max_frames, int64_t *time_us);
    bool (MEGLER_CB *seek)(void *user, void *media, int64_t time_us);
    void (MEGLER_CB *close)(void *user, void *media);
} megler_media_host;

/*
 * Decoders of samples, for an app's Java MediaCodec.
 *
 * An app that plays video or music through android.media.MediaCodec
 * (ExoPlayer does, and apps that use MediaCodec themselves) reads the file
 * itself and hands the decoder one sample at a time: an access unit of H.264
 * or H.265 (one picture), a packet of AAC or MP3. The layer takes the samples
 * in all the forms Android lets an app hand them over -- start codes or
 * length prefixes, parameter sets in the stream or apart from it as the
 * codec-specific data, ADTS headers or none -- and asks the shell for the
 * decoding proper, in one form (Media Foundation's decoder transforms on a
 * phone):
 *
 *   supports      whether a decoder of this kind (MEGLER_CODEC_*) is there.
 *                 Asked when an app first lists the decoders (MediaCodecList),
 *                 once for each kind; the app is shown those that are.
 *   open          a decoder for `format`, or NULL. Pictures: `width` and
 *                 `height` are what the app said (0 when it did not), and
 *                 `config` is the stream's parameter sets in Annex-B (each NAL
 *                 unit after a start code 00 00 00 01) -- SPS and PPS for
 *                 H.264, VPS, SPS and PPS for H.265 -- or nothing when the app
 *                 has not given them yet. Sound: `sample_rate` and `channels`
 *                 are the stream's (never 0), and `config` is AAC's
 *                 AudioSpecificConfig (always there for AAC: the layer makes
 *                 one when the app gave none) or, for MP3, nothing.
 *   push          one sample, in decode order, with its time. H.264, H.265:
 *                 one access unit in Annex-B; the first after open and after
 *                 each flush has the parameter sets in front of its slices
 *                 (the layer puts them there when the app's did not have
 *                 them), and a later one has them when they change. AAC: one
 *                 raw access unit, no ADTS header. MP3: one or more whole
 *                 frames. True when it was taken (what it decodes to may come
 *                 out later); false when the decoder failed on it, which the
 *                 shell says in its log.
 *   drain         the end of the stream: what the decoder holds back (pictures
 *                 waiting for later ones, sound) is to come out of the pulls
 *                 that follow. Samples may come again after it.
 *   pull_picture  the next picture, in display order (but see `reorder`): a
 *                 handle, with what it is in *picture; NULL when none is
 *                 ready. Its memory stays as it is until release_picture.
 *   release_picture  done with a picture. Any thread, any time, before or
 *                 after its decoder is closed.
 *   read_picture  optional, with show_picture: a MEGLER_CODEC_GPU picture
 *                 brought into memory, *picture then what it is there
 *                 (MEGLER_CODEC_BGRA or NV12, the same size and time), its
 *                 memory kept until release_picture. False when it cannot be:
 *                 the picture is then not shown.
 *   show_picture  a MEGLER_CODEC_GPU picture made the newest of the gfx
 *                 host's texture `texture_id` (megler_gfx_host.texture_frame's
 *                 ids: draw_texture draws it in a view's place, the right way
 *                 up), on the graphics processor, nothing read back: 1, and
 *                 the view is to be drawn again; 2 where the shell has shown
 *                 it already, the texture's video lying under the views of
 *                 the window it composes, and the view need not be drawn
 *                 again. 0 when it cannot be: the layer then reads it and
 *                 draws it as a picture in memory.
 *   pull_sound    the next block of 16-bit sound into *sound, true; false when
 *                 none is ready. Its samples stay until the next call for that
 *                 decoder.
 *   flush         what was pushed and not pulled is dropped, and so is what
 *                 the decoder holds back; the next sample starts afresh (the
 *                 app has sought to a key frame).
 *   close         the decoder is done with.
 *
 *   reorder       how many places early a picture may come out of
 *                 pull_picture: 0 when it hands them out in display order, as
 *                 Media Foundation's decoders do. Otherwise the layer holds
 *                 that many back and puts them in order by their times (a
 *                 decoder that gives pictures in decode order, as Apple's
 *                 VideoToolbox does, would say 4). Should a picture still
 *                 come out after a later one, the layer says so in the log
 *                 and holds more back from then on.
 *
 * A picture is `width` x `height` visible pixels, the top row first, and
 * `time_us` is the time of the sample it was decoded from. MEGLER_CODEC_NV12:
 * `data` is the luma of the top left visible pixel, rows `stride` bytes
 * apart, and `chroma` the U, V pair of that pixel, at half the size both
 * ways, rows `chroma_stride` apart (the visible part starts at an even x and
 * y). `matrix` says how such a picture becomes colour: MEGLER_CODEC_BT601,
 * MEGLER_CODEC_BT709, or 0 when the stream does not say (the layer then takes
 * BT.601 for pictures under 720 lines and BT.709 for the rest, as Android
 * does); `full_range` is luma from 0 to 255 rather than 16 to 235.
 * MEGLER_CODEC_RGBA: `data` is R, G, B, A bytes, rows `stride` apart, and the
 * rest is not used; MEGLER_CODEC_BGRA the same in the bitmaps' order, B, G, R,
 * A. MEGLER_CODEC_GPU: the picture is on the graphics processor, decoded there
 * and already turned into colour on the gfx host's Direct3D device, and
 * `data` and `chroma` are NULL: read_picture brings it into memory where it
 * is needed there, and show_picture shows it where a view shows the frames of
 * a texture, which is what a decoder on the graphics processor is for (a
 * TextureView's video, nothing copied through the memory).
 *
 * A block of sound is `frames` frames of `channels` interleaved samples (one
 * or two: a decoder of more channels is asked for two), at `sample_rate`;
 * `time_us` is the time of its first frame.
 *
 * The calls for one decoder come one at a time, from the app's threads (any
 * of them, never the shell's UI thread); calls for two decoders may come at
 * once, and so may release_picture. All of it may be left zero: no decoder
 * is listed then, and an app that asks for one is told there is none, as on
 * a phone without them.
 */
enum { MEGLER_CODEC_H264 = 1, MEGLER_CODEC_H265 = 2, MEGLER_CODEC_AAC = 3, MEGLER_CODEC_MP3 = 4 };
enum { MEGLER_CODEC_NV12 = 1, MEGLER_CODEC_RGBA = 2, MEGLER_CODEC_BGRA = 3, MEGLER_CODEC_GPU = 4 };
enum { MEGLER_CODEC_BT601 = 1, MEGLER_CODEC_BT709 = 2 };

typedef struct megler_codec_format {
    int width, height;                 /* pictures; 0: not said */
    int sample_rate, channels;         /* sound */
    const uint8_t *config;
    size_t config_size;
} megler_codec_format;

typedef struct megler_codec_picture {
    int format;                        /* MEGLER_CODEC_NV12, _RGBA, _BGRA or _GPU */
    int width, height;
    const uint8_t *data, *chroma;
    int stride, chroma_stride;
    int matrix;
    bool full_range;
    int64_t time_us;
} megler_codec_picture;

typedef struct megler_codec_sound {
    const int16_t *samples;
    int frames, sample_rate, channels;
    int64_t time_us;
} megler_codec_sound;

typedef struct megler_codec_host {
    void *user;
    int reorder;
    bool (MEGLER_CB *supports)(void *user, int kind);
    void *(MEGLER_CB *open)(void *user, int kind, const megler_codec_format *format);
    bool (MEGLER_CB *push)(void *user, void *codec, const uint8_t *data, size_t size, int64_t time_us);
    void (MEGLER_CB *drain)(void *user, void *codec);
    void *(MEGLER_CB *pull_picture)(void *user, void *codec, megler_codec_picture *picture);
    void (MEGLER_CB *release_picture)(void *user, void *picture);
    bool (MEGLER_CB *pull_sound)(void *user, void *codec, megler_codec_sound *sound);
    void (MEGLER_CB *flush)(void *user, void *codec);
    void (MEGLER_CB *close)(void *user, void *codec);
    bool (MEGLER_CB *read_picture)(void *user, void *picture, megler_codec_picture *out);
    int (MEGLER_CB *show_picture)(void *user, void *picture, int texture_id);
} megler_codec_host;

/*
 * Cameras, for an app's Java android.hardware.Camera.
 *
 * An app that scans a code or shows what the camera sees opens a camera,
 * picks a size from those it offers, has its pictures shown on a view and
 * copied to it as they come (NV21), and asks it to focus. The layer is the
 * Android side of all of that (core/src/drivers/mg_java_camera.c: the
 * parameters, the rotation and mirroring for the view, the app's buffers and
 * the threads its callbacks run on); what it asks of the shell is the camera
 * itself (Windows.Media.Capture on a phone):
 *
 *   count        how many cameras there are.
 *   info         what camera `index` (0 .. count - 1) is, without opening it:
 *                which way it faces, MEGLER_CAMERA_BACK (away from the user) or
 *                MEGLER_CAMERA_FRONT; `orientation`, how many degrees clockwise
 *                its picture must be turned to stand upright on the phone
 *                held in its natural position (Android's CameraInfo
 *                .orientation: 90 for the back camera of most phones, whose
 *                sensor is laid along the long side; 270 for most front
 *                cameras; 0 for a camera on a stand); `focus` and `light`,
 *                1 when it focuses or has a torch, 0 when not, -1 when that is
 *                not known before it has been opened once; `name`, for the
 *                log. False for an index that is no camera.
 *   open         connects to camera `index`, for this app alone, and fills
 *                *caps: the sizes it streams at (the largest first), each with
 *                the slowest and fastest whole frames a second it offers, and
 *                what it can do -- focus once and continuously, focus close
 *                up, a torch. On a phone this is where Windows asks the user,
 *                the first time, whether Megler may use the camera; it returns
 *                when the user has answered. A handle, or NULL with *error
 *                one of MEGLER_CAMERA_DENIED (the user or the system said no),
 *                MEGLER_CAMERA_BUSY (something else has the camera),
 *                MEGLER_CAMERA_FAILED, and caps->why a sentence for the log that
 *                says what Windows said.
 *   start        streams at `width` x `height`, one of caps' sizes, with at
 *                most `fps` frames a second (the shell takes the nearest it
 *                offers). Each picture comes to `frame`, called on a thread of
 *                the shell's with `core` as its first argument, one call at a
 *                time, until stop has returned; the picture is the shell's for
 *                the call alone (the layer copies it). `frame` with NULL for
 *                the picture: the camera stopped for good (it was taken away,
 *                or unplugged); nothing comes after that. False when it would
 *                not start; the shell's log says why.
 *   stop         no more pictures: when this returns, no call of `frame` is
 *                under way and none will come. Focusing (below) ends with it.
 *   focus        MEGLER_CAMERA_FOCUS_ONCE: focus once (Android's autoFocus in its
 *                "auto" mode); _MACRO: the same, close up; _CONTINUOUS: keep
 *                focusing by itself from now on. `done` is called once, from
 *                any thread, with `core` and whether the picture came into
 *                focus (for _CONTINUOUS, when it first did or at once when it
 *                already was). MEGLER_CAMERA_FOCUS_NONE: stop focusing, whatever
 *                was asked; `done` of an earlier call is not called after this
 *                returns. While streaming only. False when it cannot (and
 *                `done` is not called).
 *   torch        the light on or off. While streaming; the shell turns it off
 *                when streaming stops.
 *   close        the handle is done with (the layer stops it first).
 *
 * A picture is NV12: `width` x `height`, as the camera's sensor sees it (not
 * turned: `orientation` says how far the layer turns it), `y` the luma of the
 * top left pixel with rows `y_stride` bytes apart, `uv` the U, V pair of the
 * top left 2 x 2 pixels with rows `uv_stride` apart. `time_us` is when it was
 * taken, on any clock that only goes forward.
 *
 * The calls come from the app's threads (any of them, one at a time for a
 * camera), never the shell's UI thread; open may wait for the user. All of it
 * may be left zero: an app is then told there is no camera, as on a phone
 * without one.
 */
enum { MEGLER_CAMERA_BACK = 0, MEGLER_CAMERA_FRONT = 1 };
enum { MEGLER_CAMERA_OK = 0, MEGLER_CAMERA_DENIED, MEGLER_CAMERA_BUSY, MEGLER_CAMERA_FAILED };
enum { MEGLER_CAMERA_FOCUS_NONE = 0, MEGLER_CAMERA_FOCUS_ONCE, MEGLER_CAMERA_FOCUS_MACRO, MEGLER_CAMERA_FOCUS_CONTINUOUS };
#define MEGLER_CAMERA_MAX_SIZES 24

typedef struct megler_camera_info {
    int facing;                        /* MEGLER_CAMERA_BACK or MEGLER_CAMERA_FRONT */
    int orientation;                   /* 0, 90, 180 or 270 */
    int focus, light;                  /* 1, 0, or -1: not known yet */
    char name[96];
} megler_camera_info;

typedef struct megler_camera_size {
    int width, height;
    int min_fps, max_fps;
} megler_camera_size;

typedef struct megler_camera_caps {
    int size_count;
    megler_camera_size sizes[MEGLER_CAMERA_MAX_SIZES];
    bool focus;                        /* focuses once and continuously */
    bool macro;                        /* focuses close up */
    bool torch;
    char why[256];                     /* open failed: why, for the log */
} megler_camera_caps;

typedef struct megler_camera_frame {
    int width, height;
    const uint8_t *y, *uv;
    int y_stride, uv_stride;
    int64_t time_us;
} megler_camera_frame;

typedef void (MEGLER_CB *megler_camera_frame_fn)(void *core, const megler_camera_frame *frame);
typedef void (MEGLER_CB *megler_camera_focus_fn)(void *core, bool focused);

typedef struct megler_camera_host {
    void *user;
    int (MEGLER_CB *count)(void *user);
    bool (MEGLER_CB *info)(void *user, int index, megler_camera_info *info);
    void *(MEGLER_CB *open)(void *user, int index, megler_camera_caps *caps, int *error);
    bool (MEGLER_CB *start)(void *user, void *camera, int width, int height, int fps, megler_camera_frame_fn frame, void *core);
    void (MEGLER_CB *stop)(void *user, void *camera);
    bool (MEGLER_CB *focus)(void *user, void *camera, int mode, megler_camera_focus_fn done, void *core);
    void (MEGLER_CB *torch)(void *user, void *camera, bool on);
    void (MEGLER_CB *close)(void *user, void *camera);
} megler_camera_host;

/*
 * The phone's own folders in an app's storage.
 *
 * Android's shared storage (/sdcard, /storage/emulated/0) is one folder under
 * the data folder, <data_dir>/sdcard, the same for every app, with Android's
 * standard folders in it: Alarms, DCIM, Documents, Download, Movies, Music,
 * Notifications, Pictures, Podcasts, Ringtones. An app's own part of it,
 * Android/data/<package> and Android/obb/<package>, is kept with the app:
 * <data_dir>/apps/<package>/sdcard/Android/....
 *
 * A link puts a folder of the phone's own in that storage: the user's music
 * as /sdcard/Music, a folder they chose as /sdcard/Documents. The layer
 * opens files by path with the plain file functions, and an app container
 * may not name the user's folders to those; what Windows gives an app there
 * is handles, through Windows.Storage's broker. So for everything in a link
 * the layer asks the shell, here:
 *
 *   links        the links, `link_count` of them (at most
 *                MEGLER_STORAGE_MAX_LINKS). A link's number in the calls below
 *                is its place in this array. `name` is what the folder is
 *                called in /sdcard: one name, not a path, and not "Android".
 *                One of the standard folders' names takes that folder's
 *                place; any other name is a folder more. The array and the
 *                names are copied by megler_run.
 *                `writable` false: the app may read there and nothing else.
 *                The layer refuses the rest itself (EACCES), and the shell is
 *                never asked to change anything in such a link.
 *
 *   open         the operating system's own handle for something in a link
 *                (a HANDLE; the layer closes it with CloseHandle), or NULL
 *                with *error one of MEGLER_STORAGE_*. `kind` says what for:
 *                  MEGLER_STORAGE_FILE    a file, to read and write as `flags`
 *                      say (MEGLER_STORAGE_READ, _WRITE; _CREATE: made if it
 *                      is not there; _EXCLUSIVE with it: and refused,
 *                      MEGLER_STORAGE_EXISTS, if it is; _TRUNCATE: emptied;
 *                      _APPEND may be ignored, the layer seeks before each
 *                      write itself). Shared for reading, writing and
 *                      deleting, as Android's files are. A folder of that
 *                      name is MEGLER_STORAGE_IS_A_FOLDER.
 *                  MEGLER_STORAGE_FOLDER  a folder, to list it: a handle that
 *                      GetFileInformationByHandleEx(FileIdBothDirectoryInfo)
 *                      reads the entries from. A file of that name is
 *                      MEGLER_STORAGE_NOT_A_FOLDER.
 *                  MEGLER_STORAGE_ANY     whichever is there, to ask its kind,
 *                      size and times: a handle with FILE_READ_ATTRIBUTES is
 *                      enough.
 *   make_folder  a new folder; MEGLER_STORAGE_EXISTS when the name is taken.
 *   remove       a file (`folder` false; a folder of that name is
 *                MEGLER_STORAGE_IS_A_FOLDER), or a folder, which must be empty
 *                (MEGLER_STORAGE_NOT_EMPTY): what it holds is never removed
 *                with it.
 *   rename       as rename(2): a new name, in the same folder, another, or
 *                another link. A file that has the new name is replaced by
 *                it. MEGLER_STORAGE_OTHER_DEVICE when it cannot be done as a
 *                rename: the app is told EXDEV, and copies and removes, as
 *                it would between two disks.
 *
 * `path` is UTF-8 and relative to the link's folder, parts joined by "/",
 * none at either end; "" is the link's folder itself (only ever opened, as a
 * folder or as whichever). The layer has resolved "." and "..". Names arrive
 * as Windows takes them: Android allows any character in a name but '/',
 * and the layer has already mapped what Windows refuses -- < > : " \ | ? *,
 * the characters 1 to 31, a dot or a space at a name's end, the first
 * letter of a device's name (CON, NUL, AUX, COM1 ...) -- to the character
 * U+F000 above it, as Cygwin and WSL do. A backslash or a colon in `path` is
 * never a separator or a drive, then: they do not arrive at all. The names a
 * folder's handle lists are mapped back by the layer, so the shell neither
 * maps nor needs to know.
 *
 * The calls come on the app's own threads, any of them and several at once,
 * and never on the shell's UI thread; each returns when it is done. They are
 * the cost of every open() and stat() in a link, so what was found out about
 * a folder is worth keeping.
 *
 * All of it may be left zero: the shared storage is then the layer's own
 * folder and nothing else.
 */
#define MEGLER_STORAGE_MAX_LINKS 32
enum { MEGLER_STORAGE_READ = 1, MEGLER_STORAGE_WRITE = 2, MEGLER_STORAGE_CREATE = 4, MEGLER_STORAGE_TRUNCATE = 8, MEGLER_STORAGE_APPEND = 16, MEGLER_STORAGE_EXCLUSIVE = 32 };
enum { MEGLER_STORAGE_FILE = 0, MEGLER_STORAGE_FOLDER = 1, MEGLER_STORAGE_ANY = 2 };
enum {
    MEGLER_STORAGE_OK = 0, MEGLER_STORAGE_NOT_FOUND, MEGLER_STORAGE_DENIED, MEGLER_STORAGE_EXISTS, MEGLER_STORAGE_NOT_A_FOLDER, MEGLER_STORAGE_IS_A_FOLDER,
    MEGLER_STORAGE_FULL, MEGLER_STORAGE_FAILED, MEGLER_STORAGE_INVALID, MEGLER_STORAGE_NOT_EMPTY, MEGLER_STORAGE_BUSY, MEGLER_STORAGE_OTHER_DEVICE
};

typedef struct megler_storage_link {
    const char *name;
    bool writable;
} megler_storage_link;

typedef struct megler_storage_host {
    void *user;
    const megler_storage_link *links;
    int link_count;
    void *(MEGLER_CB *open)(void *user, int link, const char *path, int flags, int kind, int *error);
    bool (MEGLER_CB *make_folder)(void *user, int link, const char *path, int *error);
    bool (MEGLER_CB *remove)(void *user, int link, const char *path, bool folder, int *error);
    bool (MEGLER_CB *rename)(void *user, int from_link, const char *from, int to_link, const char *to, int *error);
} megler_storage_host;

/*
 * Drawing, for an app whose user interface is Java.
 *
 * Such an app draws with android.graphics: a Canvas over a Bitmap, a Paint,
 * text, pictures out of image files. The layer runs those classes and keeps
 * the bitmaps and all the bookkeeping (save counts, the current matrix, the
 * clip's bounds, paths taken apart, parameters checked). What it asks of the
 * shell is the drawing proper, text and image files: on a phone Direct2D,
 * DirectWrite and WIC. This comment is the whole contract. Where the numbers
 * come from, the framework's megler.Gfx class and what it packs into arrays, is
 * written out in core/src/drivers/mg_java_gfx.c; nothing of that packing
 * reaches the shell.
 *
 * Any function may be NULL. What it would have drawn is then missing, what it
 * would have returned is zero, and the log says so once.
 *
 * PIXELS AND COORDINATES
 *
 *   A bitmap is memory the layer owns: width x height pixels of four bytes,
 *   in memory B, G, R, A (the word 0xAARRGGBB), colour already multiplied by
 *   alpha (so no colour byte is above the alpha byte), top row first, `stride`
 *   bytes from one row to the next (4 * width). That is
 *   DXGI_FORMAT_B8G8R8A8_UNORM with premultiplied alpha, WIC's 32bppPBGRA.
 *   The address does not change while the bitmap lives.
 *
 *   x grows to the right and y DOWN. (0, 0) is the top left corner of the top
 *   left pixel: pixel (i, j) covers i..i+1 by j..j+1 and its centre is at
 *   (i + 0.5, j + 0.5).
 *
 *   A colour argument (a paint's, a gradient's, draw_color's) is 0xAARRGGBB
 *   and NOT premultiplied. Only pixels in memory are premultiplied. Colours
 *   are sRGB values used as they are: nothing is converted to linear light
 *   for blending or for gradients, and there is no colour management.
 *
 *   A matrix is six floats and takes a point of the drawing's own coordinates
 *   to pixels:
 *       x' = m[0] * x + m[1] * y + m[2]
 *       y' = m[3] * x + m[4] * y + m[5]
 *   (As a D2D1_MATRIX_3X2_F: _11 = m[0], _12 = m[3], _21 = m[1], _22 = m[4],
 *   _31 = m[2], _32 = m[5].) It can rotate, skew and mirror; there is no
 *   perspective. A rectangle is left, top, right, bottom, with left <= right
 *   and top <= bottom.
 *
 *   Handles (canvases, typefaces, shaders) are the shell's own pointers,
 *   opaque to the layer. A NULL typeface is the default face; a NULL shader is
 *   no shader.
 *
 * THREADS
 *
 *   The app's threads call these, mostly its main thread; never the shell's
 *   UI thread. Calls that use one canvas come one at a time. Calls for
 *   different canvases, and the text measuring, typeface, shader and image
 *   functions, may come from several threads at once (apps decode pictures
 *   and lay out text in the background): the layer does not serialise them.
 *   That includes one shader or one typeface used by drawing calls on two
 *   canvases from two threads at the same moment, and shader_matrix for a
 *   shader that another thread is drawing with: the shell keeps such a
 *   shader whole itself (a lock, or a brush made per call from what the
 *   shader holds). Android promises an app no more than that its own two
 *   threads do not corrupt each other's drawing.
 *
 * CANVASES
 *
 *   canvas_new    a canvas that draws into a bitmap's memory. What the memory
 *                 holds at that moment is its picture to start with. The
 *                 matrix is the identity and the clip is the whole bitmap.
 *                 The shell may draw straight into the memory, or keep a
 *                 surface of its own and collect drawing calls; flush and
 *                 touched are what keeps the memory and such a surface one
 *                 picture. Several canvases can exist on one bitmap. NULL:
 *                 it cannot be made, and nothing is drawn through it.
 *   canvas_free   done with. Every save has been restored and the canvas has
 *                 been flushed by then. The memory itself is the layer's, and
 *                 is not touched after this returns.
 *   flush         when this returns, the memory holds everything drawn through
 *                 the canvas so far: the layer, the app or another canvas is
 *                 about to read it. Only called when something was drawn since
 *                 the last flush.
 *   touched       the memory was written by someone else (the layer itself,
 *                 the app's native code, another canvas) since the last flush:
 *                 take it as the picture again before drawing more. Called
 *                 once, before the next drawing call on the canvas, however
 *                 many writes there were; and not at all when that call is a
 *                 draw_color that replaces every pixel (SRC or CLEAR with
 *                 nothing saved and no clip), which is how every frame of an
 *                 app's window begins. Only a shell whose drawing is in the
 *                 memory when each drawing call returns needs neither
 *                 function. One that draws straight into the memory but only
 *                 has it there after ending a batch (Direct2D's EndDraw) needs
 *                 flush to end the batch, and touched only if it keeps
 *                 anything of the picture outside the memory.
 *                 A flush can come while layers are open (save_layer below):
 *                 the memory then holds what was drawn outside them, as on
 *                 Android, where a layer is a picture of its own until its
 *                 restore; the layers stay open and what is drawn next still
 *                 goes into them.
 *   present       optional: the window's picture, as this canvas has drawn
 *                 it, shown on the screen from the shell's own surface and
 *                 not through the memory: in place of flushing the window's
 *                 bitmap and handing its memory to present_pixels(_part)
 *                 (megler_window_host). For a shell that draws elsewhere than
 *                 in the memory, which would otherwise read its picture back
 *                 at every frame. [x, x + width) x [y, y + height) is what
 *                 changed since the picture shown before; the whole bitmap
 *                 for the first picture, or when all of it changed. True:
 *                 shown. False: nothing was done, and the layer flushes and
 *                 calls present_pixels as it would without this. The canvas
 *                 is not flushed by it: the memory keeps what the last flush
 *                 put there, and the layer flushes, as always, before
 *                 anything reads the memory. The layer calls it for the
 *                 window's bitmap only, when this canvas is that bitmap's
 *                 only one, nothing has been written into the memory behind
 *                 it, and the layer does not compose OpenGL ES frames under
 *                 the views itself (it then needs the memory; see gl_frame);
 *                 on the thread that draws the canvas, never on the UI
 *                 thread, one call at a time with present_pixels. NULL:
 *                 never; the layer flushes.
 *   gl_frame      optional, with present and gl_place: OpenGL ES views'
 *   gl_place      frames shown under the window's views by the shell, the
 *                 layer reading neither back. A view that draws with EGL
 *                 (a GLSurfaceView, a SurfaceView with an EGL window
 *                 surface) has a `layer`, 0..7, and its frames are shown
 *                 at the rectangle gl_place gives it (x, y, width, height
 *                 of the window; width 0: the layer shows nothing any more,
 *                 and its frame is dropped), scaled to it, opaque, with the
 *                 window's views over them -- their premultiplied colours,
 *                 as present last showed them; the views leave holes where
 *                 the frames are to show. gl_frame is a new frame:
 *                 `share_handle` is the DXGI shared handle of ANGLE's
 *                 Direct3D 11 texture of the surface (an OpenGL ES pbuffer;
 *                 EGL_ANGLE_surface_d3d_texture_2d_share_handle), width x
 *                 height, its rows bottom first as OpenGL has them. The
 *                 frame is finished in it (the layer has called glFinish),
 *                 and the texture may be drawn into again as soon as this
 *                 returns: the shell takes what it keeps first. The window
 *                 is shown again with the new frame when the views have
 *                 been shown and the layer placed, else the frame is kept
 *                 for the next present. False: the shell cannot; the layer
 *                 then composes the window itself from then on, reading the
 *                 frames back and the views out of the memory, and hands the
 *                 pictures to present_pixels. gl_frame comes on the thread
 *                 that swapped, gl_place on any thread, before the present
 *                 or the gl_frame it is for. All three NULL, or EGL without
 *                 share handles: the layer composes the window itself.
 *   texture_frame optional, with draw_texture: pictures an app makes with
 *   draw_texture  OpenGL ES for a view to draw among the others (a
 *                 TextureView's frames), kept on the graphics processor and
 *                 drawn there, nothing read back. texture_frame is a new
 *                 picture of `id` (1 and up): `share_handle`, width and
 *                 height as gl_frame's (ANGLE's texture, rows bottom first,
 *                 finished, its keyed mutex let go of for the call); the
 *                 shell keeps a copy of its own before it returns. A NULL
 *                 handle: the id is done with, its copy let go. False: the
 *                 shell cannot, and the layer reads that picture back as it
 *                 would without this. draw_texture draws the id's last
 *                 picture into the canvas, the whole of it into the
 *                 rectangle (left, top, right, bottom) under the canvas's
 *                 matrix and clip, the right way up, smoothly, at `alpha`
 *                 (0..255); nothing where the id has none yet, or the canvas
 *                 is drawn in memory. On the thread that drew the picture,
 *                 and the canvas's, at any time.
 *
 *   save          remembers the clip. restore brings back the clip of the
 *                 matching save. They nest; the layer never calls restore
 *                 without a save before it, and does the counting.
 *   save_layer    a save, and from here to its restore everything is drawn
 *                 into a transparent layer of its own instead of onto the
 *                 picture. At that restore the layer is put onto what was
 *                 there before: each of its pixels multiplied by alpha / 255
 *                 (alpha is 0..255) and combined by `blend` (MEGLER_GFX_BLEND_*
 *                 below, the layer being the source). `bounds`, when not
 *                 NULL, is a rectangle in the matrix of that moment that is
 *                 all the layer has to cover: the clip is intersected with it
 *                 until the restore. Under a matrix that turns or slants it,
 *                 what the layer covers and the clip is cut to is the
 *                 upright box round the rectangle in the bitmap's pixels, as
 *                 on Android (and as the layer's own clip box has it), not
 *                 the slanted rectangle. The blend takes in the layer's whole
 *                 extent (the clip at the save, within bounds), transparent
 *                 pixels too: a DST_IN layer erases what its empty parts
 *                 cover. Layers nest. A layer has to outlive a flush (above)
 *                 and can stay open across many calls, so it is a surface of
 *                 the shell's own, drawn into and put onto the picture at the
 *                 restore; something that must be closed before a batch of
 *                 drawing ends, like Direct2D's PushLayer, cannot be it.
 *   save_layer_blur  save_layer with the source-over blend whose layer is
 *                 blurred as it is put onto the picture: a Gaussian blur of
 *                 radius_x and radius_y (Android's radii, in the matrix's
 *                 units at the save: a standard deviation of 0.57735 *
 *                 radius + 0.5, as Skia's blur takes them), what lies outside
 *                 the layer taken as transparent, or with `clamp` as the
 *                 layer's edge carried on. What a view with Android's
 *                 RenderEffect.createBlurEffect draws comes this way. May be
 *                 left NULL: such a view is then drawn through save_layer,
 *                 sharp.
 *   set_matrix    the matrix for what is drawn from now on, whole: the layer
 *                 keeps the current matrix and does Android's concatenation
 *                 itself. save and restore do not keep it; after a restore
 *                 the layer sets it again when it differs.
 *
 *   clip_rect     op MEGLER_GFX_CLIP_INTERSECT (1): from now on only what is
 *   clip_path     inside the shape and inside the clip so far can be drawn
 *                 on. MEGLER_GFX_CLIP_DIFFERENCE (0): the shape is cut out of
 *                 the clip. The shape is taken in the matrix of this moment
 *                 and is fixed in pixels after that: a later set_matrix does
 *                 not move it. Under a matrix that rotates, the clip is the
 *                 rotated shape, not its bounding box. A path is clipped to
 *                 as it would be filled (`fill` as for draw_path); one with
 *                 nothing in it (count can be 0) has no inside. Whether the
 *                 clip's edge is smoothed is the shell's choice. The layer
 *                 keeps the clip's bounding box itself and makes no drawing
 *                 call while that is empty.
 *
 * DRAWING
 *
 *   A shape is drawn with a megler_gfx_paint (below). `style` says whether its
 *   inside is filled, its outline stroked, or both. A stroke is centred on
 *   the outline, half its width to each side; the width is in the drawing's
 *   coordinates and goes through the matrix like everything else; width 0 is
 *   a hairline, one pixel wide whatever the matrix. Caps are for the ends of
 *   open figures and joins for corners. A miter join that would reach
 *   further than miter_limit times the stroke's width, measured from the
 *   corner's inner point to its tip, is a bevel (PostScript's rule and
 *   Android's; Direct2D's "miter or bevel" with the same number).
 *
 *   With MEGLER_GFX_ANTI_ALIAS in the paint's flags, edges are smoothed.
 *   Without it a pixel is drawn whole or not at all, by whether its centre is
 *   inside the shape (a centre exactly on a left or top edge is inside, on a
 *   right or bottom edge outside). Either way a filled rectangle with whole
 *   numbers for corners, under a matrix that only translates by whole
 *   numbers, covers exactly those pixels.
 *
 *   What is drawn is combined with what is there by the paint's `blend`:
 *   one of the numbers below, Android's PorterDuff modes. S is what is drawn
 *   and D what is there, both premultiplied, Sa and Da their alphas:
 *
 *       0 CLEAR     0                 6 DST_IN    D*Sa                 12 PLUS      min(S + D, 1)
 *       1 SRC       S                 7 SRC_OUT   S*(1-Da)             13 MODULATE  S*D
 *       2 DST       D                 8 DST_OUT   D*(1-Sa)             14 SCREEN    S + D - S*D
 *       3 SRC_OVER  S + D*(1-Sa)      9 SRC_ATOP  S*Da + D*(1-Sa)
 *       4 DST_OVER  D + S*(1-Da)     10 DST_ATOP  D*Sa + S*(1-Da)
 *       5 SRC_IN    S*Da             11 XOR       S*(1-Da) + D*(1-Sa)
 *
 *      15 OVERLAY   S*(1-Da) + D*(1-Sa) + (2*D <= Da ? 2*S*D : Sa*Da - 2*(Da-D)*(Sa-S))
 *      16 DARKEN    S*(1-Da) + D*(1-Sa) + min(S*Da, D*Sa)
 *      17 LIGHTEN   S*(1-Da) + D*(1-Sa) + max(S*Da, D*Sa)
 *
 *   Each is for every channel, the alpha too (with Sa for S and Da for D;
 *   the last three then all come to Sa + Da - Sa*Da). 3 is the ordinary case
 *   and by far the commonest. A paint's blend is never DST, which draws
 *   nothing; a layer's can be, and such a layer is thrown away at its
 *   restore. Whatever the mode, only pixels inside the shape drawn (and the
 *   clip) change: a rectangle drawn with CLEAR clears that rectangle. For a
 *   partly covered pixel of a smoothed edge the result is mixed with what
 *   was there by the coverage.
 *
 *   A shader, when not NULL, gives the colours in place of paint->color: the
 *   fill, the stroke or the glyphs are filled with it, and the alpha of
 *   paint->color (its top byte) multiplies the result.
 *
 *   draw_color       fills the whole clip with a colour, whatever the matrix.
 *                    `blend` as above: CLEAR empties the clip to transparent,
 *                    SRC replaces what is there, alpha included.
 *   draw_rect        the rectangle; draw_oval the ellipse that touches the
 *   draw_oval        rectangle's four sides; draw_round_rect the rectangle
 *   draw_round_rect  with a quarter ellipse of radii rx, ry at each corner
 *                    (0 < rx <= half the width, 0 < ry <= half the height).
 *   draw_path        a path: `count` floats, each figure a verb followed by
 *                    its points, every coordinate absolute:
 *                        MEGLER_GFX_PATH_MOVE  (0)  x y              a figure starts here
 *                        MEGLER_GFX_PATH_LINE  (1)  x y              a line to
 *                        MEGLER_GFX_PATH_QUAD  (2)  cx cy x y        a quadratic Bezier: control, end
 *                        MEGLER_GFX_PATH_CUBIC (3)  ax ay bx by x y  a cubic Bezier: two controls, end
 *                        MEGLER_GFX_PATH_CLOSE (4)                   a line back to the figure's start; the figure is closed
 *                    The stream is well formed: it begins with a MOVE, every
 *                    figure does, and nothing follows a CLOSE but a MOVE or
 *                    the end. A figure without CLOSE is open: filled as if
 *                    closed, stroked with caps at its two ends. A MOVE with
 *                    nothing after it draws nothing. `fill` says what is
 *                    inside: MEGLER_GFX_WINDING (0), where the outline winds
 *                    round a point a number of times that is not zero, or
 *                    MEGLER_GFX_EVEN_ODD (1), where a ray from it crosses the
 *                    outline an odd number of times. Arcs, the corners of
 *                    Android's round rectangles and its ovals inside paths
 *                    arrive as cubics; drawArc, drawLine(s) and drawPoint(s)
 *                    arrive as paths too (a line is one MOVE and one LINE,
 *                    stroked; a point is a small square or circle, filled).
 *   draw_bitmap      the part `src` (a rectangle in the image's pixels, inside
 *                    the image) of another bitmap, stretched onto the
 *                    rectangle `dst` of the drawing's coordinates, through the
 *                    matrix. Each pixel is multiplied by the alpha of
 *                    paint->color and combined by paint->blend; the rest of
 *                    paint->color is not used. MEGLER_GFX_FILTER_BITMAP in the
 *                    flags: smooth sampling (bilinear); without it the
 *                    nearest pixel. MEGLER_GFX_ANTI_ALIAS in the flags: the
 *                    edges of the rectangle it lands on are smoothed, as a
 *                    filled rectangle's are, where they fall between pixels
 *                    or the matrix turns them; without it they are hard.
 *                    Pixels outside `src` must not show at
 *                    the edges of the result: Android draws its stretchable
 *                    pictures (nine-patches) as a dozen pieces of one bitmap.
 *                    The image's memory is only good during the call: a shell
 *                    that draws later copies what it needs first. The memory
 *                    can be the canvas's own; it has been flushed.
 *   draw_text        one run of text on one line, its baseline at y. x is
 *                    where the run starts for text_align 0, its middle for 1,
 *                    its end for 2, by the width text_measure gives. See TEXT.
 *
 * THE PAINT
 *
 *   The layer fills it in from Android's Paint and has checked it: every
 *   enumeration is in range, text_size, miter_limit and text_scale_x are
 *   above zero, stroke_width is not negative.
 *
 *   A colour filter (filter_mode not -1) changes the colours of what is drawn
 *   before it is combined with the picture: each colour C about to be drawn
 *   -- every pixel of the image in draw_bitmap, every colour the shader
 *   gives; premultiplied, and already multiplied by the alpha of
 *   paint->color -- becomes the blend above, by the number in filter_mode,
 *   of S = filter_color and D = C. Then that is combined with the picture by
 *   paint->blend. (5, SRC_IN, is the common filter: the picture's shape in
 *   the filter's colour, which is how Android tints icons.) For a plain
 *   colour the layer has done it already, so a filter only arrives in
 *   draw_bitmap and with a shader.
 *
 *   A shadow (shadow_radius above 0) is a blurred copy of what the call
 *   draws, in one colour, under it and moved by (shadow_dx, shadow_dy). As
 *   Android draws it:
 *     - Offset and radius are in the drawing's coordinates, like everything
 *       else of the call: the offset goes through the matrix as a direction
 *       (a turned canvas turns it, a doubled one doubles it), and the blur
 *       grows with the matrix's scale. The blur's standard deviation is
 *       0.57735 * radius + 0.5 before that scale. (The original draws both
 *       in the bitmap's pixels whatever the matrix; the two agree while the
 *       matrix only moves, which is nearly always.)
 *     - Its colour is shadow_color's red, green and blue. Its alpha is
 *       shadow_color's when that is less than 255, and the paint's (the
 *       alpha of paint->color) when shadow_color is opaque; times how much
 *       the call's shape covers each pixel. The shape's own colour, shader
 *       and colour filter do not show in the shadow.
 *     - It is part of the call: the clip cuts it, paint->blend combines it
 *       with the picture as it does the shape, and it is drawn first, the
 *       shape over it.
 *   Text is what uses it; of the other calls only those of a canvas on a
 *   bitmap are given one on Android, which is every canvas here.
 *
 * TEXT
 *
 *   Text is UTF-16, `count` code units, not terminated. It can hold whatever
 *   a Java string can: unpaired surrogates, control characters. It is one
 *   run on one line; a line feed or a tab in it draws nothing and is not a
 *   line break. Shaping and the fallback to other fonts for characters the
 *   face lacks are the system's. The size is paint->text_size, in the
 *   drawing's coordinates: the height of the em, as Android and DirectWrite
 *   both mean it. Three paint fields change the plain run: every advance is
 *   multiplied by text_scale_x and the glyphs are that much wider; every
 *   character's advance grows by letter_spacing * text_size * text_scale_x
 *   -- every cluster's, not every code unit's: a surrogate pair, a letter
 *   with its combining marks or a ligature the face does not split gets it
 *   once -- half of it before the cluster and half after, so that a run
 *   starts half a spacing in and ends half a spacing early, as on Android;
 *   and a point of a glyph at (x, y) from its place on the baseline, y
 *   down, is drawn at (x + text_skew_x * y, y), so -0.25 leans to the right.
 *   The skew, the style and the fake bold below change what a glyph looks
 *   like and where its ink reaches, never an advance. The measuring
 *   functions answer for the same paint that draws, scale and spacing
 *   included, and are called very often (the framework lays text out by
 *   measuring it piece by piece): they want a cache.
 *
 *   The paint's style applies to glyphs as to shapes: filled, their outlines
 *   stroked with stroke_width, or both. MEGLER_GFX_FAKE_BOLD thickens them by
 *   about text_size / 24 (DirectWrite's bold simulation will do).
 *   MEGLER_GFX_ANTI_ALIAS smooths them in grey levels; no ClearType, the
 *   bitmap may be scaled or blended afterwards. Underlines and strike-through
 *   are drawn by the layer as rectangles.
 *
 *   What has no glyph: a tab advances as a space does and draws nothing (the
 *   framework's own line layout places what follows a tab; a tab only comes
 *   here inside a run that is measured or drawn as it stands). Every other
 *   control character -- below U+0020, and U+007F to U+009F -- and the
 *   characters Unicode says to ignore when a face lacks them (zero-width
 *   joiners and spaces, direction marks, variation selectors) advance
 *   nothing and draw nothing. A line feed is one of those to text_measure,
 *   text_widths, text_bounds and draw_text. None of this was read out of
 *   Android's text code, which is not at hand here: it is what Android's
 *   widgets are seen to do, and is the rule to keep the functions agreeing
 *   with each other.
 *
 *   Glyphs with colours of their own (emoji) keep them: paint->color does
 *   not paint them and a shader does not fill them. The paint's alpha, its
 *   colour filter and its blend apply to them as to a bitmap.
 *
 *   text_measure   the run's advance width: from where it starts to where
 *                  the next character would go.
 *   text_metrics   the face's vertical measures at paint->text_size, as
 *                  distances from the baseline with y down, into out[0..4]:
 *                      out[0]  top      the highest any glyph of the face reaches (negative)
 *                      out[1]  ascent   the recommended distance above the baseline (negative)
 *                      out[2]  descent  the recommended distance below it (positive)
 *                      out[3]  bottom   the lowest any glyph reaches (positive)
 *                      out[4]  leading  the extra gap the face asks for between lines (zero or more)
 *                  and returns the line spacing, -ascent + descent + leading.
 *                  top <= ascent and bottom >= descent. They do not depend
 *                  on the text. (DirectWrite's font metrics times
 *                  text_size / designUnitsPerEm: ascent, descent, lineGap,
 *                  and the glyph box for top and bottom.)
 *   text_bounds    the smallest rectangle round the run's ink when drawn at
 *                  (0, 0) aligned left: out[0..3] = left, top, right, bottom,
 *                  y down from the baseline (top is negative for most text).
 *                  All zero when nothing would be drawn. It is the ink as
 *                  this paint draws it: slanted by text_skew_x (the box of
 *                  the slanted glyphs, not the upright box slanted), thicker
 *                  by fake bold, and wider all round by half the stroke width
 *                  when the style strokes. The shadow is not part of it.
 *   text_break     how many code units from the start fit in max_width.
 *                  `words` false: as many whole characters (clusters) as fit,
 *                  possibly 0. `words` true: what the first line would
 *                  hold were the text wrapped at max_width: the longest start
 *                  that ends where a line may break (after a space, by the
 *                  Unicode rules) and fits, spaces at its end not counting
 *                  against the width; when not even the first word fits,
 *                  what `words` false gives. Either way a line feed ends the
 *                  line and is counted in it: the answer never reaches past
 *                  the first line feed, and is `count` only when all of the
 *                  text fits and no line feed comes before its last unit.
 *                  (The original's typesetter ends a line there in both
 *                  modes, and the framework's layout cuts paragraphs at line
 *                  feeds before it asks. Android's own Paint.breakText, which
 *                  is `words` false, measures on past a line feed; an app
 *                  that hands it text of several lines gets a smaller answer
 *                  here.)
 *   text_widths    the advance of each code unit into out[0..count): a unit
 *                  with none of its own (the second half of a surrogate pair,
 *                  a combining mark, the later letters of a ligature unless
 *                  the system splits it) gets 0. They add up to text_measure.
 *
 * TYPEFACES
 *
 *   typeface_create     a face by Android's family name. `family` is in
 *                       lower case: "sans-serif", "serif", "monospace",
 *                       "sans-serif-medium", "sans-serif-condensed",
 *                       "cursive", or anything an app thinks of. The layer
 *                       has read out of it what Android would: `generic` is
 *                       MEGLER_GFX_SANS (0), MEGLER_GFX_SERIF (1) or
 *                       MEGLER_GFX_MONOSPACE (2); `weight` is 1..999 as CSS and
 *                       DirectWrite count it, 400 regular and 700 bold. Any
 *                       system font of that kind will do; on Android the
 *                       sans-serif is Roboto.
 *   typeface_derive     the same font as `typeface` (NULL: the default) at
 *                       another weight, upright or italic.
 *   typeface_from_data  a face out of a font file's bytes (TrueType or
 *                       OpenType; apps carry their own fonts). The bytes are
 *                       the app's: copy them. NULL when it is not a font.
 *
 *   A typeface is never freed: megler.Gfx has no call for it. The layer makes
 *   each derived face once. The default face, NULL, is the upright
 *   sans-serif at weight 400.
 *
 * SHADERS
 *
 *   A shader's coordinates are the drawing's, at the draw that uses it: it
 *   moves with the canvas's matrix. shader_matrix gives it a matrix of its
 *   own, applied first (shader coordinates to drawing coordinates); NULL is
 *   the identity again.
 *
 *   shader_linear   colours along the line from (x0, y0) to (x1, y1).
 *   shader_radial   colours by the distance from (cx, cy): position 0 at the
 *                   centre, 1 at `radius`.
 *   shader_sweep    colours by the angle round (cx, cy): position 0 points
 *                   along +x (three o'clock), positions grow clockwise as
 *                   seen on screen, 1 is the full turn.
 *                   For all three: `count` colours (at least 2), 0xAARRGGBB
 *                   not premultiplied, each at positions[i], which are in
 *                   0..1 and never go down. Between two stops the colour is
 *                   interpolated as it stands, not premultiplied, and
 *                   premultiplied afterwards (Direct2D's straight mode).
 *                   Before the first stop and after the last, by `tile`:
 *                   MEGLER_GFX_TILE_CLAMP (0) the end colours go on,
 *                   MEGLER_GFX_TILE_REPEAT (1) the gradient starts again,
 *                   MEGLER_GFX_TILE_MIRROR (2) it runs back and forth.
 *   shader_bitmap   an image as the pattern: its top left pixel's corner at
 *                   (0, 0), a pixel to a unit, continued by `tile` in both
 *                   directions (clamp: the edge pixels go on). The pixels are
 *                   copied when the shader is made: it does not follow the
 *                   bitmap afterwards, and the memory is not used after the
 *                   call returns. MEGLER_GFX_FILTER_BITMAP of the paint that
 *                   draws with it chooses the sampling.
 *   shader_free     done with.
 *
 * IMAGE FILES
 *
 *   image_size     whether the bytes are an image file the shell can decode,
 *                  and its size in pixels. Apps ask for the size alone, often:
 *                  read the header, do not decode.
 *   image_decode   the same bytes into `pixels`: a bitmap of the format above,
 *                  exactly width x height. The layer asks for the picture's
 *                  own size divided by 1, 2, 4... (Android's inSampleSize),
 *                  never below 1 pixel; when that is not the picture's own
 *                  size, scale with a filter that is good for reducing. The
 *                  first frame of an animation. A picture without alpha is
 *                  opaque (A = 255). Orientation in the file's metadata is
 *                  not applied. False: it could not be decoded, and the
 *                  memory may hold anything.
 *   image_encode   the bitmap as an image file, handed to `write` in as many
 *                  pieces as is convenient (its second argument is `sink`,
 *                  passed through; false from it means stop and fail).
 *                  MEGLER_GFX_PNG (1): with alpha, the colours un-premultiplied
 *                  as PNG wants them; `quality` means nothing.
 *                  MEGLER_GFX_JPEG (0): `quality` 0..100; alpha is dropped and
 *                  the colours written as they are in memory, which is the
 *                  picture over black. False when it cannot be written.
 *   bitmap_gone    a bitmap whose image was passed in (megler_gfx_image.id) is
 *                  freed. See below.
 *
 * An image handed to draw_bitmap or shader_bitmap comes with two numbers for
 * a shell that keeps textures: `id` is the bitmap's own for as long as it
 * lives, and `version` is different whenever the pixels may have changed. A
 * copy kept under (id, version) can be used again, and bitmap_gone says when
 * to drop it. A shell that copies every time needs none of the three.
 */
enum { MEGLER_GFX_FILL = 0, MEGLER_GFX_STROKE = 1, MEGLER_GFX_FILL_AND_STROKE = 2 };
enum { MEGLER_GFX_CAP_BUTT = 0, MEGLER_GFX_CAP_ROUND = 1, MEGLER_GFX_CAP_SQUARE = 2 };
enum { MEGLER_GFX_JOIN_MITER = 0, MEGLER_GFX_JOIN_ROUND = 1, MEGLER_GFX_JOIN_BEVEL = 2 };
enum { MEGLER_GFX_ALIGN_LEFT = 0, MEGLER_GFX_ALIGN_CENTER = 1, MEGLER_GFX_ALIGN_RIGHT = 2 };
enum { MEGLER_GFX_ANTI_ALIAS = 1, MEGLER_GFX_FILTER_BITMAP = 2, MEGLER_GFX_FAKE_BOLD = 4 };
enum {
    MEGLER_GFX_BLEND_CLEAR = 0, MEGLER_GFX_BLEND_SRC = 1, MEGLER_GFX_BLEND_DST = 2, MEGLER_GFX_BLEND_SRC_OVER = 3, MEGLER_GFX_BLEND_DST_OVER = 4,
    MEGLER_GFX_BLEND_SRC_IN = 5, MEGLER_GFX_BLEND_DST_IN = 6, MEGLER_GFX_BLEND_SRC_OUT = 7, MEGLER_GFX_BLEND_DST_OUT = 8, MEGLER_GFX_BLEND_SRC_ATOP = 9,
    MEGLER_GFX_BLEND_DST_ATOP = 10, MEGLER_GFX_BLEND_XOR = 11, MEGLER_GFX_BLEND_PLUS = 12, MEGLER_GFX_BLEND_MODULATE = 13, MEGLER_GFX_BLEND_SCREEN = 14,
    MEGLER_GFX_BLEND_OVERLAY = 15, MEGLER_GFX_BLEND_DARKEN = 16, MEGLER_GFX_BLEND_LIGHTEN = 17
};
enum { MEGLER_GFX_PATH_MOVE = 0, MEGLER_GFX_PATH_LINE = 1, MEGLER_GFX_PATH_QUAD = 2, MEGLER_GFX_PATH_CUBIC = 3, MEGLER_GFX_PATH_CLOSE = 4 };
enum { MEGLER_GFX_WINDING = 0, MEGLER_GFX_EVEN_ODD = 1 };
enum { MEGLER_GFX_CLIP_DIFFERENCE = 0, MEGLER_GFX_CLIP_INTERSECT = 1 };
enum { MEGLER_GFX_TILE_CLAMP = 0, MEGLER_GFX_TILE_REPEAT = 1, MEGLER_GFX_TILE_MIRROR = 2 };
enum { MEGLER_GFX_SANS = 0, MEGLER_GFX_SERIF = 1, MEGLER_GFX_MONOSPACE = 2 };
enum { MEGLER_GFX_JPEG = 0, MEGLER_GFX_PNG = 1 };

typedef struct megler_gfx_paint {
    uint32_t color;                    /* 0xAARRGGBB, not premultiplied; its alpha is the paint's alpha */
    int style;                         /* MEGLER_GFX_FILL, _STROKE, _FILL_AND_STROKE */
    float stroke_width;                /* 0: a hairline */
    int flags;                         /* MEGLER_GFX_ANTI_ALIAS | MEGLER_GFX_FILTER_BITMAP | MEGLER_GFX_FAKE_BOLD */
    int cap, join;                     /* MEGLER_GFX_CAP_*, MEGLER_GFX_JOIN_* */
    float miter_limit;                 /* 4 unless the app says otherwise */
    float text_size;                   /* the em's height, in the drawing's coordinates */
    int text_align;                    /* MEGLER_GFX_ALIGN_*: what draw_text's x is */
    float text_scale_x;                /* 1: as the face is */
    float text_skew_x;                 /* 0: upright */
    float letter_spacing;              /* in ems, added to every advance */
    int blend;                         /* MEGLER_GFX_BLEND_*; never DST */
    int filter_mode;                   /* -1: no colour filter; else a MEGLER_GFX_BLEND_* number */
    uint32_t filter_color;             /* 0xAARRGGBB, not premultiplied */
    float shadow_radius;               /* 0: no shadow */
    float shadow_dx, shadow_dy;
    uint32_t shadow_color;             /* 0xAARRGGBB, not premultiplied */
} megler_gfx_paint;

/* Another bitmap's pixels as the source of a drawing call. */
typedef struct megler_gfx_image {
    const void *pixels;
    int width, height, stride;
    uint32_t id;                       /* this bitmap's, never 0, until bitmap_gone(id) */
    uint32_t version;                  /* changes whenever the pixels may have */
} megler_gfx_image;

typedef bool (MEGLER_CB *megler_gfx_write)(void *sink, const void *data, size_t size);

typedef struct megler_gfx_host {
    void *user;

    void *(MEGLER_CB *canvas_new)(void *user, void *pixels, int width, int height, int stride);
    void (MEGLER_CB *canvas_free)(void *user, void *canvas);
    void (MEGLER_CB *flush)(void *user, void *canvas);
    void (MEGLER_CB *touched)(void *user, void *canvas);

    void (MEGLER_CB *save)(void *user, void *canvas);
    void (MEGLER_CB *save_layer)(void *user, void *canvas, const float *bounds, int alpha, int blend);
    void (MEGLER_CB *save_layer_blur)(void *user, void *canvas, const float *bounds, int alpha, float radius_x, float radius_y, bool clamp);
    void (MEGLER_CB *restore)(void *user, void *canvas);
    void (MEGLER_CB *set_matrix)(void *user, void *canvas, const float *matrix);
    void (MEGLER_CB *clip_rect)(void *user, void *canvas, float left, float top, float right, float bottom, int op);
    void (MEGLER_CB *clip_path)(void *user, void *canvas, const float *path, int count, int fill, int op);

    void (MEGLER_CB *draw_color)(void *user, void *canvas, uint32_t color, int blend);
    void (MEGLER_CB *draw_rect)(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader);
    void (MEGLER_CB *draw_round_rect)(void *user, void *canvas, float left, float top, float right, float bottom, float rx, float ry,
                            const megler_gfx_paint *paint, void *shader);
    void (MEGLER_CB *draw_oval)(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader);
    void (MEGLER_CB *draw_path)(void *user, void *canvas, const float *path, int count, int fill, const megler_gfx_paint *paint, void *shader);
    void (MEGLER_CB *draw_bitmap)(void *user, void *canvas, const megler_gfx_image *image, const float *src, const float *dst, const megler_gfx_paint *paint);
    void (MEGLER_CB *draw_text)(void *user, void *canvas, const uint16_t *text, int count, float x, float y, const megler_gfx_paint *paint,
                      void *typeface, void *shader);

    float (MEGLER_CB *text_measure)(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface);
    float (MEGLER_CB *text_metrics)(void *user, const megler_gfx_paint *paint, void *typeface, float *out);
    void (MEGLER_CB *text_bounds)(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface, float *out);
    int (MEGLER_CB *text_break)(void *user, const uint16_t *text, int count, float max_width, const megler_gfx_paint *paint, void *typeface, bool words);
    void (MEGLER_CB *text_widths)(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface, float *out);

    void *(MEGLER_CB *typeface_create)(void *user, const char *family, int generic, int weight, bool italic);
    void *(MEGLER_CB *typeface_derive)(void *user, void *typeface, int weight, bool italic);
    void *(MEGLER_CB *typeface_from_data)(void *user, const void *data, size_t size);

    void *(MEGLER_CB *shader_linear)(void *user, float x0, float y0, float x1, float y1, const uint32_t *colors, const float *positions, int count, int tile);
    void *(MEGLER_CB *shader_radial)(void *user, float cx, float cy, float radius, const uint32_t *colors, const float *positions, int count, int tile);
    void *(MEGLER_CB *shader_sweep)(void *user, float cx, float cy, const uint32_t *colors, const float *positions, int count);
    void *(MEGLER_CB *shader_bitmap)(void *user, const megler_gfx_image *image, int tile);
    void (MEGLER_CB *shader_matrix)(void *user, void *shader, const float *matrix);
    void (MEGLER_CB *shader_free)(void *user, void *shader);

    bool (MEGLER_CB *image_size)(void *user, const void *data, size_t size, int *width, int *height);
    bool (MEGLER_CB *image_decode)(void *user, const void *data, size_t size, void *pixels, int width, int height, int stride);
    bool (MEGLER_CB *image_encode)(void *user, const void *pixels, int width, int height, int stride, int format, int quality, megler_gfx_write write, void *sink);
    void (MEGLER_CB *bitmap_gone)(void *user, uint32_t id);

    bool (MEGLER_CB *present)(void *user, void *canvas, int x, int y, int width, int height);
    bool (MEGLER_CB *gl_frame)(void *user, int layer, void *share_handle, int width, int height);
    void (MEGLER_CB *gl_place)(void *user, int layer, int x, int y, int width, int height);
    bool (MEGLER_CB *texture_frame)(void *user, int id, void *share_handle, int width, int height);
    void (MEGLER_CB *draw_texture)(void *user, void *canvas, int id, float left, float top, float right, float bottom, int alpha);
    /* Optional bitmap-to-canvas drawing without a CPU readback. The source is
       another canvas of the source bitmap, current and with no open layers.
       False leaves the destination unchanged; the core then settles the bitmap
       and uses draw_bitmap. The source remains readable through flush. */
    bool (MEGLER_CB *draw_canvas)(void *user, void *canvas, void *source, const float *src, const float *dst, const megler_gfx_paint *paint);
    /* Free a canvas whose pixels will never be read again. Like canvas_free,
       but its memory need not receive the last drawing. */
    void (MEGLER_CB *canvas_discard)(void *user, void *canvas);
} megler_gfx_host;

/*
 * The touch keyboard, for an app whose user interface is Java (any other app draws a keyboard of its own or has none).
 *
 * Such an app has text fields, and on Android the system's keyboard comes up for the one that is being edited. Here the
 * shell's keyboard is that keyboard. There are two directions, and they are separate.
 *
 * The app asks, through megler_keyboard_host in megler_run_config:
 *
 *   show   A text field wants the keyboard: the user touched it, or the app asked for it. `kind` is what is typed there
 *          (MEGLER_KEYBOARD_*), `flags` more about it (MEGLER_KEYBOARD_F_*), `enter` what the Enter key does there
 *          (MEGLER_ENTER_*), for the key's label and so that the shell knows whether Enter can mean a new line. `show` while
 *          the keyboard is already up means that another field is being edited now -- the three may differ, the keyboard
 *          stays up -- or the same field once more (the framework asks when a field takes the focus and again when it is
 *          touched): a repeated `show` must be harmless.
 *   hide   No field wants it any more: the field lost the focus, its screen went away, or the app asked.
 *
 *   Both are called on the app's main thread -- the thread megler_run made for the app, on which its views live -- from
 *   inside the framework, in the middle of whatever the app was doing. They must return at once: hand the request to the
 *   UI thread and do not wait for it. Neither is called after on_exit. Left NULL, the app runs and no keyboard comes.
 *
 * The shell tells, with the four functions below (after megler_touch). Any thread may call them; each returns at once. What
 * they say is queued and reaches the app on its main thread with the next turn of its message loop, at most 8 ms later,
 * in the order of the calls, also among each other and with megler_key. They do nothing for an app that is not Java.
 *
 *   megler_text_commit      Text the user typed, pasted or chose from the suggestions, UTF-8: put at the cursor of the field
 *                         being edited, in place of its selection. There is no composing text: a keyboard that composes
 *                         (as for Chinese or Japanese) sends each piece when it is final.
 *   megler_text_key         An editing key (MEGLER_TEXT_*). Backspace deletes the selection, or what is before the cursor (a
 *                         surrogate pair whole). Enter does what `enter` said: a new line, or the field's action (Done
 *                         closes the keyboard, Next moves on, Search and Go and Send are the app's). The others -- Delete,
 *                         the four arrows, Home, End -- act as those keys of a hardware keyboard do.
 *   megler_keyboard_closed  The user put the keyboard away, with its own hide key or with Back. The app is told, and does not
 *                         call `hide` for it. The next touch of a field calls `show` again.
 *   megler_keyboard_height  How much of the bottom of the surface the keyboard covers, in pixels; 0 when it is gone. The
 *                         framework keeps the field being edited above it. Call it when the keyboard has come up, when its
 *                         height changes, and when it has gone.
 *
 * While the keyboard is up the Back key is the keyboard's, as on Android: the shell hides the keyboard and calls
 * megler_keyboard_closed, and does not send megler_key(4).
 */
enum { MEGLER_KEYBOARD_TEXT = 0, MEGLER_KEYBOARD_NUMBER, MEGLER_KEYBOARD_PHONE, MEGLER_KEYBOARD_EMAIL, MEGLER_KEYBOARD_URL, MEGLER_KEYBOARD_PASSWORD,
       MEGLER_KEYBOARD_NUMBER_PASSWORD, MEGLER_KEYBOARD_DATETIME };
enum {
    MEGLER_KEYBOARD_F_MULTILINE = 1,         /* the field has lines: Enter may be a new line (see `enter`) */
    MEGLER_KEYBOARD_F_NO_SUGGESTIONS = 2,    /* no corrections, no suggestions */
    MEGLER_KEYBOARD_F_CAP_SENTENCES = 4,     /* capitals at the start of a sentence, of every word, everywhere */
    MEGLER_KEYBOARD_F_CAP_WORDS = 8,
    MEGLER_KEYBOARD_F_CAP_ALL = 16,
    MEGLER_KEYBOARD_F_SIGNED = 32,           /* a number that may have a sign, a decimal point */
    MEGLER_KEYBOARD_F_DECIMAL = 64,
};
enum { MEGLER_ENTER_DONE = 0, MEGLER_ENTER_NEWLINE, MEGLER_ENTER_GO, MEGLER_ENTER_SEARCH, MEGLER_ENTER_SEND, MEGLER_ENTER_NEXT, MEGLER_ENTER_PREVIOUS };
enum { MEGLER_TEXT_BACKSPACE = 1, MEGLER_TEXT_ENTER, MEGLER_TEXT_DELETE, MEGLER_TEXT_LEFT, MEGLER_TEXT_RIGHT, MEGLER_TEXT_UP, MEGLER_TEXT_DOWN, MEGLER_TEXT_HOME, MEGLER_TEXT_END };

typedef struct megler_keyboard_host {
    void *user;
    void (MEGLER_CB *show)(void *user, int kind, int flags, int enter);
    void (MEGLER_CB *hide)(void *user);
} megler_keyboard_host;

/*
 * The phone's bars, for an app whose user interface is Java: Android's status bar and navigation bar are the phone's own.
 * The surface stays the whole screen and the bars lie over it, as on Android, where they are a window's insets: the app
 * keeps its content off them, or draws under them when it lays itself out so.
 *
 * The app asks, through megler_system_bars_host in megler_run_config:
 *   wanted(bars)  which bars its window wants hidden -- MEGLER_BAR_STATUS (Window's FLAG_FULLSCREEN, View's
 *                 SYSTEM_UI_FLAG_FULLSCREEN, WindowInsetsController.hide(statusBars)) and MEGLER_BAR_NAVIGATION
 *                 (SYSTEM_UI_FLAG_HIDE_NAVIGATION, hide(navigationBars)) -- and MEGLER_BAR_LIGHT_STATUS when its status bar is
 *                 light, so that the bar's icons are to be dark. Called on the app's main thread, at its first frame and
 *                 whenever that changes; the shell does what it asks on its UI thread and does not wait for it. Left NULL,
 *                 the app runs and the bars are as the shell keeps them.
 * The shell tells:
 *   megler_set_insets(l, t, r, b)  how many pixels of each edge of the surface the bars cover now, 0 for a bar that is
 *                 hidden. Call it when the bars come and go (the user's swipe, wanted above) and when the surface turns.
 *                 Until it is called, nothing is covered.
 */
enum { MEGLER_BAR_STATUS = 1, MEGLER_BAR_NAVIGATION = 2, MEGLER_BAR_LIGHT_STATUS = 4 };

typedef struct megler_system_bars_host {
    void *user;
    void (MEGLER_CB *wanted)(void *user, int bars);
} megler_system_bars_host;

/*
 * The app's screen -- its size, its density and which way up it is -- for every app, as Android has them in its
 * configuration (Configuration.orientation, screenWidthDp, screenHeightDp, smallestScreenWidthDp, screenLayout,
 * densityDpi), in DisplayMetrics, WindowMetrics and Display.getRotation, and as a game's AConfiguration and ANativeWindow.
 *
 * The app asks, through megler_display_host in megler_run_config:
 *   orientation(requested)  how the activity in front wants the screen held, as Android's
 *                 ActivityInfo.SCREEN_ORIENTATION_* says it (MEGLER_ORIENTATION_*): from its manifest's
 *                 android:screenOrientation as it comes to the front and from Activity.setRequestedOrientation -- a Java
 *                 app's, or a game's through JNI (Unity's). "behind" is never said: what the activity under it wants is
 *                 said instead. Called when it changes, on one of the app's threads, and must return at once; the shell
 *                 turns the screen on its own thread (Windows' DisplayInformation.AutoRotationPreferences) or, where the
 *                 window does not turn (a PC's), gives the app a surface of the shape it asked for, letterboxed, as
 *                 Android does on a large screen. Left NULL, the app runs and the screen is as the shell keeps it.
 * The shell tells:
 *   megler_set_surface(width, height, density_dpi, rotation)  the app's surface is now `width` x `height` pixels at
 *                 `density_dpi`, drawn `rotation` quarter turns from the screen's natural way up (Android's
 *                 Surface.ROTATION_0 to _270: 1 is a phone upright by nature turned a quarter anticlockwise, Windows'
 *                 DisplayOrientations.Landscape on it): the window was resized, or the phone turned. Any thread; it
 *                 returns at once. A Java app's windows are laid out again at the new size, and its activities have
 *                 Android's configuration change: one whose android:configChanges takes all that changed hears
 *                 onConfigurationChanged, the others are made again with the state they saved, as ActivityThread
 *                 relaunches them (one that is not on the screen, when it comes back to it); a change of size that
 *                 crosses none of the sizes the app's resources are made for (w600dp, sw720dp...) relaunches nothing,
 *                 as on Android. A game's window is made again at a new size, and the game hears onConfigurationChanged.
 *                 A screen only turned upside down changes no configuration: the display's rotation says it. The
 *                 pictures after it are of the new size; one already on its way may still be of the old. Call it when a
 *                 resize has settled, not at every step of a drag: each call is a configuration change, which may
 *                 relaunch the activity. Until it is called the surface is megler_run_config's.
 */
enum {
    MEGLER_ORIENTATION_UNSPECIFIED = -1, MEGLER_ORIENTATION_LANDSCAPE = 0, MEGLER_ORIENTATION_PORTRAIT = 1, MEGLER_ORIENTATION_USER = 2,
    MEGLER_ORIENTATION_BEHIND = 3, MEGLER_ORIENTATION_SENSOR = 4, MEGLER_ORIENTATION_NOSENSOR = 5, MEGLER_ORIENTATION_SENSOR_LANDSCAPE = 6,
    MEGLER_ORIENTATION_SENSOR_PORTRAIT = 7, MEGLER_ORIENTATION_REVERSE_LANDSCAPE = 8, MEGLER_ORIENTATION_REVERSE_PORTRAIT = 9,
    MEGLER_ORIENTATION_FULL_SENSOR = 10, MEGLER_ORIENTATION_USER_LANDSCAPE = 11, MEGLER_ORIENTATION_USER_PORTRAIT = 12,
    MEGLER_ORIENTATION_FULL_USER = 13, MEGLER_ORIENTATION_LOCKED = 14
};

typedef struct megler_display_host {
    void *user;
    void (MEGLER_CB *orientation)(void *user, int requested);
} megler_display_host;

/*
 * What a Java app asks of the phone beyond its own window. Each call comes on the app's thread and must return at once; the
 * shell does it on its own thread. Any call left NULL: the app runs, and the log says what it asked.
 *   open_url     A link to open, UTF-8: what the app starts with Intent.ACTION_VIEW (or SENDTO, DIAL) when none of its own
 *                activities takes it -- an http(s) page, mailto:, tel:, a Play Store page (market:// made https://).
 *   share_text   Text the app shares (ACTION_SEND with EXTRA_TEXT), and its subject or NULL.
 *   install_apk  An APK the app wants installed, a host path the shell can read (in the app's data folder or the shared
 *                storage): what it opens with ACTION_VIEW or ACTION_INSTALL_PACKAGE (a file manager, a browser's download,
 *                an app store's), or the files of a PackageInstaller session it commits, then a folder with the APKs of
 *                one app (its base and its splits). `name` is the file's name, for showing. True: the shell took it, and
 *                the app is told its install goes on; false: the app is told it cannot be installed.
 *   launch_app   Another app the phone has, to start: an Android package and the activity's class name, or NULL for its
 *                launcher activity (a launcher's tap, an app opening another). True: the shell starts it.
 *   uninstall_app  Another app the phone has, to uninstall: an Android package (an app store's Uninstall,
 *                PackageInstaller.uninstall; ACTION_DELETE of package:<name>). True: the shell asks the user. It is gone
 *                when apps_dir's apps.txt no longer has it; installs/<package>.uninstall.txt says when the user said no.
 *   permission   Whether the app may have what one of Android's dangerous permission groups guards: `group` is the group's
 *                name in small letters ("location", "camera", "microphone", "storage", "read_media_visual",
 *                "read_media_aural", "sms", "phone", "call_log", "contacts", "calendar", "sensors", "nearby_devices",
 *                "notifications", "activity_recognition"). MEGLER_PERMISSION_GRANTED; _DENIED, the user said no and may be
 *                asked again; _DENIED_FIXED, no, and not to be asked again (Windows' own setting, or a second no); or
 *                _UNDECIDED, never asked. With `ask` false the answer comes at once and nothing is asked; that comes on
 *                any of the app's threads, its main thread too. With `ask` true the user is asked first if they have not
 *                decided or have said no once -- Windows' own question for the camera, the microphone and the location,
 *                the shell's for the rest -- and the call waits for the answer (what it was before, when the question was
 *                put away unanswered or could not be put); it comes on a thread of the app's that may wait, never its main
 *                thread. The shell keeps
 *                what the user said, for the app, from run to run; it may grant a group it has nothing behind at once.
 *                The framework (megler.Permissions) asks it of a group whose permissions the app's manifest asks for and
 *                whose answer Android's rules do not settle by themselves. Left NULL: every group is granted, nothing asked.
 */
enum { MEGLER_PERMISSION_UNDECIDED = 0, MEGLER_PERMISSION_GRANTED = 1, MEGLER_PERMISSION_DENIED = 2, MEGLER_PERMISSION_DENIED_FIXED = 3 };

typedef struct megler_system_host {
    void *user;
    void (MEGLER_CB *open_url)(void *user, const char *uri);
    void (MEGLER_CB *share_text)(void *user, const char *text, const char *subject);
    bool (MEGLER_CB *install_apk)(void *user, const char *host_path, const char *name);
    bool (MEGLER_CB *launch_app)(void *user, const char *package, const char *activity);
    bool (MEGLER_CB *uninstall_app)(void *user, const char *package);
    int (MEGLER_CB *permission)(void *user, const char *group, bool ask);
} megler_system_host;

/*
 * A Java app's media session (android.media.session.MediaSession, which androidx's MediaSessionCompat stands on): what
 * the app says it plays and how, for the phone's own media controls (Windows' system media transport controls: the lock
 * screen's, the volume flyout's). Only the session that was made active last is told. Both calls come on the thread the
 * app calls from and must return at once. Left NULL, the app runs and nobody is told.
 *   state     Android's PlaybackState: its state (STATE_*: 0 none, 1 stopped, 2 paused, 3 playing, 6 buffering...) and
 *             the ACTION_* bits it allows (4 play, 2 pause, 32 next, 16 previous...). State 0 also when the session is
 *             released or made inactive.
 *   metadata  What plays: its title, artist and album (UTF-8, or NULL), and its picture as 0xAARRGGBB pixels, art_w x
 *             art_h of them row by row (at most 256 on a side), or NULL.
 * The other way, the controls' buttons, come as Android's media keys through megler_key (KEYCODE_MEDIA_PLAY 126,
 * KEYCODE_MEDIA_PAUSE 127, KEYCODE_MEDIA_NEXT 87...), which the framework hands to that session as Android does.
 */
typedef struct megler_media_session_host {
    void *user;
    void (MEGLER_CB *state)(void *user, int state, long long actions);
    void (MEGLER_CB *metadata)(void *user, const char *title, const char *artist, const char *album, const uint32_t *art, int art_w,
                     int art_h);
} megler_media_session_host;

/*
 * The phone's location, for a Java app's android.location.LocationManager.
 *
 * A fix is where the phone was and when: degrees on WGS 84, metres, metres a second, degrees clockwise from north, and the time
 * in milliseconds since 1970 (UTC). `has` says which of the optional ones are there. `source` is what found it: the satellites
 * (Android's "gps" provider), Wi-Fi or the cell network ("network"), or a place set by hand for testing, which the app is told
 * is the satellites'.
 *
 * The host is asked:
 *   access(ask)  whether the app may be told where the phone is: MEGLER_LOCATION_ON; OFF when the phone's location is switched
 *                off in its settings; DENIED when the user has not let Megler have it; NONE when the device has no way to find
 *                it; UNKNOWN while the user has not been asked. With `ask`, the user is asked first if they have not been
 *                (Windows' own question, which may take as long as the user does: the caller is the app's own thread, which may
 *                wait); without it, nothing is asked.
 *   start        to find the phone every `interval_ms` or so, until stop -- each fix kept for last;
 *   last         the newest fix (false while there is none). It may be from before start, and stay after stop.
 * Each may be called from any of the app's threads.
 */
enum { MEGLER_LOCATION_NONE = 0, MEGLER_LOCATION_ON = 1, MEGLER_LOCATION_OFF = 2, MEGLER_LOCATION_DENIED = 3, MEGLER_LOCATION_UNKNOWN = 4 };
enum { MEGLER_LOCATION_HAS_ALTITUDE = 1, MEGLER_LOCATION_HAS_ALTITUDE_ACCURACY = 2, MEGLER_LOCATION_HAS_SPEED = 4, MEGLER_LOCATION_HAS_BEARING = 8 };
enum { MEGLER_LOCATION_SATELLITES = 0, MEGLER_LOCATION_NETWORK = 1, MEGLER_LOCATION_SET_BY_HAND = 2 };

typedef struct megler_location {
    double latitude, longitude, altitude;
    float accuracy_m, altitude_accuracy_m, speed_mps, bearing_deg;
    long long time_ms;
    unsigned has;
    int source;
} megler_location;

typedef struct megler_location_host {
    void *user;
    int (MEGLER_CB *access)(void *user, bool ask);
    bool (MEGLER_CB *start)(void *user, int interval_ms);
    void (MEGLER_CB *stop)(void *user);
    bool (MEGLER_CB *last)(void *user, megler_location *fix);
} megler_location_host;

/*
 * The phone's microphone, for a Java app's AudioRecord. While an app records:
 *   start  to hear the microphone as 16-bit PCM at `rate` Hz in `channels` (1 or 2) -- Windows asks the user the first time.
 *          True when it does; false (no microphone, or the user said no): the app reads silence, at the microphone's pace.
 *   stop   to stop.
 * While it runs, the shell hands what it hears to megler_microphone_push, from any thread. Called on the app's threads.
 * May be left zero: no microphone.
 */
typedef struct megler_microphone_host {
    void *user;
    bool (MEGLER_CB *start)(void *user, int rate, int channels);
    void (MEGLER_CB *stop)(void *user);
} megler_microphone_host;
/* What the microphone heard: `frames` frames of the channels start was asked for, interleaved. */
MEGLER_API void megler_microphone_push(const int16_t *samples, int frames);

/*
 * The phone's check of its user, for a Java app's BiometricPrompt, BiometricManager and FingerprintManager (Windows Hello:
 * a fingerprint, the iris, a face or the PIN). Called on the app's threads, never its main thread; each may block:
 *   available  whether the phone can check its user: MEGLER_BIOMETRIC_READY, NOT_SET_UP (it could, but the user has set
 *              nothing up) or NONE (nothing on the phone checks).
 *   verify     the phone's own dialog, with the message (UTF-8: what the app's prompt says), until the user is done:
 *              MEGLER_BIOMETRIC_VERIFIED, CANCELED (the user said no), LOCKED (too many tries) or UNAVAILABLE (not set up,
 *              busy, refused by policy).
 *   cancel     the dialog of a verify that runs, closed: the app no longer wants it (its CancellationSignal); that verify
 *              returns CANCELED.
 * May be left zero: the app is told the phone has nothing that checks its user.
 */
enum { MEGLER_BIOMETRIC_READY = 0, MEGLER_BIOMETRIC_NOT_SET_UP = 1, MEGLER_BIOMETRIC_NONE = 2 };
enum { MEGLER_BIOMETRIC_VERIFIED = 0, MEGLER_BIOMETRIC_CANCELED = 1, MEGLER_BIOMETRIC_LOCKED = 2, MEGLER_BIOMETRIC_UNAVAILABLE = 3 };
typedef struct megler_biometric_host {
    void *user;
    int (MEGLER_CB *available)(void *user);
    int (MEGLER_CB *verify)(void *user, const char *message);
    void (MEGLER_CB *cancel)(void *user);
} megler_biometric_host;

/*
 * The phone's sensors (Windows.Devices.Sensors), for an app's android.hardware.SensorManager and the NDK's ASensorManager.
 * The shell says what the device has and hands over what each sensor reads as Windows reads it; the core makes Android's
 * sensors of them -- Android's types, units, axes and times (core/src/ndk/mg_sensors.c) -- and keeps them stopped while the
 * app is paused, as Android sends no events to an app in the background.
 *
 * A sensor is one of Windows', MEGLER_SENSOR_*, its values as its reading has them:
 *   ACCELEROMETER     Accelerometer, AccelerometerReadingType Standard: AccelerationX, Y, Z in g, gravity in them, which
 *                     points the way Windows has it: -1 on Z lying face up, -1 on Y standing upright.
 *   GRAVITY           Accelerometer of AccelerometerReadingType Gravity: the gravity alone, the same way.
 *   LINEAR_ACCELERATION  Accelerometer of AccelerometerReadingType Linear: without the gravity, the same way.
 *   GYROMETER         Gyrometer: AngularVelocityX, Y, Z in degrees a second, positive counterclockwise seen from the axis's end.
 *   MAGNETOMETER      Magnetometer: MagneticFieldX, Y, Z in microteslas.
 *   ORIENTATION       OrientationSensor (absolute: north by the magnetometer): its Quaternion's X, Y, Z, W.
 *   RELATIVE_ORIENTATION  OrientationSensor for relative readings (no magnetometer, its heading arbitrary): the same.
 *   INCLINOMETER      Inclinometer: PitchDegrees, RollDegrees, YawDegrees.
 *   LIGHT             LightSensor: IlluminanceInLux.
 *   PROXIMITY         ProximitySensor: 1 when something is near (IsDetected) and 0 when not, then the distance to it in
 *                     millimetres (DistanceInMillimeters), or -1 when the sensor does not say one.
 *   BAROMETER         Barometer: StationPressureInHectopascals.
 * The axes are the device's own, as its native orientation has them (x to the right, y up, z out of the screen), never
 * turned with the display. `accuracy` is MEGLER_SENSOR_ACCURACY_*, Windows' MagnetometerAccuracy, where the reading has
 * one (the magnetometer's DirectionalAccuracy, the orientation's and the inclinometer's YawAccuracy); UNKNOWN elsewhere.
 * `time_ns` is when it was read, the reading's Timestamp, in nanoseconds since 1970 (UTC); 0 for now.
 *
 * The host is asked, on any of the app's threads, and must not wait on the app:
 *   list(info, max)   what the device has: up to max sensors described into info, their count returned. Asked once, when
 *                     the app first wants to know; it may wait until the shell has looked.
 *   start(kind, interval_ms)  that sensor reported every interval_ms, or as often as it can when that is shorter than it
 *                     can (0: as often as it can), until stop. Asked again with another interval when the app wants
 *                     another. Returns at once: what it reads comes through megler_sensor_push, from any thread, the first
 *                     reading as soon as there is one (a sensor that reports only changes: the reading it has now).
 *   stop(kind)
 * May be left zero: the app is told the device has no sensors.
 */
enum {
    MEGLER_SENSOR_ACCELEROMETER = 1, MEGLER_SENSOR_GRAVITY = 2, MEGLER_SENSOR_LINEAR_ACCELERATION = 3, MEGLER_SENSOR_GYROMETER = 4,
    MEGLER_SENSOR_MAGNETOMETER = 5, MEGLER_SENSOR_ORIENTATION = 6, MEGLER_SENSOR_RELATIVE_ORIENTATION = 7, MEGLER_SENSOR_INCLINOMETER = 8,
    MEGLER_SENSOR_LIGHT = 9, MEGLER_SENSOR_PROXIMITY = 10, MEGLER_SENSOR_BAROMETER = 11
};
enum { MEGLER_SENSOR_ACCURACY_UNKNOWN = 0, MEGLER_SENSOR_ACCURACY_UNRELIABLE = 1, MEGLER_SENSOR_ACCURACY_APPROXIMATE = 2, MEGLER_SENSOR_ACCURACY_HIGH = 3 };

typedef struct megler_sensor_info {
    int kind;                /* MEGLER_SENSOR_* */
    char name[64];           /* UTF-8: its device's name as Windows has it */
    char vendor[64];         /* UTF-8: who made it, where Windows says; else empty */
    int min_interval_ms;     /* the shortest interval it reports at (MinimumReportInterval); 0 for one that reports changes only */
    double max_range;        /* the most it reads, in its reading's unit (the proximity's MaxDistanceInMillimeters); 0: not said */
    double resolution;       /* the least step it reads, likewise; 0: not said */
} megler_sensor_info;

typedef struct megler_sensor_sample {
    int kind;
    int accuracy;
    double values[4];
    long long time_ns;
} megler_sensor_sample;

typedef struct megler_sensor_host {
    void *user;
    int (MEGLER_CB *list)(void *user, megler_sensor_info *info, int max);
    void (MEGLER_CB *start)(void *user, int kind, int interval_ms);
    void (MEGLER_CB *stop)(void *user, int kind);
} megler_sensor_host;
/* What a sensor read, from any thread. Nothing happens when no app runs or none wants that sensor. */
MEGLER_API void megler_sensor_push(const megler_sensor_sample *sample);

/*
 * Bluetooth Low Energy, the central's side, for an app's android.bluetooth: BluetoothAdapter, BluetoothLeScanner,
 * BluetoothDevice.connectGatt and BluetoothGatt, their callbacks and broadcasts (core/src/run/mg_bluetooth.c; the
 * framework's android.bluetooth over megler.Bluetooth). The shell has the radio (Windows.Devices.Bluetooth); the core
 * keeps Android's side: the devices, the GATT objects, the callbacks on their threads, the status codes.
 *
 * Every call comes on one of the app's threads -- its main thread among them -- and returns at once; what it asked for
 * comes back through megler_bluetooth_push, from any thread, as an event (MEGLER_BT_EVENT_*):
 *   state(name, size)  what the radio is: MEGLER_BT_RADIO_NONE (none, or none that can be an LE central), OFF or ON, and its
 *                  name (the phone's, as other devices see it) into `name`. Asked when the app first wants Bluetooth; it may
 *                  wait until the shell has looked. A change comes as STATE.
 *   scan(mode)     MEGLER_BT_SCAN_ACTIVE (asking for scan responses), PASSIVE, or OFF: each advertisement heard as an
 *                  ADVERTISEMENT, until scan(OFF); a scan that cannot start, or stops by itself, as SCAN_FAILED.
 *   connect(conn, address, type, auto)  a GATT connection to the device, numbered `conn` by the core: CONNECTION with value
 *                  1 when it is made, or 0 with a status when it is not (Android's: 133 when it could not be made within
 *                  30 s). With `auto`, it is made whenever the device is in range and made again after it drops, until
 *                  disconnect; without, once. A connection that drops comes as CONNECTION 0 with Android's reason (8, the
 *                  link's supervision timeout; 19, ended by the device).
 *   disconnect(conn)  the connection ended from this side and no longer made again: CONNECTION 0 with status 0. Nothing
 *                  else comes for that conn after it.
 *   discover(conn) the device's services: SERVICES, the attributes in order -- a service, then its included services and its
 *                  characteristics, each characteristic followed by its descriptors -- each with its handle.
 *   read(conn, handle)  a characteristic's value or a descriptor's: READ with the bytes.
 *   write(conn, handle, data, size, type)  MEGLER_BT_WRITE_REQUEST (with the device's response), COMMAND (without) or SIGNED
 *                  (the bytes are the call's: the shell copies them before it returns): WRITTEN once the device answered
 *                  or, without a response, once it is sent. A descriptor is written with a
 *                  request. A characteristic's Client Characteristic Configuration descriptor (0x2902) is the subscription
 *                  to it: bit 0 of its value notifications, bit 1 indications, neither none; from then on its notifications
 *                  and indications come as CHANGED.
 *   mtu(conn, mtu) the ATT MTU the app asks for: MTU with what is in force, at most what was asked (Windows negotiates its
 *                  own as it connects).
 *   rssi(conn)     the connection's signal strength: RSSI. May be NULL: the core answers with the last advertisement heard.
 *   bond(address, type, create)  pairing (an app's createBond), or the pairing removed: BOND events, BONDING then BONDED or
 *                  NONE (with Android's UNBOND_REASON_* as the status), and PAIRING on the way when the device asks for a
 *                  PIN, a confirmation or a consent.
 *   pairing_reply(address, answer, pin)  the answer to a PAIRING: MEGLER_BT_PAIR_REFUSE; ACCEPT, with the PIN (UTF-8 digits,
 *                  copied) where one was asked for; or ASK_USER: the app said nothing, and the shell asks the user, as
 *                  Android's own pairing dialog would.
 *   settings()     the phone's Bluetooth settings shown, for the user to turn the radio on: an app's request to enable it.
 * Statuses are Android's: 0 success; a GATT operation's ATT error as BluetoothGatt's GATT_* (5 insufficient authentication,
 * 15 insufficient encryption ...); 133 GATT_ERROR when the device could not be reached; 257 GATT_FAILURE otherwise.
 * May be left zero: the app is told the device has no Bluetooth.
 */
enum { MEGLER_BT_RADIO_NONE = 0, MEGLER_BT_RADIO_OFF = 1, MEGLER_BT_RADIO_ON = 2 };
enum { MEGLER_BT_SCAN_OFF = 0, MEGLER_BT_SCAN_PASSIVE = 1, MEGLER_BT_SCAN_ACTIVE = 2 };
/* An address's kind, as Windows' BluetoothAddressType and Android's BluetoothDevice.ADDRESS_TYPE_* number them. */
enum { MEGLER_BT_ADDRESS_PUBLIC = 0, MEGLER_BT_ADDRESS_RANDOM = 1, MEGLER_BT_ADDRESS_UNKNOWN = 2 };
enum { MEGLER_BT_WRITE_COMMAND = 0, MEGLER_BT_WRITE_REQUEST = 1, MEGLER_BT_WRITE_SIGNED = 2 };
/* Android's BOND_* and PAIRING_VARIANT_*, as they are. */
enum { MEGLER_BT_BOND_NONE = 10, MEGLER_BT_BOND_BONDING = 11, MEGLER_BT_BOND_BONDED = 12 };
enum {
    MEGLER_BT_PAIRING_PIN = 0, MEGLER_BT_PAIRING_PASSKEY = 1, MEGLER_BT_PAIRING_CONFIRMATION = 2, MEGLER_BT_PAIRING_CONSENT = 3,
    MEGLER_BT_PAIRING_DISPLAY_PASSKEY = 4, MEGLER_BT_PAIRING_DISPLAY_PIN = 5
};
enum { MEGLER_BT_PAIR_REFUSE = 0, MEGLER_BT_PAIR_ACCEPT = 1, MEGLER_BT_PAIR_ASK_USER = 2 };
enum {
    MEGLER_BT_ATTR_SERVICE = 1,        /* a primary service; its handle is its first */
    MEGLER_BT_ATTR_SECONDARY = 2,      /* a secondary service */
    MEGLER_BT_ATTR_INCLUDED = 3,       /* a service the one before includes: its handle and UUID are that service's */
    MEGLER_BT_ATTR_CHARACTERISTIC = 4, /* its handle is its value's */
    MEGLER_BT_ATTR_DESCRIPTOR = 5
};
/*
 * The events, with what each uses of megler_bluetooth_event:
 *   STATE          value: MEGLER_BT_RADIO_*; name: the radio's name, or NULL for the same as before
 *   ADVERTISEMENT  address, address_type, value: the RSSI in dBm, extra: 1 when the device takes connections; data: the
 *                  advertisement's AD structures as they were sent (length, type, data) with the scan response's after them;
 *                  time_ns: when it was heard (Windows' Timestamp, nanoseconds since 1970 UTC; 0 for now)
 *   SCAN_FAILED    status: Android's ScanCallback.SCAN_FAILED_*
 *   CONNECTION     conn, value: 1 connected, 0 not; status
 *   SERVICES       conn, status, attributes and count
 *   READ           conn, handle, status, data (none on a failure)
 *   WRITTEN        conn, handle, status
 *   CHANGED        conn, handle, data: a notification or an indication
 *   MTU            conn, value: the MTU, status
 *   RSSI           conn, value: dBm, status
 *   BOND           address, address_type, value: MEGLER_BT_BOND_*, status: Android's UNBOND_REASON_* when it failed or was
 *                  removed (0 otherwise); name where the shell knows it
 *   PAIRING        address, address_type, value: MEGLER_BT_PAIRING_*, extra: the passkey to show or to confirm (else 0);
 *                  answered with pairing_reply
 *   PAIRED         address, address_type, name: a device the phone was paired with before; none of the app's broadcasts.
 *                  The shell says each as it starts.
 */
enum {
    MEGLER_BT_EVENT_STATE = 1, MEGLER_BT_EVENT_ADVERTISEMENT = 2, MEGLER_BT_EVENT_SCAN_FAILED = 3, MEGLER_BT_EVENT_CONNECTION = 4,
    MEGLER_BT_EVENT_SERVICES = 5, MEGLER_BT_EVENT_READ = 6, MEGLER_BT_EVENT_WRITTEN = 7, MEGLER_BT_EVENT_CHANGED = 8,
    MEGLER_BT_EVENT_MTU = 9, MEGLER_BT_EVENT_RSSI = 10, MEGLER_BT_EVENT_BOND = 11, MEGLER_BT_EVENT_PAIRING = 12,
    MEGLER_BT_EVENT_PAIRED = 13
};

typedef struct megler_bluetooth_attribute {
    int kind;               /* MEGLER_BT_ATTR_* */
    int handle;
    unsigned char uuid[16]; /* as it is written: 0000180f-0000-1000-8000-00805f9b34fb is 00 00 18 0f 00 00 10 00 80 00 ... fb */
    int properties;         /* a characteristic's: the specification's bits, which are Android's PROPERTY_* (2 read, 8 write ...) */
} megler_bluetooth_attribute;

typedef struct megler_bluetooth_event {
    int kind;                       /* MEGLER_BT_EVENT_* */
    int conn;
    unsigned long long address;     /* 48 bits: AA:BB:CC:DD:EE:FF is 0xAABBCCDDEEFF */
    int address_type;               /* MEGLER_BT_ADDRESS_* */
    int status;
    int handle;
    int value, extra;
    const unsigned char *data;
    int size;
    const char *name;               /* UTF-8 */
    const megler_bluetooth_attribute *attributes;
    int count;
    long long time_ns;
} megler_bluetooth_event;

typedef struct megler_bluetooth_host {
    void *user;
    int (MEGLER_CB *state)(void *user, char *name, int size);
    void (MEGLER_CB *scan)(void *user, int mode);
    void (MEGLER_CB *connect)(void *user, int conn, unsigned long long address, int address_type, bool auto_connect);
    void (MEGLER_CB *disconnect)(void *user, int conn);
    void (MEGLER_CB *discover)(void *user, int conn);
    void (MEGLER_CB *read)(void *user, int conn, int handle);
    void (MEGLER_CB *write)(void *user, int conn, int handle, const unsigned char *data, int size, int type);
    void (MEGLER_CB *mtu)(void *user, int conn, int mtu);
    void (MEGLER_CB *rssi)(void *user, int conn);
    void (MEGLER_CB *bond)(void *user, unsigned long long address, int address_type, bool create);
    void (MEGLER_CB *pairing_reply)(void *user, unsigned long long address, int answer, const char *pin);
    void (MEGLER_CB *settings)(void *user);
} megler_bluetooth_host;
/* What the radio did, from any thread; everything it points to is copied before it returns. Nothing happens when no app runs. */
MEGLER_API void megler_bluetooth_push(const megler_bluetooth_event *event);

/*
 * The phone's Wi-Fi, for an app's android.net.wifi.WifiManager: the networks around the phone as a scan hears them, and the
 * one it is connected to (core/src/drivers/mg_java_wifi.c; the framework's megler.Wifi). Google Play services in the app
 * finds where the phone is from them, as on Android (Google's network location: docs/JAVA.md, "Wi-Fi").
 *
 * A network is an access point as a scan heard it: its BSSID (48 bits: AA:BB:CC:DD:EE:FF is 0xAABBCCDDEEFF), its SSID's
 * octets as the access point sends them (UTF-8 mostly; none for a hidden network), its signal in dBm, the centre of its
 * channel in MHz, its security (MEGLER_WIFI_SECURITY_*: the authentication, and the cipher where Windows says one), its
 * physical layer (MEGLER_WIFI_PHY_*, Windows' WiFiPhyKind), and its kind (an access point's or an ad hoc network's).
 *
 * The host is asked, on one thread of the framework's, which may wait:
 *   state(ask)   whether the phone can scan: MEGLER_WIFI_ON; OFF when its Wi-Fi is switched off; DENIED when Windows does not
 *                let Megler have the Wi-Fi (the package declares no wiFiControl, or the user or a policy said no); NONE when
 *                it has no Wi-Fi adapter; UNKNOWN while Windows has not been asked. With `ask`, Windows is asked first if it
 *                has not been (WiFiAdapter.RequestAccessAsync: nothing is shown to the user for it).
 *   scan(out, max, time_ns)  a scan: the networks heard into out (at most max), their number returned; -1 when no scan could
 *                be made (the state is not ON, or Windows' scan failed). *time_ns: when Windows' report was made,
 *                nanoseconds since 1970 (UTC). It waits for Windows' scan, a few seconds at most; the core asks no more often
 *                than every ten seconds.
 *   connection(out)  the network the phone is connected to by Wi-Fi: true, with as much of it as Windows says (its SSID; its
 *                BSSID, signal, channel and security where the last scan heard an access point of that SSID, the strongest
 *                such); false when the phone is not on Wi-Fi. May be NULL.
 * May be left zero: the app is told the phone has no Wi-Fi to scan.
 */
enum { MEGLER_WIFI_NONE = 0, MEGLER_WIFI_ON = 1, MEGLER_WIFI_OFF = 2, MEGLER_WIFI_DENIED = 3, MEGLER_WIFI_UNKNOWN = 4 };
/* The authentication (the low byte) and the cipher (bits 8 to 15), as Windows' NetworkAuthenticationType and NetworkEncryptionType have them. */
enum {
    MEGLER_WIFI_AUTH_UNKNOWN = 0, MEGLER_WIFI_AUTH_OPEN = 1, MEGLER_WIFI_AUTH_SHARED_KEY = 2, MEGLER_WIFI_AUTH_WPA = 3, MEGLER_WIFI_AUTH_WPA_PSK = 4,
    MEGLER_WIFI_AUTH_RSNA = 5, MEGLER_WIFI_AUTH_RSNA_PSK = 6, MEGLER_WIFI_AUTH_WPA3 = 7, MEGLER_WIFI_AUTH_WPA3_SAE = 8, MEGLER_WIFI_AUTH_OWE = 9,
    MEGLER_WIFI_AUTH_OTHER = 10
};
enum {
    MEGLER_WIFI_CIPHER_UNKNOWN = 0, MEGLER_WIFI_CIPHER_NONE = 1, MEGLER_WIFI_CIPHER_WEP = 2, MEGLER_WIFI_CIPHER_TKIP = 3, MEGLER_WIFI_CIPHER_CCMP = 4,
    MEGLER_WIFI_CIPHER_GCMP = 5
};
#define MEGLER_WIFI_SECURITY(auth, cipher) ((auth) | (cipher) << 8)
/* Windows' WiFiPhyKind, as it numbers them: 802.11b is HRDSSS, 802.11a OFDM, 802.11g ERP, 802.11n HT, 802.11ac VHT, 802.11ad DMG, 802.11ax HE. */
enum {
    MEGLER_WIFI_PHY_UNKNOWN = 0, MEGLER_WIFI_PHY_FHSS = 1, MEGLER_WIFI_PHY_DSSS = 2, MEGLER_WIFI_PHY_IR = 3, MEGLER_WIFI_PHY_OFDM = 4,
    MEGLER_WIFI_PHY_HRDSSS = 5, MEGLER_WIFI_PHY_ERP = 6, MEGLER_WIFI_PHY_HT = 7, MEGLER_WIFI_PHY_VHT = 8, MEGLER_WIFI_PHY_DMG = 9,
    MEGLER_WIFI_PHY_HE = 10
};

typedef struct megler_wifi_network {
    unsigned long long bssid;
    unsigned char ssid[32];
    int ssid_size;          /* 0..32; 0 for a hidden network */
    int rssi_dbm;
    int frequency_mhz;
    int security;           /* MEGLER_WIFI_SECURITY(auth, cipher) */
    int phy;                /* MEGLER_WIFI_PHY_* */
    int adhoc;              /* 1 for an ad hoc network (IBSS), 0 for an access point's */
} megler_wifi_network;

typedef struct megler_wifi_host {
    void *user;
    int (MEGLER_CB *state)(void *user, bool ask);
    int (MEGLER_CB *scan)(void *user, megler_wifi_network *out, int max, long long *time_ns);
    bool (MEGLER_CB *connection)(void *user, megler_wifi_network *out);
} megler_wifi_host;

typedef struct megler_run_config {
    const char *apk_path;
    /* Optional installed microG APK. Opened as the service package; no copy in each client. */
    const char *gms_apk_path;
    /* Optional Google Services Framework APK, shared by all client packages. */
    const char *gsf_apk_path;
    /*
     * An app in several APKs (megler_apk_report_splits): its splits, as a folder whose .apk files they are (the folder
     * megler_bundle_unpack makes), and as a list of paths; either may be left NULL. The app runs as one, as Android runs
     * an installed app with its splits: native libraries out of the split that has them, its resources and assets out of
     * all of them, the code of feature splits on its class path. At most ten splits are opened for a Java app.
     */
    const char *splits_dir;
    const char *const *split_apks;
    int split_count;
    /* The surface the app draws to, in pixels, and the density Android should report (160 is 1:1; a phone is 320-480), as it
       starts: megler_set_surface changes them while it runs. */
    int width, height;
    int density_dpi;
    /* Which way up the surface is drawn as the app starts, in quarter turns from the screen's natural way up (Android's
       Surface.ROTATION_*; see megler_display_host). */
    int rotation;
    megler_window_host window;
    megler_audio_host audio;
    megler_media_host media;
    /* Decoders of samples for a Java app's MediaCodec (see megler_codec_host). May be left zero. */
    megler_codec_host codec;
    /* The phone's cameras for a Java app's android.hardware.Camera (see megler_camera_host). May be left zero. */
    megler_camera_host camera;
    /* The phone's own folders in the app's shared storage (see megler_storage_host). May be left zero. */
    megler_storage_host storage;
    /* The phone's location for a Java app's LocationManager (see megler_location_host). May be left zero: no location. */
    megler_location_host location;
    /* The phone's microphone for a Java app's AudioRecord (see megler_microphone_host). May be left zero: silence. */
    megler_microphone_host microphone;
    /* The phone's check of its user for a Java app's BiometricPrompt (see megler_biometric_host). May be left zero: none. */
    megler_biometric_host biometric;
    /* The phone's sensors for an app's SensorManager and ASensorManager (see megler_sensor_host). May be left zero: none. */
    megler_sensor_host sensors;
    /* The phone's Bluetooth LE radio for an app's android.bluetooth (see megler_bluetooth_host). May be left zero: none. */
    megler_bluetooth_host bluetooth;
    /* The phone's Wi-Fi, its scans and its connection, for an app's WifiManager (see megler_wifi_host). May be left zero: none. */
    megler_wifi_host wifi;
    /*
     * Drawing for an app whose user interface is Java (see megler_gfx_host). All of it may be left NULL: such an app still starts,
     * lays its views out and takes input, but nothing is drawn (its bitmaps hold only what the layer does itself: erasing, single
     * pixels, copies), text measures nothing wide and nothing high, and image files do not decode. Other apps never use it.
     */
    megler_gfx_host gfx;
    /*
     * Where the files are that an app whose code is Java runs on: a folder laid out like Android's /system, the "Android root"
     * (docs/JAVA.md, "Starting on Android's libcore": framework/ with libcore's DEX files, the Java framework and the jars;
     * lib/ with libcore's native libraries; usr/ with ICU's data and the time zones). It is only read, so it can stay in the
     * installed package. NULL: the folder `android` in megler_config.package_dir. An app with a native engine never opens it;
     * a Java app started without it is refused, and megler_last_error says what is missing.
     */
    const char *android_root;
    /* The touch keyboard for a Java app's text fields (see megler_keyboard_host). May be left NULL. */
    megler_keyboard_host keyboard;
    /* The bars a Java app wants hidden (see megler_system_bars_host). May be left NULL. */
    megler_system_bars_host bars;
    /* Which way up the app wants its screen (see megler_display_host). May be left NULL. */
    megler_display_host display;
    /* What a Java app's media session says it plays (see megler_media_session_host). May be left NULL. */
    megler_media_session_host media_session;
    /* What a Java app asks of the phone beyond its window: links, sharing, installing, other apps (see megler_system_host). May be left NULL. */
    megler_system_host system;
    /*
     * The apps the phone has, for a Java app that lists or starts them (a launcher; PackageManager.queryIntentActivities
     * of the launcher's intent, LauncherApps): a host folder with apps.txt -- a line for each app, its fields separated by
     * tabs: Android package, launcher activity's class, label, version name, version code -- and each app's icon as
     * <package>.png. And installs/<name>.txt: what the shell's installer last told of an APK the app handed it under that
     * name (install_apk), "<when>\t<what>\t<why>" -- declined, incompatible, failed or busy -- for the app's installer
     * session (megler.Installs), which is installed when apps.txt has the app at the APK's version. NULL: the app knows of
     * none but itself.
     */
    const char *apps_dir;
    /* The app has ended: by its own exit (status), by a crash (reason names it), or by megler_stop. Called once, from any thread. */
    void (MEGLER_CB *on_exit)(void *user, int status, const char *reason);
    void *user;
} megler_run_config;

/*
 * Starts the app on threads of its own and returns. False, with the reason in
 * the log and in megler_last_error, when it cannot start.
 */
MEGLER_API bool megler_run(const megler_run_config *config);
/*
 * Whether the app runs. When it ends this turns false first and on_exit is
 * called second, in that order always: inside on_exit and after it, this is
 * false. So a shell that is told by on_exit may act on "no app runs" at once
 * (megler_storage_check, for one, refuses while this is true). The other way
 * about does not hold: seeing this turn false does not mean on_exit has been
 * through yet, and the status and the reason are on_exit's alone -- wait for
 * it before reading what it stores.
 */
MEGLER_API bool megler_running(void);
MEGLER_API const char *megler_last_error(void);
/*
 * Where another thread of the process stands, in one line: its pc and lr, and the ways back into a module or into Android
 * code its stack holds, as a fault's report says them (not an unwinding: there are no tables for most of it). The thread
 * is stopped while its registers and its stack's top are copied. For the shell's watch of its UI thread: what holds it
 * when it does not answer. False when it could not be stopped (it ended, it is the caller, or the host cannot).
 */
MEGLER_API bool megler_thread_where(unsigned thread_id, char *out, size_t size);
/*
 * A module of the process's (a DLL of the system's, by file name: "CoreUIComponents.dll") as it lies in memory, into a
 * file: its image from its base, SizeOfImage long, with what is not committed or not readable as zeros. For reading a
 * fault in the system's own code, where the phone does not hand out the DLL itself. False when there is no such module
 * or the file could not be written.
 */
MEGLER_API bool megler_dump_module(const char *module, const char *path);
/*
 * Whether the system stops a thread at a hardware breakpoint set through its context (DBGBVR/DBGBCR), on another thread
 * and on this one, Thumb and ARM: a probe, said in `out` and in the log. True when one stopped.
 */
MEGLER_API bool megler_probe_breakpoints(char *out, size_t size);

/* Touch, in surface pixels with y down (after undoing the flip). phase: 0 down, 1 move, 2 up, 3 cancel. */
MEGLER_API void megler_touch(int phase, int pointer_id, float x, float y);
/* An Android key code (AKEYCODE_BACK is 4), pressed or released. */
MEGLER_API void megler_key(int android_keycode, bool down);
/*
 * The phone's Back key for a Java app, pressed and released, and what the app did with it, waited for at most max_ms:
 *   MEGLER_BACK_TAKEN  the app took it: a page or a dialog went, it changed what it shows, or it showed nothing new within
 *                      quiet_ms of having had the key (or did not answer in time);
 *   MEGLER_BACK_LEAVE  the app went to the back with it, as Android 12 and later send a task whose root activity is given
 *                      Back (and as moveTaskToBack does): nothing finished, and the window is the system's to take away --
 *                      the shell lets the system have its Back -- the activities stopped while it is hidden and started
 *                      again when it is shown;
 *   MEGLER_BACK_ENDED  the app finished its last activity with it, and ends.
 * quiet_ms is how long an app that showed nothing new is given: one whose Back goes on to JavaScript or Dart (React Native,
 * Flutter) sends itself to the back from there a little later. One that does it later than that, or calls moveTaskToBack
 * by itself, has the next Back -- within five seconds and with no touch between -- answered MEGLER_BACK_LEAVE at once,
 * without the app. Blocks the calling thread (the shell's UI thread is fine: the app's main thread answers, and the host
 * functions it calls meanwhile never wait for that thread). For an app that is not Java: megler_key(4) twice, TAKEN.
 */
enum { MEGLER_BACK_TAKEN = 0, MEGLER_BACK_LEAVE = 1, MEGLER_BACK_ENDED = 2 };
MEGLER_API int megler_back(unsigned quiet_ms, unsigned max_ms);
/* The touch keyboard's half of text entry, for a Java app: see megler_keyboard_host. Any thread. */
MEGLER_API void megler_text_commit(const char *utf8);
MEGLER_API void megler_text_key(int key);
MEGLER_API void megler_keyboard_closed(void);
MEGLER_API void megler_keyboard_height(int pixels);
/* How much of each edge of the surface the phone's bars cover, in pixels, for a Java app: see megler_system_bars_host. Any thread. */
MEGLER_API void megler_set_insets(int left, int top, int right, int bottom);
/* The app's surface resized or turned, for any app: see megler_display_host. Any thread. */
MEGLER_API void megler_set_surface(int width, int height, int density_dpi, int rotation);
/* The app went to the background or came back; lost or gained input focus. */
MEGLER_API void megler_set_paused(bool paused);
/*
 * Waits up to `ms` milliseconds until the app holds no lock on a database: Windows freezes a suspended app as it is, and a
 * lock it held would keep the other apps waiting on that database (Google Play services' are every app's). The shell calls
 * it as the app is suspended. True when none is held.
 */
MEGLER_API bool megler_settle_databases(int ms);
/*
 * Writes what the app wrote into its shared mappings of files to the files now (they hold it, not the files, until msync,
 * munmap or the app's end): Windows ends a suspended app without notice. The shell calls it as the app is suspended.
 */
MEGLER_API void megler_settle_files(void);
MEGLER_API void megler_set_focused(bool focused);
/* Asks the app to finish. Returns at once; on_exit follows. */
MEGLER_API void megler_stop(void);
/*
 * Another app asks this one to show one of its activities, by class name: what Android's startActivity of an intent that
 * names a component of this app does (microG's sign-in, LoginActivity, asked for by Play Store's AccountManager through
 * megler_system_host.launch_app). A Java app's main thread starts it on top of what it shows, if its manifest has the
 * activity and exports it; asked before the app runs, once its first activity is up. Any thread; the latest ask wins.
 */
MEGLER_API void megler_start_activity(const char *activity_class);
/*
 * Clears an app's data, as Android's "Clear storage" does, when the app next starts: before anything of it is open, its
 * private folder (/data/data/<package>) and its own Android/data folder in the shared storage go; its Android/obb stays, as
 * on Android. Asking is a file named after the package in the folder clear-data of the shared storage (megler_init's
 * storage_dir), which every package Megler made of an app sees: the shell's app page asks so, and the app itself, from
 * Android's app info screen. True when the ask is written. Needs megler_init.
 */
MEGLER_API bool megler_ask_clear_data(const char *package);
/* Whether such an ask waits for the app's next start: the shell forgets then what it keeps of the app's own, as Android's
   Clear storage does -- the user's answers to its permissions (player/PermissionHost.h). Needs megler_init. */
MEGLER_API bool megler_clear_data_asked(const char *package);
/* Frames presented since the app started. */
MEGLER_API uint32_t megler_frames(void);

/* ---------------------------------------------------- the phone, for an app
 *
 * What an Android app asks of its phone and only the shell can know.
 */

/*
 * The phone's time zone, as tzdata names it ("Europe/Moscow"). Before
 * megler_run: an app reads it once. NULL or "": as before -- a Java app's
 * default zone is GMT, and the property persist.sys.timezone says "UTC".
 *
 * With a name, a Java app's default zone (java.util.TimeZone.getDefault, and
 * with it Calendar, the date formats and java.time) is that zone with
 * tzdata's rules for it, and the property says the name. A name the Android
 * root's tzdata does not have breaks nothing: the app's zone is GMT, and the
 * log says so when the app first asks. Native code's localtime is not
 * changed by this; it already follows the host's clock.
 */
MEGLER_API void megler_set_timezone(const char *id);

/*
 * The language and region an app is told the phone has, as a BCP 47 tag ("ru-RU", "en-US", "zh-Hant-TW"): Java's
 * Locale.getDefault() and the resources it chooses, the property persist.sys.locale, a native app's AConfiguration, the
 * country TelephonyManager says the SIM and the network are in. Before megler_run: a Java app reads it as it starts. NULL, an
 * empty tag or one that is not a language's leaves "en-US", which is also what an app has until the first call.
 */
MEGLER_API void megler_set_locale(const char *tag);

/*
 * The phone's look, for a Java app. `accent_argb` is the phone's accent colour (Windows' UISettings Accent): Android's
 * DeviceDefault and Material themes give it as their colorAccent -- switches, check boxes, progress bars, a dialog's
 * buttons -- as do AppCompat's when the app names no accent of its own, and Material You's system colours
 * (android.R.color.system_*, which Material 3's dynamic colours are) are made from it (megler_palette_tone). `night`
 * is Configuration.uiMode's night bit, which an app's DayNight theme follows. Before megler_run: an app reads them as it
 * starts. Until the first call there is no accent (Android's own teal; Google's blue for the system colours) and no night.
 */
MEGLER_API void megler_set_theme(uint32_t accent_argb, bool night);

/*
 * A colour of Material You's tonal palettes made from a seed colour, roughly as Android 12 makes its system colours of a
 * wallpaper's (core/src/run/mg_palette.c): `palette` one of these, `tone` the lightness from 0 (black) to 100 (white).
 * The framework's android.R.color.system_* are made with it, and a shell may make its own look of the same colours.
 */
enum {
    MEGLER_PALETTE_NEUTRAL1 = 0,
    MEGLER_PALETTE_NEUTRAL2 = 1,
    MEGLER_PALETTE_ACCENT1 = 2,
    MEGLER_PALETTE_ACCENT2 = 3,
    MEGLER_PALETTE_ACCENT3 = 4,
    MEGLER_PALETTE_ERROR = 5,
};
MEGLER_API uint32_t megler_palette_tone(uint32_t seed_argb, int palette, int tone);

/*
 * The battery, in Android's own numbers. Any thread, any time, also while an
 * app runs. Until the first call an app has a full, healthy battery on mains
 * power (4.2 V, 25.0 degrees); NULL is that battery again.
 *
 * An app reads it when it asks (the sticky ACTION_BATTERY_CHANGED intent,
 * BatteryManager), and a Java app is told when it changes: a receiver
 * registered for ACTION_BATTERY_CHANGED is called again when the level, the
 * status or `plugged` is no longer what the app was last told, and
 * ACTION_POWER_CONNECTED or ACTION_POWER_DISCONNECTED is sent when `plugged`
 * goes between 0 and not 0 -- on the app's main thread, and no more often
 * than those three change: calling this with what is already there sends
 * nothing. A change of the voltage or the temperature alone is not sent; the
 * app has it when it next asks.
 *
 * What is not in here is filled in: the scale is 100, the health good (while
 * there is a battery), the technology "Li-ion". A level outside 0..100 is
 * brought inside, a status outside 1..5 is "unknown".
 */
typedef struct megler_battery {
    int level;        /* 0..100 */
    int status;       /* 1 unknown, 2 charging, 3 discharging, 4 not charging, 5 full */
    int plugged;      /* 0 on battery, 1 mains, 2 USB, 4 wireless */
    int voltage_mv;   /* 0: the phone does not say (Windows does not) -- the app is given a plausible one for the level, 3.4 V empty to 4.2 V full */
    int temperature;  /* tenths of a degree Celsius; INT_MIN: the phone does not say -- then 25.0 */
    bool present;
} megler_battery;
MEGLER_API void megler_set_battery(const megler_battery *now);

/*
 * The phone's cellular side, for a Java app's TelephonyManager: whether a SIM is in it and ready, its operator's name and
 * MCC and MNC ("25001"), the network's type as TelephonyManager numbers it (13 LTE, 10 HSPA, 3 UMTS, 2 EDGE, 1 GPRS, 0 not
 * known) and whether the phone is roaming. Said before an app starts and whenever it changes; until it is said, no SIM. An
 * app is told it runs on a phone all the same (PHONE_TYPE_GSM, the telephony feature): apps that see neither take a
 * phone with no SIM for a tablet. NULL: no SIM.
 */
typedef struct megler_cellular {
    bool sim_ready;
    const char *operator_name; /* UTF-8; NULL: not known */
    const char *mcc_mnc;       /* NULL: not known */
    int network_type;
    bool roaming;
} megler_cellular;
MEGLER_API void megler_set_cellular(const megler_cellular *now);

/*
 * The phone's text messages and calls, for a Java app as Android has them (the framework's megler.Phone). Any thread.
 *
 * megler_sms_received: a text message that came to the phone -- who it is from as the phone has it (a number, or a name
 * the sender gave), its text (UTF-8), when it came (milliseconds since 1970, UTC). The app is sent Android's SMS_RECEIVED
 * with it (receivers that hold RECEIVE_SMS: Google Play services' SMS Retriever, the app's own), an app-specific SMS token
 * in its text sends that token's PendingIntent, and it is in content://sms/inbox for an app that holds READ_SMS.
 *
 * megler_call_logged: a call the phone's history has -- the other side's number, when it began (ms since 1970, UTC), its
 * type as Android's call log numbers it (1 incoming, 2 outgoing, 3 missed, 5 rejected) and how long it went on (seconds).
 * An incoming, missed or rejected one is told as Android tells a call that rings: PHONE_STATE RINGING with its number (to
 * receivers that hold READ_PHONE_STATE, the number to those that hold READ_CALL_LOG as well) and then IDLE, and
 * TelephonyManager's listeners hear the same; and it is in content://call_log/calls for an app that holds READ_CALL_LOG.
 *
 * Only what comes while an app runs is the app's: the phone's earlier messages and calls are not given to it. Nothing of
 * either is written in the log but that there was one. Before an app runs, and with no Java app, they are dropped.
 */
MEGLER_API void megler_sms_received(const char *from, const char *body, int64_t time_ms);
MEGLER_API void megler_call_logged(const char *number, int64_t time_ms, int type, int duration_s);

/* ------------------------------------------------------------ diagnostics */

typedef void (MEGLER_CB *megler_line_fn)(const char *line, void *user);

/*
 * The core's own tests, run in this process: the host layer, the APK reader,
 * and -- with the guest test libraries from <package_dir>/selftest -- real
 * Android code loaded and called, and a small app started and stopped.
 * Returns the number of failed checks. Takes a few seconds; call it from a
 * background thread.
 *
 * That small app is the one app this process may run (see "running"): after
 * the self-test, megler_run fails until Megler is closed and opened again.
 *
 * Each line is also appended to <data_dir>/megler-selftest-steps.log and
 * flushed before the next step, so a step that ends the process has still
 * said how far the test got.
 */
MEGLER_API int megler_selftest(megler_line_fn out, void *user);

/*
 * Facts about this device that decide how Android code can run on it, one per
 * line as "name: value" -- whether a thread survives in ARM (not Thumb) state
 * and for how long, what the thread-pointer registers hold, what executable
 * memory costs, the Direct3D feature level ANGLE gets. Takes several seconds;
 * call it from a background thread. docs/ARCHITECTURE.md says what each
 * answer means. The lines also go, flushed one by one, to
 * <data_dir>/megler-probe-steps.log: some probes can end the process.
 */
MEGLER_API void megler_probe(megler_line_fn out, void *user);

/*
 * Whether the phone's folders do in an app's storage what an app will ask of
 * them. The shared storage is set up with `storage` as megler_run would set it
 * up, and in every link that is writable the layer does, through its own C
 * library, what an app does to files: lists the folder, makes a folder,
 * writes a file and reads it back through the storage's other name, stats,
 * appends, empties, renames, replaces, moves into a folder, is refused the
 * removal of a folder that holds something, renames and moves folders,
 * removes everything again; then renames a file from the first link to the
 * second. One line for each step ("ok" or "FAIL" first), and which of two
 * allowed answers was given where there are two. Returns the number of steps
 * that failed and adds the number of steps to *checks (which may be NULL).
 *
 * It works in a folder of its own in each link, megler-storage-check, removes
 * it at the end (and at the start, should an earlier run have been cut
 * short), and opens, changes and removes nothing else that is there.
 *
 * Not while an app runs: it sets the storage's links. It runs no app, so
 * megler_run is as free afterwards as it was. Takes a few seconds; call it
 * from a background thread, not the UI thread.
 */
MEGLER_API int megler_storage_check(const megler_storage_host *storage, megler_line_fn out, void *user, int *checks);

/*
 * Whether the network does for an app what an app will ask of it, through
 * the layer's own C library as an app's code calls it: "localhost" is
 * resolved without the network; the interfaces are listed (their names and
 * how many addresses each has, not the addresses); a public name
 * (connectivitycheck.gstatic.com, which Android itself asks) is resolved, a
 * TCP connection made to its port 80, a HEAD request sent and the answer's
 * status line read; then the same against a server of the check's own on
 * 127.0.0.1, and a UDP round trip between two sockets on 127.0.0.1. One line
 * for each step, "ok" or "FAIL" first. Returns the number of steps that
 * failed and adds the number of steps to *checks (which may be NULL).
 *
 * A phone may refuse an app a connection to itself over loopback; that is
 * said on a line of its own beginning "REFUSED BY THE PHONE", counted in
 * *refused (which may be NULL) and not among the failures.
 *
 * Takes up to half a minute when the network does not answer; call it from
 * a background thread, not the UI thread.
 */
MEGLER_API int megler_net_check(megler_line_fn out, void *user, int *checks, int *refused);

/* ------------------------------------------------------- an app's own package */

/*
 * The package of one Android app, made and signed here (core/src/appx, docs/PER-APP-PACKAGES.md): Megler's own files, the
 * app's APK and a manifest of its own, so that the app runs as an app of the phone's, in its own process. Windows only;
 * elsewhere the calls fail. Paths are UTF-8.
 *
 * The key: an RSA key and a self-signed code signing certificate whose subject is `publisher` (Megler's Publisher), made on
 * the first use and kept in two files (the certificate as DER: what the phone has to trust).
 */
typedef struct mg_appx_key megler_appx_key;
MEGLER_API megler_appx_key *megler_appx_key_open(const char *key_path, const char *cert_path, const char *publisher, char *err, size_t errlen);
MEGLER_API void megler_appx_key_close(megler_appx_key *key);

/* A package being written: files added in order (names of letters, digits, '.', '-', '_' and '/'), then finished with its
   block map, content types and signature; finish frees `a` whatever the outcome, and removes the file on a failure. */
typedef struct mg_appx megler_appx;
MEGLER_API megler_appx *megler_appx_begin(const char *path, char *err, size_t errlen);
MEGLER_API bool megler_appx_add_file(megler_appx *a, const char *name, const char *src_path);
MEGLER_API bool megler_appx_add_data(megler_appx *a, const char *name, const void *data, size_t size);
MEGLER_API bool megler_appx_finish(megler_appx *a, const megler_appx_key *key, char *err, size_t errlen);

/* The identity name of the package of the Android app `android_package` ("MeglerApp." and the package), into out (at least
   51 bytes); false for a name that is not an Android package's. */
MEGLER_API bool megler_appx_app_identity(const char *android_package, char *out, size_t outlen);
/*
 * What that package declares to Windows, from the permissions the Android app's manifest asks for (megler_apk_report's
 * "permissions") and whether it uses Google Play services' SMS Retriever ("smsRetriever"): a mask of these, each
 * the capabilities named after it, Megler's own needs in every package added:
 *   NETWORK       internetClient, internetClientServer, privateNetworkClientServer: INTERNET
 *   PICTURES      picturesLibrary: every package (Megler's diagnosis folder Pictures\Megler\diag; an app's own pictures)
 *   VIDEOS        videosLibrary: the storage permissions, READ_MEDIA_VIDEO (and its visual ones)
 *   MUSIC         musicLibrary: the storage permissions, READ_MEDIA_AUDIO
 *   WEBCAM        webcam: CAMERA
 *   MICROPHONE    microphone: RECORD_AUDIO
 *   LOCATION      location: ACCESS_FINE_LOCATION, ACCESS_COARSE_LOCATION, ACCESS_BACKGROUND_LOCATION
 *   CHAT          chat: RECEIVE_SMS, READ_SMS, or Google Play services' SMS Retriever
 *   CALL_HISTORY  phoneCallHistoryPublic: READ_PHONE_STATE, READ_CALL_LOG
 *   BLUETOOTH     bluetooth (a device capability): BLUETOOTH_SCAN, BLUETOOTH_CONNECT, BLUETOOTH_ADVERTISE, and before Android 12
 *                 BLUETOOTH and BLUETOOTH_ADMIN
 * Megler's other capabilities are every package's (codeGeneration, packageQuery, backgroundMediaPlayback) or no app's
 * (documentsLibrary and removableStorage, which want file types declared; packageManagement).
 */
enum {
    MEGLER_APPX_CAP_NETWORK = 1,
    MEGLER_APPX_CAP_PICTURES = 2,
    MEGLER_APPX_CAP_VIDEOS = 4,
    MEGLER_APPX_CAP_MUSIC = 8,
    MEGLER_APPX_CAP_WEBCAM = 16,
    MEGLER_APPX_CAP_MICROPHONE = 32,
    MEGLER_APPX_CAP_LOCATION = 64,
    MEGLER_APPX_CAP_CHAT = 128,
    MEGLER_APPX_CAP_CALL_HISTORY = 256,
    MEGLER_APPX_CAP_BLUETOOTH = 512,
};
MEGLER_API unsigned megler_appx_capabilities(const char *const *permissions, int count, bool sms_retriever);
/* That package's manifest, made from Megler's (the text of its AppxManifest.xml): the identity name, phone product id and
   shown names changed, the tile's and the splash screen's background colours when given ("#rrggbb" or "transparent";
   NULL keeps Megler's), its capabilities those of `capabilities` (megler_appx_capabilities; -1 keeps all of Megler's), all
   else Megler's. A string to free with megler_free, or NULL (err says why). */
MEGLER_API char *megler_appx_app_manifest(const char *megler_manifest, const char *android_package, const char *label, const char *tile_color,
                             const char *splash_color, int capabilities, char *err, size_t errlen);
/* That package's resources.pri, from the one Megler carries made for a placeholder name as long (appx-template\resources.pri,
   whose resources.txt says the placeholder and the two tails; tools/build-appx.sh): into `out`, as large as `pri`. False
   (err says why) when the template is not such a file. */
MEGLER_API bool megler_appx_pri_rename(const uint8_t *pri, size_t size, const char *placeholder, uint32_t tail1, uint32_t tail2, const char *name,
                          uint8_t *out, char *err, size_t errlen);

#if defined(_M_IX86) || (defined(__i386__) && defined(MG_CORE_ELF))
#pragma pack(pop)
#endif

#ifdef __cplusplus
}
#endif

#endif /* MEGLER_H */
