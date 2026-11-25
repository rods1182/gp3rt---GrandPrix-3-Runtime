/* DirectSound for the native runtime.
 * A software mixer: every secondary buffer is resampled (linear
 * interpolation), volume/pan scaled and mixed into stereo s16 at the output
 * rate. The mixed stream goes to the platform (plat_audio_*): on Linux the
 * host plays it with SDL; headless runs just discard it.
 * GP3 uses plain IDirectSound / IDirectSoundBuffer (no 3D, no notify);
 * Bink uses a streaming buffer through the same interfaces. GPxPatch's GPxSound
 * adds IDirectSound3DListener / IDirectSound3DBuffer (engine sounds of the other
 * cars, pit siren): those are done here as distance attenuation and stereo pan. */
#include "rt.h"
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

/* platform audio sink (plat_bridge.c / plat_headless.c) */
int plat_audio_rate(void);                         /* output rate, 0 = no device */
uint32_t plat_audio_space(void);                   /* stereo frames the sink can take now */
void plat_audio_write(const int16_t *lr, uint32_t frames);

static int g_d3trace = -1;
#define D3T(...) do { if (g_d3trace < 0) g_d3trace = getenv("GP3_DS3D_TRACE") ? atoi(getenv("GP3_DS3D_TRACE")) : 0; if (g_d3trace > 0) { g_d3trace--; LOG(__VA_ARGS__); } } while (0)
#define DS_OK 0
#define DSERR_INVALIDPARAM ((HRESULT)0x80070057)
#define DSERR_NOINTERFACE ((HRESULT)0x80004002)
#define DSERR_CONTROLUNAVAIL ((HRESULT)0x8878001E)
#define DSERR_OUTOFMEMORY ((HRESULT)0x8007000E)
#define DSBCAPS_PRIMARYBUFFER 0x1
#define DSBCAPS_CTRLFREQUENCY 0x20
#define DSBCAPS_CTRLPAN 0x40
#define DSBCAPS_CTRLVOLUME 0x80
#define DSBCAPS_GETCURRENTPOSITION2 0x10000
#define DSBPLAY_LOOPING 1
#define DSBLOCK_FROMWRITECURSOR 1
#define DSBLOCK_ENTIREBUFFER 2

#pragma pack(push, 1)
typedef struct { WORD tag, channels; DWORD rate, avg; WORD align, bits, cb; } wfx_t;
#pragma pack(pop)
typedef struct { DWORD size, flags, bytes, reserved; uint32_t wfx; } dsbdesc_t;

typedef struct { uint8_t *p; uint32_t n; int refs; } sdata_t;
typedef struct dsbuf {
    void **vtbl; LONG refs;
    struct dsbuf *next;
    int primary;
    DWORD flags;
    wfx_t fmt;
    sdata_t *data;
    uint32_t fsize, nframes;             /* bytes per frame, frames in buffer */
    uint64_t pos;                        /* 32.32 frames */
    int playing, looping;
    LONG vol, pan; DWORD freq;
    float gl, gr;                        /* gains from vol/pan */
    struct ds3dbuf *b3d;                 /* IDirectSound3DBuffer, when asked for */
} dsbuf_t;
typedef struct ds3dbuf {
    void **vtbl; dsbuf_t *owner; LONG refs;   /* own count: the buffer lives until both are released */
    float pos[3], vel[3], cone[3];
    DWORD cin, cout; LONG cvol;
    float mind, maxd; DWORD mode;        /* DS3DMODE_NORMAL 0, HEADRELATIVE 1, DISABLE 2 */
} ds3dbuf_t;
typedef struct {
    void **vtbl; LONG refs;
    float pos[3], vel[3], front[3], top[3];
    float dist, rolloff, doppler;
} ds3dlis_t;
static ds3dlis_t g_lis = { NULL, 1, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1, 1, 1 };
static void **g_d3b_vtbl;
typedef struct { void **vtbl; LONG refs; } ds_t;

static void **g_ds_vtbl, **g_dsb_vtbl;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static dsbuf_t *g_bufs;
static int g_out_rate = 44100;
static pthread_t g_mixer;
static int g_mixer_started;
static dsbuf_t *g_primary;
static wfx_t g_primary_fmt = { 1, 2, 22050, 22050 * 4, 4, 16, 0 };

static float db_to_lin(LONG hdb) { if (hdb <= -10000) return 0; return powf(10.f, hdb / 2000.f); }
static void update_gain(dsbuf_t *b) {
    float v = db_to_lin(b->vol), l = 1, r = 1;
    if (b->pan > 0) l = db_to_lin(-b->pan); else if (b->pan < 0) r = db_to_lin(b->pan);
    b->gl = v * l; b->gr = v * r;
}

