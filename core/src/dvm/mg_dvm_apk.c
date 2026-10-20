/* DEX files out of an APK or a jar. Apart from the rest of the runtime because this alone needs the zip reader, and with it zlib. */
#include "mg_dvm_internal.h"

#include <stdio.h>

#include "../zip/mg_zip.h"

int mg_dvm_add_apk(const char *path)
{
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\'), *base;
    mg_zip z;
    char err[160];
    int added = 0, i;
    if (back && (!slash || back > slash)) slash = back;
    base = slash ? slash + 1 : path;
    if (!mg_zip_open(&z, path, err, sizeof(err))) {
        mg_warn("dvm: %s: %s", path, err);
        return 0;
    }
    /* classes.dex, classes2.dex, classes3.dex... with no gap, as Android reads them. */
    for (i = 1; i < 100; i++) {
        const mg_zip_entry *e;
        char name[32], label[96];
        size_t size = 0;
        u8 *data;
        if (i == 1) snprintf(name, sizeof(name), "classes.dex");
        else snprintf(name, sizeof(name), "classes%d.dex", i);
        e = mg_zip_find(&z, name);
        if (!e) break;
        /* Read whole into memory of its own: the classes are used from there for as long as the process lives. */
        data = mg_zip_read(&z, e, (size_t)256 << 20, &size);
        if (!data) {
            mg_warn("dvm: %s: %s cannot be read", path, name);
            continue;
        }
        snprintf(label, sizeof(label), "%.60s!%s", base, name);
        if (mg_dvm_add_dex(data, size, label)) added++;
        else free(data);
    }
    mg_zip_close(&z);
    return added;
}
