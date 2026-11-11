/*
 * The app's native half: an asset read through the NDK's AAssetManager, given the AssetManager of a context -- the app's own,
 * or Google Play services' (their package context's), as the map library inside Google Play services reads its sprites and
 * glyphs (microG's libmapbox-gl.so: AAssetManager_fromJava on its own context's AssetManager, AAssetManager_open,
 * AAsset_getBuffer and AAsset_getLength64). Main says what each gave.
 */
#include <jni.h>
#include <string.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

/* The asset's text, its line end left out; NULL where the manager has no such asset. */
JNIEXPORT jstring JNICALL Java_megler_test_gmshost_Main_readAsset(JNIEnv *env, jclass cls, jobject assets, jstring name)
{
    AAssetManager *am = AAssetManager_fromJava(env, assets);
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    AAsset *a = am ? AAssetManager_open(am, n, AASSET_MODE_BUFFER) : NULL;
    char text[200];
    size_t len = 0;
    (void)cls;
    (*env)->ReleaseStringUTFChars(env, name, n);
    if (!a) return NULL;
    if (AAsset_getBuffer(a) && AAsset_getLength64(a) < (off64_t)sizeof(text)) {
        len = (size_t)AAsset_getLength64(a);
        memcpy(text, AAsset_getBuffer(a), len);
    }
    AAsset_close(a);
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    text[len] = 0;
    return (*env)->NewStringUTF(env, text);
}

/* The names of a folder's assets, each followed by a space; "" for a folder the manager has nothing in. */
JNIEXPORT jstring JNICALL Java_megler_test_gmshost_Main_listAssets(JNIEnv *env, jclass cls, jobject assets, jstring folder)
{
    AAssetManager *am = AAssetManager_fromJava(env, assets);
    const char *f = (*env)->GetStringUTFChars(env, folder, NULL);
    AAssetDir *d = am ? AAssetManager_openDir(am, f) : NULL;
    char names[400];
    const char *name;
    size_t at = 0;
    (void)cls;
    (*env)->ReleaseStringUTFChars(env, folder, f);
    while (d && (name = AAssetDir_getNextFileName(d)) != NULL) {
        size_t n = strlen(name);
        if (at + n + 2 > sizeof(names)) break;
        memcpy(names + at, name, n);
        at += n;
        names[at++] = ' ';
    }
    if (d) AAssetDir_close(d);
    names[at] = 0;
    return (*env)->NewStringUTF(env, names);
}

/* Whether two AssetManagers are one manager to native code. */
JNIEXPORT jboolean JNICALL Java_megler_test_gmshost_Main_sameManager(JNIEnv *env, jclass cls, jobject a, jobject b)
{
    (void)cls;
    return AAssetManager_fromJava(env, a) == AAssetManager_fromJava(env, b) ? JNI_TRUE : JNI_FALSE;
}
