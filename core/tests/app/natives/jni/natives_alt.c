/*
 * A library of the APK's that nothing loads by its name. The step own-file-same-name copies it into the app's files with the
 * "version 1" of its text made "version 2": a library of the APK's name and size and other bytes, as an app's own build of
 * one is (TikTok Lite unpacks its libnpth.so so), and loads that file by its path. On Android System.load loads the file it is
 * given, and the answer is the copy's.
 */
#include <jni.h>

static const char k_version[] = "natives_alt version 1";

JNIEXPORT jstring JNICALL Java_megler_test_natives_Alt_version(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, k_version);
}
