/*
 * A whole app, start to end (included by mg_tests.c).
 *
 * native-test.apk through megler_run: its manifest read, its library loaded,
 * the NativeActivity driver playing Android's part, the app's own thread
 * running a looper, bringing up EGL and drawing. OpenGL ES is a stand-in here
 * that records what it was called with -- with real floats, as ANGLE would
 * be, so the adapters that carry floats over from the guest are checked too.
 */
static struct {
    float clear[4], color[4], time, line_width;
    int viewport[4], clears, draws, draw_count, shaders, surface_type, pbuffer[2], current;
} M;

static void *m_GetPlatformDisplay(unsigned platform, void *native, const int *attribs) { (void)platform; (void)native; (void)attribs; return (void *)(mg_addr)0xD15; }
static unsigned m_Initialize(void *d, int *major, int *minor)
{
    (void)d;
    if (major) *major = 1;
    if (minor) *minor = 5;
    return 1;
}
static int m_GetError(void) { return 0x3000; }
static const char *m_QueryString(void *d, int name) { (void)d; (void)name; return "stand-in"; }
static unsigned m_ChooseConfig(void *d, const int *attribs, void **configs, int size, int *num)
{
    int i;
    (void)d;
    for (i = 0; attribs && attribs[i] != 0x3038; i += 2)
        if (attribs[i] == 0x3033) M.surface_type = attribs[i + 1];
    if (configs && size > 0) configs[0] = (void *)(mg_addr)0xC0F;
    *num = 1;
    return 1;
}
static unsigned m_GetConfigAttrib(void *d, void *c, int attrib, int *value)
{
    (void)d; (void)c;
    *value = attrib == 0x3033 ? 1 : 8;
    return 1;
}
static void *m_CreatePbufferSurface(void *d, void *c, const int *attribs)
{
    int i;
    (void)d; (void)c;
    for (i = 0; attribs && attribs[i] != 0x3038; i += 2) {
        if (attribs[i] == 0x3057) M.pbuffer[0] = attribs[i + 1];
        if (attribs[i] == 0x3056) M.pbuffer[1] = attribs[i + 1];
    }
    return (void *)(mg_addr)0x5F;
}
static void *m_CreateContext(void *d, void *c, void *share, const int *attribs) { (void)d; (void)c; (void)share; (void)attribs; return (void *)(mg_addr)0xC7; }
static unsigned m_MakeCurrent(void *d, void *draw, void *read, void *ctx) { (void)d; (void)draw; (void)read; M.current = ctx != NULL; return 1; }
static unsigned m_Destroy(void *d, void *what) { (void)d; (void)what; return 1; }
static void m_Viewport(int x, int y, int w, int h) { M.viewport[0] = x; M.viewport[1] = y; M.viewport[2] = w; M.viewport[3] = h; }
static void m_ClearColor(float r, float g, float b, float a) { M.clear[0] = r; M.clear[1] = g; M.clear[2] = b; M.clear[3] = a; }
static void m_Clear(unsigned mask) { (void)mask; M.clears++; }
static unsigned m_CreateShader(unsigned type) { (void)type; return (unsigned)++M.shaders; }
static void m_ShaderSource(unsigned s, int n, const char *const *text, const int *len) { (void)s; (void)n; (void)text; (void)len; }
static void m_OneUnsigned(unsigned v) { (void)v; }
static unsigned m_CreateProgram(void) { return 77; }
static void m_AttachShader(unsigned p, unsigned s) { (void)p; (void)s; }
static int m_GetUniformLocation(unsigned p, const char *name) { (void)p; return name[2] == 'c' ? 1 : 2; }
static int m_GetAttribLocation(unsigned p, const char *name) { (void)p; (void)name; return 0; }
static void m_Uniform1f(int loc, float v) { if (loc == 2) M.time = v; }
static void m_Uniform4f(int loc, float a, float b, float c, float d)
{
    if (loc == 1) { M.color[0] = a; M.color[1] = b; M.color[2] = c; M.color[3] = d; }
}
static void m_LineWidth(float w) { M.line_width = w; }
static void m_VertexAttribPointer(unsigned i, int size, unsigned type, unsigned char norm, int stride, const void *p) { (void)i; (void)size; (void)type; (void)norm; (void)stride; (void)p; }
static void m_DrawArrays(unsigned mode, int first, int count) { (void)mode; (void)first; M.draws++; M.draw_count = count; }
static const unsigned char *m_GetString(unsigned name) { (void)name; return (const unsigned char *)"OpenGL ES 2.0 (stand-in)"; }
static void m_Flush(void) { }
/* What only --run needs: an app that asks whether its shaders compiled is told that they did. */
static void m_GetStatus(unsigned object, unsigned name, int *value) { (void)object; (void)name; if (value) *value = 1; }
static void m_GetInfoLog(unsigned object, int size, int *length, char *text) { (void)object; if (length) *length = 0; if (text && size > 0) text[0] = 0; }
static void m_BindAttribLocation(unsigned p, unsigned index, const char *name) { (void)p; (void)index; (void)name; }
static unsigned m_GetGLError(void) { return 0; }

