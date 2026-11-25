/* Joysticks / steering wheels for the Linux host: SDL joystick state is
 * published in shared memory (br_joy_t) for the game's DirectInput, and
 * force-feedback effects created by the game are played through SDL_Haptic
 * (Linux evdev force feedback: Logitech, Thrustmaster, Fanatec... wheels
 * with kernel drivers). */
#include <SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "../src/bridge.h"

static br_shared_t *B;
static SDL_Joystick *g_js[BR_MAXJOY];
static SDL_Haptic *g_hap[BR_MAXJOY];
static unsigned g_hapcaps[BR_MAXJOY];
#define MAXEFF 64
static struct { uint32_t id; int joy, sdl, type, running; br_ff_t p; } g_eff[MAXEFF];
static int g_gain[BR_MAXJOY];

void hlog(const char *fmt, ...);
static void jlog(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    hlog("%s", b);
}

static void open_joy(int dev) {
    int slot = -1;
    for (int i = 0; i < BR_MAXJOY; i++) if (!g_js[i]) { slot = i; break; }
    if (slot < 0) return;
    SDL_Joystick *js = SDL_JoystickOpen(dev);
    if (!js) return;
    for (int i = 0; i < BR_MAXJOY; i++) if (g_js[i] && SDL_JoystickInstanceID(g_js[i]) == SDL_JoystickInstanceID(js)) { SDL_JoystickClose(js); return; }
    g_js[slot] = js;
    br_joy_t *j = &B->joy[slot];
    memset((void *)j, 0, sizeof *j);
    snprintf(j->name, sizeof j->name, "%s", SDL_JoystickName(js) ? SDL_JoystickName(js) : "Joystick");
    SDL_JoystickGUID g = SDL_JoystickGetGUID(js); memcpy(j->guid, g.data, 16);
    j->naxes = (uint32_t)SDL_JoystickNumAxes(js); j->nbuttons = (uint32_t)SDL_JoystickNumButtons(js); j->nhats = (uint32_t)SDL_JoystickNumHats(js);
    if (j->naxes > BR_JAXES) j->naxes = BR_JAXES; if (j->nbuttons > BR_JBUTTONS) j->nbuttons = BR_JBUTTONS; if (j->nhats > 4) j->nhats = 4;
    for (int h = 0; h < 4; h++) j->hat[h] = -1;
    g_hap[slot] = SDL_HapticOpenFromJoystick(js);
    g_hapcaps[slot] = g_hap[slot] ? SDL_HapticQuery(g_hap[slot]) : 0;
    j->has_ff = g_hap[slot] && (g_hapcaps[slot] & (SDL_HAPTIC_CONSTANT | SDL_HAPTIC_SPRING | SDL_HAPTIC_DAMPER)) ? 1 : 0;
    g_gain[slot] = 100;
    if (g_hap[slot] && (g_hapcaps[slot] & SDL_HAPTIC_GAIN)) SDL_HapticSetGain(g_hap[slot], 100);
    if (getenv("GP3_FAKE_FF")) j->has_ff = 1;   /* testing: pretend the device has force feedback (commands are logged) */
    j->present = 1;
    if ((uint32_t)slot + 1 > B->njoy) B->njoy = (uint32_t)slot + 1;
    jlog("controller %d: \"%s\" - %u axes, %u buttons, %u hats, force feedback: %s%s%s%s", slot, j->name, j->naxes, j->nbuttons, j->nhats,
         j->has_ff ? "yes" : "no", (g_hapcaps[slot] & SDL_HAPTIC_CONSTANT) ? " constant" : "", (g_hapcaps[slot] & SDL_HAPTIC_SPRING) ? " spring" : "",
         (g_hapcaps[slot] & SDL_HAPTIC_DAMPER) ? " damper" : "");
}
static void close_slot(int s) {
    for (int k = 0; k < MAXEFF; k++) if (g_eff[k].id && g_eff[k].joy == s) g_eff[k].id = 0;
    if (g_hap[s]) SDL_HapticClose(g_hap[s]);
    if (g_js[s]) SDL_JoystickClose(g_js[s]);
    g_hap[s] = NULL; g_js[s] = NULL; B->joy[s].present = 0;
}