/* ------------------------------------------------------------------ */
/* mixer                                                                */
static inline float sample(const dsbuf_t *b, uint32_t f, int ch) {
    const uint8_t *p = b->data->p + (size_t)f * b->fsize;
    if (b->fmt.bits == 16) { const int16_t *s = (const int16_t *)p; return s[b->fmt.channels > 1 ? ch : 0] * (1.f / 32768.f); }
    return (p[b->fmt.channels > 1 ? ch : 0] - 128) * (1.f / 128.f);
}
/* 3D: inverse-distance rolloff (as DirectSound) and pan from the listener's right axis */
static void gain3d(const ds3dbuf_t *d, float *gl, float *gr) {
    if (d->mode == 2) return;
    float rel[3];
    if (d->mode == 1) { rel[0] = d->pos[0]; rel[1] = d->pos[1]; rel[2] = d->pos[2]; }
    else for (int i = 0; i < 3; i++) rel[i] = d->pos[i] - g_lis.pos[i];
    float dist = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
    float mind = d->mind > 0 ? d->mind : 1e-3f, dd = dist > d->maxd ? d->maxd : dist;
    float g = dd <= mind ? 1.f : mind / (mind + g_lis.rolloff * (dd - mind));
    float pan = 0;
    if (dist > 1e-4f) {
        const float *f = g_lis.front, *t = g_lis.top;
        float r[3] = { t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2], t[0] * f[1] - t[1] * f[0] };
        float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (d->mode == 1) { r[0] = 1; r[1] = 0; r[2] = 0; rl = 1; }
        if (rl > 1e-6f) pan = (rel[0] * r[0] + rel[1] * r[1] + rel[2] * r[2]) / (rl * dist);
        float near = dist < mind ? dist / mind : 1.f;   /* a source on top of the listener is centred */
        pan *= near * 0.8f;
    }
    *gl *= g * (pan > 0 ? 1 - pan : 1); *gr *= g * (pan < 0 ? 1 + pan : 1);
}
static void mix(float *acc, uint32_t frames) {
    for (dsbuf_t *b = g_bufs; b; b = b->next) {
        if (!b->playing || b->primary || !b->nframes) continue;
        uint64_t step = ((uint64_t)(b->freq ? b->freq : b->fmt.rate) << 32) / (uint32_t)g_out_rate;
        uint64_t end = (uint64_t)b->nframes << 32;
        float gl = b->gl, gr = b->gr;
        if (b->b3d) gain3d(b->b3d, &gl, &gr);
        for (uint32_t i = 0; i < frames; i++) {
            if (b->pos >= end) {
                if (b->looping) b->pos %= end;
                else { b->playing = 0; b->pos = 0; break; }
            }
            uint32_t f0 = (uint32_t)(b->pos >> 32), f1 = f0 + 1;
            if (f1 >= b->nframes) f1 = b->looping ? 0 : f0;
            float t = (float)(uint32_t)b->pos * (1.f / 4294967296.f);
            float l0 = sample(b, f0, 0), r0 = sample(b, f0, 1), l1 = sample(b, f1, 0), r1 = sample(b, f1, 1);
            acc[2 * i] += (l0 + (l1 - l0) * t) * gl;
            acc[2 * i + 1] += (r0 + (r1 - r0) * t) * gr;
            b->pos += step;
        }
    }
}
static void *mixer_thread(void *arg) {
    (void)arg;
    float acc[2 * 1024]; int16_t out[2 * 1024];
    struct timespec last; clock_gettime(CLOCK_MONOTONIC, &last);
    double carry = 0;
    for (;;) {
        struct timespec ts = { 0, 4 * 1000000 }; nanosleep(&ts, NULL);
        uint32_t want;
        int rate = plat_audio_rate();
        if (rate > 0) want = plat_audio_space();
        else {   /* no device: advance with the clock so positions still move */
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - last.tv_sec) + (now.tv_nsec - last.tv_nsec) / 1e9; last = now;
            carry += dt * g_out_rate; want = (uint32_t)carry; carry -= want;
        }
        while (want) {
            uint32_t n = want > 1024 ? 1024 : want;
            memset(acc, 0, sizeof(float) * 2 * n);
            pthread_mutex_lock(&g_mx);
            mix(acc, n);
            pthread_mutex_unlock(&g_mx);
            static FILE *rec; static int rec_init;   /* analysis: GP3_WAV=file records the mix (raw s16 stereo) */
            if (!rec_init) { rec_init = 1; const char *w = getenv("GP3_WAV"); if (w) rec = fopen(w, "wb"); }
            if (rate > 0 || rec) {
                for (uint32_t i = 0; i < 2 * n; i++) { float v = acc[i] * 32767.f; out[i] = v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v; }
                if (rate > 0) plat_audio_write(out, n);
                if (rec) fwrite(out, 4, n, rec);
            }
            want -= n;
        }
    }
    return NULL;
}
static void start_mixer(void) {
    if (g_mixer_started) return;
    g_mixer_started = 1;
    int r = plat_audio_rate(); if (r > 0) g_out_rate = r;
    pthread_create(&g_mixer, NULL, mixer_thread, NULL);
    LOG("dsound: software mixer at %d Hz (%s)", g_out_rate, r > 0 ? "audio device" : "no device, silent");
}

