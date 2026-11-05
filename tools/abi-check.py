"""What guest code is given by the core, checked against Android's own headers, for each of Android's ABIs.

    python tools/abi-check.py [--abi x86_64,aarch64,i686,armv7a] [--unknown] [--variadic] [--jobs N]

The core gives guest code two things whose ABI is Android's: the functions and objects its tables bind to the names of
bionic and the NDK (the mg_sym tables, bionic/mg_bionic.h), and the structures it lays out where guest code reads or
writes them (struct stat, struct dirent, ANativeActivity...). They were written for armeabi-v7a; on the desktop the core
is compiled for Android's ABI of x86_64, arm64-v8a and x86 (docs/DESKTOP.md), where `long` and pointers may be 64 bits,
floats are passed as floats and not as their bits, and x86 aligns a 64-bit field to 4 bytes. This says, for each ABI,
where the core's version differs from Android's.

Clang parses each file of the core that holds a table, with the flags tools/build-desktop.sh compiles the core's ELF
with, and a file of this script's that includes the NDK's headers (API 29; again without _GNU_SOURCE, for the POSIX
strerror_r), and dumps both as JSON (-ast-dump=json); then the layouts of the records that matter, each forced by a
sizeof in a file that includes the first (-fdump-record-layouts). Then, per ABI:

  functions   for each name bound to a function: variadic or not, how many parameters, and the class of each parameter
              and of the result as the ABI passes it -- an integer of N bytes (a pointer is one of the pointer's size,
              which is how all four pass it), a float, a double, a long double, a structure of N bytes by value (one
              of integers alone, up to a pointer's size, as that integer). On armeabi-v7a a float is passed as an
              integer of 4 bytes and a double as one of 8 (softfp), which is what the wrappers there take (a float or
              a double of the core's own is the phone's MSVC's, in VFP registers, and differs). A function
              that takes fewer parameters than the name has and leaves the rest is right on all four (the caller
              removes what it passed); one that takes more is not. So is a wider integer result than the name's (the
              caller reads its low bits), and an int for a char or a short except on arm64-v8a (whose callers do not
              extend them).
  variadic    a function that is not variadic bound to a variadic name (open, fcntl, ioctl...) is right on all four
              as far as the convention goes -- the arguments are where a call that is not variadic puts them -- and
              is listed apart with what it takes for the variadic part (--variadic): whether that is what callers pass
              (a pointer for fcntl's F_GETLK, an int for open's mode) is for a person to read.
              And for a pointer parameter, the size of what it points at where that is an integer, a pointer or a
              float on both sides (a size_t's out-parameter written as a u32).
  interfaces  the same for each entry of a table of functions guest code calls through as an interface of the NDK's
              (OpenSL ES's, JNI's), against the structure of function pointers it stands for (VTABLES below).
  objects     for each name bound to an object (MG_DATA): its size, and for an array of no declared length its
              elements'.
  structures  for each structure of the core's in STRUCTS below: its size, its alignment, and each field of the NDK's
              structure it stands for against the core's field at that offset (by offset: the core may name fields
              otherwise, merge them or keep a field as bytes); and a field of the core's where the NDK has none, or
              one named as the NDK's somewhere else. One the core keeps in storage the guest owns (the inside of a
              pthread_rwlock_t) need only fit there; one the guest reads only the start of, only that. A difference
              EXPLAINED below says why it is right is printed with its reason and not counted.

Names the NDK's headers do not declare (and EXTRA_DECLS below does not) are counted and listed with --unknown:
bionic's own (__sF, __aeabi_*), Android's libraries without a header (libopenjdkjvm's JVM_*), what is newer than API
29, and what is in a header a C file cannot include (android/surface_control.h is C++ in NDK r21e). The exit status is
1 when anything differs, 2 when clang fails.

Environment, as tools/build-desktop.sh's: MEGLER_NDK (the NDK, r21e), MEGLER_LLVM (LLVM's bin, clang 15 or newer).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ABIS = {
    # name: (clang's target, flags of tools/build-desktop.sh for that ABI only)
    'x86_64': ('x86_64-linux-android29', []),
    'aarch64': ('aarch64-linux-android29', ['-ffixed-x18']),
    'i686': ('i686-linux-android29', []),
    'armv7a': ('armv7a-linux-androideabi29', []),
}

# The core's sources the tables are in: what tools/build-desktop.sh compiles into the ELF.
SOURCE_DIRS = ('core/src',)
SKIP_DIRS = ('core/src/host', 'core/src/appx')

# Bionic's exports guest code imports that the NDK's headers of API 29 do not declare, as bionic defines them: what
# _FORTIFY_SOURCE calls through clang's builtins, what older headers declared (or declared as macros over these), the
# C++ runtime's (operators new and delete named as each ABI mangles them: size_t is `unsigned long` on the 64-bit ABIs).
EXTRA_DECLS = r'''
void *__memcpy_chk(void *, const void *, size_t, size_t);
void *__memmove_chk(void *, const void *, size_t, size_t);
void *__memset_chk(void *, int, size_t, size_t);
char *__strcpy_chk(char *, const char *, size_t);
char *__stpcpy_chk(char *, const char *, size_t);
char *__strcat_chk(char *, const char *, size_t);
char *__strncat_chk(char *, const char *, size_t, size_t);
char *__strncpy_chk(char *, const char *, size_t, size_t);
int __sprintf_chk(char *, int, size_t, const char *, ...);
int __snprintf_chk(char *, size_t, int, size_t, const char *, ...);
int __vsprintf_chk(char *, int, size_t, const char *, va_list);
int __vsnprintf_chk(char *, size_t, int, size_t, const char *, va_list);
int __printf_chk(int, const char *, ...);
int __fprintf_chk(FILE *, int, const char *, ...);
int __vfprintf_chk(FILE *, int, const char *, va_list);
int __fpclassify(double); int __fpclassifyd(double); int __fpclassifyf(float); int __fpclassifyl(long double);
int __isfinite(double); int __isfinitef(float); int __isfinitel(long double);
int __isinf(double); int __isinff(float); int __isinfl(long double);
int __isnan(double); int __isnanf(float); int __isnanl(long double);
int __isnormal(double); int __isnormalf(float); int __isnormall(long double);
int __signbit(double); int __signbitf(float); int __signbitl(long double);
int isinff(float);
double exp10(double); float exp10f(float); double pow10(double); float pow10f(float);
int (bcmp)(const void *, const void *, size_t);
void (bcopy)(const void *, void *, size_t);
void (bzero)(void *, size_t);
char *(index)(const char *, int);
char *(rindex)(const char *, int);
long long strtoq(const char *, char **, int);
unsigned long long strtouq(const char *, char **, int);
char *secure_getenv(const char *);
const char *__gnu_basename(const char *);
sighandler_t bsd_signal(int, sighandler_t);
sighandler_t sysv_signal(int, sighandler_t);
void *valloc(size_t);
void *pvalloc(size_t);
int malloc_trim(size_t);
int pthread_cancel(pthread_t);
int pthread_setcancelstate(int, int *);
int pthread_setcanceltype(int, int *);
void pthread_testcancel(void);
int pthread_cond_timedwait_monotonic(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_timedwait_relative_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_timeout_np(pthread_cond_t *, pthread_mutex_t *, unsigned);
int pthread_mutex_lock_timeout_np(pthread_mutex_t *, unsigned);
int __register_atfork(void (*)(void), void (*)(void), void (*)(void), void *);
int __srget(FILE *);
int __swbuf(int, FILE *);
int android_getaddrinfofornet(const char *, const char *, const struct addrinfo *, unsigned, unsigned,
                              struct addrinfo **);
struct hostent *android_gethostbynamefornet(const char *, int, unsigned, unsigned);
int __android_log_is_loggable(int, const char *, int);
ssize_t getdents64(int, void *, size_t);
int memfd_create(const char *, unsigned int);
int close_range(unsigned int, unsigned int, int);
gzFile gzopen64(const char *, const char *);
long long gztell64(gzFile);
#if !defined(__LP64__)
void *__mmap2(void *, size_t, int, int, int, size_t);
#endif
extern const char *__progname;
extern uintptr_t __stack_chk_guard;
extern const short *_tolower_tab_;
extern const short *_toupper_tab_;
int __cxa_atexit(void (*)(void *), void *, void *);
void __cxa_finalize(void *);
int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
int __cxa_guard_acquire(void *);
void __cxa_guard_release(void *);
void __cxa_guard_abort(void *);
void __cxa_pure_virtual(void);
void __stack_chk_fail(void);
#ifdef __LP64__
#define MG_ABI_Z(n32, n64) __asm__(#n64)
#else
#define MG_ABI_Z(n32, n64) __asm__(#n32)
#endif
typedef struct mg_abi_nothrow mg_abi_nothrow;
void *mg_abi_new(size_t) MG_ABI_Z(_Znwj, _Znwm);
void *mg_abi_new_array(size_t) MG_ABI_Z(_Znaj, _Znam);
void *mg_abi_new_nothrow(size_t, const mg_abi_nothrow *) MG_ABI_Z(_ZnwjRKSt9nothrow_t, _ZnwmRKSt9nothrow_t);
void *mg_abi_new_array_nothrow(size_t, const mg_abi_nothrow *) MG_ABI_Z(_ZnajRKSt9nothrow_t, _ZnamRKSt9nothrow_t);
void mg_abi_delete(void *) __asm__("_ZdlPv");
void mg_abi_delete_array(void *) __asm__("_ZdaPv");
void mg_abi_delete_nothrow(void *, const mg_abi_nothrow *) __asm__("_ZdlPvRKSt9nothrow_t");
void mg_abi_delete_array_nothrow(void *, const mg_abi_nothrow *) __asm__("_ZdaPvRKSt9nothrow_t");
void mg_abi_delete_sized(void *, size_t) MG_ABI_Z(_ZdlPvj, _ZdlPvm);
void mg_abi_delete_array_sized(void *, size_t) MG_ABI_Z(_ZdaPvj, _ZdaPvm);
'''

# The NDK's headers a C file can include, API 29 (and before them what a C file needs of them that they leave out).
NDK_HEADERS = '''
alloca.h assert.h byteswap.h complex.h ctype.h dirent.h dlfcn.h elf.h endian.h err.h errno.h error.h fcntl.h fenv.h
fnmatch.h fts.h ftw.h getopt.h glob.h grp.h iconv.h ifaddrs.h inttypes.h langinfo.h lastlog.h libgen.h limits.h link.h
locale.h malloc.h math.h memory.h mntent.h netdb.h nl_types.h paths.h poll.h pthread.h pty.h pwd.h regex.h resolv.h
sched.h search.h semaphore.h setjmp.h signal.h spawn.h stdatomic.h stdint.h stdio.h stdio_ext.h stdlib.h string.h
strings.h syscall.h sysexits.h syslog.h tar.h termios.h threads.h time.h uchar.h ucontext.h unistd.h utime.h utmp.h
wait.h wchar.h wctype.h xlocale.h zlib.h
arpa/inet.h arpa/nameser.h net/if.h net/ethernet.h net/if_arp.h net/route.h netinet/in.h netinet/tcp.h netinet/udp.h
netinet/ip.h netinet/ip6.h netinet/icmp6.h netinet/ether.h netinet/ip_icmp.h netpacket/packet.h
sys/auxv.h sys/capability.h sys/endian.h sys/epoll.h sys/eventfd.h sys/fcntl.h sys/file.h sys/fsuid.h sys/inotify.h
sys/ioctl.h sys/ipc.h sys/klog.h sys/mman.h sys/mount.h sys/msg.h sys/param.h sys/personality.h sys/poll.h sys/prctl.h
sys/ptrace.h sys/quota.h sys/random.h sys/reboot.h sys/resource.h sys/select.h sys/sem.h sys/sendfile.h sys/shm.h
sys/signalfd.h sys/socket.h sys/stat.h sys/statfs.h sys/statvfs.h sys/swap.h sys/syscall.h sys/sysinfo.h
sys/sysmacros.h sys/system_properties.h sys/time.h sys/timerfd.h sys/times.h sys/timex.h sys/types.h sys/uio.h
sys/un.h sys/utsname.h sys/vfs.h sys/wait.h sys/xattr.h
android/api-level.h android/asset_manager.h android/asset_manager_jni.h android/bitmap.h android/choreographer.h
android/configuration.h android/dlext.h android/fdsan.h android/font.h android/font_matcher.h android/hardware_buffer.h
android/hardware_buffer_jni.h android/imagedecoder.h android/input.h android/keycodes.h android/log.h android/looper.h
android/multinetwork.h android/native_activity.h android/native_window.h android/native_window_jni.h android/obb.h
android/rect.h android/sensor.h android/set_abort_message.h android/sharedmem.h android/sharedmem_jni.h
android/storage_manager.h android/surface_texture.h android/surface_texture_jni.h android/sync.h
android/system_fonts.h android/thermal.h android/trace.h android/window.h android/NeuralNetworks.h
android/binder_ibinder.h android/binder_ibinder_jni.h android/binder_parcel.h android/binder_parcel_jni.h
android/binder_status.h
aaudio/AAudio.h amidi/AMidi.h camera/NdkCameraCaptureSession.h camera/NdkCameraDevice.h camera/NdkCameraError.h
camera/NdkCameraManager.h camera/NdkCameraMetadata.h camera/NdkCameraMetadataTags.h camera/NdkCameraWindowType.h
camera/NdkCaptureRequest.h media/NdkImage.h media/NdkImageReader.h media/NdkMediaCodec.h media/NdkMediaCrypto.h
media/NdkMediaDataSource.h media/NdkMediaDrm.h media/NdkMediaError.h media/NdkMediaExtractor.h media/NdkMediaFormat.h
media/NdkMediaMuxer.h SLES/OpenSLES.h SLES/OpenSLES_Android.h SLES/OpenSLES_AndroidConfiguration.h
SLES/OpenSLES_AndroidMetadata.h OMXAL/OpenMAXAL.h OMXAL/OpenMAXAL_Android.h EGL/egl.h EGL/eglext.h GLES/gl.h
GLES/glext.h GLES3/gl32.h GLES2/gl2ext.h vulkan/vulkan.h jni.h
'''.split()
NDK_BEFORE = {
    # NDK r21e's font headers and NdkMediaCodec.h name a structure without its tag, as C++ may.
    'android/font.h': 'typedef struct AFont AFont;\ntypedef struct AFontMatcher AFontMatcher;\n'
                      'typedef struct ASystemFontIterator ASystemFontIterator;\n',
    'media/NdkMediaCodec.h': 'typedef struct AMediaCodecOnAsyncNotifyCallback AMediaCodecOnAsyncNotifyCallback;\n',
}

# The core's structures guest code sees, and the NDK's each stands for:
#   (file, the core's structure, the NDK's type, how):
#     'same'    every field of the NDK's at its offset, the same size and alignment;
#     'start'   the NDK's fields as far as the core's structure goes (guest code reads only those: zlib's gzgetc reads
#               the start of the core's gzFile);
#     'fits'    no more than the NDK's size and alignment: storage the guest owns and only the core reads.
STRUCTS = [
    ('bionic/mg_libc_io.c', 'g_iovec', 'struct iovec', 'same'),
    ('bionic/mg_libc_io.c', 'g_stat', 'struct stat', 'same'),
    ('bionic/mg_libc_io.c', 'g_statfs', 'struct statfs', 'same'),
    ('bionic/mg_libc_io.c', 'g_statvfs', 'struct statvfs', 'same'),
    ('bionic/mg_libc_io.c', 'g_dirent', 'struct dirent', 'same'),
    ('bionic/mg_libc_io.c', 'g_itimerspec', 'struct itimerspec', 'same'),
    ('bionic/mg_libc_io.c', 'g_epoll_event', 'struct epoll_event', 'same'),
    ('bionic/mg_libc_mem.c', 'g_mallinfo_t', 'struct mallinfo', 'same'),
    ('bionic/mg_libc_misc.c', 'g_utsname', 'struct utsname', 'same'),
    ('bionic/mg_libc_misc.c', 'g_rlimit', 'struct rlimit', 'same'),
    ('bionic/mg_libc_misc.c', 'g_rlimit64', 'struct rlimit64', 'same'),
    ('bionic/mg_libc_misc.c', 'g_sigaction', 'struct sigaction', 'same'),
    ('bionic/mg_libc_misc.c', 'g_stack_t', 'stack_t', 'same'),
    ('bionic/mg_libc_misc.c', 'g_lconv', 'struct lconv', 'same'),
    ('bionic/mg_libc_misc.c', 'g_passwd', 'struct passwd', 'same'),
    ('bionic/mg_libc_misc.c', 'g_group', 'struct group', 'same'),
    ('bionic/mg_libc_misc.c', 'g_sysinfo_buf', 'struct sysinfo', 'same'),
    ('bionic/mg_libc_misc.c', 'g_vm_iovec', 'struct iovec', 'same'),
    ('bionic/mg_libc_str.c', 'g_div_t', 'div_t', 'same'),
    ('bionic/mg_libc_str.c', 'g_ldiv_t', 'ldiv_t', 'same'),
    ('bionic/mg_libc_str.c', 'g_lldiv_t', 'lldiv_t', 'same'),
    ('bionic/mg_libc_time.c', 'g_timespec', 'struct timespec', 'same'),      # (mg_bionic.h's, for every file)
    ('bionic/mg_libc_time.c', 'g_timeval', 'struct timeval', 'same'),
    ('bionic/mg_libc_time.c', 'g_tm', 'struct tm', 'same'),
    ('bionic/mg_libc_time.c', 'g_tms', 'struct tms', 'same'),
    ('bionic/mg_libdl.c', 'g_dlextinfo', 'android_dlextinfo', 'same'),
    ('bionic/mg_libdl.c', 'g_dl_info', 'Dl_info', 'same'),
    ('bionic/mg_libdl.c', 'g_dl_phdr_info', 'struct dl_phdr_info', 'same'),
    ('bionic/mg_libz.c', 'g_gz', 'struct gzFile_s', 'start'),
    ('bionic/mg_net.c', 'g_iovec', 'struct iovec', 'same'),
    ('bionic/mg_net.c', 'g_msghdr', 'struct msghdr', 'same'),
    ('bionic/mg_net.c', 'g_mmsghdr', 'struct mmsghdr', 'same'),
    ('bionic/mg_net.c', 'g_cmsghdr', 'struct cmsghdr', 'same'),
    ('bionic/mg_netdb.c', 'g_addrinfo', 'struct addrinfo', 'same'),
    ('bionic/mg_netdb.c', 'g_hostent', 'struct hostent', 'same'),
    ('bionic/mg_netdb.c', 'g_servent', 'struct servent', 'same'),
    ('bionic/mg_netdb.c', 'g_protoent', 'struct protoent', 'same'),
    ('bionic/mg_netdb.c', 'g_ifaddrs', 'struct ifaddrs', 'same'),
    ('bionic/mg_netdb.c', 'g_nameindex', 'struct if_nameindex', 'same'),
    ('bionic/mg_pthread.c', 'g_rwlock', 'pthread_rwlock_t', 'fits'),
    ('bionic/mg_pthread.c', 'g_attr', 'pthread_attr_t', 'same'),
    ('bionic/mg_pthread.c', 'g_cleanup', 'struct __pthread_cleanup_t', 'same'),
    ('bionic/mg_regex.c', 'g_regex', 'regex_t', 'same'),
    ('bionic/mg_regex.c', 'g_regmatch', 'regmatch_t', 'same'),
    ('ndk/mg_ahb.c', 'g_desc', 'AHardwareBuffer_Desc', 'same'),
    ('ndk/mg_media.c', 'g_buffer_info', 'AMediaCodecBufferInfo', 'same'),
    ('ndk/mg_opensles.c', 'sl_iid', 'struct SLInterfaceID_', 'same'),
    ('ndk/mg_opensles.c', 'g_data_source', 'SLDataSource', 'same'),
    ('ndk/mg_opensles.c', 'g_queue_state', 'SLAndroidSimpleBufferQueueState', 'same'),
    ('drivers/mg_native_activity.c', 'g_callbacks', 'ANativeActivityCallbacks', 'same'),
    ('drivers/mg_native_activity.c', 'g_activity', 'ANativeActivity', 'same'),
    ('drivers/mg_java_ndk.c', 'g_bitmap_info', 'AndroidBitmapInfo', 'same'),
    ('jni/mg_jni.c', 'g_native_method', 'JNINativeMethod', 'same'),
    ('jni/mg_jni.c', 'g_attach_args', 'JavaVMAttachArgs', 'same'),
]
# What differs and is right: (file, the core's structure) -> (the ABIs, why). Said, and not counted.
EXPLAINED = {
    ('bionic/mg_libz.c', 'g_gz'): (('i686', 'armv7a'), "gzFile_s.pos is a z_off64_t, 4 bytes in 32-bit Android unless the "
                                   "app was built with _LARGEFILE64_SOURCE, 8 with it; the core's 8 have the 4 as their "
                                   "low half, which is all gzgetc's macro touches"),
}

# The core's tables of functions that guest code calls through as an interface of the NDK's: (file, the table, the
# NDK's structure of function pointers its entries stand for in order). Built by an initializer, or by assignments to
# its elements (g_env_table[4] = (void *)j_GetVersion).
VTABLES = [
    ('ndk/mg_opensles.c', 'g_object_table', 'struct SLObjectItf_'),
    ('ndk/mg_opensles.c', 'g_engine_table', 'struct SLEngineItf_'),
    ('ndk/mg_opensles.c', 'g_play_table', 'struct SLPlayItf_'),
    ('ndk/mg_opensles.c', 'g_queue_table', 'struct SLAndroidSimpleBufferQueueItf_'),
    ('ndk/mg_opensles.c', 'g_queue_table', 'struct SLBufferQueueItf_'),
    ('ndk/mg_opensles.c', 'g_volume_table', 'struct SLVolumeItf_'),
    ('ndk/mg_opensles.c', 'g_config_table', 'struct SLAndroidConfigurationItf_'),
    ('jni/mg_jni.c', 'g_env_table', 'struct JNINativeInterface'),
    ('jni/mg_jni.c', 'g_vm_table', 'struct JNIInvokeInterface'),
]
# Structures named g_* in those files that guest code never sees inside: it holds a pointer to them and no more.
OPAQUE = {'g_dir', 'g_gz', 'g_format', 'g_media', 'g_data_source', 'g_track', 'g_extractor', 'g_codec', 'g_input_event',
          'g_asset', 'g_asset_dir', 'g_config', 'g_font_iter', 'g_font', 'g_stransaction', 'g_tls_index'}


def host_path(p):
    """A path as Git Bash spells it (/c/Users/...) for Windows' programs."""
    m = re.match(r'^/([a-zA-Z])/(.*)$', p)
    return '%s:/%s' % (m.group(1).upper(), m.group(2)) if m and os.name == 'nt' else p


