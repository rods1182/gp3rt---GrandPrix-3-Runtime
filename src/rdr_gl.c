/* OpenGL 3.3 core renderer backend.
 * Platform independent: the host provides a GL function loader and a
 * "present" callback (swap buffers) through rdr_gl_init(). Used by the
 * Windows ddraw.dll (WGL) and the Linux host (SDL2/EGL), and by the test
 * replay tool (EGL surfaceless). */
#include "rdr.h"
#include "rdr_gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>

typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean, GLubyte;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr, GLintptr;
#ifdef _WIN32
#define GLAPI __attribute__((stdcall))
#else
#define GLAPI
#endif

#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_BORDER_COLOR 0x1004
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE0 0x84C0
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_DEPTH_CLAMP 0x864F
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_MULTISAMPLE 0x809D
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_NEVER 0x0200
#define GL_TRIANGLES 0x0004
#define GL_LINES 0x0001
#define GL_POINTS 0x0000
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_FIRST_VERTEX_CONVENTION 0x8E4D
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

#ifndef GL_PIXEL_UNPACK_BUFFER
#define GL_PIXEL_UNPACK_BUFFER 0x88EC
#endif
#define GLFUNCS \
    F(void, glGenTextures, (GLsizei, GLuint *)) F(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    F(void, glBindTexture, (GLenum, GLuint)) F(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    F(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    F(void, glTexParameteri, (GLenum, GLenum, GLint)) F(void, glTexParameterf, (GLenum, GLenum, GLfloat)) \
    F(void, glTexParameterfv, (GLenum, GLenum, const GLfloat *)) F(void, glGenerateMipmap, (GLenum)) \
    F(void, glActiveTexture, (GLenum)) F(void, glPixelStorei, (GLenum, GLint)) \
    F(void, glGenFramebuffers, (GLsizei, GLuint *)) F(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    F(void, glBindFramebuffer, (GLenum, GLuint)) F(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    F(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) F(GLenum, glCheckFramebufferStatus, (GLenum)) \
    F(void, glGenRenderbuffers, (GLsizei, GLuint *)) F(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    F(void, glBindRenderbuffer, (GLenum, GLuint)) F(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
    F(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    F(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) F(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
    F(void, glEnable, (GLenum)) F(void, glDisable, (GLenum)) F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void, glClearDepth, (double)) F(void, glClear, (GLbitfield)) F(void, glDepthFunc, (GLenum)) F(void, glDepthMask, (GLboolean)) \
    F(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) F(void, glBlendFunc, (GLenum, GLenum)) \
    F(void, glCullFace, (GLenum)) F(void, glFrontFace, (GLenum)) F(void, glPolygonOffset, (GLfloat, GLfloat)) \
    F(GLuint, glCreateShader, (GLenum)) F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    F(void, glCompileShader, (GLuint)) F(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) F(GLuint, glCreateProgram, (void)) \
    F(void, glAttachShader, (GLuint, GLuint)) F(void, glLinkProgram, (GLuint)) F(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    F(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) F(void, glUseProgram, (GLuint)) \
    F(GLint, glGetUniformLocation, (GLuint, const GLchar *)) F(void, glUniform1i, (GLint, GLint)) \
    F(void, glUniform1f, (GLint, GLfloat)) F(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    F(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat)) F(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    F(void, glDeleteShader, (GLuint)) \
    F(void, glGenVertexArrays, (GLsizei, GLuint *)) F(void, glBindVertexArray, (GLuint)) F(void, glGenBuffers, (GLsizei, GLuint *)) \
    F(void, glBindBuffer, (GLenum, GLuint)) F(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    F(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    F(void, glEnableVertexAttribArray, (GLuint)) F(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
    F(void, glDrawArrays, (GLenum, GLint, GLsizei)) F(GLenum, glGetError, (void)) F(void, glFinish, (void)) F(void, glFlush, (void)) \
    F(void, glGetIntegerv, (GLenum, GLint *)) F(const GLubyte *, glGetString, (GLenum)) F(void, glProvokingVertex, (GLenum))

#define F(ret, name, args) static ret (GLAPI *name) args;
GLFUNCS
#undef F

static rdr_gl_host_t g_host;
static int g_scale = 1;
static int g_aniso = 0, g_force_filter = 0, g_gen_mips = 1;
static float g_max_aniso = 1.f;

static void glog(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (g_host.log) g_host.log(b); else fprintf(stderr, "%s\n", b);
}

/* ------------------------------------------------------------------ */
struct rtex { GLuint id; int w, h, levels, have_mips; GLint p_mag, p_min, p_ws, p_wt; float p_aniso, p_lod; uint32_t p_border; int p_set; };
struct rtarget { int w, h, sw, sh; GLuint fbo, color, depth; GLuint fbo_small, color_small; unsigned gen, small_gen; };

static void batch_flush(void);
static void gl_state_stale(void);
static GLuint g_vao, g_qvao, g_vbo, g_ibo, g_quad_prog;
/* Staging textures for CPU pixels (GP3's overlays, shadows, blits) used round-robin:
 * re-filling a single texture that the previous quad still reads made the driver
 * wait for the GPU on every overlay run (hundreds per frame on detailed tracks). */
#define UP_RING 16
static GLuint g_up_tex[UP_RING]; static int g_up_w[UP_RING], g_up_h[UP_RING], g_up_cur;
#define g_upload_tex g_up_tex[g_up_cur]
#define g_upload_w g_up_w[g_up_cur]
#define g_upload_h g_up_h[g_up_cur]
static GLint g_quad_u_tex, g_quad_u_rect, g_quad_u_flipv, g_quad_u_keyed;

/* ------------------------------------------------------------------ */
static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); glog("GL shader error: %s\n%s", log, src); }
    return s;
}
static GLuint link_prog(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v); glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_diff");
    glBindAttribLocation(p, 2, "a_spec"); glBindAttribLocation(p, 3, "a_uv");
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); glog("GL link error: %s", log); }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

/* fixed-function pipeline emulation: one generated program per state combination */
typedef struct {
    uint8_t nst, flat, spec, ckey, atest, afunc, fog, fogtable;
    struct { uint8_t cop, ca1, ca2, aop, aa1, aa2, tci, hastex, hasalpha, hasck; } st[2];
} pkey_t;
typedef struct { pkey_t key; GLuint prog; GLint u_tex[2], u_tf, u_aref, u_fogc, u_fogp, u_yflip;
                 int u_set; uint32_t c_tf, c_aref, c_fogc; float c_fs, c_fe, c_fd; } prog_t;
#define MAXPROG 256
static prog_t g_progs[MAXPROG];
static int g_nprogs;

static const char *VS =
    "#version 330 core\n"
    "in vec4 a_pos; in vec4 a_diff; in vec4 a_spec; in vec4 a_uv;\n"
    "uniform float u_yflip;\n"
    "out vec4 v_diff; out vec4 v_spec; out vec4 v_uv; out float v_w;\n"
    "flat out vec4 f_diff; flat out vec4 f_spec;\n"
    "void main(){\n"
    "  gl_Position = vec4(a_pos.x, a_pos.y * u_yflip, a_pos.z * 2.0 - a_pos.w, a_pos.w);\n"
    "  v_diff = a_diff.bgra; v_spec = a_spec.bgra; f_diff = v_diff; f_spec = v_spec; v_uv = a_uv; v_w = a_pos.w;\n"
    "}\n";

static void arg_expr(char *o, size_t n, int a, const char *chan) {
    const char *b;
    switch (a & 0xf) {
    case TA_DIFFUSE: b = "diff"; break; case TA_CURRENT: b = "cur"; break; case TA_TEXTURE: b = "tex"; break;
    case TA_TFACTOR: b = "u_tf"; break; case TA_SPECULAR: b = "spec"; break; default: b = "cur";
    }
    char base[64];
    if (a & TA_ALPHAREPLICATE) snprintf(base, sizeof base, "vec4(%s.a)", b); else snprintf(base, sizeof base, "%s", b);
    if (a & TA_COMPLEMENT) snprintf(o, n, "(1.0 - %s.%s)", base, chan); else snprintf(o, n, "%s.%s", base, chan);
}
static void op_expr(char *o, size_t n, int op, const char *a1, const char *a2, const char *a1a) {
    switch (op) {
    case TOP_SELECTARG1: snprintf(o, n, "%s", a1); break;
    case TOP_SELECTARG2: snprintf(o, n, "%s", a2); break;
    case TOP_MODULATE: snprintf(o, n, "(%s * %s)", a1, a2); break;
    case TOP_MODULATE2X: snprintf(o, n, "(%s * %s * 2.0)", a1, a2); break;
    case TOP_MODULATE4X: snprintf(o, n, "(%s * %s * 4.0)", a1, a2); break;
    case TOP_ADD: snprintf(o, n, "(%s + %s)", a1, a2); break;
    case TOP_ADDSIGNED: snprintf(o, n, "(%s + %s - 0.5)", a1, a2); break;
    case TOP_ADDSIGNED2X: snprintf(o, n, "((%s + %s - 0.5) * 2.0)", a1, a2); break;
    case TOP_SUBTRACT: snprintf(o, n, "(%s - %s)", a1, a2); break;
    case TOP_ADDSMOOTH: snprintf(o, n, "(%s + %s - %s * %s)", a1, a2, a1, a2); break;
    case TOP_BLENDDIFFUSEALPHA: snprintf(o, n, "mix(%s, %s, diff.a)", a2, a1); break;
    case TOP_BLENDTEXTUREALPHA: snprintf(o, n, "mix(%s, %s, tex.a)", a2, a1); break;
    case TOP_BLENDFACTORALPHA: snprintf(o, n, "mix(%s, %s, u_tf.a)", a2, a1); break;
    case TOP_BLENDTEXTUREALPHAPM: snprintf(o, n, "(%s + %s * (1.0 - tex.a))", a1, a2); break;
    case TOP_BLENDCURRENTALPHA: snprintf(o, n, "mix(%s, %s, cur.a)", a2, a1); break;
    case TOP_MODULATEALPHA_ADDCOLOR: snprintf(o, n, "(%s + %s * %s)", a1, a1a, a2); break;
    case TOP_MODULATECOLOR_ADDALPHA: snprintf(o, n, "(%s * %s + %s)", a1, a2, a1a); break;
    case TOP_MODULATEINVALPHA_ADDCOLOR: snprintf(o, n, "((1.0 - %s) * %s + %s)", a1a, a2, a1); break;
    case TOP_MODULATEINVCOLOR_ADDALPHA: snprintf(o, n, "((1.0 - %s) * %s + %s)", a1, a2, a1a); break;
    default: snprintf(o, n, "%s", a1); break;
    }
}
static const char *cmp_op(int f) {
    switch (f) { case 1: return "false"; case 2: return "<"; case 3: return "=="; case 4: return "<="; case 5: return ">"; case 6: return "!="; case 7: return ">="; }
    return "true";
}

static prog_t *get_prog(const pkey_t *k) {
    for (int i = 0; i < g_nprogs; i++) if (!memcmp(&g_progs[i].key, k, sizeof *k)) return &g_progs[i];
    char fs[8192]; size_t o = 0;
    o += snprintf(fs + o, sizeof fs - o,
        "#version 330 core\n"
        "in vec4 v_diff; in vec4 v_spec; in vec4 v_uv; in float v_w;\n"
        "flat in vec4 f_diff; flat in vec4 f_spec;\n"
        "uniform sampler2D u_tex0; uniform sampler2D u_tex1; uniform vec4 u_tf; uniform float u_aref;\n"
        "uniform vec4 u_fogc; uniform vec3 u_fogp;\n"
        "out vec4 o_col;\n"
        "void main(){\n"
        "  vec4 diff = %s; vec4 spec = vec4((%s).rgb, v_spec.a);\n"
        "  vec4 cur = diff; vec4 tex;\n", k->flat ? "f_diff" : "v_diff", k->flat ? "f_spec" : "v_spec");
    for (int i = 0; i < k->nst; i++) {
        const char *uv = k->st[i].tci ? "v_uv.zw" : "v_uv.xy";
        if (k->st[i].hastex) {
            o += snprintf(fs + o, sizeof fs - o, "  tex = texture(u_tex%d, %s);\n", i, uv);
            if (k->st[i].hasck && k->ckey) o += snprintf(fs + o, sizeof fs - o, "  if (tex.a < 0.5) discard;\n");
            if (!k->st[i].hasalpha || (k->st[i].hasck && !k->ckey)) o += snprintf(fs + o, sizeof fs - o, "  tex.a = 1.0;\n");
            else if (k->st[i].hasck && k->ckey && !k->st[i].hasalpha) o += snprintf(fs + o, sizeof fs - o, "  tex.a = 1.0;\n");
        } else o += snprintf(fs + o, sizeof fs - o, "  tex = vec4(1.0);\n");
        char a1[64], a2[64], a1a[64], b1[64], b2[64], e[256], ea[256];
        o += snprintf(fs + o, sizeof fs - o, "  { vec4 n = cur;\n");
        if (k->st[i].cop == TOP_DOTPRODUCT3) {
            arg_expr(a1, sizeof a1, k->st[i].ca1, "rgb"); arg_expr(a2, sizeof a2, k->st[i].ca2, "rgb");
            o += snprintf(fs + o, sizeof fs - o, "    n = vec4(clamp(4.0 * dot(%s - 0.5, %s - 0.5), 0.0, 1.0));\n", a1, a2);
        } else if (k->st[i].cop != TOP_DISABLE) {
            arg_expr(a1, sizeof a1, k->st[i].ca1, "rgb"); arg_expr(a2, sizeof a2, k->st[i].ca2, "rgb"); arg_expr(a1a, sizeof a1a, k->st[i].ca1, "a");
            op_expr(e, sizeof e, k->st[i].cop, a1, a2, a1a);
            o += snprintf(fs + o, sizeof fs - o, "    n.rgb = clamp(%s, 0.0, 1.0);\n", e);
        }
        if (k->st[i].aop != TOP_DISABLE && k->st[i].cop != TOP_DOTPRODUCT3) {
            arg_expr(b1, sizeof b1, k->st[i].aa1, "a"); arg_expr(b2, sizeof b2, k->st[i].aa2, "a");
            op_expr(ea, sizeof ea, k->st[i].aop, b1, b2, b1);
            o += snprintf(fs + o, sizeof fs - o, "    n.a = clamp(%s, 0.0, 1.0);\n", ea);
        }
        o += snprintf(fs + o, sizeof fs - o, "    cur = n; }\n");
    }
    if (k->spec) o += snprintf(fs + o, sizeof fs - o, "  cur.rgb = clamp(cur.rgb + spec.rgb, 0.0, 1.0);\n");
    if (k->atest && k->afunc == 1) o += snprintf(fs + o, sizeof fs - o, "  discard;\n");
    else if (k->atest && k->afunc >= 2 && k->afunc <= 7)
        o += snprintf(fs + o, sizeof fs - o, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref)) discard;\n", cmp_op(k->afunc));
    if (k->fog) {
        if (k->fogtable == 0) o += snprintf(fs + o, sizeof fs - o, "  float f = clamp(v_spec.a, 0.0, 1.0);\n");
        else {
            o += snprintf(fs + o, sizeof fs - o, "  float d = u_fogp.y > 1.5 ? v_w : gl_FragCoord.z;\n");
            if (k->fogtable == 3) o += snprintf(fs + o, sizeof fs - o, "  float f = clamp((u_fogp.y - d) / max(u_fogp.y - u_fogp.x, 1e-6), 0.0, 1.0);\n");
            else if (k->fogtable == 1) o += snprintf(fs + o, sizeof fs - o, "  float f = clamp(exp(-d * u_fogp.z), 0.0, 1.0);\n");
            else o += snprintf(fs + o, sizeof fs - o, "  float f = clamp(exp(-(d * u_fogp.z) * (d * u_fogp.z)), 0.0, 1.0);\n");
        }
        o += snprintf(fs + o, sizeof fs - o, "  cur.rgb = mix(u_fogc.rgb, cur.rgb, f);\n");
    }
    o += snprintf(fs + o, sizeof fs - o, "  o_col = cur;\n}\n");
    if (g_nprogs == MAXPROG) g_nprogs = 0;   /* crude eviction; never expected in practice */
    prog_t *p = &g_progs[g_nprogs++];
    p->key = *k;
    p->prog = link_prog(VS, fs);
    p->u_tex[0] = glGetUniformLocation(p->prog, "u_tex0");
    p->u_tex[1] = glGetUniformLocation(p->prog, "u_tex1");
    p->u_tf = glGetUniformLocation(p->prog, "u_tf");
    p->u_aref = glGetUniformLocation(p->prog, "u_aref");
    p->u_fogc = glGetUniformLocation(p->prog, "u_fogc");
    p->u_fogp = glGetUniformLocation(p->prog, "u_fogp");
    p->u_yflip = glGetUniformLocation(p->prog, "u_yflip");
    glUseProgram(p->prog);
    glUniform1i(p->u_tex[0], 0); glUniform1i(p->u_tex[1], 1);
    return p;
}

/* ------------------------------------------------------------------ */
static rtex_t *g_tex_create(int w, int h, int levels) {
    batch_flush(); gl_state_stale();
    rtex_t *t = calloc(1, sizeof *t);
    t->w = w; t->h = h; t->levels = levels;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    int lw = w, lh = h;
    for (int i = 0; i < levels; i++) {
        glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, lw, lh, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
    return t;
}
static void g_tex_upload(rtex_t *t, int level, int w, int h, const uint32_t *argb) {
    batch_flush(); gl_state_stale();
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    if (level == 0 && t->levels == 1 && g_gen_mips && w > 1 && h > 1 && (w & (w - 1)) == 0 && (h & (h - 1)) == 0) {
        /* generated mip chain for textures the game supplied without one (only used when the game asks for mip filtering
         * or when filtering is forced by the user) */
        int n = 1; for (int a = w, b = h; a > 1 || b > 1; a = a > 1 ? a / 2 : 1, b = b > 1 ? b / 2 : 1) n++;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n - 1);
        glGenerateMipmap(GL_TEXTURE_2D);
        t->have_mips = 1;
    } else if (t->levels > 1) t->have_mips = 1;
}
static void g_tex_destroy(rtex_t *t) {
    batch_flush(); glDeleteTextures(1, &t->id); free(t); }

static rtarget_t *g_target_create(int w, int h, int scale) {
    batch_flush(); gl_state_stale();
    (void)scale;
    rtarget_t *t = calloc(1, sizeof *t);
    t->w = w; t->h = h; t->sw = w * g_scale; t->sh = h * g_scale;
    glGenTextures(1, &t->color);
    glBindTexture(GL_TEXTURE_2D, t->color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->sw, t->sh, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenRenderbuffers(1, &t->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, t->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, t->sw, t->sh);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) glog("GL: incomplete framebuffer %dx%d", t->sw, t->sh);
    glViewport(0, 0, t->sw, t->sh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1); glClearDepth(1.0); glDepthMask(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (g_scale > 1) {
        glGenTextures(1, &t->color_small);
        glBindTexture(GL_TEXTURE_2D, t->color_small);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glGenFramebuffers(1, &t->fbo_small);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo_small);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color_small, 0);
    }
    return t;
}
static void g_target_destroy(rtarget_t *t) {
    batch_flush(); gl_state_stale();
    glDeleteFramebuffers(1, &t->fbo); glDeleteTextures(1, &t->color); glDeleteRenderbuffers(1, &t->depth);
    if (t->fbo_small) { glDeleteFramebuffers(1, &t->fbo_small); glDeleteTextures(1, &t->color_small); }
    free(t);
}

/* draw a textured quad (texture rows top-first) into the currently bound framebuffer */
static void quad(GLuint tex, float x0, float y0, float x1, float y1, int fbw, int fbh, int flipv, int keyed, int linear) {
    glUseProgram(g_quad_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* rect in NDC */
    glUniform4f(g_quad_u_rect, x0 / fbw * 2 - 1, y0 / fbh * 2 - 1, x1 / fbw * 2 - 1, y1 / fbh * 2 - 1);
    glUniform1f(g_quad_u_flipv, flipv ? 1.f : 0.f);
    glUniform1f(g_quad_u_keyed, keyed ? 1.f : 0.f);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glDepthMask(0);
    glBindVertexArray(g_qvao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDepthMask(1);
}

static void upload_staging(const uint32_t *argb, int pitch_px, int w, int h) {
    g_up_cur = (g_up_cur + 1) % UP_RING;
    if (!g_upload_tex || g_upload_w < w || g_upload_h < h) {
        if (!g_upload_tex) glGenTextures(1, &g_upload_tex);
        glBindTexture(GL_TEXTURE_2D, g_upload_tex);
        g_upload_w = w > g_upload_w ? w : g_upload_w; g_upload_h = h > g_upload_h ? h : g_upload_h;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_upload_w, g_upload_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glBindTexture(GL_TEXTURE_2D, g_upload_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    /* through a pixel buffer that is orphaned every time: the driver takes a copy of the
     * pixels and queues the texture update, instead of making us wait until the GPU is
     * done with the texture (that wait was most of the "upload" time on NVIDIA) */
    static GLuint pbo; if (!pbo) glGenBuffers(1, &pbo);
    size_t bytes = (size_t)w * h * 4;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)bytes, NULL, GL_STREAM_DRAW);
    if (pitch_px == w) glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)bytes, argb);
    else for (int y = 0; y < h; y++) glBufferSubData(GL_PIXEL_UNPACK_BUFFER, (GLintptr)((size_t)y * w * 4), (GLsizeiptr)((size_t)w * 4), argb + (size_t)y * pitch_px);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, (const void *)0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

/* Render targets are stored top row first: rendering uses a y-flipped projection. */
static void g_target_upload(rtarget_t *t, const uint32_t *argb, int pitch_px, int x, int y, int w, int h) {
    batch_flush(); gl_state_stale();
    t->gen++;
    if (g_scale == 1) {
        glBindTexture(GL_TEXTURE_2D, t->color);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch_px);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb + (size_t)y * pitch_px + x);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return;
    }
    upload_staging(argb + (size_t)y * pitch_px + x, pitch_px, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->sw, t->sh);
    glDisable(GL_SCISSOR_TEST);
    /* staging texture holds the rect at (0,0); map texcoords through the quad shader's rect uniform */
    float u1 = (float)w / g_upload_w, v1 = (float)h / g_upload_h;
    (void)u1; (void)v1;
    glUseProgram(g_quad_prog);
    glUniform4f(glGetUniformLocation(g_quad_prog, "u_uvrect"), 0, 0, (float)w / g_upload_w, (float)h / g_upload_h);
    quad(g_upload_tex, (float)x * g_scale, (float)y * g_scale, (float)(x + w) * g_scale, (float)(y + h) * g_scale, t->sw, t->sh, 0, 0, 0);
    glUniform4f(glGetUniformLocation(g_quad_prog, "u_uvrect"), 0, 0, 1, 1);
}
static void g_target_download(rtarget_t *t, uint32_t *argb, int pitch_px, int y0, int n) {
    batch_flush(); gl_state_stale();
    GLuint src = t->fbo;
    if (y0 < 0) y0 = 0;
    if (y0 + n > t->h) n = t->h - y0;
    if (n <= 0) return;
    if (g_scale > 1) {
        if (t->small_gen != t->gen + 1) {
            /* one downscale of the whole target serves every read until something is drawn again */
            glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo_small);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(0, 0, t->sw, t->sh, 0, 0, t->w, t->h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            t->small_gen = t->gen + 1;
        }
        src = t->fbo_small;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, pitch_px);
    glReadPixels(0, y0, t->w, n, GL_BGRA, GL_UNSIGNED_BYTE, argb + (size_t)y0 * pitch_px);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
}
static void g_clear(rtarget_t *t, const int *rects, int n, int color, uint32_t argb, int depth, float z, int vx, int vy, int vw, int vh) {
    batch_flush(); gl_state_stale();
    t->gen++;
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->sw, t->sh);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1); glDepthMask(1);
    glClearColor(((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f, (argb >> 24) / 255.f);
    glClearDepth(z);
    GLbitfield m = (color ? GL_COLOR_BUFFER_BIT : 0) | (depth ? GL_DEPTH_BUFFER_BIT : 0);
    int one[4] = { vx, vy, vx + vw, vy + vh };
    if (!n) { rects = one; n = 1; }
    for (int i = 0; i < n; i++) {
        int x0 = rects[i * 4], y0 = rects[i * 4 + 1], x1 = rects[i * 4 + 2], y1 = rects[i * 4 + 3];
        /* Direct3D clips clear rectangles to the viewport */
        if (vw > 0 && vh > 0) {
            if (x0 < vx) x0 = vx; if (y0 < vy) y0 = vy;
            if (x1 > vx + vw) x1 = vx + vw; if (y1 > vy + vh) y1 = vy + vh;
        }
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > t->w) x1 = t->w; if (y1 > t->h) y1 = t->h;
        if (x1 <= x0 || y1 <= y0) continue;
        glScissor(x0 * g_scale, y0 * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
        glClear(m);
    }
    glDisable(GL_SCISSOR_TEST);
}

static GLenum blend_factor(int b) {
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA; case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR; case 11: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}
static GLint wrap_mode(int a) { return a == 2 ? GL_MIRRORED_REPEAT : a == 3 ? GL_CLAMP_TO_EDGE : a == 4 ? GL_CLAMP_TO_BORDER : GL_REPEAT; }

/* ---- GL state cache for the draw path (other operations mark it stale) ---- */
#define VBO_BYTES (8 << 20)
#define IBO_BYTES (4 << 20)
static struct {
    int valid;
    GLuint fbo, prog, tex[2]; int vp[4]; int sc_on, sc[4];
    int zt, zf, zm, po; float pov; int bl, bs, bd; int cu, cf;
} gs;
static size_t g_vbo_off, g_ibo_off;
static void gl_state_stale(void) { gs.valid = 0; }
static void draw_now(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, uint32_t *idx, int ni) {
    if (!gs.valid) {
        memset(&gs, 0, sizeof gs); gs.zt = gs.zm = gs.po = gs.bl = gs.cu = gs.sc_on = -1; gs.fbo = gs.prog = (GLuint)-1;
        gs.tex[0] = gs.tex[1] = (GLuint)-1; gs.vp[2] = -1; gs.valid = 1;
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    }
    if (gs.fbo != t->fbo) { glBindFramebuffer(GL_FRAMEBUFFER, t->fbo); gs.fbo = t->fbo; }
    if (gs.vp[2] != t->sw || gs.vp[3] != t->sh) { glViewport(0, 0, t->sw, t->sh); gs.vp[2] = t->sw; gs.vp[3] = t->sh; }
    int scon = s->vw && s->vh;
    if (scon != gs.sc_on) { if (scon) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST); gs.sc_on = scon; }
    if (scon && (gs.sc[0] != s->vx || gs.sc[1] != s->vy || gs.sc[2] != s->vw || gs.sc[3] != s->vh)) {
        glScissor(s->vx * g_scale, s->vy * g_scale, s->vw * g_scale, s->vh * g_scale);
        gs.sc[0] = s->vx; gs.sc[1] = s->vy; gs.sc[2] = s->vw; gs.sc[3] = s->vh;
    }
    pkey_t k; memset(&k, 0, sizeof k);
    k.nst = (uint8_t)s->nstages; k.flat = (uint8_t)s->flat; k.spec = (uint8_t)s->specular; k.ckey = (uint8_t)s->ckey;
    k.atest = (uint8_t)s->atest; k.afunc = (uint8_t)(s->atest ? s->afunc : 0); k.fog = (uint8_t)s->fog; k.fogtable = (uint8_t)(s->fog ? s->fogtable : 0);
    for (int i = 0; i < s->nstages; i++) {
        const rstage_t *st = &s->st[i];
        k.st[i].cop = st->colorop; k.st[i].ca1 = st->carg1; k.st[i].ca2 = st->carg2;
        k.st[i].aop = st->alphaop; k.st[i].aa1 = st->aarg1; k.st[i].aa2 = st->aarg2;
        k.st[i].tci = (uint8_t)(st->texcoord != 0); k.st[i].hastex = st->tex != NULL;
        k.st[i].hasalpha = (uint8_t)st->has_alpha; k.st[i].hasck = (uint8_t)st->has_ckey;
        GLuint id = st->tex ? st->tex->id : 0;
        if (gs.tex[i] != id || st->tex) {
            glActiveTexture(GL_TEXTURE0 + i);
            if (gs.tex[i] != id) { glBindTexture(GL_TEXTURE_2D, id); gs.tex[i] = id; }
        }
        if (st->tex) {
            rtex_t *tx = st->tex;
            int lin_mag = st->magf == 2 || g_force_filter, lin_min = st->minf == 2 || g_force_filter;
            int mips = tx->have_mips && (st->mipf > 1 || g_force_filter);
            GLint minf = lin_min ? (mips ? (st->mipf == 2 && !g_force_filter ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR) : GL_LINEAR)
                                 : (mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
            GLint magf = lin_mag ? GL_LINEAR : GL_NEAREST, ws = wrap_mode(st->addru), wt = wrap_mode(st->addrv);
            float an = g_aniso > 1 ? ((float)g_aniso < g_max_aniso ? (float)g_aniso : g_max_aniso) : 0.f;
            if (!tx->p_set || tx->p_mag != magf) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
            if (!tx->p_set || tx->p_min != minf) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
            if (!tx->p_set || tx->p_ws != ws) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
            if (!tx->p_set || tx->p_wt != wt) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
            if (an > 0 && (!tx->p_set || tx->p_aniso != an)) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, an);
            if ((st->addru == 4 || st->addrv == 4) && (!tx->p_set || tx->p_border != st->border)) {
                uint32_t c = st->border;
                GLfloat bc[4] = { ((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f };
                glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc);
            }
            if (!tx->p_set || tx->p_lod != st->lodbias) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, st->lodbias);
            tx->p_mag = magf; tx->p_min = minf; tx->p_ws = ws; tx->p_wt = wt; tx->p_aniso = an; tx->p_lod = st->lodbias;
            tx->p_border = st->border; tx->p_set = 1;
        }
    }
    static pkey_t lastk; static prog_t *lastp;
    prog_t *p = (lastp && !memcmp(&lastk, &k, sizeof k)) ? lastp : get_prog(&k);
    lastk = k; lastp = p;
    if (gs.prog != p->prog) { glUseProgram(p->prog); gs.prog = p->prog; }
    uint32_t tf = s->tfactor, fc = s->fogcolor;
    if (!p->u_set || p->c_tf != tf) { glUniform4f(p->u_tf, ((tf >> 16) & 255) / 255.f, ((tf >> 8) & 255) / 255.f, (tf & 255) / 255.f, (tf >> 24) / 255.f); p->c_tf = tf; }
    if (!p->u_set || p->c_aref != s->aref) { glUniform1f(p->u_aref, (float)s->aref); p->c_aref = s->aref; }
    if (!p->u_set || p->c_fogc != fc) { glUniform4f(p->u_fogc, ((fc >> 16) & 255) / 255.f, ((fc >> 8) & 255) / 255.f, (fc & 255) / 255.f, 1.f); p->c_fogc = fc; }
    if (!p->u_set || p->c_fs != s->fogstart || p->c_fe != s->fogend || p->c_fd != s->fogdensity) {
        glUniform3f(p->u_fogp, s->fogstart, s->fogend, s->fogdensity); p->c_fs = s->fogstart; p->c_fe = s->fogend; p->c_fd = s->fogdensity;
    }
    if (!p->u_set) { glUniform1f(p->u_yflip, -1.f); p->u_set = 1; }   /* top row first in the target texture */

    if (s->zenable) {
        static const GLenum zf[9] = { 0x0207, 0x0200, 0x0201, 0x0202, 0x0203, 0x0204, 0x0205, 0x0206, 0x0207 };
        GLenum f = zf[s->zfunc & 15 ? (s->zfunc <= 8 ? s->zfunc : 8) : 4];
        if (gs.zt != 1) { glEnable(GL_DEPTH_TEST); gs.zt = 1; }
        if (gs.zf != (int)f) { glDepthFunc(f); gs.zf = (int)f; }
        int m = s->zwrite ? 1 : 0; if (gs.zm != m) { glDepthMask(m); gs.zm = m; }
    } else {
        if (gs.zt != 0) { glDisable(GL_DEPTH_TEST); gs.zt = 0; }
        if (gs.zm != 0) { glDepthMask(0); gs.zm = 0; }
    }
    if (s->zbias != 0) {
        if (gs.po != 1) { glEnable(GL_POLYGON_OFFSET_FILL); gs.po = 1; }
        if (gs.pov != s->zbias) { glPolygonOffset(-s->zbias, -s->zbias); gs.pov = s->zbias; }
    } else if (gs.po != 0) { glDisable(GL_POLYGON_OFFSET_FILL); gs.po = 0; }
    if (s->blend) {
        GLenum bs, bd;
        if (s->srcblend == 12) { bs = GL_SRC_ALPHA; bd = GL_ONE_MINUS_SRC_ALPHA; }
        else if (s->srcblend == 13) { bs = GL_ONE_MINUS_SRC_ALPHA; bd = GL_SRC_ALPHA; }
        else { bs = blend_factor(s->srcblend); bd = blend_factor(s->dstblend); }
        if (gs.bl != 1) { glEnable(GL_BLEND); gs.bl = 1; }
        if (gs.bs != (int)bs || gs.bd != (int)bd) { glBlendFunc(bs, bd); gs.bs = (int)bs; gs.bd = (int)bd; }
    } else if (gs.bl != 0) { glDisable(GL_BLEND); gs.bl = 0; }
    /* culling: y is flipped when rendering into the target, which mirrors the winding */
    if (s->cull == 2 || s->cull == 3) {
        if (gs.cu != 1) { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); gs.cu = 1; }
        int ff = s->cull == 3 ? GL_CCW : GL_CW;
        if (gs.cf != ff) { glFrontFace(ff); gs.cf = ff; }
    } else if (gs.cu != 0) { glDisable(GL_CULL_FACE); gs.cu = 0; }

    /* append to the streaming buffers (orphaned when full) */
    size_t vb = sizeof(rvert_t) * (size_t)nv, ib = 4 * (size_t)ni;
    if (g_vbo_off + vb > VBO_BYTES || g_ibo_off + ib > IBO_BYTES) {
        glBufferData(GL_ARRAY_BUFFER, VBO_BYTES, NULL, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, IBO_BYTES, NULL, GL_STREAM_DRAW);
        g_vbo_off = g_ibo_off = 0;
    }
    uint32_t base = (uint32_t)(g_vbo_off / sizeof(rvert_t));
    if (base) for (int i = 0; i < ni; i++) idx[i] += base;
    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)g_vbo_off, (GLsizeiptr)vb, v);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)g_ibo_off, (GLsizeiptr)ib, idx);
    GLenum mode = prim == RPRIM_TRIS ? GL_TRIANGLES : prim == RPRIM_LINES ? GL_LINES : GL_POINTS;
    glDrawElements(mode, ni, GL_UNSIGNED_INT, (void *)(uintptr_t)g_ibo_off);
    g_vbo_off += (vb + sizeof(rvert_t) - 1) / sizeof(rvert_t) * sizeof(rvert_t);
    g_ibo_off += ib;
}
/* ---- batching: consecutive draws with identical state become one draw call ---- */
uint32_t g_rdr_batches, g_rdr_draws;   /* statistics */
static struct { rtarget_t *t; rstate_t s; int prim; rvert_t *v; int nv, cap_v; uint32_t *idx; int ni, cap_i; } g_b;
static void batch_flush(void) {
    if (!g_b.nv) return;
    draw_now(g_b.t, &g_b.s, g_b.prim, g_b.v, g_b.nv, g_b.idx, g_b.ni);
    g_rdr_batches++;
    g_b.nv = g_b.ni = 0;
}
static void g_draw(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni) {
    t->gen++;
    g_rdr_draws++;
#ifdef BATCH_DEBUG
    if (g_b.nv && memcmp(&g_b.s, s, sizeof *s)) {
        extern uint32_t g_bd[16];
        const rstate_t *a = &g_b.s;
        if (a->st[0].tex != s->st[0].tex) g_bd[0]++;
        if (a->st[1].tex != s->st[1].tex) g_bd[1]++;
        if (a->blend != s->blend || a->srcblend != s->srcblend || a->dstblend != s->dstblend) g_bd[2]++;
        if (a->atest != s->atest || a->aref != s->aref) g_bd[3]++;
        if (a->nstages != s->nstages || memcmp(&a->st[0].colorop, &s->st[0].colorop, sizeof(int) * 6) || memcmp(&a->st[1].colorop, &s->st[1].colorop, sizeof(int) * 6)) g_bd[4]++;
        if (a->cull != s->cull) g_bd[5]++;
        if (a->vx != s->vx || a->vy != s->vy || a->vw != s->vw || a->vh != s->vh) g_bd[6]++;
        if (a->ckey != s->ckey || a->specular != s->specular || a->fog != s->fog) g_bd[7]++;
        if (a->tfactor != s->tfactor) g_bd[8]++;
        if (memcmp(&a->st[0].addru, &s->st[0].addru, sizeof(int) * 5)) g_bd[9]++;
    } else if (g_b.nv && g_b.prim != prim) { extern uint32_t g_bd[16]; g_bd[10]++; }
#endif
    if (g_b.nv && (g_b.t != t || g_b.prim != prim || g_b.nv + nv > (1 << 20) || memcmp(&g_b.s, s, sizeof *s))) batch_flush();
    if (!g_b.nv) { g_b.t = t; g_b.s = *s; g_b.prim = prim; }
    if (g_b.nv + nv > g_b.cap_v) { g_b.cap_v = (g_b.nv + nv) * 2 + 1024; g_b.v = realloc(g_b.v, sizeof(rvert_t) * g_b.cap_v); }
    if (g_b.ni + ni > g_b.cap_i) { g_b.cap_i = (g_b.ni + ni) * 2 + 4096; g_b.idx = realloc(g_b.idx, 4 * (size_t)g_b.cap_i); }
    memcpy(g_b.v + g_b.nv, v, sizeof(rvert_t) * nv);
    for (int i = 0; i < ni; i++) g_b.idx[g_b.ni + i] = (uint32_t)idx[i] + (uint32_t)g_b.nv;
    g_b.nv += nv; g_b.ni += ni;
}