/* ------------------------------------------------------------------ */
/* IDirectSoundBuffer                                                   */
static const uint8_t IID_DS3DLISTENER[16] = { 0x84, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 };
static const uint8_t IID_DSBUFFER[16]     = { 0x85, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 };
static const uint8_t IID_DS3DBUFFER[16]   = { 0x86, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 };
static HRESULT WINAPI b_QueryInterface(dsbuf_t *b, const uint8_t *iid, void **out) {
    static const uint8_t iid_unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
    if (!memcmp(iid, IID_DSBUFFER, 16) || !memcmp(iid, iid_unk, 16)) { b->refs++; *out = b; return DS_OK; }
    static int no3d = -1; if (no3d < 0) no3d = getenv("GP3_NO_DS3D") != NULL;   /* [game] sound3d=0: 2D only, as before 0.4 */
    if (b->primary && !no3d && !memcmp(iid, IID_DS3DLISTENER, 16)) { static int n; if (!n++) LOG("dsound: 3D listener in use"); g_lis.refs++; *out = &g_lis; return DS_OK; }
    if (!b->primary && !no3d && !memcmp(iid, IID_DS3DBUFFER, 16)) {
        pthread_mutex_lock(&g_mx);
        if (!b->b3d) {
            static int n; if (n++ < 3) LOG("dsound: 3D buffer %p", (void *)b);
            ds3dbuf_t *d = calloc(1, sizeof *d); d->vtbl = g_d3b_vtbl; d->owner = b;
            d->cin = d->cout = 360; d->cone[2] = 1; d->mind = 1; d->maxd = 1e9f;
            b->b3d = d;
        }
        pthread_mutex_unlock(&g_mx);
        b->b3d->refs++; *out = b->b3d; return DS_OK;
    }
    LOG("dsound: buffer QueryInterface %08x-... -> E_NOINTERFACE", *(const uint32_t *)iid);
    *out = NULL; return DSERR_NOINTERFACE;
}
static DWORD WINAPI b_AddRef(dsbuf_t *b) { return (DWORD)++b->refs; }
static DWORD WINAPI b_Release(dsbuf_t *b) {
    LONG r = --b->refs;
    if (r > 0 || b->primary) return (DWORD)(r > 0 ? r : 0);
    if (b->b3d && b->b3d->refs > 0) return 0;   /* the 3D interface still holds it */
    pthread_mutex_lock(&g_mx);
    for (dsbuf_t **pp = &g_bufs; *pp; pp = &(*pp)->next) if (*pp == b) { *pp = b->next; break; }
    pthread_mutex_unlock(&g_mx);
    if (b->data && --b->data->refs == 0) { free(b->data->p); free(b->data); }
    /* the small object itself is kept (never reused): code holding a stale pointer, such as
       a GPx module still holding a buffer GP3 released, keeps hitting a valid object instead
       of freed memory */
    b->data = NULL; b->nframes = 0; b->playing = 0; b->refs = 0;
    return 0;
}
static HRESULT WINAPI b_GetCaps(dsbuf_t *b, DWORD *caps) {
    if (!caps) return DSERR_INVALIDPARAM;
    caps[1] = b->flags | 0x8 /*LOCSOFTWARE*/; caps[2] = b->primary ? 4096 : b->data ? b->data->n : 0; caps[3] = 0; caps[4] = 0;
    return DS_OK;
}
static uint32_t cur_bytes(dsbuf_t *b) { return b->primary || !b->nframes ? 0 : (uint32_t)(b->pos >> 32) % b->nframes * b->fsize; }
static HRESULT WINAPI b_GetCurrentPosition(dsbuf_t *b, DWORD *play, DWORD *write) {
    pthread_mutex_lock(&g_mx);
    uint32_t p = cur_bytes(b);
    uint32_t ahead = 0;
    if (!b->primary && b->nframes) {   /* ~10 ms ahead, like real hardware */
        uint32_t fr = (b->freq ? b->freq : b->fmt.rate) / 100;
        ahead = b->playing ? fr * b->fsize : 0;
        if (ahead >= b->data->n) ahead = 0;
    }
    if (play) *play = p;
    if (write) *write = b->primary || !b->data ? 0 : (p + ahead) % b->data->n;
    pthread_mutex_unlock(&g_mx);
    return DS_OK;
}
static HRESULT WINAPI b_GetFormat(dsbuf_t *b, wfx_t *w, DWORD size, DWORD *written) {
    DWORD n = sizeof(wfx_t); if (w) { if (size < 16) return DSERR_INVALIDPARAM; memcpy(w, b->primary ? &g_primary_fmt : &b->fmt, size < n ? size : n); }
    if (written) *written = n;
    return DS_OK;
}
static HRESULT WINAPI b_GetVolume(dsbuf_t *b, LONG *v) { if (v) *v = b->vol; return DS_OK; }
static HRESULT WINAPI b_GetPan(dsbuf_t *b, LONG *v) { if (v) *v = b->pan; return DS_OK; }
static HRESULT WINAPI b_GetFrequency(dsbuf_t *b, DWORD *v) { if (v) *v = b->freq ? b->freq : b->fmt.rate; return DS_OK; }
static HRESULT WINAPI b_GetStatus(dsbuf_t *b, DWORD *s) {
    if (s) { pthread_mutex_lock(&g_mx); *s = b->playing ? (1 | (b->looping ? 4 : 0)) : 0; if (b->primary) *s = 1 | 4; pthread_mutex_unlock(&g_mx); }
    return DS_OK;
}
static HRESULT WINAPI b_Initialize(dsbuf_t *b, void *ds, const dsbdesc_t *d) { (void)b; (void)ds; (void)d; return 0x88780082; /* DSERR_ALREADYINITIALIZED */ }
static HRESULT WINAPI b_Lock(dsbuf_t *b, DWORD off, DWORD bytes, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags) {
    if (b->primary || !b->data) return DSERR_INVALIDPARAM;
    uint32_t n = b->data->n;
    if (flags & DSBLOCK_FROMWRITECURSOR) { DWORD w; b_GetCurrentPosition(b, NULL, &w); off = w; }
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = n;
    if (off >= n || bytes > n || !p1 || !n1) return DSERR_INVALIDPARAM;
    *p1 = b->data->p + off;
    if (off + bytes <= n) { *n1 = bytes; if (p2) *p2 = NULL; if (n2) *n2 = 0; }
    else { *n1 = n - off; if (p2) { *p2 = b->data->p; if (n2) *n2 = bytes - (n - off); } else if (n2) *n2 = 0; }
    return DS_OK;
}
static HRESULT WINAPI b_Play(dsbuf_t *b, DWORD r1, DWORD prio, DWORD flags) {
    (void)r1; (void)prio;
    if (b->primary) return DS_OK;
    pthread_mutex_lock(&g_mx); b->playing = 1; b->looping = (flags & DSBPLAY_LOOPING) != 0; pthread_mutex_unlock(&g_mx);
    return DS_OK;
}
static HRESULT WINAPI b_SetCurrentPosition(dsbuf_t *b, DWORD pos) {
    if (b->primary || !b->fsize || !b->data) return DS_OK;
    pthread_mutex_lock(&g_mx); b->pos = (uint64_t)((pos % (b->data->n ? b->data->n : 1)) / b->fsize) << 32; pthread_mutex_unlock(&g_mx);
    return DS_OK;
}
static HRESULT WINAPI b_SetFormat(dsbuf_t *b, const wfx_t *w) {
    if (!b->primary || !w) return DSERR_INVALIDPARAM;
    g_primary_fmt = *w; g_primary_fmt.cb = 0;
    LOG("dsound: primary format %u Hz %u-bit %u ch", w->rate, w->bits, w->channels);
    return DS_OK;
}
static HRESULT WINAPI b_SetVolume(dsbuf_t *b, LONG v) {
    if (v > 0 || v < -10000) return DSERR_INVALIDPARAM;
    pthread_mutex_lock(&g_mx); b->vol = v; update_gain(b); pthread_mutex_unlock(&g_mx); return DS_OK;
}
static HRESULT WINAPI b_SetPan(dsbuf_t *b, LONG v) {
    if (v > 10000 || v < -10000) return DSERR_INVALIDPARAM;
    pthread_mutex_lock(&g_mx); b->pan = v; update_gain(b); pthread_mutex_unlock(&g_mx); return DS_OK;
}
static HRESULT WINAPI b_SetFrequency(dsbuf_t *b, DWORD f) {
    if (b->primary) return DSERR_CONTROLUNAVAIL;
    if (f && (f < 100 || f > 200000)) return DSERR_INVALIDPARAM;
    pthread_mutex_lock(&g_mx); b->freq = f; pthread_mutex_unlock(&g_mx); return DS_OK;
}
static HRESULT WINAPI b_Stop(dsbuf_t *b) { pthread_mutex_lock(&g_mx); b->playing = 0; pthread_mutex_unlock(&g_mx); return DS_OK; }
static HRESULT WINAPI b_Unlock(dsbuf_t *b, void *p1, DWORD n1, void *p2, DWORD n2) { (void)b; (void)p1; (void)n1; (void)p2; (void)n2; return DS_OK; }
static HRESULT WINAPI b_Restore(dsbuf_t *b) { (void)b; return DS_OK; }

