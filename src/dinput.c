/* DirectInput 7 for the native runtime: joysticks / steering wheels with
 * force feedback. Device state comes from the Linux host (SDL joysticks)
 * through shared memory (bridge.h); force-feedback effects are forwarded to
 * the host, which plays them with SDL_Haptic. The keyboard reaches the game
 * through window messages, so only game controllers are exposed here.
 * GP3 uses: EnumDevices(joystick), CreateDevice, QueryInterface(Device2),
 * SetDataFormat, Get/SetProperty, EnumObjects, Acquire, Poll, GetDeviceState,
 * CreateEffect (constant force, spring, damper, friction), SendForceFeedbackCommand. */
#include "rt.h"
#include "bridge.h"

br_shared_t *g_br __attribute__((weak));
void plat_ff(const br_ff_t *f) __attribute__((weak));
void plat_ff(const br_ff_t *f) { (void)f; }

#define DI_OK 0
#define DI_NOEFFECT 1
#define DIERR_INVALIDPARAM ((HRESULT)0x80070057)
#define DIERR_NOINTERFACE ((HRESULT)0x80004002)
#define DIERR_NOTFOUND ((HRESULT)0x80070002)
#define DIERR_UNSUPPORTED ((HRESULT)0x80004001)
#define DIERR_DEVICENOTREG ((HRESULT)0x80040154)
#define DIERR_INPUTLOST ((HRESULT)0x8007001E)
#define DIERR_NOTACQUIRED ((HRESULT)0x8007000C)
#define DIERR_OBJECTNOTFOUND DIERR_NOTFOUND

/* GUIDs (little-endian layout as in memory) */
typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } guid_t;
static const guid_t G_XAxis = { 0xA36D02E0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
#define AXG(n) { n, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0 } }
static const guid_t G_axes[8] = { AXG(0xA36D02E0), AXG(0xA36D02E1), AXG(0xA36D02E2), AXG(0xA36D02F4), AXG(0xA36D02F5), AXG(0xA36D02E3), AXG(0xA36D02E4), AXG(0xA36D02E4) };
static const guid_t G_Button = AXG(0xA36D02F0), G_POV = AXG(0xA36D02F2);
#define EFG(n) { n, 0x8E33, 0x11D0, { 0x9A, 0xD0, 0x00, 0xA0, 0xC9, 0xA0, 0x6E, 0x35 } }
static const guid_t G_eff[] = { EFG(0x13541C20), EFG(0x13541C27), EFG(0x13541C28), EFG(0x13541C2A), EFG(0x13541C29),
                                EFG(0x13541C23), EFG(0x13541C22), EFG(0x13541C24), EFG(0x13541C25), EFG(0x13541C26), EFG(0x13541C21) };
static const int T_eff[] = { FFT_CONSTANT, FFT_SPRING, FFT_DAMPER, FFT_FRICTION, FFT_INERTIA, FFT_SINE, FFT_SQUARE, FFT_TRIANGLE, FFT_SAWUP, FFT_SAWDOWN, FFT_RAMP };
static const char *N_eff[] = { "Constant Force", "Spring", "Damper", "Friction", "Inertia", "Sine", "Square", "Triangle", "Sawtooth Up", "Sawtooth Down", "Ramp Force" };
#define NEFF 11
static const guid_t G_Joystick = { 0x6F1D2B70, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0, 0 } };
static const guid_t G_FFDriver = { 0x47503346, 0x4646, 0x4752, { 'G', 'P', '3', 'R', 'T', 'F', 'F', 0 } };
static const uint8_t IID_Unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 };
static int geq(const void *a, const void *b) { return !memcmp(a, b, 16); }

/* ------------------------------------------------------------------ */
/* device objects                                                       */
enum { OT_AXIS, OT_BUTTON, OT_POV };
typedef struct { int kind, idx; uint32_t ofs; } fmtobj_t;
typedef struct didev {
    void **vtbl; LONG refs;
    int joy;                                 /* index in g_br->joy */
    int acquired;
    uint32_t data_size; fmtobj_t fobj[160]; int nfobj;
    int32_t rmin[8], rmax[8]; uint32_t dead[8], sat[8];
    uint32_t gain;
} didev_t;
typedef struct dieff {
    void **vtbl; LONG refs;
    didev_t *dev; uint32_t id; int type; int gidx;
    br_ff_t p;
} dieff_t;
typedef struct { void **vtbl; LONG refs; } dinput_t;
static void **g_di_vtbl, **g_dev_vtbl, **g_eff_vtbl;
static uint32_t g_next_eff = 1;

static br_joy_t *J(didev_t *d) { return g_br && d->joy < (int)g_br->njoy ? &g_br->joy[d->joy] : NULL; }
static int joy_present(int i) { return g_br && i < (int)g_br->njoy && g_br->joy[i].present; }
static int naxes(int j) { int n = (int)g_br->joy[j].naxes; return n > 8 ? 8 : n; }
static int nbtn(int j) { int n = (int)g_br->joy[j].nbuttons; return n > 32 ? 32 : n; }
static int npov(int j) { int n = (int)g_br->joy[j].nhats; return n > 4 ? 4 : n; }

