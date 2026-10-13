/* android.content.SharedPreferences that are kept between runs (Unity's PlayerPrefs are these). See hk_jni_prefs.c. */
#ifndef HK_JNI_PREFS_H
#define HK_JNI_PREFS_H

#include "hk_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the classes and their methods; after a driver's own table, so that these answer. */
void hk_jni_prefs_install(void);
/* The SharedPreferences object of a name, read from its file the first time. */
hk_jobj *hk_jni_prefs(const char *name);

#ifdef __cplusplus
}
#endif

#endif