static void g_blit(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *argb, int sw, int sh, int pitch_px, int keyed) {
    batch_flush(); gl_state_stale();
    t->gen++;
    upload_staging(argb, pitch_px, sw, sh);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->sw, t->sh);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_quad_prog);
    glUniform4f(glGetUniformLocation(g_quad_prog, "u_uvrect"), 0, 0, (float)sw / g_upload_w, (float)sh / g_upload_h);
    quad(g_upload_tex, (float)dx * g_scale, (float)dy * g_scale, (float)(dx + dw) * g_scale, (float)(dy + dh) * g_scale, t->sw, t->sh, 0, keyed, 0);
    glUniform4f(glGetUniformLocation(g_quad_prog, "u_uvrect"), 0, 0, 1, 1);
}

static GLuint g_copy_tex, g_copy_fbo; static int g_copy_w, g_copy_h;
static void g_copy(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    batch_flush(); gl_state_stale();
    t->gen++;
    int S = g_scale;
    /* go through a temporary texture: source and destination may overlap */
    if (!g_copy_tex || g_copy_w < sw * S || g_copy_h < sh * S) {
        if (!g_copy_tex) { glGenTextures(1, &g_copy_tex); glGenFramebuffers(1, &g_copy_fbo); }
        g_copy_w = sw * S > g_copy_w ? sw * S : g_copy_w; g_copy_h = sh * S > g_copy_h ? sh * S : g_copy_h;
        glBindTexture(GL_TEXTURE_2D, g_copy_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_copy_w, g_copy_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, g_copy_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_copy_tex, 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
    glBlitFramebuffer(sx * S, sy * S, (sx + sw) * S, (sy + sh) * S, 0, 0, sw * S, sh * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_copy_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glBlitFramebuffer(0, 0, sw * S, sh * S, dx * S, dy * S, (dx + dw) * S, (dy + dh) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
}

static void g_copy_from(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) {
    batch_flush(); gl_state_stale();
    t->gen++;
    int S = g_scale;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glBlitFramebuffer(sx * S, sy * S, (sx + sw) * S, (sy + sh) * S, dx * S, dy * S, (dx + dw) * S, (dy + dh) * S,
                      GL_COLOR_BUFFER_BIT, (sw == dw && sh == dh) ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
}

/* the window's back buffer, top row first (screenshots) */
int rdr_gl_read_window(uint32_t *argb, int w, int h) {
    batch_flush(); gl_state_stale();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_host.window_fbo ? g_host.window_fbo(g_host.ctx) : 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    for (int y = 0; y < h / 2; y++) for (int x = 0; x < w; x++) {
        uint32_t t = argb[(size_t)y * w + x]; argb[(size_t)y * w + x] = argb[(size_t)(h - 1 - y) * w + x]; argb[(size_t)(h - 1 - y) * w + x] = t;
    }
    return 0;
}

/* letterboxed presentation into the window (default framebuffer) */
static void present_tex(GLuint tex, int w, int h, float u1, float v1) {
    int ww = 0, wh = 0;
    g_host.window_size(g_host.ctx, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, g_host.window_fbo ? g_host.window_fbo(g_host.ctx) : 0);
    glViewport(0, 0, ww, wh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* keep the game's aspect ratio */
    double sa = (double)w / h, wa = (double)ww / wh;
    int vw = ww, vh = wh;
    if (wa > sa) vw = (int)(wh * sa + 0.5); else vh = (int)(ww / sa + 0.5);
    int vx = (ww - vw) / 2, vy = (wh - vh) / 2;
    glUseProgram(g_quad_prog);
    glUniform4f(glGetUniformLocation(g_quad_prog, "u_uvrect"), 0, 0, u1, v1);
    /* target textures are stored top-first: flip v when drawing to the window */
    quad(tex, (float)vx, (float)vy, (float)(vx + vw), (float)(vy + vh), ww, wh, 1, 0, 1);
    glUniform4f(glGetUniformLocation(g_quad_prog, "u_uvrect"), 0, 0, 1, 1);
    g_host.swap(g_host.ctx);
}
static void g_present(rtarget_t *t, int w, int h) { (void)w; (void)h; batch_flush(); gl_state_stale(); present_tex(t->color, t->w, t->h, 1, 1); }
static void g_present_argb(const uint32_t *argb, int w, int h, int pitch_px) {
    batch_flush(); gl_state_stale();
    upload_staging(argb, pitch_px, w, h);
    present_tex(g_upload_tex, w, h, (float)w / g_upload_w, (float)h / g_upload_h);
}

static rbackend_t g_gl = {
    "opengl", g_tex_create, g_tex_upload, g_tex_destroy, g_target_create, g_target_destroy, g_target_upload,
    g_target_download, g_clear, g_draw, g_blit, g_copy, g_present, g_present_argb, g_copy_from
};

rbackend_t *rdr_gl_init(const rdr_gl_host_t *host, const rdr_gl_options_t *opt) {
    g_host = *host;
#define F(ret, name, args) name = (ret (GLAPI *) args)host->getproc(#name); if (!name) { glog("GL: missing %s", #name); return NULL; }
    GLFUNCS
#undef F
    if (opt) { g_scale = opt->scale > 0 ? opt->scale : 1; g_aniso = opt->anisotropy; g_force_filter = opt->force_linear; g_gen_mips = opt->generate_mipmaps; }
    glog("GL: %s / %s / %s (internal scale %dx, aniso %d)", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), g_scale, g_aniso);
    GLint ma = 0; glGetIntegerv(0x84FF /*MAX_TEXTURE_MAX_ANISOTROPY*/, &ma); g_max_aniso = ma > 0 ? (float)ma : 1.f; glGetError();
    glGenVertexArrays(1, &g_qvao);
    glGenVertexArrays(1, &g_vao); glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo); glGenBuffers(1, &g_ibo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, VBO_BYTES, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, IBO_BYTES, NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(rvert_t), (void *)offsetof(rvert_t, x));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, sizeof(rvert_t), (void *)offsetof(rvert_t, diffuse));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(rvert_t), (void *)offsetof(rvert_t, specular));
    glVertexAttribPointer(3, 4, GL_FLOAT, 0, sizeof(rvert_t), (void *)offsetof(rvert_t, uv));
    for (int i = 0; i < 4; i++) glEnableVertexAttribArray(i);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnable(GL_DEPTH_CLAMP);           /* GP3 sends TL vertices with z outside [0,1] */
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);   /* D3D flat shading uses the first vertex */
    static const char *qvs =
        "#version 330 core\n"
        "uniform vec4 u_rect; uniform float u_flipv; uniform vec4 u_uvrect;\n"
        "out vec2 v_uv;\n"
        "void main(){ vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
        "  gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);\n"
        "  vec2 t = vec2(c.x, u_flipv > 0.5 ? 1.0 - c.y : c.y);\n"
        "  v_uv = mix(u_uvrect.xy, u_uvrect.zw, t); }\n";
    static const char *qfs =
        "#version 330 core\n"
        "in vec2 v_uv; uniform sampler2D u_tex; uniform float u_keyed; out vec4 o;\n"
        "void main(){ vec4 c = texture(u_tex, v_uv); if (u_keyed > 0.5 && c.a < 0.5) discard; o = c; }\n";
    g_quad_prog = link_prog(qvs, qfs);
    g_quad_u_tex = glGetUniformLocation(g_quad_prog, "u_tex");
    g_quad_u_rect = glGetUniformLocation(g_quad_prog, "u_rect");
    g_quad_u_flipv = glGetUniformLocation(g_quad_prog, "u_flipv");
    g_quad_u_keyed = glGetUniformLocation(g_quad_prog, "u_keyed");
    glUseProgram(g_quad_prog);
    glUniform1i(g_quad_u_tex, 0);
    glUniform4f(glGetUniformLocation(g_quad_prog, "u_uvrect"), 0, 0, 1, 1);
    GLenum e = glGetError();
    if (e) glog("GL: init error %x", e);
    return &g_gl;
}
