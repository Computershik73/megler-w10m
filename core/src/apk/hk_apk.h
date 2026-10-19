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

/* As many split APKs as an app is read with (the Java driver opens ten of them: drivers/hk_java.c). */
#define HK_APK_MAX_SPLITS 16

typedef struct hk_apk_info {
    char error[200];               /* why the APK could not be read at all */
    char package[160], label[160], version_name[64];
    int version_code, min_sdk, target_sdk;
    char abis[8][24];              /* every ABI there is native code for: in the APKs read, and in a bundle's splits that were left out */
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
    /* The platform's libraries the application asks for (<uses-library android:name>, required or not), in the manifest's order. */
    char uses_libraries[8][96];
    int nuses_libraries;
    char engine[32];
    bool supported, runnable;
    char reason[240];
    char icon_path[200];           /* the icon's entry, when one was found ... */
    int icon_apk;                  /* ... in this APK: 0 the base, n the nth of the splits given to hk_apk_read_app */
    u32 icon_id, label_id;         /* the resources the manifest names as the icon and the label, 0 for none */
    char notes[8][200];
    int nnotes;
    /*
     * Split APKs. A split's own manifest names it (split="config.armeabi_v7a") and, for a split of a feature split, the
     * feature it belongs to (configForSplit); a feature split says so (android:isFeatureSplit). These are empty and false
     * for a base or a whole APK.
     */
    char split[96], config_for_split[96];
    bool feature_split;
    /*
     * A base's: whether it says it is not whole without splits -- android:isSplitRequired, android:requiredSplitTypes (which
     * kinds, as "base__abi,base__density"), Google Play's com.android.vending.splits.required -- and those kinds.
     */
    bool split_required;
    char required_split_types[160];
    /* The splits the app was read with (hk_apk_read_app), by their names, in the order given. */
    char splits[HK_APK_MAX_SPLITS][96];
    int nsplits;
    /* What the unpacking of the bundle the app came in left out (its record, core/src/apk/hk_bundle.h): the splits' names. */
    char left_out[32][96];
    int nleft_out;
} hk_apk_info;

/* False, with info->error set, only when the file is not a readable APK. */
bool hk_apk_read_info(const char *apk_path, hk_apk_info *info);
/*
 * The same for an app in several APKs: the base at apk_path and its splits, read as one app, as Android has it once the
 * splits are installed -- the base's manifest, the native libraries of every APK, the icon wherever the densest picture
 * of it is. A split that is another app's or no split at all is left out and noted. `record_dir`, when not NULL, is a
 * folder megler_bundle_unpack made, whose record says what of the bundle was left out: a bundle's splits for other
 * processors count as the app's ABIs. False only when the base cannot be read.
 */
bool hk_apk_read_app(const char *apk_path, const char *const *splits, int nsplits, const char *record_dir, hk_apk_info *info);
/* The split APKs in a folder: its .apk files, sorted by name, as malloc'd paths in a malloc'd array (free each, then it). */
int hk_apk_list_splits(const char *dir, char ***paths);
void hk_apk_free_list(char **paths, int n);
void hk_apk_info_free(hk_apk_info *info);
const char *hk_apk_meta_value(const hk_apk_info *info, const char *name);

#ifdef __cplusplus
}
#endif

#endif