/* DIDFT type of object i of a joystick (axes, then buttons, then POVs) */
static uint32_t obj_type(int kind, int idx) {
    uint32_t inst = (uint32_t)idx << 8;
    if (kind == OT_AXIS) return 2 /*ABSAXIS*/ | inst | 0x01000000 /*FFACTUATOR*/;
    if (kind == OT_BUTTON) return 4 /*PSHBUTTON*/ | inst;
    return 0x10 /*POV*/ | inst;
}
static uint32_t default_ofs(int kind, int idx) {   /* DIJOYSTATE layout */
    if (kind == OT_AXIS) return idx < 6 ? (uint32_t)idx * 4 : 24 + (uint32_t)(idx - 6) * 4;
    if (kind == OT_POV) return 32 + (uint32_t)idx * 4;
    return 48 + (uint32_t)idx;
}

/* ------------------------------------------------------------------ */
/* IDirectInputEffect                                                   */
static void eff_send(dieff_t *e, uint32_t cmd) { e->p.cmd = cmd; e->p.joy = (uint32_t)e->dev->joy; e->p.id = e->id; e->p.type = (uint32_t)e->type; plat_ff(&e->p); }
static HRESULT WINAPI ef_QueryInterface(dieff_t *e, const uint8_t *iid, void **out) { (void)iid; e->refs++; *out = e; return DI_OK; }
static DWORD WINAPI ef_AddRef(dieff_t *e) { return (DWORD)++e->refs; }
static DWORD WINAPI ef_Release(dieff_t *e) { LONG r = --e->refs; if (r <= 0) { eff_send(e, FF_DESTROY); free(e); return 0; } return (DWORD)r; }
static HRESULT WINAPI ef_Initialize(dieff_t *e, uint32_t i, DWORD v, const void *g) { (void)e; (void)i; (void)v; (void)g; return DI_OK; }
static HRESULT WINAPI ef_GetEffectGuid(dieff_t *e, guid_t *g) { if (g) *g = G_eff[e->gidx]; return DI_OK; }
/* DIEFFECT: size flags duration sampleperiod gain trigbtn trigrep cAxes rgdwAxes rglDirection lpEnvelope cbTSP lpvTSP [startdelay] */
static void read_params(dieff_t *e, const uint32_t *de, DWORD fl) {
    br_ff_t *p = &e->p;
    if (fl & 0x1) p->duration_ms = de[2] == 0xffffffffu ? -1 : (int32_t)(de[2] / 1000);
    if (fl & 0x4) p->gain = (int32_t)de[4];
    if ((fl & 0x200) && de[0] >= 56) p->delay_ms = (int32_t)(de[13] / 1000);
    if (fl & 0x40) {
        uint32_t cax = de[7]; const int32_t *dir = (const int32_t *)(uintptr_t)de[9];
        if (dir && cax) {
            if (de[1] & 0x10) p->direction = dir[0] >= 0 ? 9000 : 27000;      /* cartesian: sign of x */
            else p->direction = dir[0];                                       /* polar / spherical: degrees*100 */
        }
    }
    if (fl & 0x80) {
        const uint32_t *env = (const uint32_t *)(uintptr_t)de[10];
        if (env) { p->attack_level = (int32_t)env[1]; p->attack_ms = (int32_t)(env[2] / 1000); p->fade_level = (int32_t)env[3]; p->fade_ms = (int32_t)(env[4] / 1000); }
        else p->attack_level = p->attack_ms = p->fade_level = p->fade_ms = 0;
    }
    if (fl & 0x100) {
        uint32_t cb = de[11]; const int32_t *t = (const int32_t *)(uintptr_t)de[12];
        if (t) switch (e->type) {
        case FFT_CONSTANT: if (cb >= 4) p->level = t[0]; break;
        case FFT_RAMP: if (cb >= 8) { p->start = t[0]; p->end = t[1]; } break;
        case FFT_SPRING: case FFT_DAMPER: case FFT_FRICTION: case FFT_INERTIA:   /* DICONDITION (first axis) */
            if (cb >= 24) { p->offset = t[0]; p->coef_pos = t[1]; p->coef_neg = t[2]; p->sat_pos = t[3]; p->sat_neg = t[4]; p->deadband = t[5]; }
            break;
        default:   /* DIPERIODIC: magnitude offset phase period */
            if (cb >= 16) { p->magnitude = t[0]; p->offset = t[1]; p->phase = t[2]; p->period_us = t[3]; }
            break;
        }
    }
}
static HRESULT WINAPI ef_GetParameters(dieff_t *e, uint32_t *de, DWORD fl) {
    if (!de) return DIERR_INVALIDPARAM;
    if (fl & 0x1) de[2] = e->p.duration_ms < 0 ? 0xffffffffu : (uint32_t)e->p.duration_ms * 1000;
    if (fl & 0x4) de[4] = (uint32_t)e->p.gain;
    if (fl & 0x100) {
        int32_t *t = (int32_t *)(uintptr_t)de[12];
        if (t && e->type == FFT_CONSTANT && de[11] >= 4) t[0] = e->p.level;
    }
    return DI_OK;
}
static HRESULT WINAPI ef_SetParameters(dieff_t *e, const uint32_t *de, DWORD fl) {
    if (!de) return DIERR_INVALIDPARAM;
    read_params(e, de, fl);
    if (!(fl & 0x80000000u /*NODOWNLOAD*/)) eff_send(e, FF_UPDATE);
    if (fl & 0x20000000u /*START*/) { e->p.iterations = 1; eff_send(e, FF_START); }
    return DI_OK;
}
static HRESULT WINAPI ef_Start(dieff_t *e, DWORD iter, DWORD fl) { (void)fl; e->p.iterations = iter == 0xffffffffu ? -1 : (int32_t)iter; eff_send(e, FF_START); return DI_OK; }
static HRESULT WINAPI ef_Stop(dieff_t *e) { eff_send(e, FF_STOP); return DI_OK; }
static HRESULT WINAPI ef_GetEffectStatus(dieff_t *e, DWORD *s) { (void)e; if (s) *s = 0; return DI_OK; }
static HRESULT WINAPI ef_Download(dieff_t *e) { eff_send(e, FF_UPDATE); return DI_OK; }
static HRESULT WINAPI ef_Unload(dieff_t *e) { eff_send(e, FF_STOP); return DI_OK; }
static HRESULT WINAPI ef_Escape(dieff_t *e, void *x) { (void)e; (void)x; return DIERR_UNSUPPORTED; }

