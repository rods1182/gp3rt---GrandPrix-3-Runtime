/* Shared-memory protocol between the Linux host (gp3, 64-bit: SDL2 window,
 * OpenGL, audio, input) and the game process (gp3-game, 32-bit static: runs
 * Gp3.exe). Layout uses only fixed-size types so both sides agree.
 *
 * guest -> host : command ring (renderer calls, mode changes, ...)
 * host  -> guest: input event ring, reply buffer for synchronous requests
 * guest -> host : audio ring (stereo s16 PCM, mixed by the guest)
 * host  -> guest: joystick state
 * Waiting uses futexes on words of the shared mapping. */
#ifndef BRIDGE_H
#define BRIDGE_H
#include <stdint.h>

#define BR_MAGIC   0x42335047u     /* "GP3B" */
#define BR_VERSION 1

#define BR_RING_SIZE   (64u << 20)
#define BR_REPLY_SIZE  (40u << 20)
#define BR_AUDIO_SIZE  (1u << 18)  /* bytes, power of two */
#define BR_NEV         1024
#define BR_MAXJOY      8
#define BR_JAXES       8
#define BR_JBUTTONS    32

/* command ops (guest -> host) */
enum {
    BR_WRAP = 0,        /* rest of ring is padding, continue at 0 */
    BR_CHUNK,           /* payload appended to the host staging buffer */
    BR_TEXC,            /* id w h levels */
    BR_TEXU,            /* id level w h [pixels] */
    BR_TEXD,            /* id */
    BR_TGTC,            /* id w h scale */
    BR_TGTD,            /* id */
    BR_TGTU,            /* id x y w h [pixels w*h] */
    BR_TGTDL,           /* id y0 n -> reply: n rows of target width */
    BR_CLEAR,           /* id nrects [rects] color argb depth z vx vy vw vh */
    BR_DRAW,            /* id wstate prim nv [verts] ni [idx16] */
    BR_BLIT,            /* id dx dy dw dh sw sh keyed [pixels sw*sh] */
    BR_COPY,            /* id dx dy dw dh sx sy sw sh */
    BR_COPYFROM,        /* id dx dy dw dh src sx sy sw sh */
    BR_PRESENT,         /* id w h frame */
    BR_PRESENTA,        /* w h frame [pixels] */
    BR_SETMODE,         /* w h bpp */
    BR_CURSOR,          /* show */
    BR_MSGBOX,          /* caption\0text\0 */
    BR_SYNC,            /* -> reply (no data): host has processed everything before */
    BR_TITLE,           /* text\0 */
    BR_FF,              /* joystick force feedback command (see br_ff_t) */
    BR_SHOT,            /* screenshot request (name\0) */
    BR_NOPS
};
#define BR_STAGED 0x80000000u     /* op flag: payload is in the staging buffer */

typedef struct { uint32_t op, len; } br_hdr_t;    /* len = bytes after header (8-aligned) */

/* wire form of rstate_t */
typedef struct {
    uint32_t tex, has_alpha, has_ckey, colorop, carg1, carg2, alphaop, aarg1, aarg2, texcoord,
             addru, addrv, magf, minf, mipf, border;
    float lodbias;
} br_stage_t;
typedef struct {
    int32_t zenable, zwrite, zfunc, cull, blend, srcblend, dstblend, atest, afunc;
    uint32_t aref;
    int32_t fog, fogtable; uint32_t fogcolor; float fogstart, fogend, fogdensity;
    int32_t flat, specular, ckey, dither; uint32_t tfactor; float zbias; int32_t nstages;
    br_stage_t st[2];
    int32_t vx, vy, vw, vh; float minz, maxz;
} br_state_t;

/* input events (host -> guest) */
enum { BE_KEY = 1, BE_CHAR, BE_MOUSE, BE_MOVE, BE_ACTIVATE, BE_QUIT, BE_WHEEL };
typedef struct { uint32_t type; int32_t a, b, c, d; } br_event_t;

