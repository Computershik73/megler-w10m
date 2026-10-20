/* android.content.SharedPreferences that are kept between runs (Unity's PlayerPrefs are these). See mg_jni_prefs.c. */
#ifndef MG_JNI_PREFS_H
#define MG_JNI_PREFS_H

#include "mg_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the classes and their methods; after a driver's own table, so that these answer. */
void mg_jni_prefs_install(void);
/* The SharedPreferences object of a name, read from its file the first time. */
mg_jobj *mg_jni_prefs(const char *name);

#ifdef __cplusplus
}
#endif

#endif