def tools():
    ndk = host_path(os.environ.get('MEGLER_NDK') or r'C:\Users\User\Documents\android-ndk-r21e')
    llvm = host_path(os.environ.get('MEGLER_LLVM') or r'C:\Program Files\LLVM\bin')
    prebuilt = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt')
    hosts = sorted(os.listdir(prebuilt)) if os.path.isdir(prebuilt) else []
    sysroot = os.path.join(prebuilt, hosts[0], 'sysroot') if hosts else ''
    clang = os.path.join(llvm, 'clang.exe' if os.name == 'nt' else 'clang')
    if not os.path.isdir(os.path.join(sysroot, 'usr', 'include')):
        sys.exit('abi-check: no NDK sysroot under %s (set MEGLER_NDK)' % prebuilt)
    if not os.path.isfile(clang):
        sys.exit('abi-check: no clang in %s (set MEGLER_LLVM)' % llvm)
    return clang, sysroot


def core_flags(abi, sysroot):
    """tools/build-desktop.sh's flags for the core's ELF, those that decide types and layouts."""
    triple, extra = ABIS[abi]
    flags = ['--target=' + triple, '--sysroot=' + sysroot, '-std=gnu11', '-fPIC', '-fvisibility=hidden', '-femulated-tls',
             '-fno-stack-protector', '-D_GNU_SOURCE', '-DMG_CORE_ELF=1', '-Icore/third_party/zlib'] + extra
    if os.path.isfile(os.path.join(ROOT, 'build', 'sqlite', 'sqlite3.c')):
        flags += ['-DMG_HAVE_SQLITE=1', '-Ibuild/sqlite']
    if os.path.isfile(os.path.join(ROOT, 'build', 'webp', 'src', 'dec', 'webp_dec.c')):
        flags += ['-DMG_HAVE_WEBP=1', '-Ibuild/webp']
    return flags