static void *stand_in(const char *name)
{
    static const struct { const char *name; void *fn; } table[] = {
        { "eglGetPlatformDisplayEXT", (void *)m_GetPlatformDisplay }, { "eglInitialize", (void *)m_Initialize }, { "eglGetError", (void *)m_GetError },
        { "eglQueryString", (void *)m_QueryString }, { "eglChooseConfig", (void *)m_ChooseConfig }, { "eglGetConfigAttrib", (void *)m_GetConfigAttrib },
        { "eglCreatePbufferSurface", (void *)m_CreatePbufferSurface }, { "eglCreateContext", (void *)m_CreateContext },
        { "eglMakeCurrent", (void *)m_MakeCurrent }, { "eglDestroySurface", (void *)m_Destroy }, { "eglDestroyContext", (void *)m_Destroy },
        { "glViewport", (void *)m_Viewport }, { "glClearColor", (void *)m_ClearColor }, { "glClear", (void *)m_Clear },
        { "glCreateShader", (void *)m_CreateShader }, { "glShaderSource", (void *)m_ShaderSource }, { "glCompileShader", (void *)m_OneUnsigned },
        { "glCreateProgram", (void *)m_CreateProgram }, { "glAttachShader", (void *)m_AttachShader }, { "glLinkProgram", (void *)m_OneUnsigned },
        { "glUseProgram", (void *)m_OneUnsigned }, { "glGetUniformLocation", (void *)m_GetUniformLocation },
        { "glGetAttribLocation", (void *)m_GetAttribLocation }, { "glUniform1f", (void *)m_Uniform1f }, { "glUniform4f", (void *)m_Uniform4f },
        { "glLineWidth", (void *)m_LineWidth }, { "glVertexAttribPointer", (void *)m_VertexAttribPointer },
        { "glEnableVertexAttribArray", (void *)m_OneUnsigned }, { "glDrawArrays", (void *)m_DrawArrays }, { "glGetString", (void *)m_GetString },
        { "glFlush", (void *)m_Flush }, { "glGetShaderiv", (void *)m_GetStatus }, { "glGetProgramiv", (void *)m_GetStatus },
        { "glGetShaderInfoLog", (void *)m_GetInfoLog }, { "glGetProgramInfoLog", (void *)m_GetInfoLog },
        { "glBindAttribLocation", (void *)m_BindAttribLocation }, { "glGetError", (void *)m_GetGLError },
    };
    size_t i;
    for (i = 0; i < MG_COUNT(table); i++)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return NULL;
}

/* t_native.c's record of what it saw. */
struct t_native_state {
    int started, resumed, paused, stopped, destroyed, focused;
    int window_width, window_height, surface_width, surface_height;
    int frames, touches, last_action, pointers, keys, last_key;
    float last_x, last_y;
    int density, asset_length, package_ok, files_dir_ok, sdk_int, finished_drawing;
    char asset_text[32];
    int audio_ok, audio_callbacks;
};

/* The shell's audio output, standing in: what was handed over, counted. */
static struct {
    int rate, frames, loud, peak, mismatched;
} G_AUDIO;

static bool audio_open(void *user, int rate)
{
    (void)user;
    G_AUDIO.rate = rate;
    return true;
}

static void audio_write(void *user, const int16_t *samples, int frames)
{
    int i;
    (void)user;
    for (i = 0; i < frames; i++) {
        int l = samples[i * 2], r = samples[i * 2 + 1];
        if (l != r) G_AUDIO.mismatched++;
        if (l) G_AUDIO.loud++;
        if (l > G_AUDIO.peak) G_AUDIO.peak = l;
    }
    G_AUDIO.frames += frames;
    /* A real output takes ten milliseconds to play ten milliseconds; a fraction of that keeps the test short. */
    mg_sleep_ns(1000000ull);
}

static volatile i32 g_app_exited;
static int g_app_status;
static void app_exited(void *user, int status, const char *reason)
{
    (void)user; (void)reason;
    g_app_status = status;
    mg_atomic_xchg(&g_app_exited, 1);
}

/* Waits for a condition the app's threads bring about, up to five seconds. */
#define WAIT_FOR(cond)                                                                 \
    do {                                                                               \
        int tries_;                                                                    \
        for (tries_ = 0; tries_ < 1000 && !(cond); tries_++) mg_sleep_ns(5000000ull);  \
    } while (0)