/* ------------------------------------------------------------------ */
/* IDirectSound3DBuffer (shares the reference count of its buffer)       */
typedef struct { DWORD size; float pos[3], vel[3]; DWORD cin, cout; float cone[3]; LONG cvol; float mind, maxd; DWORD mode; } ds3dbuffer_params_t;
typedef struct { DWORD size; float pos[3], vel[3], front[3], top[3]; float dist, rolloff, doppler; } ds3dlistener_params_t;
#define LK pthread_mutex_lock(&g_mx)
#define UL pthread_mutex_unlock(&g_mx)
static HRESULT WINAPI d3b_QueryInterface(ds3dbuf_t *d, const uint8_t *iid, void **out) { return b_QueryInterface(d->owner, iid, out); }
static DWORD WINAPI d3b_AddRef(ds3dbuf_t *d) { return (DWORD)++d->refs; }
static DWORD WINAPI d3b_Release(ds3dbuf_t *d) {
    LONG r = --d->refs;
    if (r > 0) return (DWORD)r;
    d->refs = 0;
    dsbuf_t *b = d->owner;
    if (b->refs <= 0 && !b->primary) { b->refs = 1; b_Release(b); }   /* buffer interface already gone: destroy now */
    return 0;
}
static HRESULT WINAPI d3b_GetAllParameters(ds3dbuf_t *d, ds3dbuffer_params_t *p) {
    if (!p) return DSERR_INVALIDPARAM;
    LK; memcpy(p->pos, d->pos, 12); memcpy(p->vel, d->vel, 12); p->cin = d->cin; p->cout = d->cout; memcpy(p->cone, d->cone, 12);
    p->cvol = d->cvol; p->mind = d->mind; p->maxd = d->maxd; p->mode = d->mode; UL; return DS_OK;
}
static HRESULT WINAPI d3b_GetConeAngles(ds3dbuf_t *d, DWORD *i, DWORD *o) { if (i) *i = d->cin; if (o) *o = d->cout; return DS_OK; }
static HRESULT WINAPI d3b_GetConeOrientation(ds3dbuf_t *d, float *v) { if (v) memcpy(v, d->cone, 12); return DS_OK; }
static HRESULT WINAPI d3b_GetConeOutsideVolume(ds3dbuf_t *d, LONG *v) { if (v) *v = d->cvol; return DS_OK; }
static HRESULT WINAPI d3b_GetMaxDistance(ds3dbuf_t *d, float *v) { if (v) *v = d->maxd; return DS_OK; }
static HRESULT WINAPI d3b_GetMinDistance(ds3dbuf_t *d, float *v) { if (v) *v = d->mind; return DS_OK; }
static HRESULT WINAPI d3b_GetMode(ds3dbuf_t *d, DWORD *v) { if (v) *v = d->mode; return DS_OK; }
static HRESULT WINAPI d3b_GetPosition(ds3dbuf_t *d, float *v) { if (v) memcpy(v, d->pos, 12); return DS_OK; }
static HRESULT WINAPI d3b_GetVelocity(ds3dbuf_t *d, float *v) { if (v) memcpy(v, d->vel, 12); return DS_OK; }
static HRESULT WINAPI d3b_SetAllParameters(ds3dbuf_t *d, const ds3dbuffer_params_t *p, DWORD a) {
    (void)a; if (!p) return DSERR_INVALIDPARAM;
    D3T("ds3d: buf %p all: pos %.2f %.2f %.2f min %.2f max %.2f mode %u", (void *)d, p->pos[0], p->pos[1], p->pos[2], p->mind, p->maxd, p->mode);
    LK; memcpy(d->pos, p->pos, 12); memcpy(d->vel, p->vel, 12); d->cin = p->cin; d->cout = p->cout; memcpy(d->cone, p->cone, 12);
    d->cvol = p->cvol; d->mind = p->mind; d->maxd = p->maxd; d->mode = p->mode; UL; return DS_OK;
}
static HRESULT WINAPI d3b_SetConeAngles(ds3dbuf_t *d, DWORD i, DWORD o, DWORD a) { (void)a; d->cin = i; d->cout = o; return DS_OK; }
static HRESULT WINAPI d3b_SetConeOrientation(ds3dbuf_t *d, float x, float y, float z, DWORD a) { (void)a; d->cone[0] = x; d->cone[1] = y; d->cone[2] = z; return DS_OK; }
static HRESULT WINAPI d3b_SetConeOutsideVolume(ds3dbuf_t *d, LONG v, DWORD a) { (void)a; d->cvol = v; return DS_OK; }
static HRESULT WINAPI d3b_SetMaxDistance(ds3dbuf_t *d, float v, DWORD a) { (void)a; d->maxd = v; return DS_OK; }
static HRESULT WINAPI d3b_SetMinDistance(ds3dbuf_t *d, float v, DWORD a) { (void)a; d->mind = v; return DS_OK; }
static HRESULT WINAPI d3b_SetMode(ds3dbuf_t *d, DWORD v, DWORD a) { (void)a; if (v > 2) return DSERR_INVALIDPARAM; d->mode = v; return DS_OK; }
static HRESULT WINAPI d3b_SetPosition(ds3dbuf_t *d, float x, float y, float z, DWORD a) { (void)a; D3T("ds3d: buf %p pos %.2f %.2f %.2f mode %u min %.2f max %.2f", (void *)d, x, y, z, d->mode, d->mind, d->maxd); LK; d->pos[0] = x; d->pos[1] = y; d->pos[2] = z; UL; return DS_OK; }
static HRESULT WINAPI d3b_SetVelocity(ds3dbuf_t *d, float x, float y, float z, DWORD a) { (void)a; d->vel[0] = x; d->vel[1] = y; d->vel[2] = z; return DS_OK; }