/* ------------------------------------------------------------------ */
/* IDirectInputDevice2A / 7A                                            */
static HRESULT WINAPI dv_QueryInterface(didev_t *d, const uint8_t *iid, void **out) {
    /* IDirectInputDevice(A), Device2A, Device7A all share our vtable */
    if ((((const uint32_t *)iid)[0] == 0x5944E680 || ((const uint32_t *)iid)[0] == 0x5944E682 || ((const uint32_t *)iid)[0] == 0x57D7C6BC) || geq(iid, IID_Unknown)) {
        d->refs++; *out = d; return DI_OK;
    }
    LOG("dinput: device QueryInterface %08x -> E_NOINTERFACE", *(const uint32_t *)iid);
    *out = NULL; return DIERR_NOINTERFACE;
}
static DWORD WINAPI dv_AddRef(didev_t *d) { return (DWORD)++d->refs; }
static DWORD WINAPI dv_Release(didev_t *d) { LONG r = --d->refs; if (r <= 0) { free(d); return 0; } return (DWORD)r; }
static HRESULT WINAPI dv_GetCapabilities(didev_t *d, DWORD *c) {
    if (!c) return DIERR_INVALIDPARAM;
    br_joy_t *j = J(d); if (!j) return DIERR_INPUTLOST;
    DWORD sz = c[0]; memset(c + 1, 0, sz > 4 ? sz - 4 : 0);
    c[1] = 1 /*ATTACHED*/ | (j->has_ff ? 0x100 /*FORCEFEEDBACK*/ | 0x10000 /*DEADBAND*/ | 0x4000 /*SATURATION*/ : 0);
    c[2] = 4 | (2 << 8) | 0x10000;           /* joystick / wheel / HID */
    c[3] = (DWORD)naxes(d->joy); c[4] = (DWORD)nbtn(d->joy); c[5] = (DWORD)npov(d->joy);
    if (sz >= 44) { c[6] = 1000; c[7] = 1000; c[8] = 1; c[9] = 1; c[10] = 1; }
    return DI_OK;
}
typedef BOOL (WINAPI *enumobj_cb)(const void *doi, void *ctx);
static void fill_doi(uint8_t *o, int joy, int kind, int idx) {
    memset(o, 0, 316);
    *(uint32_t *)o = 316;
    const guid_t *g = kind == OT_AXIS ? &G_axes[idx] : kind == OT_BUTTON ? &G_Button : &G_POV;
    memcpy(o + 4, g, 16);
    *(uint32_t *)(o + 20) = default_ofs(kind, idx);
    *(uint32_t *)(o + 24) = obj_type(kind, idx);
    *(uint32_t *)(o + 28) = kind == OT_AXIS && g_br->joy[joy].has_ff && idx == 0 ? 1 /*DIDOI_FFACTUATOR*/ : 0;
    static const char *an[8] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider", "Slider 2" };
    if (kind == OT_AXIS) snprintf((char *)o + 32, 260, "%s", an[idx]);
    else if (kind == OT_BUTTON) snprintf((char *)o + 32, 260, "Button %d", idx + 1);
    else snprintf((char *)o + 32, 260, "POV %d", idx + 1);
    if (kind == OT_AXIS) { *(uint32_t *)(o + 292) = 10000; *(uint32_t *)(o + 296) = 1; }   /* FFMaxForce, FFForceResolution */
}
static HRESULT WINAPI dv_EnumObjects(didev_t *d, enumobj_cb cb, void *ctx, DWORD flags) {
    if (!J(d)) return DIERR_INPUTLOST;
    uint8_t o[316];
    int want_axes = !flags || (flags & 0x3), want_btn = !flags || (flags & 0xC), want_pov = !flags || (flags & 0x10);
    int ff_only = (flags & 0x01000000) != 0;
    for (int i = 0; want_axes && i < naxes(d->joy); i++) {
        if (ff_only && (!g_br->joy[d->joy].has_ff || i)) continue;
        fill_doi(o, d->joy, OT_AXIS, i); if (!cb(o, ctx)) return DI_OK;
    }
    if (ff_only) return DI_OK;
    for (int i = 0; want_btn && i < nbtn(d->joy); i++) { fill_doi(o, d->joy, OT_BUTTON, i); if (!cb(o, ctx)) return DI_OK; }
    for (int i = 0; want_pov && i < npov(d->joy); i++) { fill_doi(o, d->joy, OT_POV, i); if (!cb(o, ctx)) return DI_OK; }
    return DI_OK;
}
/* which axes a property header (dwObj/dwHow) addresses; returns bitmask */
static uint32_t prop_axes(didev_t *d, const uint32_t *hdr) {
    uint32_t obj = hdr[2], how = hdr[3];
    if (how == 0) return 0xff;                         /* DIPH_DEVICE */
    for (int i = 0; i < d->nfobj; i++) if (d->fobj[i].kind == OT_AXIS) {
        if ((how == 1 && d->fobj[i].ofs == obj) || (how == 2 && (obj & 0xffff00) == ((uint32_t)d->fobj[i].idx << 8))) return 1u << d->fobj[i].idx;
    }
    if (how == 1) { for (int i = 0; i < 8; i++) if (default_ofs(OT_AXIS, i) == obj) return 1u << i; }
    if (how == 2) { int i = (int)((obj >> 8) & 0xffff); if (i < 8 && (obj & 3)) return 1u << i; }
    return 0;
}
static HRESULT WINAPI dv_GetProperty(didev_t *d, uintptr_t prop, uint32_t *ph) {
    if (!ph) return DIERR_INVALIDPARAM;
    uint32_t m = prop_axes(d, ph); int a = 0; while (a < 8 && !(m & (1u << a))) a++; if (a == 8) a = 0;
    switch (prop) {
    case 4: ph[4] = (uint32_t)d->rmin[a]; ph[5] = (uint32_t)d->rmax[a]; return DI_OK;
    case 5: ph[4] = d->dead[a]; return DI_OK;
    case 6: ph[4] = d->sat[a]; return DI_OK;
    case 7: ph[4] = d->gain; return DI_OK;
    case 9: ph[4] = 0; return DI_OK;
    case 1: ph[4] = 0; return DI_OK;
    case 2: ph[4] = 0; return DI_OK;
    }
    if (prop > 0xffff) return DIERR_UNSUPPORTED;
    return DIERR_UNSUPPORTED;
}
static HRESULT WINAPI dv_SetProperty(didev_t *d, uintptr_t prop, const uint32_t *ph) {
    if (!ph) return DIERR_INVALIDPARAM;
    uint32_t m = prop_axes(d, ph);
    switch (prop) {
    case 4: for (int i = 0; i < 8; i++) if (m & (1u << i)) { d->rmin[i] = (int32_t)ph[4]; d->rmax[i] = (int32_t)ph[5]; }
        TRACE("dinput: range %d..%d (axes %x)", (int)ph[4], (int)ph[5], m); return DI_OK;
    case 5: for (int i = 0; i < 8; i++) if (m & (1u << i)) d->dead[i] = ph[4]; return DI_OK;
    case 6: for (int i = 0; i < 8; i++) if (m & (1u << i)) d->sat[i] = ph[4]; return DI_OK;
    case 7: { d->gain = ph[4]; br_ff_t f = { 0 }; f.cmd = FF_GAIN; f.joy = (uint32_t)d->joy; f.gain = (int32_t)ph[4]; plat_ff(&f); return DI_OK; }
    case 9: { br_ff_t f = { 0 }; f.cmd = FF_AUTOCENTER; f.joy = (uint32_t)d->joy; f.level = (int32_t)ph[4]; plat_ff(&f); LOG("dinput: autocenter %s", ph[4] ? "on" : "off"); return DI_OK; }
    case 1: case 2: return DI_OK;
    }
    return DI_OK;
}
static HRESULT WINAPI dv_Acquire(didev_t *d) { if (!J(d)) return DIERR_INPUTLOST; if (d->acquired) return DI_NOEFFECT; d->acquired = 1; return DI_OK; }
static HRESULT WINAPI dv_Unacquire(didev_t *d) { if (!d->acquired) return DI_NOEFFECT; d->acquired = 0; return DI_OK; }
static int32_t scale_axis(didev_t *d, int a, int32_t raw) {
    /* raw -32768..32767 -> [-10000,10000] deadzone/saturation -> range */
    double v = raw / 32767.5 + 1.0 / 65535.0;   /* -1..1 */
    if (v > 1) v = 1; if (v < -1) v = -1;
    double dz = d->dead[a] / 10000.0, sat = d->sat[a] / 10000.0;
    if (sat <= dz) sat = 1;
    double av = v < 0 ? -v : v;
    av = av <= dz ? 0 : av >= sat ? 1 : (av - dz) / (sat - dz);
    v = v < 0 ? -av : av;
    double lo = d->rmin[a], hi = d->rmax[a];
    double r = lo + (v + 1) * 0.5 * (hi - lo);
    return (int32_t)(r < 0 ? r - 0.5 : r + 0.5);
}
static HRESULT WINAPI dv_GetDeviceState(didev_t *d, DWORD cb, uint8_t *data) {
    br_joy_t *j = J(d);
    if (!data) return DIERR_INVALIDPARAM;
    if (!j || !j->present) return DIERR_INPUTLOST;
    if (!d->acquired) return DIERR_NOTACQUIRED;
    memset(data, 0, cb);
    for (int i = 0; i < d->nfobj; i++) {
        fmtobj_t *o = &d->fobj[i];
        if (o->ofs + 4 > cb && o->kind != OT_BUTTON) continue;
        if (o->kind == OT_AXIS) { int32_t v = o->idx < naxes(d->joy) ? scale_axis(d, o->idx, j->axis[o->idx]) : scale_axis(d, o->idx, 0); memcpy(data + o->ofs, &v, 4); }
        else if (o->kind == OT_POV) { int32_t v = o->idx < npov(d->joy) ? j->hat[o->idx] : -1; memcpy(data + o->ofs, &v, 4); }
        else if (o->ofs < cb) data[o->ofs] = o->idx < nbtn(d->joy) && j->button[o->idx] ? 0x80 : 0;
    }
    return DI_OK;
}
static HRESULT WINAPI dv_GetDeviceData(didev_t *d, DWORD cb, void *od, DWORD *inout, DWORD fl) { (void)d; (void)cb; (void)od; (void)fl; if (inout) *inout = 0; return DI_OK; }
/* DIDATAFORMAT: size objsize flags datasize numobjs rgodf; DIOBJECTDATAFORMAT: pguid ofs type flags */
static HRESULT WINAPI dv_SetDataFormat(didev_t *d, const uint32_t *df) {
    if (!df || !df[5]) return DIERR_INVALIDPARAM;
    d->data_size = df[3]; d->nfobj = 0;
    int na = 0, nb = 0, np = 0;
    const uint32_t *o = (const uint32_t *)(uintptr_t)df[5];
    for (uint32_t i = 0; i < df[4] && d->nfobj < 160; i++, o += df[1] / 4) {
        const guid_t *g = (const guid_t *)(uintptr_t)o[0];
        uint32_t ofs = o[1], type = o[2], inst = (type >> 8) & 0xffff, any = inst == 0xffff;
        fmtobj_t f = { 0, 0, ofs };
        if (type & 0x3) {        /* axis */
            int idx = -1;
            if (g) { for (int k = 0; k < 7; k++) if (geq(g, &G_axes[k])) { idx = k; if (k == 6) idx = na >= 6 ? na : 6; break; } }
            else idx = any ? na : (int)inst;
            if (idx < 0 || idx > 7) continue;
            f.kind = OT_AXIS; f.idx = idx; na++;
        } else if (type & 0xC) {
            f.kind = OT_BUTTON; f.idx = any ? nb : (int)inst; nb++;
            if (f.idx >= 32) continue;
        } else if (type & 0x10) {
            f.kind = OT_POV; f.idx = any ? np : (int)inst; np++;
            if (f.idx >= 4) continue;
        } else continue;
        d->fobj[d->nfobj++] = f;
    }
    LOG("dinput: data format %u bytes, %d objects", d->data_size, d->nfobj);
    return DI_OK;
}
static HRESULT WINAPI dv_SetEventNotification(didev_t *d, HANDLE h) { (void)d; (void)h; return DI_OK; }
static HRESULT WINAPI dv_SetCooperativeLevel(didev_t *d, HWND h, DWORD f) { (void)d; (void)h; LOG("dinput: device SetCooperativeLevel(%x)", f); return DI_OK; }
static HRESULT WINAPI dv_GetObjectInfo(didev_t *d, uint8_t *o, DWORD obj, DWORD how) {
    if (!o || !J(d)) return DIERR_INVALIDPARAM;
    uint8_t t[316]; uint32_t sz = *(uint32_t *)o; if (sz > 316 || sz < 32) sz = 316;
    int found = 0;
    for (int i = 0; !found && i < naxes(d->joy); i++) if ((how == 1 && default_ofs(OT_AXIS, i) == obj) || (how == 2 && ((obj >> 8) & 0xffff) == (uint32_t)i && (obj & 3))) { fill_doi(t, d->joy, OT_AXIS, i); found = 1; }
    for (int i = 0; !found && i < nbtn(d->joy); i++) if ((how == 1 && default_ofs(OT_BUTTON, i) == obj) || (how == 2 && ((obj >> 8) & 0xffff) == (uint32_t)i && (obj & 0xC))) { fill_doi(t, d->joy, OT_BUTTON, i); found = 1; }
    if (!found) return DIERR_OBJECTNOTFOUND;
    memcpy(o + 4, t + 4, sz - 4);
    return DI_OK;
}
static void fill_instance(uint8_t *di, int joy) {
    uint32_t sz = *(uint32_t *)di; if (sz < 548 || sz > 580) sz = 580;
    memset(di + 4, 0, sz - 4);
    guid_t inst = G_Joystick; inst.a = 0x47503300 + (uint32_t)joy;   /* "GP3" + index: stable per session */
    memcpy(di + 4, &inst, 16);
    memcpy(di + 20, g_br->joy[joy].guid, 16);
    *(uint32_t *)(di + 36) = 4 | (2 << 8) | 0x10000;
    snprintf((char *)di + 40, 260, "%s", g_br->joy[joy].name);
    snprintf((char *)di + 300, 260, "%s", g_br->joy[joy].name);
    if (sz >= 580 && g_br->joy[joy].has_ff) memcpy(di + 560, &G_FFDriver, 16);
    if (sz >= 580) { *(uint16_t *)(di + 576) = 1; *(uint16_t *)(di + 578) = 4; }
}
static HRESULT WINAPI dv_GetDeviceInfo(didev_t *d, uint8_t *di) { if (!di || !J(d)) return DIERR_INVALIDPARAM; fill_instance(di, d->joy); return DI_OK; }
static HRESULT WINAPI dv_RunControlPanel(didev_t *d, HWND h, DWORD f) { (void)d; (void)h; (void)f; return DI_OK; }
static HRESULT WINAPI dv_Initialize(didev_t *d, uint32_t i, DWORD v, const void *g) { (void)d; (void)i; (void)v; (void)g; return DI_OK; }
static HRESULT WINAPI dv_CreateEffect(didev_t *d, const guid_t *g, const uint32_t *de, dieff_t **out, void *outer) {
    (void)outer;
    if (!g || !out) return DIERR_INVALIDPARAM;
    int k = 0; while (k < NEFF && !geq(g, &G_eff[k])) k++;
    if (k == NEFF) { LOG("dinput: CreateEffect: unknown effect %08x", g->a); return DIERR_DEVICENOTREG; }
    br_joy_t *j = J(d); if (!j || !j->has_ff) return DIERR_UNSUPPORTED;
    dieff_t *e = calloc(1, sizeof *e); e->vtbl = g_eff_vtbl; e->refs = 1; e->dev = d; e->type = T_eff[k]; e->gidx = k;
    e->id = g_next_eff++; e->p.gain = 10000; e->p.duration_ms = -1; e->p.sat_pos = e->p.sat_neg = 10000;
    eff_send(e, FF_CREATE);
    if (de) { read_params(e, de, 0x3ff); eff_send(e, FF_UPDATE); }
    LOG("dinput: CreateEffect(%s) -> #%u", N_eff[k], e->id);
    *out = e;
    return DI_OK;
}
typedef BOOL (WINAPI *enumeff_cb)(const void *ei, void *ctx);
static void fill_effinfo(uint8_t *ei, int k) {   /* DIEFFECTINFOA: size guid type static dynamic name[260] = 292 bytes */
    memset(ei, 0, 292); *(uint32_t *)ei = 292;
    memcpy(ei + 4, &G_eff[k], 16);
    uint32_t t = T_eff[k] == FFT_CONSTANT ? 5 : T_eff[k] == FFT_RAMP ? 2 : T_eff[k] <= FFT_INERTIA ? 4 : 3;   /* DIEFT_ type */
    *(uint32_t *)(ei + 20) = t | 0x200 /*FFATTACK*/ | 0x400 /*FFFADE*/ | 0x800 /*SATURATION*/ | 0x1000 /*POSNEGCOEFF*/ | 0x4000 /*DEADBAND*/;
    *(uint32_t *)(ei + 24) = 0x3ff; *(uint32_t *)(ei + 28) = 0x3ff;
    snprintf((char *)ei + 32, 260, "%s", N_eff[k]);
}
static HRESULT WINAPI dv_EnumEffects(didev_t *d, enumeff_cb cb, void *ctx, DWORD type) {
    br_joy_t *j = J(d); if (!j || !j->has_ff) return DI_OK;
    uint8_t ei[292];
    for (int k = 0; k < NEFF; k++) { (void)type; fill_effinfo(ei, k); if (!cb(ei, ctx)) break; }
    return DI_OK;
}
static HRESULT WINAPI dv_GetEffectInfo(didev_t *d, uint8_t *ei, const guid_t *g) {
    (void)d; for (int k = 0; k < NEFF; k++) if (geq(g, &G_eff[k])) { fill_effinfo(ei, k); return DI_OK; }
    return DIERR_DEVICENOTREG;
}
static HRESULT WINAPI dv_GetForceFeedbackState(didev_t *d, DWORD *s) { (void)d; if (s) *s = 0x200 /*ACTUATORSON*/ | 0x10 /*POWERON*/; return DI_OK; }
static HRESULT WINAPI dv_SendForceFeedbackCommand(didev_t *d, DWORD c) {
    br_ff_t f = { 0 }; f.joy = (uint32_t)d->joy;
    if (c & (1 | 2)) { f.cmd = FF_STOPALL; plat_ff(&f); }
    TRACE("dinput: SendForceFeedbackCommand(%x)", c);
    return DI_OK;
}
static HRESULT WINAPI dv_EnumCreatedEffectObjects(didev_t *d, void *cb, void *ctx, DWORD f) { (void)d; (void)cb; (void)ctx; (void)f; return DI_OK; }
static HRESULT WINAPI dv_Escape(didev_t *d, void *e) { (void)d; (void)e; return DIERR_UNSUPPORTED; }
static HRESULT WINAPI dv_Poll(didev_t *d) { return J(d) ? DI_NOEFFECT : DIERR_INPUTLOST; }
static HRESULT WINAPI dv_SendDeviceData(didev_t *d, DWORD cb, const void *od, DWORD *io, DWORD f) { (void)d; (void)cb; (void)od; (void)f; if (io) *io = 0; return DI_OK; }
static HRESULT WINAPI dv_EnumEffectsInFile(didev_t *d, const char *f, void *cb, void *ctx, DWORD fl) { (void)d; (void)f; (void)cb; (void)ctx; (void)fl; return DI_OK; }
static HRESULT WINAPI dv_WriteEffectToFile(didev_t *d, const char *f, DWORD n, void *e, DWORD fl) { (void)d; (void)f; (void)n; (void)e; (void)fl; return DIERR_UNSUPPORTED; }