# MARK: types


class Cls:
    """A type as an ABI passes it or lays it out: kind is int, ptr, float, double, ldouble, record, array, func, void, or
    unknown; size in bytes (None when not known); for a record, whether it holds integers and pointers alone (which
    all four pass as an integer of its size, up to a pointer's)."""
    __slots__ = ('kind', 'size', 'elem', 'count', 'name', 'intonly')

    def __init__(self, kind, size, elem=None, count=None, name=None, intonly=False):
        self.kind, self.size, self.elem, self.count, self.name, self.intonly = kind, size, elem, count, name, intonly

    def __repr__(self):
        if self.kind == 'array':
            return '%r[%s]' % (self.elem, self.count if self.count is not None else '')
        if self.kind == 'record':
            return 's%s' % (self.size if self.size is not None else '?')
        if self.kind == 'unknown':
            return '?' + (self.name or '')
        if self.kind == 'void':
            return 'void'
        letter = {'int': 'i', 'ptr': 'p', 'float': 'f', 'double': 'f', 'ldouble': 'f', 'func': 'p'}[self.kind]
        return '%s%s' % (letter, self.size)


QUALIFIERS = {'const', 'volatile', 'restrict', '__restrict', '_Nonnull', '_Nullable', '_Null_unspecified', '__unaligned'}


