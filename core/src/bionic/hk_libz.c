/*
 * libz.so: the vendored zlib, as it is. Its interface is integers, pointers
 * and a z_stream whose layout is the same in 32-bit Android and here, and the
 * allocation callbacks it may be given are called with plain integer
 * arguments, so nothing needs wrapping.
 */
#include "hk_bionic.h"

#include "zlib.h"

const hk_sym hk_tab_zlib[] = {
    HK_DIRECT(zlibVersion), HK_DIRECT(deflate), HK_DIRECT(deflateEnd), HK_DIRECT(inflate), HK_DIRECT(inflateEnd),
    HK_DIRECT(deflateSetDictionary), HK_DIRECT(deflateCopy), HK_DIRECT(deflateReset), HK_DIRECT(deflateParams),
    HK_DIRECT(deflateTune), HK_DIRECT(deflateBound), HK_DIRECT(deflatePrime), HK_DIRECT(deflateSetHeader),
    HK_DIRECT(inflateSetDictionary), HK_DIRECT(inflateSync), HK_DIRECT(inflateCopy), HK_DIRECT(inflateReset),
    HK_DIRECT(inflateReset2), HK_DIRECT(inflatePrime), HK_DIRECT(inflateMark), HK_DIRECT(inflateGetHeader),
    HK_DIRECT(inflateBack), HK_DIRECT(inflateBackEnd), HK_DIRECT(zlibCompileFlags), HK_DIRECT(compress), HK_DIRECT(compress2),
    HK_DIRECT(compressBound), HK_DIRECT(uncompress), HK_DIRECT(adler32), HK_DIRECT(crc32), HK_DIRECT(adler32_combine),
    HK_DIRECT(crc32_combine), HK_DIRECT(deflateInit_), HK_DIRECT(inflateInit_), HK_DIRECT(deflateInit2_), HK_DIRECT(inflateInit2_),
    HK_DIRECT(inflateBackInit_), HK_DIRECT(zError), HK_DIRECT(inflateSyncPoint), HK_DIRECT(get_crc_table),
    HK_END
};