/* IDirectSound3DListener (one, on the primary buffer) */
static HRESULT WINAPI lis_QueryInterface(ds3dlis_t *l, const uint8_t *iid, void **out) {
    if (!memcmp(iid, IID_DS3DLISTENER, 16)) { l->refs++; *out = l; return DS_OK; }
    if (g_primary) return b_QueryInterface(g_primary, iid, out);
    *out = NULL; return DSERR_NOINTERFACE;
}
static DWORD WINAPI lis_AddRef(ds3dlis_t *l) { return (DWORD)++l->refs; }
static DWORD WINAPI lis_Release(ds3dlis_t *l) { LONG r = --l->refs; if (r < 1) l->refs = 1; return (DWORD)(r > 0 ? r : 0); }
static HRESULT WINAPI lis_GetAllParameters(ds3dlis_t *l, ds3dlistener_params_t *p) {
    if (!p) return DSERR_INVALIDPARAM;
    LK; memcpy(p->pos, l->pos, 12); memcpy(p->vel, l->vel, 12); memcpy(p->front, l->front, 12); memcpy(p->top, l->top, 12);
    p->dist = l->dist; p->rolloff = l->rolloff; p->doppler = l->doppler; UL; return DS_OK;
}
static HRESULT WINAPI lis_GetDistanceFactor(ds3dlis_t *l, float *v) { if (v) *v = l->dist; return DS_OK; }
static HRESULT WINAPI lis_GetDopplerFactor(ds3dlis_t *l, float *v) { if (v) *v = l->doppler; return DS_OK; }
static HRESULT WINAPI lis_GetOrientation(ds3dlis_t *l, float *f, float *t) { if (f) memcpy(f, l->front, 12); if (t) memcpy(t, l->top, 12); return DS_OK; }
static HRESULT WINAPI lis_GetPosition(ds3dlis_t *l, float *v) { if (v) memcpy(v, l->pos, 12); return DS_OK; }
static HRESULT WINAPI lis_GetRolloffFactor(ds3dlis_t *l, float *v) { if (v) *v = l->rolloff; return DS_OK; }
static HRESULT WINAPI lis_GetVelocity(ds3dlis_t *l, float *v) { if (v) memcpy(v, l->vel, 12); return DS_OK; }
static HRESULT WINAPI lis_SetAllParameters(ds3dlis_t *l, const ds3dlistener_params_t *p, DWORD a) {
    (void)a; if (!p) return DSERR_INVALIDPARAM;
    LK; memcpy(l->pos, p->pos, 12); memcpy(l->vel, p->vel, 12); memcpy(l->front, p->front, 12); memcpy(l->top, p->top, 12);
    l->dist = p->dist; l->rolloff = p->rolloff; l->doppler = p->doppler; UL; return DS_OK;
}
static HRESULT WINAPI lis_SetDistanceFactor(ds3dlis_t *l, float v, DWORD a) { (void)a; l->dist = v; return DS_OK; }
static HRESULT WINAPI lis_SetDopplerFactor(ds3dlis_t *l, float v, DWORD a) { (void)a; l->doppler = v; return DS_OK; }
static HRESULT WINAPI lis_SetOrientation(ds3dlis_t *l, float fx, float fy, float fz, float tx, float ty, float tz, DWORD a) {
    (void)a; LK; l->front[0] = fx; l->front[1] = fy; l->front[2] = fz; l->top[0] = tx; l->top[1] = ty; l->top[2] = tz; UL; return DS_OK;
}
static HRESULT WINAPI lis_SetPosition(ds3dlis_t *l, float x, float y, float z, DWORD a) { (void)a; D3T("ds3d: listener %.2f %.2f %.2f front %.2f %.2f %.2f", x, y, z, l->front[0], l->front[1], l->front[2]); LK; l->pos[0] = x; l->pos[1] = y; l->pos[2] = z; UL; return DS_OK; }
static HRESULT WINAPI lis_SetRolloffFactor(ds3dlis_t *l, float v, DWORD a) { (void)a; l->rolloff = v; return DS_OK; }
static HRESULT WINAPI lis_SetVelocity(ds3dlis_t *l, float x, float y, float z, DWORD a) { (void)a; l->vel[0] = x; l->vel[1] = y; l->vel[2] = z; return DS_OK; }
static HRESULT WINAPI lis_CommitDeferredSettings(ds3dlis_t *l) { (void)l; return DS_OK; }