void joy_init(br_shared_t *b) {
    B = b;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC)) { jlog("joystick: %s", SDL_GetError()); SDL_InitSubSystem(SDL_INIT_JOYSTICK); }
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) open_joy(i);
    jlog("%d game controller(s)", n);
}
void joy_event(const SDL_Event *e) {
    if (e->type == SDL_JOYDEVICEADDED) open_joy(e->jdevice.which);
    else if (e->type == SDL_JOYDEVICEREMOVED) {
        for (int s = 0; s < BR_MAXJOY; s++) if (g_js[s] && SDL_JoystickInstanceID(g_js[s]) == e->jdevice.which) { jlog("controller %d removed", s); close_slot(s); }
    }
}
static int32_t hat_to_pov(Uint8 h) {
    switch (h) {
    case SDL_HAT_UP: return 0; case SDL_HAT_RIGHTUP: return 4500; case SDL_HAT_RIGHT: return 9000; case SDL_HAT_RIGHTDOWN: return 13500;
    case SDL_HAT_DOWN: return 18000; case SDL_HAT_LEFTDOWN: return 22500; case SDL_HAT_LEFT: return 27000; case SDL_HAT_LEFTUP: return 31500;
    }
    return -1;
}
void joy_poll(void) {
    for (int s = 0; s < BR_MAXJOY; s++) {
        if (!g_js[s]) continue;
        br_joy_t *j = &B->joy[s];
        for (uint32_t a = 0; a < j->naxes; a++) j->axis[a] = SDL_JoystickGetAxis(g_js[s], (int)a);
        for (uint32_t b = 0; b < j->nbuttons; b++) j->button[b] = SDL_JoystickGetButton(g_js[s], (int)b);
        for (uint32_t h = 0; h < j->nhats; h++) j->hat[h] = hat_to_pov(SDL_JoystickGetHat(g_js[s], (int)h));
        __atomic_add_fetch(&j->seq, 1, __ATOMIC_RELEASE);
    }
}

/* ---- force feedback ---- */
static Sint16 lvl(int32_t di, int32_t gain) {   /* DI -10000..10000 (times effect gain 0..10000) -> SDL */
    long v = (long)di * 32767 / 10000; v = v * gain / 10000;
    return (Sint16)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}
