/*
 * Stands in for external/icu/icu4c/source/stubdata/stubdata.cpp (AOSP's static
 * library libicuuc_stubdata), which tools/android9-sources.sh does not fetch:
 * only icu4c/source/common and icu4c/source/i18n are on disk.
 *
 * libicuuc refers to one data symbol, icudt60_dat (U_ICUDATA_ENTRY_POINT in
 * common/udata.cpp): "the data linked into the library". Android links no
 * data in. It links this: a common-data header with an empty table of
 * contents, so that every lookup in it fails and ICU goes on to the data
 * handed to it at run time with udata_setCommonData() -- which is what
 * libjavacore does with $ANDROID_ROOT/usr/icu/icudt60l.dat when it is loaded
 * (ICURegistration in libcore_icu_ICU.cpp).
 *
 * The layout is the one common/ucmndata.cpp reads: a DataHeader (MappedData,
 * UDataInfo), padded to the header size it declares, then a "ToCP" pointer
 * table of contents with a count of zero. The 56 bytes this compiles to are
 * the 56 bytes of icudt60_dat in Android 9's own libicuuc.so (the arm64 one of
 * the emulator image, where the symbol has that size); the build script
 * compares them when that library is there.
 */
#include "unicode/utypes.h"
#include "unicode/udata.h"
#include "unicode/uversion.h"

typedef struct {
    uint16_t headerSize;
    uint8_t magic1, magic2;
    UDataInfo info;
    char padding[8];
    uint32_t count, reserved;
    int fakeNameAndData[4];     /* one entry's worth of nothing after a count of none */
} ICU_Data_Header;

extern "C" U_EXPORT const ICU_Data_Header U_ICUDATA_ENTRY_POINT = {
    32,                         /* headerSize: up to the table of contents */
    0xda, 0x27,                 /* the magic of every ICU data header */
    {
        sizeof(UDataInfo),
        0,
        U_IS_BIG_ENDIAN,
        U_CHARSET_FAMILY,
        sizeof(UChar),
        0,
        { 0x54, 0x6f, 0x43, 0x50 },     /* dataFormat "ToCP": a table of contents of pointers */
        { 1, 0, 0, 0 },                 /* formatVersion */
        { 0, 0, 0, 0 }                  /* dataVersion */
    },
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    0,                          /* count: nothing is in here */
    0,
    { 0, 0, 0, 0 }
};