static void test_app(const char *libdir)
{
    char *apk = mg_sprintf("%s/native-test.apk", libdir), *report, *saved;
    megler_run_config cfg;
    volatile struct t_native_state *s;
    mg_lib *lib;
    mg_stat st;
    int frames;
    say("an app, start to end (NativeActivity)");
    if (!megler_data_dir()[0]) {
        say("  (skipped: no data directory -- megler_init was not called)");
        free(apk);
        return;
    }
    report = megler_apk_report(apk);
    CHECK(report && strstr(report, "\"ok\":true"));
    if (report) {
        CHECK(strstr(report, "\"package\":\"org.megler.test.native\""));
        CHECK(strstr(report, "\"label\":\"Megler Native Test\""));
        CHECK(strstr(report, "\"versionName\":\"1.2\"") && strstr(report, "\"versionCode\":7"));
        CHECK(strstr(report, "\"minSdk\":16") && strstr(report, "\"targetSdk\":28"));
        CHECK(strstr(report, "\"abi\":\"armeabi-v7a\""));
        CHECK(strstr(report, "\"engine\":\"native-activity\"") && strstr(report, "\"supported\":true"));
        /* Its manifest does not say how it wants to be held. */
        CHECK(strstr(report, "\"orientation\":\"unspecified\"") != NULL);
        CHECK(strstr(report, "\"nativeLibrary\":\"native-test\""));
        CHECK(strstr(report, "\"name\":\"libnative-test.so\"") && strstr(report, "\"importsNotInSystem\":0"));
        CHECK(strstr(report, "\"runnable\":true"));
        megler_free(report);
    }

    memset(&M, 0, sizeof(M));
    memset(&cfg, 0, sizeof(cfg));
    mg_egl_set_provider(stand_in);
    cfg.apk_path = apk;
    cfg.width = 640;
    cfg.height = 360;
    cfg.density_dpi = 240;
    cfg.on_exit = app_exited;
    cfg.audio.open = audio_open;
    cfg.audio.write = audio_write;
    memset(&G_AUDIO, 0, sizeof(G_AUDIO));
    CHECK(megler_run(&cfg));
    if (!megler_running()) {
        say("  not running: %s", megler_last_error());
        free(apk);
        return;
    }
    lib = mg_ld_find_lib("libnative-test.so");
    CHECK(lib != NULL);
    s = lib ? (volatile struct t_native_state *)mg_ld_sym(lib, "t_native_state") : NULL;
    CHECK(s != NULL);
    if (!s) {
        free(apk);
        return;
    }
    WAIT_FOR(megler_frames() >= 5 && s->focused);
    CHECK(megler_frames() >= 5);

    /* What the app was told and what it found out for itself. */
    CHECK_INT(s->started, 1);
    CHECK_INT(s->resumed, 1);
    CHECK_INT(s->focused, 1);
    CHECK(s->window_width == 640 && s->window_height == 360 && s->surface_width == 640 && s->surface_height == 360);
    CHECK_INT(s->package_ok, 1);
    CHECK_INT(s->files_dir_ok, 1);
    CHECK_INT(s->sdk_int, 28);
    CHECK_INT(s->density, 240);
    CHECK_INT(s->asset_length, 11);
    CHECK(!strcmp((const char *)s->asset_text, "hello asset"));

    /* What reached "OpenGL ES": the window surface as a pbuffer, and floats intact. */
    CHECK((M.surface_type & 1) && !(M.surface_type & 4));
    CHECK(M.pbuffer[0] == 640 && M.pbuffer[1] == 360 && M.current);
    CHECK(M.viewport[2] == 640 && M.viewport[3] == 360);
    CHECK(M.clear[0] == 0.25f && M.clear[1] == 0.5f && M.clear[2] == 0.75f && M.clear[3] == 1.0f);
    CHECK(M.color[0] == 1.0f && M.color[1] == 0.5f && M.color[2] == 0.25f && M.color[3] == 1.0f);
    CHECK(M.line_width == 2.5f && M.time > 0.0f);
    CHECK(M.draws >= 5 && M.draw_count == 3 && M.clears >= 5);

    /*
     * Sound: 0.3 s of a constant level at 22050 Hz mono, in three buffers each
     * queued from the callback of the one before, arrives as 0.3 s at 44100 Hz
     * on both channels.
     */
    WAIT_FOR(s->audio_callbacks >= 3 && G_AUDIO.loud >= 13000);
    CHECK_INT(s->audio_ok, 1);
    CHECK_INT(s->audio_callbacks, 3);
    CHECK_INT(G_AUDIO.rate, 44100);
    CHECK(G_AUDIO.loud >= 13200 && G_AUDIO.loud <= 13260);
    CHECK(G_AUDIO.peak >= 8188 && G_AUDIO.peak <= 8192 && G_AUDIO.mismatched == 0);

    /* Touch, with a second finger; the event's floats go back to the guest as bits. */
    megler_touch(0, 7, 100.5f, 50.25f);
    WAIT_FOR(s->touches >= 1);
    CHECK(s->touches == 1 && s->last_action == 0 && s->pointers == 1 && s->last_x == 100.5f && s->last_y == 50.25f);
    megler_touch(0, 9, 300.0f, 200.0f);
    WAIT_FOR(s->touches >= 2);
    CHECK(s->last_action == (5 | (1 << 8)) && s->pointers == 2);
    megler_touch(1, 7, 110.0f, 60.0f);
    WAIT_FOR(s->touches >= 3);
    CHECK(s->last_action == 2 && s->last_x == 110.0f && s->last_y == 60.0f);
    megler_touch(2, 9, 300.0f, 200.0f);
    WAIT_FOR(s->touches >= 4);
    CHECK(s->last_action == (6 | (1 << 8)));
    megler_touch(2, 7, 110.0f, 60.0f);
    WAIT_FOR(s->touches >= 5);
    CHECK(s->last_action == 1 && s->pointers == 1);

    /* Losing focus stops its drawing; getting it back starts it again. */
    megler_set_focused(false);
    WAIT_FOR(!s->focused);
    mg_sleep_ns(30000000ull);
    frames = (int)megler_frames();
    mg_sleep_ns(60000000ull);
    CHECK_INT(megler_frames(), frames);
    megler_set_paused(true);
    WAIT_FOR(s->paused >= 1);
    megler_set_paused(false);
    WAIT_FOR(s->resumed >= 2);
    megler_set_focused(true);
    WAIT_FOR((int)megler_frames() > frames + 2);
    CHECK((int)megler_frames() > frames + 2);
    CHECK(s->paused == 1 && s->resumed == 2);

    /* Back: the app asks to finish, and is taken down the way an activity is. */
    megler_key(4, true);
    WAIT_FOR(g_app_exited);
    CHECK(g_app_exited && g_app_status == 0);
    CHECK(s->keys == 1 && s->last_key == 4);
    CHECK(s->paused == 2 && s->stopped == 1 && s->destroyed == 1 && s->finished_drawing == 1);
    CHECK(!megler_running());

    /* What it saved is where this layer keeps the app's files. */
    saved = mg_sprintf("%s/apps/org.megler.test.native/data/files/save.dat", megler_data_dir());
    CHECK(mg_path_stat(saved, &st, NULL) && st.size == 5);
    free(saved);
    mg_egl_set_provider(NULL);
    free(apk);
}