static Uint16 ulvl(int32_t di) { long v = (long)di * 65535 / 10000; return (Uint16)(v < 0 ? 0 : v > 65535 ? 65535 : v); }
static int build(SDL_HapticEffect *h, const br_ff_t *p, unsigned caps) {
    memset(h, 0, sizeof *h);
    Uint32 len = p->duration_ms < 0 ? SDL_HAPTIC_INFINITY : (Uint32)p->duration_ms;
    int gain = p->gain ? p->gain : 10000;
    /* one-axis wheel: force along x; DirectInput polar 9000 = +x, 27000 = -x */
    int flip = p->direction > 18000 && p->direction < 36000;
    switch (p->type) {
    case FFT_CONSTANT:
        if (!(caps & SDL_HAPTIC_CONSTANT)) return 0;
        h->type = SDL_HAPTIC_CONSTANT;
        h->constant.direction.type = SDL_HAPTIC_CARTESIAN; h->constant.direction.dir[0] = 1;
        h->constant.length = len; h->constant.delay = (Uint16)p->delay_ms;
        h->constant.level = lvl(flip ? -p->level : p->level, gain);
        h->constant.attack_length = (Uint16)p->attack_ms; h->constant.attack_level = ulvl(p->attack_level);
        h->constant.fade_length = (Uint16)p->fade_ms; h->constant.fade_level = ulvl(p->fade_level);
        return 1;
    case FFT_RAMP:
        if (!(caps & SDL_HAPTIC_RAMP)) return 0;
        h->type = SDL_HAPTIC_RAMP; h->ramp.direction.type = SDL_HAPTIC_CARTESIAN; h->ramp.direction.dir[0] = 1;
        h->ramp.length = len == SDL_HAPTIC_INFINITY ? 1000 : len; h->ramp.start = lvl(p->start, gain); h->ramp.end = lvl(p->end, gain);
        return 1;
    case FFT_SPRING: case FFT_DAMPER: case FFT_FRICTION: case FFT_INERTIA: {
        unsigned t = p->type == FFT_SPRING ? SDL_HAPTIC_SPRING : p->type == FFT_DAMPER ? SDL_HAPTIC_DAMPER :
                     p->type == FFT_FRICTION ? SDL_HAPTIC_FRICTION : SDL_HAPTIC_INERTIA;
        if (!(caps & t)) return 0;
        h->type = (Uint16)t;
        SDL_HapticCondition *c = &h->condition;
        c->direction.type = SDL_HAPTIC_CARTESIAN; c->direction.dir[0] = 1;
        c->length = len; c->delay = (Uint16)p->delay_ms;
        c->right_sat[0] = (Uint16)(ulvl(p->sat_pos) * (long)gain / 10000); c->left_sat[0] = (Uint16)(ulvl(p->sat_neg) * (long)gain / 10000);
        c->right_coeff[0] = lvl(p->coef_pos, gain); c->left_coeff[0] = lvl(p->coef_neg, gain);
        c->deadband[0] = ulvl(p->deadband); c->center[0] = lvl(p->offset, 10000);
        return 1; }
    default: {
        unsigned t = p->type == FFT_SINE ? SDL_HAPTIC_SINE : p->type == FFT_TRIANGLE ? SDL_HAPTIC_TRIANGLE :
                     p->type == FFT_SAWUP ? SDL_HAPTIC_SAWTOOTHUP : p->type == FFT_SAWDOWN ? SDL_HAPTIC_SAWTOOTHDOWN : SDL_HAPTIC_SINE;
        if (!(caps & t)) return 0;
        h->type = (Uint16)t;
        SDL_HapticPeriodic *q = &h->periodic;
        q->direction.type = SDL_HAPTIC_CARTESIAN; q->direction.dir[0] = 1;
        q->length = len; q->delay = (Uint16)p->delay_ms;
        q->period = (Uint16)(p->period_us / 1000 ? p->period_us / 1000 : 1);
        q->magnitude = lvl(p->magnitude, gain); q->offset = lvl(p->offset, 10000); q->phase = (Uint16)(p->phase * 36000L / 36000);
        q->attack_length = (Uint16)p->attack_ms; q->attack_level = ulvl(p->attack_level);
        q->fade_length = (Uint16)p->fade_ms; q->fade_level = ulvl(p->fade_level);
        return 1; }
    }
}
static int find_eff(uint32_t id) { for (int k = 0; k < MAXEFF; k++) if (g_eff[k].id == id) return k; return -1; }
void joy_ff(const br_ff_t *f) {
    int s = (int)f->joy;
    static int fake = -1; if (fake < 0) fake = getenv("GP3_FAKE_FF") != NULL;
    if (fake) { static int n; if (n++ < 400) jlog("FF cmd %u eff #%u type %u level %d coef %d/%d sat %d/%d dead %d off %d dir %d dur %d gain %d",
        f->cmd, f->id, f->type, f->level, f->coef_pos, f->coef_neg, f->sat_pos, f->sat_neg, f->deadband, f->offset, f->direction, f->duration_ms, f->gain); }
    if (s < 0 || s >= BR_MAXJOY || !g_hap[s]) return;
    SDL_Haptic *h = g_hap[s];
    switch (f->cmd) {
    case FF_CREATE: {
        int k = find_eff(0); if (k < 0) return;
        g_eff[k].id = f->id; g_eff[k].joy = s; g_eff[k].sdl = -1; g_eff[k].type = (int)f->type; g_eff[k].running = 0; g_eff[k].p = *f;
        return; }
    case FF_UPDATE: {
        int k = find_eff(f->id); if (k < 0) return;
        g_eff[k].p = *f;
        SDL_HapticEffect he;
        if (!build(&he, f, g_hapcaps[s])) return;
        if (g_eff[k].sdl < 0) { g_eff[k].sdl = SDL_HapticNewEffect(h, &he); if (g_eff[k].sdl < 0) jlog("force feedback: effect type %d not created: %s", f->type, SDL_GetError()); }
        else SDL_HapticUpdateEffect(h, g_eff[k].sdl, &he);
        return; }
    case FF_START: {
        int k = find_eff(f->id); if (k < 0) return;
        if (g_eff[k].sdl < 0) { br_ff_t p = g_eff[k].p; p.cmd = FF_UPDATE; joy_ff(&p); }
        if (g_eff[k].sdl >= 0) { SDL_HapticRunEffect(h, g_eff[k].sdl, f->iterations < 0 ? SDL_HAPTIC_INFINITY : (Uint32)(f->iterations ? f->iterations : 1)); g_eff[k].running = 1; }
        return; }
    case FF_STOP: { int k = find_eff(f->id); if (k >= 0 && g_eff[k].sdl >= 0) { SDL_HapticStopEffect(h, g_eff[k].sdl); g_eff[k].running = 0; } return; }
    case FF_DESTROY: { int k = find_eff(f->id); if (k >= 0) { if (g_eff[k].sdl >= 0) SDL_HapticDestroyEffect(h, g_eff[k].sdl); g_eff[k].id = 0; } return; }
    case FF_STOPALL: SDL_HapticStopAll(h); for (int k = 0; k < MAXEFF; k++) if (g_eff[k].id && g_eff[k].joy == s) g_eff[k].running = 0; return;
    case FF_GAIN: if (g_hapcaps[s] & SDL_HAPTIC_GAIN) SDL_HapticSetGain(h, f->gain / 100); return;
    case FF_AUTOCENTER: if (g_hapcaps[s] & SDL_HAPTIC_AUTOCENTER) SDL_HapticSetAutocenter(h, f->level ? 100 : 0); return;
    }
}
void joy_shutdown(void) {
    for (int s = 0; s < BR_MAXJOY; s++) if (g_js[s]) { if (g_hap[s]) SDL_HapticStopAll(g_hap[s]); close_slot(s); }
}
