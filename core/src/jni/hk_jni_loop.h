/*
 * Android's message loops (Looper, Handler, HandlerThread, Choreographer),
 * the boxed numbers their callbacks are handed, and the Java proxies native
 * code implements interfaces with. See hk_jni_loop.c.
 */
#ifndef HK_JNI_LOOP_H
#define HK_JNI_LOOP_H

#include "hk_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the classes and their methods. Once, before the app's code asks for any of them. */
void hk_jni_loop_install(void);
/* The android.os.Looper of the app's main ("UI") thread; its thread starts when something is first sent to it. */
hk_jobj *hk_jni_loop_main(void);
/*
 * Calls a method of an interface on a proxy the app made for it
 * (JNIBridge.newInterfaceProxy), as Java would: the app's native code is
 * handed the method and the arguments, boxed. Returns what it returned (a
 * reference, or NULL); NULL too when `proxy` is not a proxy.
 */
hk_jobj *hk_jni_proxy_call(hk_jobj *proxy, const char *iface, const char *name, const char *sig, hk_jobj **args, int nargs);
/* Runs `fn(arg)` on the thread of a Handler's looper (the main looper's for NULL), after whatever is queued there already. */
void hk_jni_loop_call(hk_jobj *handler, void (*fn)(void *arg), void *arg);
/* A boxed value: hk_jni_box("java/lang/Integer", 'I', v). The kinds are JNI's letters. */
hk_jobj *hk_jni_box(const char *cls, char kind, hk_jvalue v);
/* Every loop's thread is told to end and waited for: the app is going away. */
void hk_jni_loop_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
