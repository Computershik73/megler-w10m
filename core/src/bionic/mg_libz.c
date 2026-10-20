/*
 * libz.so: the vendored zlib, as it is. Its interface is integers, pointers
 * and a z_stream whose layout is the same in 32-bit Android and here, and the
 * allocation callbacks it may be given are called with plain integer
 * arguments, so nothing needs wrapping.
 */
#include "mg_bionic.h"

#include "zlib.h"

const mg_sym mg_tab_zlib[] = {
    MG_DIRECT(zlibVersion), MG_DIRECT(deflate), MG_DIRECT(deflateEnd), MG_DIRECT(inflate), MG_DIRECT(inflateEnd),
    MG_DIRECT(deflateSetDictionary), MG_DIRECT(deflateCopy), MG_DIRECT(deflateReset), MG_DIRECT(deflateParams),
    MG_DIRECT(deflateTune), MG_DIRECT(deflateBound), MG_DIRECT(deflatePrime), MG_DIRECT(deflateSetHeader),
    MG_DIRECT(inflateSetDictionary), MG_DIRECT(inflateSync), MG_DIRECT(inflateCopy), MG_DIRECT(inflateReset),
    MG_DIRECT(inflateReset2), MG_DIRECT(inflatePrime), MG_DIRECT(inflateMark), MG_DIRECT(inflateGetHeader),
    MG_DIRECT(inflateBack), MG_DIRECT(inflateBackEnd), MG_DIRECT(zlibCompileFlags), MG_DIRECT(compress), MG_DIRECT(compress2),
    MG_DIRECT(compressBound), MG_DIRECT(uncompress), MG_DIRECT(adler32), MG_DIRECT(crc32), MG_DIRECT(adler32_combine),
    MG_DIRECT(crc32_combine), MG_DIRECT(deflateInit_), MG_DIRECT(inflateInit_), MG_DIRECT(deflateInit2_), MG_DIRECT(inflateInit2_),
    MG_DIRECT(inflateBackInit_), MG_DIRECT(zError), MG_DIRECT(inflateSyncPoint), MG_DIRECT(get_crc_table),
    MG_END
};
