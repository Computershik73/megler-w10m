/* What an APK says about itself: its manifest, its resources (as far as a label and an icon), its native libraries. */
#ifndef HK_APK_H
#define HK_APK_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_apk_meta {
    char name[96], value[200];
} hk_apk_meta;

/* android:screenOrientation of the launcher activity, reduced to what a shell can act on before the app runs. */
enum { HK_APK_UNSPECIFIED = 0, HK_APK_LANDSCAPE, HK_APK_PORTRAIT };

typedef struct hk_apk_info {
    char error[200];               /* why the APK could not be read at all */
    char package[160], label[160], version_name[64];
    int version_code, min_sdk, target_sdk;
    char abis[8][24];
    int nabis;
    char abi[24];                  /* the one this layer would use; empty when none */
    bool has_code;                 /* android:hasCode: whether the app has Java code of its own */
    char activity[200];            /* the launcher activity's class, fully qualified */
    int orientation;               /* how the launcher activity asks to be held: HK_APK_UNSPECIFIED, _LANDSCAPE or _PORTRAIT */
    char application[200];         /* the Application subclass, if any */
    char native_library[96];       /* android.app.lib_name of the launcher activity */
    char func_name[96];            /* android.app.func_name */
    hk_apk_meta meta[48];          /* <meta-data> of the application and the launcher activity */
    int nmeta;
    char engine[32];
    bool supported, runnable;
    char reason[240];
    char icon_path[200];           /* the icon's entry in the APK, when one was found */
    char notes[8][200];
    int nnotes;
} hk_apk_info;

/* False, with info->error set, only when the file is not a readable APK. */
bool hk_apk_read_info(const char *apk_path, hk_apk_info *info);
void hk_apk_info_free(hk_apk_info *info);
const char *hk_apk_meta_value(const hk_apk_info *info, const char *name);

#ifdef __cplusplus
}
#endif

#endif