/* ------------------------------------------------------------------ */
/* IDirectInputA / 2A / 7A                                              */
static HRESULT WINAPI di_QueryInterface(dinput_t *This, const uint8_t *iid, void **out) { (void)iid; *out = This; This->refs++; return DI_OK; }
static DWORD WINAPI di_AddRef(dinput_t *This) { return (DWORD)++This->refs; }
static DWORD WINAPI di_Release(dinput_t *This) { LONG r = --This->refs; return (DWORD)(r > 0 ? r : 0); }
static int joy_from_guid(const guid_t *g) {
    if (!g || !g_br) return -1;
    guid_t t = G_Joystick;
    for (int i = 0; i < (int)g_br->njoy; i++) { t.a = 0x47503300 + (uint32_t)i; if (geq(g, &t) && joy_present(i)) return i; }
    for (int i = 0; i < (int)g_br->njoy; i++) if (joy_present(i) && geq(g, g_br->joy[i].guid)) return i;
    return -1;
}
static HRESULT WINAPI di_CreateDevice(dinput_t *This, const guid_t *g, didev_t **out, void *outer) {
    (void)This; (void)outer;
    int j = joy_from_guid(g);
    if (j < 0) { LOG("dinput: CreateDevice(%08x) -> not found (keyboard/mouse are delivered as window messages)", g ? g->a : 0); *out = NULL; return DIERR_DEVICENOTREG; }
    didev_t *d = calloc(1, sizeof *d); d->vtbl = g_dev_vtbl; d->refs = 1; d->joy = j; d->gain = 10000;
    for (int i = 0; i < 8; i++) { d->rmin[i] = 0; d->rmax[i] = 65535; d->sat[i] = 10000; }
    *out = d;
    LOG("dinput: CreateDevice -> joystick %d \"%s\" (%d axes, %d buttons, %d POV, force feedback %s)", j, g_br->joy[j].name,
        naxes(j), nbtn(j), npov(j), g_br->joy[j].has_ff ? "yes" : "no");
    return DI_OK;
}
typedef BOOL (WINAPI *enumdev_cb)(const void *di, void *ctx);
static HRESULT WINAPI di_EnumDevices(dinput_t *This, DWORD type, enumdev_cb cb, void *ctx, DWORD fl) {
    (void)This;
    int n = 0;
    /* joysticks: type 0 (all) or DIDEVTYPE_JOYSTICK (4) / DI8 game controllers (4) */
    if (g_br && (type == 0 || (type & 0xff) == 4)) {
        uint8_t di[580];
        for (int i = 0; i < (int)g_br->njoy; i++) {
            if (!joy_present(i)) continue;
            if ((fl & 0x100 /*FORCEFEEDBACK*/) && !g_br->joy[i].has_ff) continue;
            *(uint32_t *)di = 580; fill_instance(di, i); n++;
            if (!cb(di, ctx)) break;
        }
    }
    LOG("DirectInput::EnumDevices(type=%u, flags=%x) -> %d controller(s)", type, fl, n);
    return DI_OK;
}
static HRESULT WINAPI di_GetDeviceStatus(dinput_t *This, const guid_t *g) { (void)This; return joy_from_guid(g) >= 0 ? DI_OK : 1; }
static HRESULT WINAPI di_RunControlPanel(dinput_t *This, HWND h, DWORD f) { (void)This; (void)h; (void)f; return DI_OK; }
static HRESULT WINAPI di_Initialize(dinput_t *This, uint32_t i, DWORD v) { (void)This; (void)i; (void)v; return DI_OK; }
static HRESULT WINAPI di_FindDevice(dinput_t *This, const guid_t *c, const char *n, guid_t *out) { (void)This; (void)c; (void)n; (void)out; return DIERR_NOTFOUND; }
static HRESULT WINAPI di_CreateDeviceEx(dinput_t *This, const guid_t *g, const uint8_t *iid, didev_t **out, void *outer) { (void)iid; return di_CreateDevice(This, g, out, outer); }
static HRESULT WINAPI DirectInputCreateA_(uint32_t inst, DWORD ver, void **out, void *outer) {
    (void)inst; (void)outer;
    dinput_t *d = calloc(1, sizeof *d); d->vtbl = g_di_vtbl; d->refs = 1; *out = d;
    LOG("DirectInputCreateA(version=%x) -> %d controller(s) available", ver, g_br ? (int)g_br->njoy : 0);
    return DI_OK;
}
static HRESULT WINAPI DirectInputCreateEx_(uint32_t inst, DWORD ver, const uint8_t *iid, void **out, void *outer) { (void)iid; return DirectInputCreateA_(inst, ver, out, outer); }

