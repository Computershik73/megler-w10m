/*
 * The native library of the test app `splits`, which is in the app's split for 32-bit ARM (config.armeabi_v7a) and not in
 * its base. One method, bound by its Java_ name: that it answers is that the library was found and loaded out of the split.
 */
#include <jni.h>

#include <android/log.h>

JNIEXPORT jint JNICALL Java_meglet_test_splits_Main_answer(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    __android_log_print(ANDROID_LOG_INFO, "SplitsLib", "answer from the split's library");
    return 4242;
}