/* force feedback (guest -> host) */
enum { FF_CREATE = 1, FF_UPDATE, FF_START, FF_STOP, FF_DESTROY, FF_STOPALL, FF_GAIN, FF_AUTOCENTER };
enum { FFT_CONSTANT = 1, FFT_SPRING, FFT_DAMPER, FFT_FRICTION, FFT_INERTIA, FFT_SINE, FFT_SQUARE, FFT_TRIANGLE,
       FFT_SAWUP, FFT_SAWDOWN, FFT_RAMP };
typedef struct {
    uint32_t cmd, joy, id, type;
    int32_t level, offset, period_us, phase, magnitude, start, end;   /* -10000..10000 (DirectInput units) */
    int32_t coef_pos, coef_neg, sat_pos, sat_neg, deadband, center;   /* conditions */
    int32_t duration_ms, delay_ms, gain, direction;                   /* direction: DI polar hundredths of degree */
    int32_t attack_level, attack_ms, fade_level, fade_ms, iterations;
} br_ff_t;

typedef struct {
    uint32_t present;            /* device connected */
    uint32_t naxes, nbuttons, nhats, has_ff;
    char name[64];
    uint8_t guid[16];
    volatile int32_t axis[BR_JAXES];     /* -32768..32767 */
    volatile uint8_t button[BR_JBUTTONS];
    volatile int32_t hat[4];             /* DirectInput POV: hundredths of degree, -1 centred */
    volatile uint32_t seq;               /* increments on every update */
} br_joy_t;

typedef struct {
    uint32_t magic, version, total_size;
    uint32_t ring_off, reply_off, audio_off;
    /* command ring */
    volatile uint32_t head, tail;        /* byte positions modulo BR_RING_SIZE (head: guest, tail: host) */
    volatile uint32_t cmd_wake;          /* futex: guest bumps after writing */
    volatile uint32_t space_wake;        /* futex: host bumps after consuming */
    volatile uint32_t host_waiting;      /* host is (about to be) asleep on cmd_wake */
    volatile uint32_t guest_waiting;     /* guest is asleep on space_wake / reply_seq / frames_done */
    /* synchronous replies */
    volatile uint32_t reply_seq;         /* futex: host bumps after filling the reply */
    volatile uint32_t reply_len;
    /* pacing */
    volatile uint32_t frames_done;       /* host: presents executed */
    /* input */
    volatile uint32_t ev_head, ev_tail;  /* host writes head, guest reads tail */
    br_event_t ev[BR_NEV];
    volatile uint32_t quit;              /* host asks the game to close */
    volatile int32_t win_w, win_h;       /* host drawable size */
    volatile uint32_t focused;
    /* audio (s16 stereo interleaved) */
    volatile uint32_t a_head, a_tail;    /* byte counters (free running) */
    volatile uint32_t a_rate;            /* host output rate */
    volatile uint32_t a_underruns;
    /* joysticks */
    volatile uint32_t njoy;
    br_joy_t joy[BR_MAXJOY];
    /* misc settings passed from host to guest */
    int32_t opt_scale, opt_vsync, opt_spray, opt_wide_w, opt_wide_h, opt_hor_plus, opt_fov, opt_reserved[8];
} br_shared_t;

#define BR_SHM_SIZE (4096u + BR_RING_SIZE + BR_REPLY_SIZE + BR_AUDIO_SIZE + (1u << 20))

#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>
#include <limits.h>
static inline void br_futex_wait(volatile uint32_t *addr, uint32_t val, int timeout_ms) {
#if defined(__i386__)
    struct { int32_t s, ns; } ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000 };   /* old 32-bit timespec for SYS_futex */
#else
    struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
#endif
    syscall(SYS_futex, addr, 0 /*FUTEX_WAIT*/, val, timeout_ms >= 0 ? (void *)&ts : NULL, NULL, 0);
}
static inline void br_futex_wake(volatile uint32_t *addr) { syscall(SYS_futex, addr, 1 /*FUTEX_WAKE*/, INT_MAX, NULL, NULL, 0); }
#endif
#endif
