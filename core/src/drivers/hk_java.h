/* The Java driver's own interface to the rest of the core and to the tests (hk_java.c). */
#ifndef HK_JAVA_H
#define HK_JAVA_H

#include "../hk_base.h"
#include "../jni/hk_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the running Java app has done so far: pictures handed to the shell, and turns of its main loop that asked for input. */
typedef struct hk_java_stats {
    volatile i32 frames;
    volatile i32 polls;
} hk_java_stats;
void hk_java_get_stats(hk_java_stats *out);
/*
 * For the tests: every frame a window draws in part (its dirty region) is checked against the same frame drawn whole, and a
 * difference is a line in the log ("ViewRoot: ... drawn in part missed"). Set before the app starts. Costs a second drawing
 * of the screen per frame.
 */
void hk_java_check_partial(bool on);
/*
 * Whether the app's main thread has ended, and how: the status and the
 * reason hk_run_ended was given. For a test that starts the driver itself, on
 * a host where megler_run refuses every app because guest code cannot run
 * there -- which a Java app without native libraries does not need.
 */
bool hk_java_ended(int *status, const char **reason);

/*
 * The shell's keyboard (megler.h, megler_keyboard_host): text, editing keys, the keyboard put away by the user, and how much of
 * the surface it covers. From any thread; queued for the app's main thread. Nothing happens when no Java app runs.
 */
void hk_java_text(const char *utf8);
void hk_java_text_key(int key);
void hk_java_keyboard_closed(void);
void hk_java_keyboard_height(int pixels);

/* hk_java_sqlite.c: megler.Sqlite's native methods, on the SQLite compiled into the core -- or, in a build without it, their refusal. */
const hk_jhle *hk_java_sqlite_natives(void);

/* hk_java_webview.c: what Android's platform gives the WebView besides Java (its library loader, its platform support). */
const hk_jhle *hk_java_webview_natives(void);

#ifdef __cplusplus
}
#endif

#endif