static dsbuf_t *new_buf(void) {
    dsbuf_t *b = calloc(1, sizeof *b); b->vtbl = g_dsb_vtbl; b->refs = 1; b->gl = b->gr = 1;
    return b;
}

/* ------------------------------------------------------------------ */
/* IDirectSound                                                         */
static HRESULT WINAPI ds_QueryInterface(ds_t *d, const uint8_t *iid, void **out) {
    static const uint8_t iid_ds[16] = { 0x83, 0xfa, 0x9a, 0x27, 0x81, 0x49, 0xce, 0x11, 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 };
    static const uint8_t iid_unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
    if (!memcmp(iid, iid_ds, 16) || !memcmp(iid, iid_unk, 16)) { d->refs++; *out = d; return DS_OK; }
    LOG("dsound: QueryInterface %08x -> E_NOINTERFACE", *(const uint32_t *)iid);
    *out = NULL; return DSERR_NOINTERFACE;
}
static DWORD WINAPI ds_AddRef(ds_t *d) { return (DWORD)++d->refs; }
static DWORD WINAPI ds_Release(ds_t *d) { LONG r = --d->refs; return (DWORD)(r > 0 ? r : 0); }   /* kept alive: buffers may outlive it */
static HRESULT WINAPI ds_CreateSoundBuffer(ds_t *d, const dsbdesc_t *desc, dsbuf_t **out, void *outer) {
    (void)d; (void)outer;
    if (!desc || !out) return DSERR_INVALIDPARAM;
    if (desc->flags & DSBCAPS_PRIMARYBUFFER) {
        if (!g_primary) { g_primary = new_buf(); g_primary->primary = 1; g_primary->flags = desc->flags; }
        g_primary->refs++; *out = g_primary;
        TRACE("dsound: primary buffer");
        return DS_OK;
    }
    const wfx_t *w = (const wfx_t *)(uintptr_t)desc->wfx;
    if (!w || (w->tag != 1) || !w->channels || (w->bits != 8 && w->bits != 16) || desc->bytes < 4) {
        LOG("dsound: unsupported buffer (tag %d, %d-bit, %u bytes)", w ? w->tag : -1, w ? w->bits : 0, desc->bytes);
        return DSERR_INVALIDPARAM;
    }
    dsbuf_t *b = new_buf();
    b->flags = desc->flags; b->fmt = *w; b->fmt.cb = 0;
    b->fsize = (uint32_t)w->channels * (w->bits / 8);
    b->data = calloc(1, sizeof *b->data); b->data->n = desc->bytes; b->data->refs = 1;
    b->data->p = malloc(desc->bytes + 16);
    if (!b->data->p) { free(b->data); free(b); return DSERR_OUTOFMEMORY; }
    memset(b->data->p, w->bits == 8 ? 0x80 : 0, desc->bytes + 16);
    b->nframes = desc->bytes / b->fsize;
    update_gain(b);
    pthread_mutex_lock(&g_mx); b->next = g_bufs; g_bufs = b; pthread_mutex_unlock(&g_mx);
    start_mixer();
    TRACE("dsound: buffer %p %u Hz %u-bit %u ch, %u bytes, flags %x", (void *)b, w->rate, w->bits, w->channels, desc->bytes, desc->flags);
    *out = b;
    return DS_OK;
}
static HRESULT WINAPI ds_GetCaps(ds_t *d, DWORD *caps) {
    (void)d;
    if (!caps || caps[0] < 8) return DSERR_INVALIDPARAM;
    DWORD sz = caps[0]; memset(caps, 0, sz); caps[0] = sz;
    caps[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x100 | 0x200 | 0x400 | 0x800 | 0x10 /*CONTINUOUSRATE*/;
    if (sz >= 16) { caps[2] = 100; caps[3] = 200000; }   /* min/max secondary rate */
    return DS_OK;
}
static HRESULT WINAPI ds_DuplicateSoundBuffer(ds_t *d, dsbuf_t *src, dsbuf_t **out) {
    (void)d;
    if (!src || !out || src->primary || !src->data) return DSERR_INVALIDPARAM;
    dsbuf_t *b = new_buf();
    b->flags = src->flags; b->fmt = src->fmt; b->fsize = src->fsize; b->nframes = src->nframes;
    b->data = src->data; b->data->refs++;
    b->vol = src->vol; b->pan = src->pan; b->freq = src->freq; update_gain(b);
    pthread_mutex_lock(&g_mx); b->next = g_bufs; g_bufs = b; pthread_mutex_unlock(&g_mx);
    *out = b;
    return DS_OK;
}
static HRESULT WINAPI ds_SetCooperativeLevel(ds_t *d, HWND h, DWORD lvl) { (void)d; (void)h; LOG("dsound: SetCooperativeLevel(%u)", lvl); return DS_OK; }
static HRESULT WINAPI ds_Compact(ds_t *d) { (void)d; return DS_OK; }
static HRESULT WINAPI ds_GetSpeakerConfig(ds_t *d, DWORD *c) { (void)d; if (c) *c = 4 /*DSSPEAKER_STEREO*/; return DS_OK; }
static HRESULT WINAPI ds_SetSpeakerConfig(ds_t *d, DWORD c) { (void)d; (void)c; return DS_OK; }
static HRESULT WINAPI ds_Initialize(ds_t *d, const uint8_t *g) { (void)d; (void)g; return DS_OK; }

static ds_t *g_ds;
static HRESULT WINAPI DirectSoundCreate_(const uint8_t *g, void **out, void *outer) {
    (void)g; (void)outer;
    if (!out) return DSERR_INVALIDPARAM;
    if (!g_ds) { g_ds = calloc(1, sizeof *g_ds); g_ds->vtbl = g_ds_vtbl; }
    g_ds->refs++;
    *out = g_ds;
    LOG("DirectSoundCreate -> software mixer (output %d Hz)", plat_audio_rate());
    return DS_OK;
}
typedef BOOL (WINAPI *dsenum_cb)(uint8_t *guid, const char *desc, const char *module, void *ctx);
static HRESULT WINAPI DirectSoundEnumerateA_(dsenum_cb cb, void *ctx) {
    if (cb) cb(NULL, "Primary Sound Driver", "", ctx);
    return DS_OK;
}
HRESULT dplay_create(const uint8_t *clsid, const uint8_t *iid, void **out) { (void)clsid; (void)iid; *out = NULL; LOG("CoCreateInstance(DirectPlay) -> class not registered"); return 0x80040154; }
void dsound_register(void) {
    static void *dv[] = { ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps, ds_DuplicateSoundBuffer,
                          ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig, ds_SetSpeakerConfig, ds_Initialize };
    static void *bv[] = { b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume, b_GetPan,
                          b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat, b_SetVolume,
                          b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore };
    static void *d3v[] = { d3b_QueryInterface, d3b_AddRef, d3b_Release, d3b_GetAllParameters, d3b_GetConeAngles, d3b_GetConeOrientation,
                           d3b_GetConeOutsideVolume, d3b_GetMaxDistance, d3b_GetMinDistance, d3b_GetMode, d3b_GetPosition, d3b_GetVelocity,
                           d3b_SetAllParameters, d3b_SetConeAngles, d3b_SetConeOrientation, d3b_SetConeOutsideVolume, d3b_SetMaxDistance,
                           d3b_SetMinDistance, d3b_SetMode, d3b_SetPosition, d3b_SetVelocity };
    static void *lv[] = { lis_QueryInterface, lis_AddRef, lis_Release, lis_GetAllParameters, lis_GetDistanceFactor, lis_GetDopplerFactor,
                          lis_GetOrientation, lis_GetPosition, lis_GetRolloffFactor, lis_GetVelocity, lis_SetAllParameters, lis_SetDistanceFactor,
                          lis_SetDopplerFactor, lis_SetOrientation, lis_SetPosition, lis_SetRolloffFactor, lis_SetVelocity, lis_CommitDeferredSettings };
    g_ds_vtbl = dv; g_dsb_vtbl = bv; g_d3b_vtbl = d3v; g_lis.vtbl = lv;
    static const export_t ex[] = { { "dsound", "DirectSoundCreate", 1, (void *)DirectSoundCreate_ },
                                   { "dsound", "DirectSoundEnumerateA", 2, (void *)DirectSoundEnumerateA_ }, { NULL, NULL, 0, NULL } };
    rt_register_exports(ex);
}