def strip_attributes(t):
    """`__attribute__((...))` out of a type as clang prints it, and the calling conventions they name; and an
    attribute that is a macro, which clang prints by the macro's name (bionic's `__INTRODUCED_IN(21)`)."""
    conv = []
    t = re.sub(r'\b__[A-Z][A-Z0-9_]*(\([^()]*\))?', ' ', t)
    while True:
        i = t.find('__attribute__((')
        if i < 0:
            return t.strip(), conv
        depth, j = 0, i + len('__attribute__')
        while j < len(t):
            if t[j] == '(':
                depth += 1
            elif t[j] == ')':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        what = t[i + len('__attribute__(('):j - 1]
        for c in ('ms_abi', 'sysv_abi', 'stdcall', 'fastcall', 'thiscall', 'vectorcall', 'regcall', 'regparm', 'pcs',
                  'aarch64_vector_pcs', 'preserve_most', 'preserve_all'):
            if re.search(r'\b%s\b' % c, what):
                conv.append(what)
        t = t[:i] + t[j + 1:]


def group_start(t, end):
    """Where the bracketed group that ends at t[end] begins."""
    close = t[end]
    open_ = {')': '(', ']': '['}[close]
    depth = 0
    for i in range(end, -1, -1):
        if t[i] == close:
            depth += 1
        elif t[i] == open_:
            depth -= 1
            if depth == 0:
                return i
    return -1


def first_top(t, ch):
    depth = 0
    for i, c in enumerate(t):
        if c in '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ch and depth == 0:
            return i
    return -1


class Env:
    """What classifying a type needs of a translation unit and an ABI."""

    def __init__(self, abi_facts, typedefs, records):
        self.ptr = abi_facts['ptr']
        self.long = abi_facts['long']
        self.ldouble = abi_facts['ldouble']
        self.softfp = abi_facts['softfp']
        self.abi = abi_facts['abi']
        self.typedefs = typedefs
        self.records = records

    def record(self, name, depth=0):
        """A record by value: its size, and whether its fields are integers and pointers alone."""
        r = self.records.get(name)
        if not r:
            return Cls('record', None, name=name)
        fields = r['fields']
        intonly = depth < 8
        for i, (off, bits, d, ftype, fname) in enumerate(fields):
            if i + 1 < len(fields) and fields[i + 1][2] > d:
                continue                             # a record laid out below
            c = self.classify(ftype, depth + 1)
            while c.kind == 'array' and c.elem is not None:
                c = c.elem
            if c.kind not in ('int', 'ptr') and not (c.kind == 'record' and c.intonly):
                intonly = False
        return Cls('record', r['size'], name=name, intonly=intonly)

    def classify(self, t, depth=0):
        t, _ = strip_attributes(t)
        t = re.sub(r'\s+', ' ', t).strip()
        while True:
            m = re.match(r'^(.*?)\s*\b(%s)$' % '|'.join(QUALIFIERS), t)
            if not m or not m.group(1):
                break
            t = m.group(1).strip()
        if not t:
            return Cls('unknown', None, name='(empty)')
        if t.endswith(']'):
            i = first_top(t, '[')
            elem = t[:i].strip()
            if elem.endswith(')'):
                return Cls('ptr', self.ptr)          # int (*)[4]: a pointer to an array
            dims = re.findall(r'\[(\d*)\]', t[i:])
            e = self.classify(elem, depth + 1)
            count = 1
            for d in dims:
                count = count * int(d) if d and count is not None else None
            size = e.size * count if e.size is not None and count is not None else None
            return Cls('array', size, elem=e, count=count)
        if t.endswith(')'):
            i = group_start(t, len(t) - 1)
            before = t[:i].rstrip()
            if before.endswith(')'):                 # RET (*)(ARGS), RET (*const)(ARGS): a pointer to a function
                inner = before[group_start(before, len(before) - 1) + 1:-1].strip()
                m = re.match(r'^\*+\s*(?:const\s*)?((?:\[\d*\])+)$', inner)
                if m:                                # RET (*[N])(ARGS): an array of them
                    count = 1
                    for d in re.findall(r'\[(\d*)\]', m.group(1)):
                        count = count * int(d) if d and count is not None else None
                    return Cls('array', self.ptr * count if count is not None else None, elem=Cls('ptr', self.ptr),
                               count=count)
                return Cls('ptr', self.ptr)
            return Cls('func', None)
        if t.endswith('*'):
            # What it points at, for a parameter's (a void, a function or an unknown type is no one's business).
            pointee = self.classify(t[:-1], depth + 1) if depth < 4 else None
            return Cls('ptr', self.ptr, elem=pointee)
        w = ' '.join(x for x in t.split() if x not in QUALIFIERS)
        if w.startswith('_Atomic(') and w.endswith(')'):
            return self.classify(w[len('_Atomic('):-1], depth + 1)
        if w.startswith(('struct ', 'union ')):
            return self.record(w, depth)
        if w.startswith('enum '):
            return Cls('int', 4)
        b = self.builtin(w)
        if b:
            return b
        if w in self.typedefs and depth < 32:
            c = self.classify(self.typedefs[w], depth + 1)
            if c.kind == 'record' and c.size is None and w in self.records:
                c = self.record(w, depth)            # a structure with no tag, known by its typedef's name
            return c
        if w in self.records:
            return self.record(w, depth)
        return Cls('unknown', None, name=w)

    def builtin(self, w):
        ints = {'_Bool': 1, 'bool': 1, 'char': 1, 'signed char': 1, 'unsigned char': 1, 'short': 2, 'short int': 2,
                'unsigned short': 2, 'unsigned short int': 2, 'int': 4, 'signed': 4, 'signed int': 4, 'unsigned': 4,
                'unsigned int': 4, 'long': self.long, 'long int': self.long, 'unsigned long': self.long,
                'unsigned long int': self.long, 'long long': 8, 'long long int': 8, 'unsigned long long': 8,
                'unsigned long long int': 8, '__int128': 16, 'unsigned __int128': 16, 'char16_t': 2, 'char32_t': 4,
                # (clang's own names of size_t and ptrdiff_t, which it prints as they are)
                '__size_t': self.ptr, '__signed_size_t': self.ptr, '__ptrdiff_t': self.ptr}
        if w in ints:
            return Cls('int', ints[w])
        if w == 'float':
            return Cls('float', 4)
        if w == 'double':
            return Cls('double', 8)
        if w == 'long double':
            return Cls('ldouble', self.ldouble)
        if w in ('_Float16', '__fp16'):
            return Cls('float', 2)
        if w == 'void':
            return Cls('void', 0)
        if w.startswith('_Complex '):
            e = self.builtin(w[len('_Complex '):])
            return Cls('record', e.size * 2 if e else None, name=w)
        if w == '__builtin_va_list':
            return {'x86_64': Cls('array', 24, elem=Cls('record', 24), count=1), 'aarch64': Cls('record', 32),
                    'armv7a': Cls('record', 4), 'i686': Cls('ptr', 4)}[self.abi]
        return None

    def key(self, c, param=False, core=False):
        """What decides how the ABI passes a value of class c (param: as an argument, else as a result). On
        armeabi-v7a the core's own float or double is passed as the phone's MSVC passes it, in VFP registers, where
        the guest's softfp has the bits in integer registers (mg_bionic.h): a wrapper takes gfloat and gdouble."""
        k = c.kind
        if param and k in ('array', 'func'):
            return ('i', self.ptr)
        if k in ('ptr', 'func'):
            return ('i', self.ptr)
        if k == 'int':
            return ('i', c.size)
        if k in ('float', 'double', 'ldouble'):
            if self.softfp and core:
                return ('vfp', c.size)
            return ('i', c.size) if self.softfp else ('f', c.size)
        if k == 'record':
            # A structure of integers alone, as small as an integer register, is passed in one as that integer would
            # be, on all four (struct in_addr); and returned in one on all but x86, which returns any structure in
            # memory the caller gives (and armeabi-v7a any of more than 4 bytes).
            if c.intonly and c.size is not None and c.size <= self.ptr and (
                    param or self.abi in ('x86_64', 'aarch64') or (self.abi == 'armv7a' and c.size <= 4)):
                return ('i', c.size)
            return ('s', c.size)
        if k == 'void':
            return ('v', 0)
        return ('?', c.name)

    def result_ok(self, core, ndk):
        """Whether a result of the core's class is read right as the NDK's: the same, or an integer at least as wide
        (the caller reads its low bits), or anything in a register where the caller expects nothing."""
        ck, nk = self.key(core, core=True), self.key(ndk)
        return ck == nk or (ck[0] == nk[0] == 'i' and ck[1] is not None and nk[1] is not None and ck[1] >= nk[1]) or \
            (nk == ('v', 0) and ck[0] in ('i', 'f'))


def function_result(t):
    """The result of a function type as clang prints it: `RET (ARGS)`; a function that returns a pointer to a function
    is printed `RET2 (*(ARGS))(ARGS2)`, whose result is a pointer."""
    t, conv = strip_attributes(t)
    t = t.strip()
    if not t.endswith(')'):
        return t, conv, False
    i = group_start(t, len(t) - 1)
    args = t[i + 1:-1].strip()
    before = t[:i].rstrip()
    if before.endswith(')'):
        return 'void *', conv, False
    return before, conv, args == ''


# MARK: clang's output