/* ------------------------------------------------------------------ */
/* winmm joystick API on top of the same devices                        */
UINT joy_count(void) { return g_br ? g_br->njoy : 0; }
UINT joy_caps(UINT id, uint8_t *caps, UINT n) {
    if (!joy_present((int)id) || n < 404) return 167;   /* JOYERR_UNPLUGGED */
    memset(caps, 0, n);
    snprintf((char *)caps + 4, 32, "%s", g_br->joy[id].name);
    uint32_t *c = (uint32_t *)(caps + 36);
    for (int i = 0; i < 6; i++) { c[2 * i] = 0; c[2 * i + 1] = 65535; }   /* x..r min/max, then period */
    *(uint32_t *)(caps + 88) = (uint32_t)nbtn((int)id);                   /* wNumButtons */
    return 0;
}
UINT joy_pos(UINT id, uint32_t *ji) {
    if (!joy_present((int)id)) return 167;
    br_joy_t *j = &g_br->joy[id];
    for (int i = 0; i < 6; i++) ji[2 + i] = (uint32_t)(i < (int)j->naxes ? j->axis[i] + 32768 : 32768);
    uint32_t b = 0; for (int i = 0; i < 32 && i < (int)j->nbuttons; i++) if (j->button[i]) b |= 1u << i;
    ji[8] = b; ji[9] = 0; ji[10] = j->nhats ? (uint32_t)j->hat[0] : 0xffff;
    return 0;
}

