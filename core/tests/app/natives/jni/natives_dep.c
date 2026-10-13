/*
 * The library the test app's main library needs (DT_NEEDED): loaded with it,
 * initialised before it, never named by the app.
 */
static int g_constructed;

__attribute__((constructor)) static void dep_construct(void) { g_constructed = 4000; }

/* 4242 when this library's constructor has run. */
int natives_dep_answer(void) { return g_constructed + 242; }

/* Something of the caller's own, handed back changed: a call across the two libraries with a 64-bit value in it. */
long long natives_dep_scale(long long v, int by) { return v * by; }