def parse_layouts(text):
    """-fdump-record-layouts' text: each record's size, alignment and fields (offset, bits, depth, type, name), nested
    records' fields after theirs, one level deeper; with -fdump-record-layouts-canonical, the types without typedefs."""
    records = {}
    lines = text.splitlines()
    i = 0
    row = re.compile(r'^\s*(\d+)(?::(\d+)-(\d+))? \| ( *)(.*)$')
    while i < len(lines):
        if not lines[i].startswith('*** Dumping AST Record Layout'):
            i += 1
            continue
        i += 1
        m = row.match(lines[i])
        name = m.group(5).strip()
        fields = []
        i += 1
        while i < len(lines) and '[sizeof=' not in lines[i]:
            m = row.match(lines[i])
            if m:
                text_ = m.group(5)
                depth = len(m.group(4)) // 2
                if re.search(r'\((?:anonymous|unnamed) at [^)]*\)\s*$', text_):
                    ftype, fname = text_.strip(), ''
                else:
                    ftype, _, fname = text_.strip().rpartition(' ')
                bits = (int(m.group(2)), int(m.group(3))) if m.group(2) is not None else None
                fields.append((int(m.group(1)), bits, depth, ftype, fname))
            i += 1
        m = re.search(r'\[sizeof=(\d+), (?:dsize=\d+, )?align=(\d+)', lines[i]) if i < len(lines) else None
        if m:
            records[name] = {'size': int(m.group(1)), 'align': int(m.group(2)), 'fields': fields}
        i += 1
    return records


def type_of(node):
    t = node.get('type') or {}
    return t.get('desugaredQualType') or t.get('qualType') or ''


def types_of(node):
    """A node's type as written and as clang desugars its outermost level: (sugared, desugared)."""
    t = node.get('type') or {}
    q = t.get('qualType') or ''
    return (q, t.get('desugaredQualType') or q)


def record_expr(t, typedefs, depth=0):
    """C for the record a value of type t is (or an array of them), as a file of the translation unit can name it, or
    None: `struct stat`, or a typedef's name for one with no tag (div_t)."""
    t, _ = strip_attributes(t)
    t = re.sub(r'\s+', ' ', t).strip()
    if t.endswith(']') and not t[:first_top(t, '[')].rstrip().endswith(')'):
        t = t[:first_top(t, '[')].strip()
    if t.endswith((')', '*')) or re.search(r'\*\s*(%s)$' % '|'.join(QUALIFIERS), t):
        return None
    w = ' '.join(x for x in t.split() if x not in QUALIFIERS)
    if w.startswith(('struct ', 'union ')):
        return w
    if w in typedefs and depth < 32 and record_expr(typedefs[w], typedefs, depth + 1):
        return w
    return None


def string_literal(node):
    """The text of the string literal in an expression, or None."""
    stack = [node]
    while stack:
        n = stack.pop()
        if n.get('kind') == 'StringLiteral':
            v = n.get('value', '')
            try:
                return json.loads(v) if v.startswith('"') else v
            except ValueError:
                return v.strip('"')
        stack.extend(reversed(n.get('inner', [])))
    return None


def target_of(expr):
    """What a table entry's address is: ('func', name), ('data', object's types, name), ('value', the pointer's types,
    name) or None."""
    k = expr.get('kind')
    inner = expr.get('inner') or []
    if k == 'DeclRefExpr':
        ref = expr.get('referencedDecl') or {}
        if ref.get('kind') == 'FunctionDecl':
            return ('func', ref.get('name'))
        return ('value', types_of(expr), ref.get('name'))
    if k == 'ImplicitCastExpr' and expr.get('castKind') == 'ArrayToPointerDecay' and inner:
        return ('data', types_of(inner[0]), name_in(inner[0]))
    if k == 'UnaryOperator' and expr.get('opcode') == '&' and inner:
        t = target_of(inner[0]) if inner[0].get('kind') == 'DeclRefExpr' else None
        if t and t[0] == 'func':
            return t
        return ('data', types_of(inner[0]), name_in(inner[0]))
    if k in ('CStyleCastExpr', 'ImplicitCastExpr', 'ParenExpr') and inner:
        return target_of(inner[0])
    return None


def name_in(expr):
    stack = [expr]
    while stack:
        n = stack.pop()
        if n.get('kind') == 'DeclRefExpr':
            return (n.get('referencedDecl') or {}).get('name')
        stack.extend(reversed(n.get('inner', [])))
    return None


def tables_in(tu):
    """Every mg_sym table's entries: (table, name, target), from initializers and from assignments in turn
    (`t[i].name = "x"; t[i].addr = f;`)."""
    entries = []
    for n in tu.get('inner', []):
        if n.get('kind') != 'VarDecl':
            continue
        qt = (n.get('type') or {}).get('qualType', '')
        if re.search(r'\bmg_sym\s*\[', qt):
            init = next((c for c in n.get('inner', []) if c.get('kind') == 'InitListExpr'), None)
            for e in (init or {}).get('inner', []):
                parts = e.get('inner') or []
                if e.get('kind') != 'InitListExpr' or len(parts) < 2:
                    continue
                name = string_literal(parts[0])
                if name is not None:
                    entries.append((n['name'], name, target_of(parts[1])))
    for n in tu.get('inner', []):
        if n.get('kind') == 'FunctionDecl' and any(c.get('kind') == 'CompoundStmt' for c in n.get('inner', [])):
            assigned_tables(n, n['name'], entries)
    return entries


def assigned_tables(node, fn, entries):
    for c in node.get('inner', []) or []:
        if c.get('kind') == 'CompoundStmt':
            pending = None
            for s in c.get('inner', []) or []:
                if s.get('kind') == 'BinaryOperator' and s.get('opcode') == '=' and len(s.get('inner', [])) == 2:
                    lhs, rhs = s['inner']
                    if lhs.get('kind') == 'MemberExpr' and 'mg_sym' in type_of((lhs.get('inner') or [{}])[0]):
                        if lhs.get('name') == 'name':
                            pending = string_literal(rhs)
                        elif lhs.get('name') == 'addr' and pending is not None:
                            t = target_of(rhs)
                            entries.append((fn + '()', pending, t if t and t[0] == 'func' else None))
                            pending = None
        assigned_tables(c, fn, entries)


def const_value(expr):
    """An integer constant expression's value (an index such as `4 + 1`), or None."""
    k = expr.get('kind')
    inner = expr.get('inner') or []
    if k == 'IntegerLiteral':
        return int(expr.get('value', '0'), 0)
    if k == 'ConstantExpr' and 'value' in expr:
        return int(expr['value'], 0)
    if k in ('ParenExpr', 'ImplicitCastExpr', 'CStyleCastExpr', 'ConstantExpr') and inner:
        return const_value(inner[0])
    if k == 'BinaryOperator' and len(inner) == 2 and expr.get('opcode') in ('+', '-', '*'):
        a, b = const_value(inner[0]), const_value(inner[1])
        if a is not None and b is not None:
            return {'+': a + b, '-': a - b, '*': a * b}[expr['opcode']]
    return None


def vtables_in(tu, names):
    """Each named table of functions: {index: target}, from its initializer or from assignments to its elements."""
    out = {n: {} for n in names}
    if not out:
        return out
    for n in tu.get('inner', []):
        if n.get('kind') == 'VarDecl' and n.get('name') in out:
            init = next((c for c in n.get('inner', []) if c.get('kind') == 'InitListExpr'), None)
            for i, e in enumerate((init or {}).get('inner', [])):
                out[n['name']][i] = target_of(e)

    def walk(node):
        for c in node.get('inner', []) or []:
            if c.get('kind') == 'BinaryOperator' and c.get('opcode') == '=' and len(c.get('inner', [])) == 2:
                lhs, rhs = c['inner']
                if lhs.get('kind') == 'ArraySubscriptExpr' and len(lhs.get('inner', [])) == 2:
                    table = name_in(lhs['inner'][0])
                    i = const_value(lhs['inner'][1])
                    if table in out and i is not None:
                        out[table][i] = target_of(rhs)
            walk(c)
    for n in tu.get('inner', []):
        if n.get('kind') == 'FunctionDecl' and any(c.get('kind') == 'CompoundStmt' for c in n.get('inner', [])):
            walk(n)
    return out


def split_top(t, sep=','):
    parts, depth, start = [], 0, 0
    for i, c in enumerate(t):
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        elif c == sep and depth == 0:
            parts.append(t[start:i].strip())
            start = i + 1
    parts.append(t[start:].strip())
    return [p for p in parts if p]


def pointer_function(t):
    """A pointer to a function as clang prints it, `RET (*)(ARGS)`, as function_raw gives a function; None if t is
    not one."""
    t = re.sub(r'\s+', ' ', strip_attributes(t)[0]).strip()
    if not t.endswith(')'):
        return None
    i = group_start(t, len(t) - 1)
    args = t[i + 1:-1].strip()
    before = t[:i].rstrip()
    if not re.search(r'\(\s*\*\s*(const\s*)?\)$', before):
        return None
    ret = before[:group_start(before, len(before) - 1)].strip()
    params = [] if args in ('', 'void') else split_top(args)
    variadic = bool(params) and params[-1] == '...'
    if variadic:
        params = params[:-1]
    return {'ret': ret, 'params': [(p, p) for p in params], 'variadic': variadic, 'noproto': args == '', 'conv': []}