void dinput_register(void) {
    static void *v[] = { di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus, di_RunControlPanel,
                         di_Initialize, di_FindDevice, di_CreateDeviceEx };
    static void *dv[] = { dv_QueryInterface, dv_AddRef, dv_Release, dv_GetCapabilities, dv_EnumObjects, dv_GetProperty, dv_SetProperty,
                          dv_Acquire, dv_Unacquire, dv_GetDeviceState, dv_GetDeviceData, dv_SetDataFormat, dv_SetEventNotification,
                          dv_SetCooperativeLevel, dv_GetObjectInfo, dv_GetDeviceInfo, dv_RunControlPanel, dv_Initialize, dv_CreateEffect,
                          dv_EnumEffects, dv_GetEffectInfo, dv_GetForceFeedbackState, dv_SendForceFeedbackCommand, dv_EnumCreatedEffectObjects,
                          dv_Escape, dv_Poll, dv_SendDeviceData, dv_EnumEffectsInFile, dv_WriteEffectToFile };
    static void *ev[] = { ef_QueryInterface, ef_AddRef, ef_Release, ef_Initialize, ef_GetEffectGuid, ef_GetParameters, ef_SetParameters,
                          ef_Start, ef_Stop, ef_GetEffectStatus, ef_Download, ef_Unload, ef_Escape };
    g_di_vtbl = v; g_dev_vtbl = dv; g_eff_vtbl = ev;
    (void)G_XAxis;
    static const export_t ex[] = { { "dinput", "DirectInputCreateA", 0, (void *)DirectInputCreateA_ },
                                   { "dinput", "DirectInputCreateEx", 0, (void *)DirectInputCreateEx_ }, { NULL, NULL, 0, NULL } };
    rt_register_exports(ex);
}
