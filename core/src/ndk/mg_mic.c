#include "mg_mic.h"

#include "../mg_core.h"
#include "../host/mg_host.h"

static struct {
    mg_lock lock;
    mg_cond more;
    bool inited, on;
    i16 *ring;
    u32 cap, head, count;   /* samples */
    int channels;
    u32 owner, tokens;      /* the recording the ring is for, and the last token handed out */
    const megler_microphone_host *host;
    /*
     * Held over the shell's start and stop, so that the stop of a recording taken over from cannot come after the start of
     * the one that took over and silence it (an AudioRecord released on one thread while an OpenSL ES recorder starts on
     * another).
     */
    mg_lock shell;
} MIC = { MG_LOCK_INIT };

static void mic_init(void)
{
    static mg_lock once = MG_LOCK_INIT;
    mg_lock_acquire(&once);
    if (!MIC.inited) {
        mg_cond_init(&MIC.more);
        mg_lock_init(&MIC.shell);
        MIC.inited = true;
    }
    mg_lock_release(&once);
}

void megler_microphone_push(const int16_t *samples, int frames)
{
    u32 n, i;
    if (!MIC.inited || frames <= 0 || !samples) return;
    mg_lock_acquire(&MIC.lock);
    if (MIC.on && MIC.ring) {
        n = (u32)frames * (u32)MIC.channels;
        for (i = 0; i < n; i++) {
            MIC.ring[(MIC.head + MIC.count) % MIC.cap] = samples[i];
            if (MIC.count < MIC.cap) MIC.count++;
            else MIC.head = (MIC.head + 1) % MIC.cap;   /* full: the oldest goes */
        }
        mg_cond_broadcast(&MIC.more);
    }
    mg_lock_release(&MIC.lock);
}

u32 mg_mic_start(const megler_microphone_host *h, int rate, int channels)
{
    u32 token;
    bool ok;
    if (!h || rate < 4000 || rate > 192000 || channels < 1 || channels > 2) return 0;
    mic_init();
    mg_lock_acquire(&MIC.shell);
    mg_lock_acquire(&MIC.lock);
    free(MIC.ring);
    MIC.cap = (u32)rate * (u32)channels * 2u;
    MIC.ring = (i16 *)mg_alloc(sizeof(i16) * MIC.cap);
    MIC.channels = channels;
    MIC.head = MIC.count = 0;
    MIC.on = true;
    if (!++MIC.tokens) MIC.tokens = 1;
    token = MIC.owner = MIC.tokens;
    MIC.host = h;
    mg_cond_broadcast(&MIC.more);      /* the recording taken over from waits no longer */
    mg_lock_release(&MIC.lock);
    ok = h->start ? h->start(h->user, rate, channels) : false;
    if (!ok) {
        mg_lock_acquire(&MIC.lock);
        MIC.on = false;
        mg_lock_release(&MIC.lock);
    }
    mg_lock_release(&MIC.shell);
    return ok ? token : 0;
}

void mg_mic_stop(u32 token)
{
    const megler_microphone_host *h = NULL;
    if (!token) return;
    mic_init();
    mg_lock_acquire(&MIC.shell);
    mg_lock_acquire(&MIC.lock);
    if (MIC.on && MIC.owner == token) {
        MIC.on = false;
        h = MIC.host;
        mg_cond_broadcast(&MIC.more);
    }
    mg_lock_release(&MIC.lock);
    if (h && h->stop) h->stop(h->user);
    mg_lock_release(&MIC.shell);
}

int mg_mic_read(u32 token, i16 *out, int want, bool blocking)
{
    int got = 0;
    if (!token || !out || want <= 0) return 0;
    mic_init();
    mg_lock_acquire(&MIC.lock);
    while (got < want && MIC.owner == token) {
        while (MIC.count > 0 && got < want) {
            out[got++] = MIC.ring[MIC.head];
            MIC.head = (MIC.head + 1) % MIC.cap;
            MIC.count--;
        }
        if (got >= want || !blocking || !MIC.on) break;
        if (!mg_cond_wait_ns(&MIC.more, &MIC.lock, 1000000000ull) && MIC.count == 0 && MIC.on && MIC.owner == token) {
            /* Nothing for a second: silence for the rest, as a microphone that hears nothing gives. */
            while (got < want) out[got++] = 0;
        }
    }
    mg_lock_release(&MIC.lock);
    return got;
}