def function_raw(node):
    """A FunctionDecl's result and parameters as clang prints them."""
    qt = (node.get('type') or {}).get('qualType', '')
    ret, conv, empty = function_result(qt)
    params = [types_of(p) for p in node.get('inner', []) if p.get('kind') == 'ParmVarDecl']
    return {'ret': ret, 'params': params, 'variadic': bool(node.get('variadic')),
            'noproto': empty and not params, 'conv': conv}


def function_info(raw, env):
    """The same as the ABI passes them."""
    return {'ret': env.classify(raw['ret']), 'params': [env.classify(d) for q, d in raw['params']],
            'variadic': raw['variadic'], 'noproto': raw['noproto'], 'conv': raw['conv']}


def flatten(rec, records, env, base_depth=1):
    """A record's leaves (offset, size, class, name, in a union): the fields that are not records laid out below."""
    leaves = []
    fields = rec['fields']
    union_depths = []
    for i, (off, bits, depth, ftype, fname) in enumerate(fields):
        if depth < base_depth:
            continue
        while union_depths and depth <= union_depths[-1]:
            union_depths.pop()
        nested = i + 1 < len(fields) and fields[i + 1][2] > depth
        if nested:
            if ftype.startswith('union '):
                union_depths.append(depth)
            continue
        c = env.classify(ftype)
        size = c.size
        if bits:
            off += bits[0] // 8
            size = max(1, (bits[1] - bits[0] + 8) // 8)
        leaves.append((off, size, c, fname, bool(union_depths)))
    return leaves


# MARK: one translation unit


def clang_run(clang, flags, path, extra):
    p = subprocess.run([clang] + flags + ['-fsyntax-only', '-w'] + extra + [path], cwd=ROOT, capture_output=True)
    if p.returncode != 0:
        err = p.stderr.decode('utf-8', 'replace')
        return None, '\n'.join(l for l in err.splitlines() if 'error' in l)[:4000] or err[-2000:]
    return p.stdout, None


def pointee(t):
    """What a pointer type as clang prints it points at ('' for one that is not)."""
    t = re.sub(r'\s+', ' ', strip_attributes(t)[0]).strip()
    m = re.match(r'^(.*?)\s*\*\s*(?:const|volatile|restrict|__restrict)?$', t)
    return m.group(1) if m and not m.group(1).endswith(')') else ''


def scan(job):
    """Runs clang on one translation unit for one ABI; what of it the comparison needs.

    Twice: for the AST, and then for the layouts of the records whose layout the comparison needs, each forced by a
    sizeof of a structure of this script's that holds one (a probe) in a file that includes the translation unit. (Not
    -fdump-record-layouts-complete: that lays a record out at its closing brace, before an attribute after the brace
    applies -- bionic's x86_64 `struct epoll_event {...} __packed;` -- and even sizeof says what it laid out then.)"""
    abi, facts, clang, flags, path, kind, wanted_records, wanted_vtables, tmp = job
    out, err = clang_run(clang, flags, path, ['-Xclang', '-ast-dump=json'])
    if err:
        return {'error': err}
    tu = json.loads(out)
    del out
    typedefs, tags = {}, set()
    for n in tu.get('inner', []):
        if n.get('kind') == 'TypedefDecl' and n.get('name'):
            typedefs[n['name']] = type_of(n)
        elif n.get('kind') == 'RecordDecl' and n.get('name') and n.get('completeDefinition'):
            tags.add('%s %s' % (n.get('tagUsed', 'struct'), n['name']))
    funcs, objs, tables, vtables = {}, {}, [], {}
    if kind == 'ndk':
        for n in tu.get('inner', []):
            if n.get('kind') == 'FunctionDecl' and n.get('storageClass') != 'static':
                sym = n.get('mangledName') or n.get('name')
                if sym and sym not in funcs:
                    funcs[sym] = function_raw(n)
            elif n.get('kind') == 'VarDecl' and n.get('storageClass') == 'extern':
                sym = n.get('mangledName') or n.get('name')
                if sym and sym not in objs:
                    objs[sym] = types_of(n)
            elif n.get('kind') == 'RecordDecl' and n.get('completeDefinition') and \
                    '%s %s' % (n.get('tagUsed', 'struct'), n.get('name')) in wanted_vtables:
                vtables['%s %s' % (n.get('tagUsed', 'struct'), n['name'])] = [
                    (f.get('name', ''), pointer_function((f.get('type') or {}).get('qualType', '')))
                    for f in n.get('inner', []) if f.get('kind') == 'FieldDecl']
    else:
        tables = tables_in(tu)
        vtables = vtables_in(tu, wanted_vtables)
        bound = {t[1] for _, _, t in tables if t and t[0] == 'func'}
        bound |= {t[1] for v in vtables.values() for t in v.values() if t and t[0] == 'func'}
        for n in tu.get('inner', []):
            if n.get('kind') == 'FunctionDecl' and n.get('name') in bound:
                body = any(c.get('kind') == 'CompoundStmt' for c in n.get('inner', []))
                if body or n['name'] not in funcs:
                    funcs[n['name']] = function_raw(n)
        for i, (table, name, t) in enumerate(tables):
            if t and t[0] == 'value':
                # The value of a pointer: the object is what it points at.
                q, d = t[1]
                tables[i] = (table, name, ('data', (pointee(q), pointee(d)), t[2]))
    del tu

    # The records whose layout is needed: the structures compared, and those passed by value or bound as objects.
    probes = set()
    for w in wanted_records:
        if kind == 'ndk':
            probes.add(w)
        elif 'struct ' + w in tags:
            probes.add('struct ' + w)
    members = [f for v in vtables.values() if isinstance(v, list) for _, f in v if f]
    for f in list(funcs.values()) + members:
        for t in [f['ret']] + [q for q, d in f['params']]:
            e = record_expr(t, typedefs)
            if e:
                probes.add(e)
    for q, d in list(objs.values()) + [t[1] for _, _, t in tables if t and t[0] == 'data']:
        e = record_expr(q, typedefs)
        if e:
            probes.add(e)
    probes = sorted(p for p in probes if p in tags or p in typedefs or kind == 'ndk')
    wrapper = os.path.join(tmp, 'probe-%s-%s.c' % (abi, re.sub(r'\W', '_', os.path.basename(path))))
    with open(wrapper, 'w', encoding='utf-8') as f:
        f.write('#include "%s"\n' % os.path.abspath(os.path.join(ROOT, path)).replace('\\', '/'))
        for i, e in enumerate(probes):
            f.write('struct mg_abi_probe_%d { %s v; };\nchar mg_abi_probe_size_%d[sizeof(struct mg_abi_probe_%d)];\n'
                    % (i, e, i, i))
    out, err = clang_run(clang, flags, wrapper, ['-Xclang', '-fdump-record-layouts',
                                                 '-Xclang', '-fdump-record-layouts-canonical'])
    os.remove(wrapper)
    if err:
        return {'error': 'the layouts: ' + err}
    records = parse_layouts(out.decode('utf-8', 'replace'))
    # A probe's record, one level down, under the C that names it; one with no tag of its own (div_t) is printed by its
    # typedef's name, and known by that as well.
    for i, e in enumerate(probes):
        r = records.get('struct mg_abi_probe_%d' % i)
        if not r:
            continue
        inner = [f for f in r['fields'] if f[2] >= 2]
        ptype = next((f[3] for f in r['fields'] if f[2] == 1), '')
        probe = {'size': r['size'], 'align': r['align'], 'fields': [(o, b, d - 1, t, n) for o, b, d, t, n in inner]}
        records['probe:' + e] = probe
        for alias in (e, ptype) + (('struct ' + ptype,) if re.match(r'^\w+$', ptype) else ()):
            records.setdefault(alias, probe)

    env = Env(facts, typedefs, records)
    result = {'error': None, 'records': {}}
    result['funcs'] = {name: function_info(f, env) for name, f in funcs.items()}
    result['vars'] = {name: env.classify(d) for name, (q, d) in objs.items()}
    result['tables'] = [(table, name, (t[0], env.classify(t[1][1]) if t[1][1] else Cls('unknown', None), t[2])
                         if t and t[0] == 'data' else t) for table, name, t in tables]
    if kind == 'ndk':
        result['vtables'] = {s: [(fname, function_info(f, env) if f else None) for fname, f in v]
                             for s, v in vtables.items()}
    else:
        result['vtables'] = {s: {i: t[1] for i, t in v.items() if t and t[0] == 'func'} for s, v in vtables.items()}
    for w in wanted_records:
        r = records.get('probe:' + (w if kind == 'ndk' else 'struct ' + w))
        if r:
            result['records'][w] = {'size': r['size'], 'align': r['align'], 'leaves': flatten(r, records, env)}
    if kind != 'ndk':
        # The structures named g_* the file itself defines (those of the headers it includes are their own files').
        src = open(os.path.join(ROOT, path), encoding='utf-8', errors='replace').read()
        mine = set(re.findall(r'^(?:typedef\s+)?struct\s+(g_\w+)\s*\{', src, re.M))
        result['g_records'] = sorted(t[len('struct '):] for t in tags if t[len('struct '):] in mine)
    return result


def ndk_source(posix):
    lines = ['/* abi-check.py: the NDK\'s headers (%s). */' % ('POSIX' if posix else 'GNU')]
    if not posix:
        lines.append('#define _GNU_SOURCE 1')
    lines.append('#define EGL_EGLEXT_PROTOTYPES 1\n#define GL_GLEXT_PROTOTYPES 1')
    for h in NDK_HEADERS:
        lines.append(NDK_BEFORE.get(h, '') + '#include <%s>' % h)
    lines.append('#if !defined(__LP64__)\n#include <time64.h>\n#endif')
    lines.append(EXTRA_DECLS)
    return '\n'.join(lines) + '\n'


def abi_facts(clang, abi, sysroot):
    out = subprocess.run([clang, '--target=' + ABIS[abi][0], '--sysroot=' + sysroot, '-dM', '-E', '-x', 'c', '-'],
                         input=b'', capture_output=True).stdout.decode()
    macros = dict(re.findall(r'#define (\w+) (.*)', out))
    return {'abi': abi, 'ptr': int(macros['__SIZEOF_POINTER__']), 'long': int(macros['__SIZEOF_LONG__']),
            'ldouble': int(macros['__SIZEOF_LONG_DOUBLE__']),
            'softfp': '__ARM_PCS' in macros and '__ARM_PCS_VFP' not in macros}


# MARK: comparing


def compare_function(core, ndk, env):
    """(the differences, a note when a function that is not variadic answers a variadic name)."""
    issues = []
    if core['conv']:
        issues.append('convention %s' % ', '.join(core['conv']))
    if core['noproto']:
        issues.append('no prototype')
    def core_text(c, param):
        return repr(c) + (' in VFP registers' if env.key(c, param, core=True)[0] == 'vfp' else '')
    if not env.result_ok(core['ret'], ndk['ret']):
        issues.append('result %s, NDK %r' % (core_text(core['ret'], False), ndk['ret']))
    cp, np_ = core['params'], ndk['params']
    for i in range(min(len(cp), len(np_))):
        ck, nk = env.key(cp[i], True, core=True), env.key(np_[i], True)
        # An integer narrower than 4 bytes is extended to 4 by the caller on armeabi-v7a (AAPCS says so) and on the
        # x86 ABIs (clang and gcc do it and clang counts on it), not on arm64-v8a: a wrapper may take it as an int.
        widened = ck == ('i', 4) and nk[0] == 'i' and nk[1] in (1, 2) and env.abi != 'aarch64'
        if ck != nk and not widened:
            issues.append('arg %d %s, NDK %r' % (i + 1, core_text(cp[i], True), np_[i]))
        elif cp[i].kind == np_[i].kind == 'ptr' and pointee_differs(cp[i].elem, np_[i].elem):
            issues.append('arg %d points at %r, NDK at %r' % (i + 1, cp[i].elem, np_[i].elem))
    note = None
    if len(cp) > len(np_):
        if ndk['variadic']:
            note = 'takes %s for "..."' % ' '.join(repr(c) for c in cp[len(np_):])
        else:
            issues.append('%d args, NDK %d' % (len(cp), len(np_)))
    elif core['variadic'] != ndk['variadic']:
        note = 'variadic: core %s, NDK %s' % ('yes' if core['variadic'] else 'no', 'yes' if ndk['variadic'] else 'no')
    if note and core['variadic'] == ndk['variadic']:
        note = None
    return issues, note


def pointee_differs(c, n):
    """Whether what a pointer parameter points at is of another size in the core than in the NDK, an integer, a pointer
    or a float on both sides, neither a byte (bytes are how the core takes a buffer): a size_t written as a u32 leaves half
    of it as it was. (A structure is STRUCTS' business: the core may hold more behind a handle than the NDK says, or keep
    its own state in the first word of a pthread_mutex_t.)"""
    known = ('int', 'ptr', 'float', 'double', 'ldouble')
    if c is None or n is None or c.kind not in known or n.kind not in known or c.size is None or n.size is None:
        return False
    return c.size != n.size and c.size > 1 and n.size > 1


def compare_object(core, ndk, env):
    """An object is memory the guest reads and writes as the NDK declares it: as many bytes, and for an array of no
    declared length, elements as wide."""
    if ndk.kind == 'array' and ndk.count is None:
        elem = core.elem if core.kind == 'array' else core
        if elem.size != ndk.elem.size:
            return ['elements of %s bytes, NDK %r' % (elem.size, ndk.elem)]
        return []
    if core.size != ndk.size:
        return ['%r, NDK %r' % (core, ndk)]
    return []


PADDING = re.compile(r'^(__)?(pad|unused|reserved|spare|glibc_reserved|rfu)\w*$|^__\w*(pad|unused|reserved)\w*$', re.I)


def compare_record(core, ndk, how):
    """The differences between the core's record and the NDK's, as short phrases."""
    out = []
    if how == 'fits':
        if core['size'] > ndk['size'] or core['align'] > ndk['align']:
            out.append('size %d, align %d: more than the NDK\'s %d, %d' % (core['size'], core['align'], ndk['size'],
                                                                            ndk['align']))
        return out
    if how == 'same':
        if core['size'] != ndk['size']:
            out.append('size %d, NDK %d' % (core['size'], ndk['size']))
        if core['align'] != ndk['align']:
            out.append('align %d, NDK %d' % (core['align'], ndk['align']))
    limit = min(core['size'], ndk['size']) if how == 'start' else max(core['size'], ndk['size'])
    cl = [x for x in core['leaves'] if x[1]]
    nl = [x for x in ndk['leaves'] if x[1] and x[0] < limit]         # (not a field of no size: char _f[0])
    said = set()
    for off, size, c, name, in_union in nl:
        end = off + size
        over = [x for x in cl if x[0] < end and x[0] + x[1] > off]
        if not over:
            if not PADDING.match(name) and end <= core['size']:
                out.append('%s@%d+%d: nothing there' % (name or '?', off, size))
                said.add(name)
            continue
        if any(x[0] == off and x[1] == size for x in over):
            continue
        if c.kind == 'array' and all(x[0] >= off and x[0] + x[1] <= end for x in over):
            continue                                 # the core's fields within the NDK's array (bytes, reserved)
        if len(over) == 1 and over[0][0] <= off and over[0][0] + over[0][1] >= end and \
                (over[0][2].kind == 'array' or in_union or PADDING.match(name)):
            continue                                 # the core's bytes over the NDK's field, or its union's widest
        x = over[0]
        out.append('%s@%d+%d: core %s@%d+%d' % (name or '?', off, size, x[3] or '?', x[0], x[1]))
        said.add(name)
    # The other way: a field of the core's where the NDK has none (padding the guest does not read or write)...
    for off, size, c, name, _ in cl:
        if off >= limit or PADDING.match(name) or c.kind == 'array':
            continue
        if not any(x[0] < off + size and x[0] + x[1] > off for x in nl):
            out.append('core %s@%d+%d: no field of the NDK\'s there' % (name or '?', off, size))
    # ...and a field the core names as the NDK does, somewhere else.
    cnames = defaultdict(list)
    for x in cl:
        cnames[x[3]].append(x)
    nnames = defaultdict(list)
    for x in nl:
        nnames[x[3]].append(x)
    for name, xs in nnames.items():
        if name and name not in said and len(xs) == 1 and len(cnames.get(name, [])) == 1:
            x, y = xs[0], cnames[name][0]
            if (x[0], x[1]) != (y[0], y[1]) and y[0] < limit:
                out.append('%s@%d+%d: core\'s %s@%d+%d' % (name, x[0], x[1], name, y[0], y[1]))
    return out


# MARK: the run


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--abi', default='x86_64,aarch64,i686,armv7a')
    ap.add_argument('--unknown', action='store_true', help='list the names the NDK does not declare')
    ap.add_argument('--variadic', action='store_true', help='list the functions that answer a variadic name')
    ap.add_argument('--jobs', type=int, default=max(1, min(8, (os.cpu_count() or 4) // 2)))
    args = ap.parse_args()
    abis = [a.strip() for a in args.abi.split(',') if a.strip()]
    for a in abis:
        if a not in ABIS:
            sys.exit('abi-check: no ABI %s (%s)' % (a, ', '.join(ABIS)))
    clang, sysroot = tools()

    files = []
    for d in SOURCE_DIRS:
        for dirpath, _, names in os.walk(os.path.join(ROOT, d)):
            rel = os.path.relpath(dirpath, ROOT).replace('\\', '/')
            if any(rel == s or rel.startswith(s + '/') for s in SKIP_DIRS) or rel.count('/') > 3:
                continue
            for n in sorted(names):
                if n.endswith('.c'):
                    src = open(os.path.join(dirpath, n), encoding='utf-8', errors='replace').read()
                    if re.search(r'\bmg_sym\b[^;(]*(\[|=)', src):
                        files.append(rel + '/' + n)
    files.sort()

    tmp = tempfile.mkdtemp(prefix='abi-check-')
    ndk_files = {}
    for posix in (False, True):
        path = os.path.join(tmp, 'ndk-%s.c' % ('posix' if posix else 'gnu'))
        with open(path, 'w', encoding='utf-8') as f:
            f.write(ndk_source(posix))
        ndk_files[posix] = path

    facts = {a: abi_facts(clang, a, sysroot) for a in abis}
    jobs, keys = [], []
    for a in abis:
        triple, extra = ABIS[a]
        nflags = ['--target=' + triple, '--sysroot=' + sysroot, '-std=gnu11', '-O2', '-D_FORTIFY_SOURCE=2'] + extra
        wanted = sorted({s[2] for s in STRUCTS})
        jobs.append((a, facts[a], clang, nflags, ndk_files[False], 'ndk', wanted, sorted({v[2] for v in VTABLES}), tmp))
        keys.append((a, 'ndk'))
        jobs.append((a, facts[a], clang, nflags, ndk_files[True], 'ndk', [], [], tmp))
        keys.append((a, 'ndk-posix'))
        for f in files:
            rel = f[len('core/src/'):]
            wanted = sorted({s[1] for s in STRUCTS if s[0] == rel})
            vt = sorted({v[1] for v in VTABLES if v[0] == rel})
            jobs.append((a, facts[a], clang, core_flags(a, sysroot), f, 'core', wanted, vt, tmp))
            keys.append((a, f))
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        results = dict(zip(keys, pool.map(scan, jobs)))
    for posix, path in ndk_files.items():
        os.remove(path)
    os.rmdir(tmp)

    failed = [(k, r['error']) for k, r in results.items() if r['error']]
    for k, err in failed:
        print('abi-check: clang failed on %s for %s:\n%s' % (k[1], k[0], err))
    if failed:
        return 2

    lines = defaultdict(list)       # text -> [abi]
    explained = defaultdict(list)
    variadic = defaultdict(list)
    unknown = defaultdict(set)      # abi -> names
    summary = {}
    line_of = {}
    for a in abis:
        env = Env(facts[a], {}, {})
        ndk = results[(a, 'ndk')]
        posix = results[(a, 'ndk-posix')]
        funcs = dict(posix['funcs'])
        funcs.update(ndk['funcs'])
        objs = dict(posix['vars'])
        objs.update(ndk['vars'])
        n_bind = n_bad = n_struct = n_struct_bad = 0
        for f in files:
            r = results[(a, f)]
            short = f[len('core/src/'):]
            for table, name, t in r['tables']:
                n_bind += 1
                if t is None:
                    continue
                if t[0] == 'func':
                    core = r['funcs'].get(t[1])
                    if name not in funcs and name in objs:
                        n_bad += 1
                        lines['%s %s -> %s: a function, NDK an object' % (short, name, t[1])].append(a)
                        continue
                    if name not in funcs:
                        unknown[a].add(name)
                        continue
                    if core is None:
                        lines['%s %s -> %s: no declaration of %s' % (short, name, t[1], t[1])].append(a)
                        n_bad += 1
                        continue
                    issues, note = compare_function(core, funcs[name], env)
                    where = '%s %s -> %s' % (where_is(f, t[1], line_of), name, t[1])
                    if issues:
                        n_bad += 1
                        lines['%s: %s' % (where, '; '.join(issues))].append(a)
                    if note:
                        variadic['%s: %s' % (where, note)].append(a)
                else:
                    if name in funcs and name not in objs:
                        n_bad += 1
                        lines['%s %s -> object %s: an object, NDK a function' % (short, name, t[2])].append(a)
                        continue
                    if name not in objs:
                        unknown[a].add(name)
                        continue
                    issues = compare_object(t[1], objs[name], env)
                    if issues:
                        n_bad += 1
                        lines['%s %s -> %s %s: %s' % (short, name, 'object' if t[0] == 'data' else 'the value of',
                                                       t[2], '; '.join(issues))].append(a)
            for g in r.get('g_records', []):
                if g not in OPAQUE and not any(s[1] == g and s[0] == short for s in STRUCTS):
                    lines['%s %s: a structure of the core\'s not in abi-check.py\'s STRUCTS' % (short, g)].append(a)
        for rel, core_name, ndk_type, how in STRUCTS:
            f = 'core/src/' + rel
            if (a, f) not in results:
                if os.path.exists(os.path.join(ROOT, f)):
                    lines['%s %s: the file holds no table, not parsed' % (rel, core_name)].append(a)
                continue
            core = results[(a, f)]['records'].get(core_name)
            nrec = ndk['records'].get(ndk_type)
            n_struct += 1
            if core is None or nrec is None:
                n_struct_bad += 1
                lines['%s %s ~ %s: %s not found' % (rel, core_name, ndk_type,
                                                    'the core\'s' if core is None else 'the NDK\'s')].append(a)
                continue
            issues = compare_record(core, nrec, how)
            why = EXPLAINED.get((rel, core_name))
            if issues and why and a in why[0]:
                explained['%s %s ~ %s: %s (%s)' % (rel, core_name, ndk_type, '; '.join(issues[:6]), why[1])].append(a)
            elif issues:
                n_struct_bad += 1
                more = ' (and %d more)' % (len(issues) - 6) if len(issues) > 6 else ''
                lines['%s %s ~ %s: %s%s' % (rel, core_name, ndk_type, '; '.join(issues[:6]), more)].append(a)
        for rel, table, iface in VTABLES:
            f = 'core/src/' + rel
            entries = results[(a, f)]['vtables'].get(table) if (a, f) in results else None
            members = ndk['vtables'].get(iface)
            if entries is None or members is None:
                lines['%s %s ~ %s: %s not found' % (rel, table, iface, "the core's" if entries is None else
                                                    "the NDK's")].append(a)
                n_bad += 1
                continue
            for i, fn in sorted(entries.items()):
                n_bind += 1
                where = '%s %s[%d] -> %s' % (where_is(f, fn, line_of), table, i, fn)
                core = results[(a, f)]['funcs'].get(fn)
                if i >= len(members):
                    lines['%s: past the end of %s (%d entries)' % (where, iface, len(members))].append(a)
                    n_bad += 1
                    continue
                mname, member = members[i]
                if core is None or member is None:
                    continue
                issues, note = compare_function(core, member, env)
                if issues:
                    n_bad += 1
                    lines['%s (%s.%s): %s' % (where, iface, mname, '; '.join(issues))].append(a)
                if note:
                    variadic['%s (%s.%s): %s' % (where, iface, mname, note)].append(a)
        summary[a] = (n_bind, n_bad, len(unknown[a]), sum(1 for v in variadic.values() if a in v), n_struct,
                      n_struct_bad)

    for a in abis:
        n_bind, n_bad, n_unknown, n_var, n_struct, n_struct_bad = summary[a]
        print('%-7s %d bindings: %d differ, %d not in the NDK, %d answer a variadic name; %d structures: %d differ'
              % (a, n_bind, n_bad, n_unknown, n_var, n_struct, n_struct_bad))
    for text in sorted(lines, key=lambda t: (t.split(' ')[0], t)):
        print('%s  %s' % (','.join(lines[text]), text))
    for text in sorted(explained):
        print('%s  explained  %s' % (','.join(explained[text]), text))
    if args.variadic:
        for text in sorted(variadic):
            print('%s  variadic  %s' % (','.join(variadic[text]), text))
    if args.unknown:
        groups = defaultdict(list)
        for name in sorted(set().union(*unknown.values())):
            groups[tuple(a for a in abis if name in unknown[a])].append(name)
        for g, names in groups.items():
            print('%s  not in the NDK: %s' % (','.join(g), ' '.join(names)))
    return 1 if lines else 0


def where_is(path, fn, cache):
    """file:line of a function's definition, for the report."""
    if path not in cache:
        cache[path] = open(os.path.join(ROOT, path), encoding='utf-8', errors='replace').read()
    src = cache[path]
    m = re.search(r'^[A-Za-z_][^;\n]*?\b%s\s*\(' % re.escape(fn), src, re.M) or re.search(r'\b%s\b' % re.escape(fn), src)
    line = src.count('\n', 0, m.start()) + 1 if m else 0
    return '%s:%d' % (path[len('core/src/'):], line)


if __name__ == '__main__':
    sys.exit(main())