/*
 * An APK run for a while with the stand-in for OpenGL ES, two fingers put down
 * and moved, then Back: enough to see that an app starts, draws, takes input,
 * makes sound and ends, before it is tried on a phone (mg_tests --run).
 */
int mg_tests_smoke_run(const char *apk, int milliseconds)
{
    megler_run_config cfg;
    int i;
    g_sink = NULL;
    memset(&M, 0, sizeof(M));
    memset(&G_AUDIO, 0, sizeof(G_AUDIO));
    memset(&cfg, 0, sizeof(cfg));
    mg_egl_set_provider(stand_in);
    cfg.apk_path = apk;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.density_dpi = 320;
    cfg.on_exit = app_exited;
    cfg.audio.open = audio_open;
    cfg.audio.write = audio_write;
    if (!megler_run(&cfg) || !megler_running()) {
        say("not running: %s", megler_last_error());
        return 1;
    }
    WAIT_FOR(megler_frames() >= 5 || g_app_exited);
    say("%d frames after the start", (int)megler_frames());
    megler_touch(0, 1, 200.0f, 100.0f);
    megler_touch(0, 2, 900.0f, 500.0f);
    for (i = 0; i < milliseconds / 20 && !g_app_exited; i++) {
        megler_touch(1, 1, 200.0f + (float)i, 100.0f);
        mg_sleep_ns(20000000ull);
    }
    megler_touch(2, 2, 900.0f, 500.0f);
    megler_touch(2, 1, 200.0f, 100.0f);
    mg_sleep_ns(100000000ull);
    say("%d frames, %d draw calls (the last of %d vertices), %d clears", (int)megler_frames(), M.draws, M.draw_count, M.clears);
    say("sound: %d frames at %d Hz, %d of them not silent, peak %d", G_AUDIO.frames, G_AUDIO.rate, G_AUDIO.loud, G_AUDIO.peak);
    megler_key(4, true);
    megler_key(4, false);
    WAIT_FOR(g_app_exited);
    say(g_app_exited ? "ended with status %d" : "did not end after Back", g_app_status);
    mg_egl_set_provider(NULL);
    return g_app_exited && g_app_status == 0 ? 0 : 1;
}
