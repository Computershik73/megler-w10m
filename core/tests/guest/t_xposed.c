/*
 * A guest library that stands in for Aliucord's libaliuhook.so, for the Dalvik runtime's tests of its hooks
 * (core/tests/mg_dvm_tests_hooks.c): XposedBridge's native methods exported under their JNI names, and a JNI_OnLoad that
 * fails as the real one fails where there is no ART (LSPlant's start, JNI_ERR). The runtime is to load it without running
 * its JNI_OnLoad (core/src/dvm/mg_dvm_jvm.c, load_native) and to answer XposedBridge's natives itself
 * (core/src/dvm/mg_dvm_hook.c): none of these functions is ever to be called, and each says so if it is.
 */
typedef void *jobject;

/* How many of the functions below were called: read by the tests (none, for the runtime answers them). */
int t_xposed_called;

int JNI_OnLoad(void *vm, void *reserved)
{
    (void)vm;
    (void)reserved;
    t_xposed_called++;
    return -1;                         /* JNI_ERR */
}

jobject Java_de_robv_android_xposed_XposedBridge_hook0(void *env, jobject cls, jobject context, jobject original, jobject callback)
{
    (void)env; (void)cls; (void)context; (void)original; (void)callback;
    t_xposed_called++;
    return 0;
}

unsigned char Java_de_robv_android_xposed_XposedBridge_isHooked0(void *env, jobject cls, jobject target)
{
    (void)env; (void)cls; (void)target;
    t_xposed_called++;
    return 0;
}

unsigned char Java_de_robv_android_xposed_XposedBridge_disableHiddenApiRestrictions(void *env, jobject cls)
{
    (void)env; (void)cls;
    t_xposed_called++;
    return 0;
}
