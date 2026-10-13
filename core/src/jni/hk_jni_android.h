#ifndef HK_JNI_ANDROID_H
#define HK_JNI_ANDROID_H

#include "hk_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sets up the Java world every driver starts from -- the activity, its
 * Context methods, files, the device's Build, the display -- and returns the
 * activity object. `activity_class` is a JNI name ("com/example/MainActivity").
 */
hk_jobj *hk_jni_android_setup(const char *activity_class, const char *package, const char *apk_host_path, int width, int height, int density_dpi);
hk_jobj *hk_jni_android_activity(void);
/* The objects a driver adds answers to: the default Display, the app's ApplicationInfo. */
hk_jobj *hk_jni_android_display(void);
hk_jobj *hk_jni_android_app_info(void);
/* Fills a DisplayMetrics object's fields. */
void hk_jni_android_fill_metrics(hk_jobj *metrics);
/* Where the app's native libraries are, as Android would have unpacked them ("/data/app/<package>-1/lib/arm"). */
const char *hk_jni_android_lib_dir(void);

#ifdef __cplusplus
}
#endif

#endif
