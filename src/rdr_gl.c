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
    F(void, glEnableVertexAttribArray, (GLuint)) F(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) F(void, glDrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void *, GLint)) \
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
struct rtex { GLuint id; int w, h, levels, have_mips, auto_mips; GLint p_mag, p_min, p_ws, p_wt; float p_aniso, p_lod; uint32_t p_border; int p_set; };
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
static GLint g_quad_u_tex, g_quad_u_rect, g_quad_u_flipv, g_quad_u_keyed, g_quad_u_uvrect;

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
static int mip_count(int w, int h) { int n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }
static rtex_t *g_tex_create(int w, int h, int levels) {
    batch_flush(); gl_state_stale();
    rtex_t *t = calloc(1, sizeof *t);
    t->w = w; t->h = h; t->levels = levels;
    /* textures the game gives without a mip chain get one made here: all its levels are
     * allocated now, so filling them later never makes the driver rebuild the texture */
    if (levels == 1 && g_gen_mips && (w > 1 || h > 1) && (w & (w - 1)) == 0 && (h & (h - 1)) == 0) t->auto_mips = mip_count(w, h);
    int n = t->auto_mips ? t->auto_mips : levels;
    glGenTextures(1, &t->id);
    glBindTexture(GL_TEXTURE_2D, t->id);
    int lw = w, lh = h;
    for (int i = 0; i < n; i++) {
        glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, lw, lh, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n - 1);
    return t;
}
/* next mip level: 2x2 box filter (what glGenerateMipmap does), on the CPU - on some
 * drivers (Intel) glGenerateMipmap per texture upload costs milliseconds */
static void mip_down(const uint32_t *s, int sw, int sh, uint32_t *d, int dw, int dh) {
    int fx = sw > dw ? 2 : 1, fy = sh > dh ? 2 : 1;
    for (int y = 0; y < dh; y++) {
        const uint32_t *r0 = s + (size_t)(y * fy) * sw, *r1 = r0 + (fy > 1 ? sw : 0);
        for (int x = 0; x < dw; x++) {
            uint32_t a = r0[x * fx], b = r0[x * fx + fx - 1], c = r1[x * fx], e = r1[x * fx + fx - 1];
            uint32_t lo = ((a & 0x00ff00ff) + (b & 0x00ff00ff) + (c & 0x00ff00ff) + (e & 0x00ff00ff) + 0x00020002) >> 2 & 0x00ff00ff;
            uint32_t hi = (((a >> 8) & 0x00ff00ff) + ((b >> 8) & 0x00ff00ff) + ((c >> 8) & 0x00ff00ff) + ((e >> 8) & 0x00ff00ff) + 0x00020002) >> 2 & 0x00ff00ff;
            d[(size_t)y * dw + x] = lo | hi << 8;
        }
    }
}
static uint32_t *g_mipbuf; static size_t g_mipcap;
/* Texture pixels go through pixel-unpack buffers (PBOs): the driver takes a plain copy and
 * moves it into the texture on the GPU later, instead of converting it into the texture's
 * memory layout on the processor while the game waits (slow on Intel graphics). */
static GLuint g_tpbo[4]; static int g_tpbo_i, g_tex_pbo = 0;   /* off: measured no gain on Intel UHD 630 (teste7); GP3_TEX_PBO=1 to try */
static void g_tex_upload(rtex_t *t, int level, int w, int h, const uint32_t *argb) {
    batch_flush(); gl_state_stale();
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    int automip = level == 0 && t->auto_mips > 1 && w == t->w && h == t->h;
    size_t n0 = (size_t)w * h, nm = 0;
    if (automip) {
        size_t need = n0 / 2 + 4;
        if (need > g_mipcap) { free(g_mipbuf); g_mipbuf = malloc(need * 4); g_mipcap = g_mipbuf ? need : 0; }
        if (!g_mipbuf) automip = 0;
    }
    /* the whole mip chain, packed after level 0 in g_mipbuf */
    int nlev = 1, lw[20], lh[20]; size_t off[20];
    lw[0] = w; lh[0] = h; off[0] = 0;
    if (automip) {
        const uint32_t *src = argb; int sw = w, sh = h; size_t o = 0;
        for (int i = 1; i < t->auto_mips && i < 20; i++) {
            int dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1;
            mip_down(src, sw, sh, g_mipbuf + o, dw, dh);
            lw[i] = dw; lh[i] = dh; off[i] = o; src = g_mipbuf + o; o += (size_t)dw * dh; sw = dw; sh = dh; nlev++;
        }
        nm = o;
    }
    if (g_tex_pbo && n0 >= 1024) {
        if (!g_tpbo[0]) glGenBuffers(4, g_tpbo);
        GLuint pb = g_tpbo[g_tpbo_i++ & 3];
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pb);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)((n0 + nm) * 4), NULL, GL_STREAM_DRAW);
        glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, (GLsizeiptr)(n0 * 4), argb);
        if (nm) glBufferSubData(GL_PIXEL_UNPACK_BUFFER, (GLintptr)(n0 * 4), (GLsizeiptr)(nm * 4), g_mipbuf);
        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, (const void *)0);
        for (int i = 1; i < nlev; i++)
            glTexSubImage2D(GL_TEXTURE_2D, i, 0, 0, lw[i], lh[i], GL_BGRA, GL_UNSIGNED_BYTE, (const void *)(uintptr_t)((n0 + off[i]) * 4));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, level, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb);
        for (int i = 1; i < nlev; i++) glTexSubImage2D(GL_TEXTURE_2D, i, 0, 0, lw[i], lh[i], GL_BGRA, GL_UNSIGNED_BYTE, g_mipbuf + off[i]);
    }
    if (automip) t->have_mips = 1;
    else if (level == 0 && t->auto_mips > 1) { glGenerateMipmap(GL_TEXTURE_2D); t->have_mips = 1; }
    else if (level == 0 && t->levels == 1 && g_gen_mips && w > 1 && h > 1 && (w & (w - 1)) == 0 && (h & (h - 1)) == 0) {
        int n = mip_count(w, h);
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
    glUniform4f(g_quad_u_uvrect, 0, 0, (float)w / g_upload_w, (float)h / g_upload_h);
    quad(g_upload_tex, (float)x * g_scale, (float)y * g_scale, (float)(x + w) * g_scale, (float)(y + h) * g_scale, t->sw, t->sh, 0, 0, 0);
    glUniform4f(g_quad_u_uvrect, 0, 0, 1, 1);
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
static void apply_state(rtarget_t *t, const rstate_t *s) {
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

}
/* ---- batching and deferred submission ----
 * Consecutive draws with identical state become one batch. Batches are not sent to GL one by one:
 * their vertices and indices go into one CPU arena and the batches into a queue. At the next point
 * where GL order matters (a texture upload, a blit, a copy, a read-back, a clear, present...) the
 * whole arena is uploaded in ONE glBufferSubData for vertices and one for indices, and the queue is
 * replayed with glDrawElementsBaseVertex. GP3 makes ~600 batches per frame but only a handful of
 * such points, so the driver sees a few large uploads instead of ~1,200 small ones per frame. */
uint32_t g_rdr_batches, g_rdr_draws, g_rdr_uploads, g_rdr_copyruns;   /* statistics */
enum { Q_DRAW, Q_COPY, Q_COPYFROM, Q_BLIT };
static void blits_upload(void);
static uint32_t *g_bpx; static size_t g_bpxn, g_bpxcap;     /* blit pixel arena */
static int g_bh;                                             /* rows used in the staging texture */
typedef struct { int kind; rtarget_t *t; rstate_t s; int prim; uint32_t v0, nv, i0, ni; int same; uint32_t r0, nr; rtarget_t *src; } qcmd_t;
typedef struct { int dx, dy, dw, dh, sx, sy, sw, sh; } crect_t;
static crect_t *g_cr; static uint32_t g_crn, g_crcap;     /* copy rectangles of the queued copies */
static void copy_run(rtarget_t *t, const crect_t *r, uint32_t n);
static void blit_now(const qcmd_t *c);
static void copy_from_now(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh);
static qcmd_t *g_q; static int g_qn, g_qcap;
static rvert_t *g_av; static uint32_t g_avn, g_avcap;      /* vertex arena */
static uint32_t *g_ai; static uint32_t g_ain, g_aicap;     /* index arena (relative to each batch) */
static struct { int open; rtarget_t *t; rstate_t s; int prim; uint32_t v0, i0; } g_b;
static size_t g_vbo_cap = VBO_BYTES, g_ibo_cap = IBO_BYTES;
static qcmd_t *q_push(void) {
    if (g_qn == g_qcap) { g_qcap = g_qcap ? g_qcap * 2 : 1024; g_q = realloc(g_q, sizeof *g_q * g_qcap); }
    return &g_q[g_qn++];
}
static void cr_push(const crect_t *r) {
    if (g_crn == g_crcap) { g_crcap = g_crcap ? g_crcap * 2 : 1024; g_cr = realloc(g_cr, sizeof *g_cr * g_crcap); }
    g_cr[g_crn++] = *r;
}
static void batch_close(void) {
    if (!g_b.open) return;
    g_b.open = 0;
    if (g_avn == g_b.v0) return;
    qcmd_t *c = q_push(); c->kind = Q_DRAW;
    c->t = g_b.t; c->s = g_b.s; c->prim = g_b.prim; c->v0 = g_b.v0; c->nv = g_avn - g_b.v0; c->i0 = g_b.i0; c->ni = g_ain - g_b.i0;
}
static void batch_flush(void) {
    batch_close();
    if (!g_qn) { g_avn = g_ain = 0; g_crn = 0; g_bpxn = 0; g_bh = 0; return; }
    size_t vb = sizeof(rvert_t) * (size_t)g_avn, ib = 4 * (size_t)g_ain, voff = 0;
    if (g_avn) {
        if (!gs.valid) { int k = 0; while (g_q[k].kind != Q_DRAW) k++; apply_state(g_q[k].t, &g_q[k].s); }   /* binds the VAO and buffers */
        if (vb > g_vbo_cap || ib > g_ibo_cap) {             /* more than ever before: bigger buffers */
            while (vb > g_vbo_cap) g_vbo_cap *= 2;
            while (ib > g_ibo_cap) g_ibo_cap *= 2;
            g_vbo_off = g_vbo_cap; g_ibo_off = g_ibo_cap;    /* forces the orphan below at the new size */
        }
        voff = (g_vbo_off + sizeof(rvert_t) - 1) / sizeof(rvert_t) * sizeof(rvert_t);
        if (voff + vb > g_vbo_cap || g_ibo_off + ib > g_ibo_cap) {
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_vbo_cap, NULL, GL_STREAM_DRAW);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)g_ibo_cap, NULL, GL_STREAM_DRAW);
            voff = g_vbo_off = g_ibo_off = 0;
        }
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)voff, (GLsizeiptr)vb, g_av);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, (GLintptr)g_ibo_off, (GLsizeiptr)ib, g_ai);
        g_rdr_uploads++;
    }
    blits_upload();
    GLint vbase = (GLint)(voff / sizeof(rvert_t));
    size_t ibase = g_ibo_off;
    for (int i = 0; i < g_qn; i++) {
        qcmd_t *c = &g_q[i];
        if (c->kind == Q_COPY) { copy_run(c->t, g_cr + c->r0, c->nr); g_rdr_copyruns++; continue; }
        if (c->kind == Q_BLIT) { blit_now(c); continue; }
        if (c->kind == Q_COPYFROM) { const crect_t *r = g_cr + c->r0; copy_from_now(c->t, r->dx, r->dy, r->dw, r->dh, c->src, r->sx, r->sy, r->sw, r->sh); continue; }
        apply_state(c->t, &c->s);
        GLenum mode = c->prim == RPRIM_TRIS ? GL_TRIANGLES : c->prim == RPRIM_LINES ? GL_LINES : GL_POINTS;
        glDrawElementsBaseVertex(mode, (GLsizei)c->ni, GL_UNSIGNED_INT, (void *)(uintptr_t)(ibase + 4 * (size_t)c->i0), vbase + (GLint)c->v0);
        g_rdr_batches++;
    }
    if (g_avn) { g_vbo_off = voff + vb; g_ibo_off += ib; }
    g_qn = 0; g_avn = g_ain = 0; g_crn = 0; g_bpxn = 0; g_bh = 0;
}
static void g_draw_q(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni) {
    t->gen++;
    g_rdr_draws++;
    if (g_b.open && (g_b.t != t || g_b.prim != prim || memcmp(&g_b.s, s, sizeof *s))) batch_close();
    /* keep one upload within the streaming buffers' size */
    if ((size_t)(g_avn + nv) * sizeof(rvert_t) > VBO_BYTES || (size_t)(g_ain + ni) * 4 > IBO_BYTES) batch_flush();
    if (!g_b.open) { g_b.open = 1; g_b.t = t; g_b.s = *s; g_b.prim = prim; g_b.v0 = g_avn; g_b.i0 = g_ain; }
    if (g_avn + nv > g_avcap) { g_avcap = (g_avn + nv) * 2 + 4096; g_av = realloc(g_av, sizeof(rvert_t) * g_avcap); }
    if (g_ain + ni > g_aicap) { g_aicap = (g_ain + ni) * 2 + 8192; g_ai = realloc(g_ai, 4 * (size_t)g_aicap); }
    memcpy(g_av + g_avn, v, sizeof(rvert_t) * nv);
    uint32_t rel = g_avn - g_b.v0, *o = g_ai + g_ain;
    for (int i = 0; i < ni; i++) o[i] = (uint32_t)idx[i] + rel;
    g_avn += nv; g_ain += ni;
}

/* CPU-drawn pixels (cars, shadows, text that GP3 draws with the processor) put on a target. They
 * are queued too: the pixels are kept in a CPU arena and, at the flush, all the blits of the queue
 * go to the GPU through ONE pixel-buffer upload, each into its own rows of a staging texture. */
#define BLIT_ROWS 8192
static void g_blit_q(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *argb, int sw, int sh, int pitch_px, int keyed) {
    t->gen++;
    if (sw <= 0 || sh <= 0) return;
    batch_close();
    /* queued only when copied 1:1 (the overlay of cars and shadows): each pixel then falls in the
     * middle of a texel, so the result is exactly the same wherever the pixels sit in the staging
     * texture. A scaled picture (the start lights panel...) goes the direct way, as before. */
    int direct = dw != sw || dh != sh || sw > 4096 || sh > BLIT_ROWS;
    if (direct || g_bh + sh > BLIT_ROWS) batch_flush();
    if (direct) {
        gl_state_stale();
        upload_staging(argb, pitch_px, sw, sh);
        glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
        glViewport(0, 0, t->sw, t->sh);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(g_quad_prog);
        glUniform4f(g_quad_u_uvrect, 0, 0, (float)sw / g_upload_w, (float)sh / g_upload_h);
        quad(g_upload_tex, (float)dx * g_scale, (float)dy * g_scale, (float)(dx + dw) * g_scale, (float)(dy + dh) * g_scale, t->sw, t->sh, 0, keyed, 0);
        glUniform4f(g_quad_u_uvrect, 0, 0, 1, 1);
        return;
    }
    size_t n = (size_t)sw * sh;
    if (g_bpxn + n > g_bpxcap) { g_bpxcap = (g_bpxn + n) * 2 + 65536; g_bpx = realloc(g_bpx, g_bpxcap * 4); }
    for (int y = 0; y < sh; y++) memcpy(g_bpx + g_bpxn + (size_t)y * sw, argb + (size_t)y * pitch_px, (size_t)sw * 4);
    qcmd_t *c = q_push(); c->kind = Q_BLIT; c->t = c->src = t; c->prim = keyed; c->v0 = (uint32_t)g_bpxn; c->i0 = (uint32_t)g_bh;
    crect_t r = { dx, dy, dw, dh, 0, 0, sw, sh }; c->r0 = g_crn; c->nr = 1; cr_push(&r);
    g_bpxn += n; g_bh += sh;
}
static GLuint g_btex, g_bpbo; static int g_btex_w, g_btex_h;
/* all the queued blits' pixels to the staging texture: one buffer upload */
static void blits_upload(void) {
    if (!g_bpxn) return;
    gl_state_stale();   /* binds a texture behind the draw state cache */
    if (!g_btex) { glGenTextures(1, &g_btex); glGenBuffers(1, &g_bpbo); }
    int w = 0;
    for (int i = 0; i < g_qn; i++) if (g_q[i].kind == Q_BLIT && g_cr[g_q[i].r0].sw > w) w = g_cr[g_q[i].r0].sw;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_btex);
    if (w > g_btex_w || g_bh > g_btex_h) {   /* grows to what the game needs (a few hundred rows) */
        if (w > g_btex_w) g_btex_w = w;
        while (g_btex_h < g_bh) g_btex_h = g_btex_h ? g_btex_h * 2 : 256;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_btex_w, g_btex_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, g_bpbo);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, (GLsizeiptr)(g_bpxn * 4), g_bpx, GL_STREAM_DRAW);
    for (int i = 0; i < g_qn; i++) {
        if (g_q[i].kind != Q_BLIT) continue;
        const crect_t *r = &g_cr[g_q[i].r0];
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)g_q[i].i0, r->sw, r->sh, GL_BGRA, GL_UNSIGNED_BYTE, (const void *)(uintptr_t)((size_t)g_q[i].v0 * 4));
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    g_rdr_uploads++;
}
static void blit_now(const qcmd_t *c) {
    const crect_t *r = &g_cr[c->r0]; rtarget_t *t = c->t;
    gl_state_stale();
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->sw, t->sh);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_quad_prog);
    glUniform4f(g_quad_u_uvrect, 0, (float)c->i0 / g_btex_h, (float)r->sw / g_btex_w, (float)(c->i0 + r->sh) / g_btex_h);
    quad(g_btex, (float)r->dx * g_scale, (float)r->dy * g_scale, (float)(r->dx + r->dw) * g_scale, (float)(r->dy + r->dh) * g_scale, t->sw, t->sh, 0, c->prim, 0);
    glUniform4f(g_quad_u_uvrect, 0, 0, 1, 1);
}

static GLuint g_copy_tex, g_copy_fbo; static int g_copy_w, g_copy_h;
static void copy_now(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    gl_state_stale();
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
    if (sw <= 0 || sh <= 0) return;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
    glBlitFramebuffer(sx * S, sy * S, (sx + sw) * S, (sy + sh) * S, 0, 0, sw * S, sh * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_copy_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glBlitFramebuffer(0, 0, sw * S, sh * S, dx * S, dy * S, (dx + dw) * S, (dy + dh) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
}

static void copy_from_now(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) {
    gl_state_stale();
    int S = g_scale;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, src->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
    glBlitFramebuffer(sx * S, sy * S, (sx + sw) * S, (sy + sh) * S, dx * S, dy * S, (dx + dw) * S, (dy + dh) * S,
                      GL_COLOR_BUFFER_BIT, (sw == dw && sh == dh) ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
}

/* GP3 copies pixel rows inside the frame all the time (mirrors, reflections on a wet track): from
 * ~200 up to ~900 one-row copies per frame, each one two framebuffer blits. Copies are queued like
 * draws; consecutive same-size copies on one target become a "run": the source area is copied once
 * into a scratch texture and all the rows are drawn from it in one draw call. A copy that reads
 * rows written earlier in the same run starts a new run, so the result is the same as copying one
 * by one. */
static void g_copy_q(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    t->gen++;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    batch_close();
    crect_t r = { dx, dy, dw, dh, sx, sy, sw, sh };
    int same = dw == sw && dh == sh;
    qcmd_t *last = g_qn ? &g_q[g_qn - 1] : NULL;
    if (same && last && last->kind == Q_COPY && last->t == t && last->same) {
        int hazard = 0;                       /* reads what this run already wrote? */
        for (uint32_t i = last->r0; i < last->r0 + last->nr; i++) {
            const crect_t *o = &g_cr[i];
            if (sx < o->dx + o->dw && o->dx < sx + sw && sy < o->dy + o->dh && o->dy < sy + sh) { hazard = 1; break; }
        }
        if (!hazard) { cr_push(&r); last->nr++; return; }
    }
    qcmd_t *c = q_push(); c->kind = Q_COPY; c->t = t; c->same = same; c->r0 = g_crn; c->nr = 1; cr_push(&r);
}
static void g_copy_from_q(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) {
    t->gen++;
    batch_close();
    qcmd_t *c = q_push(); c->kind = Q_COPYFROM; c->t = t; c->src = src; c->r0 = g_crn; c->nr = 1;
    crect_t r = { dx, dy, dw, dh, sx, sy, sw, sh }; cr_push(&r);
}
/* [advanced] batching=0: every drawing goes to the GPU at once, as in GP3RT 1.0.3 */
int g_gl_batching = 1;
static void g_draw(rtarget_t *t, const rstate_t *s, int prim, const rvert_t *v, int nv, const uint16_t *idx, int ni) { g_draw_q(t, s, prim, v, nv, idx, ni); if (!g_gl_batching) batch_flush(); }
static void g_blit(rtarget_t *t, int dx, int dy, int dw, int dh, const uint32_t *argb, int sw, int sh, int pitch_px, int keyed) { g_blit_q(t, dx, dy, dw, dh, argb, sw, sh, pitch_px, keyed); if (!g_gl_batching) batch_flush(); }
static void g_copy(rtarget_t *t, int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) { g_copy_q(t, dx, dy, dw, dh, sx, sy, sw, sh); if (!g_gl_batching) batch_flush(); }
static void g_copy_from(rtarget_t *t, int dx, int dy, int dw, int dh, rtarget_t *src, int sx, int sy, int sw, int sh) { g_copy_from_q(t, dx, dy, dw, dh, src, sx, sy, sw, sh); if (!g_gl_batching) batch_flush(); }
static GLuint g_cprog, g_cvao, g_cvbo; static GLint g_cprog_tex;
static float *g_cv; static size_t g_cvcap;
static void copy_run(rtarget_t *t, const crect_t *r, uint32_t n) {
    if (n == 1) { copy_now(t, r->dx, r->dy, r->dw, r->dh, r->sx, r->sy, r->sw, r->sh); return; }
    int S = g_scale, bx0 = r->sx, by0 = r->sy, bx1 = r->sx + r->sw, by1 = r->sy + r->sh;
    for (uint32_t i = 1; i < n; i++) {
        if (r[i].sx < bx0) bx0 = r[i].sx; if (r[i].sy < by0) by0 = r[i].sy;
        if (r[i].sx + r[i].sw > bx1) bx1 = r[i].sx + r[i].sw; if (r[i].sy + r[i].sh > by1) by1 = r[i].sy + r[i].sh;
    }
    int bw = bx1 - bx0, bh = by1 - by0;
    /* 1. the source area, once, into the scratch texture */
    copy_now(t, 0, 0, 0, 0, 0, 0, 0, 0);   /* (only makes sure the scratch texture exists) */
    if (g_copy_w < bw * S || g_copy_h < bh * S) {
        g_copy_w = bw * S > g_copy_w ? bw * S : g_copy_w; g_copy_h = bh * S > g_copy_h ? bh * S : g_copy_h;
        glBindTexture(GL_TEXTURE_2D, g_copy_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_copy_w, g_copy_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copy_fbo);
    glBlitFramebuffer(bx0 * S, by0 * S, bx1 * S, by1 * S, 0, 0, bw * S, bh * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    /* 2. every row from it, one draw call */
    if (!g_cprog) {
        static const char *vs = "#version 330 core\nlayout(location=0) in vec4 a;\nout vec2 uv;\nvoid main(){ gl_Position = vec4(a.xy, 0.0, 1.0); uv = a.zw; }\n";
        static const char *fs = "#version 330 core\nin vec2 uv; uniform sampler2D u_tex; out vec4 o;\nvoid main(){ o = texelFetch(u_tex, ivec2(uv), 0); }\n";
        g_cprog = link_prog(vs, fs); g_cprog_tex = glGetUniformLocation(g_cprog, "u_tex");
        glGenVertexArrays(1, &g_cvao); glGenBuffers(1, &g_cvbo);
        glBindVertexArray(g_cvao); glBindBuffer(GL_ARRAY_BUFFER, g_cvbo);
        glVertexAttribPointer(0, 4, GL_FLOAT, 0, 16, (void *)0); glEnableVertexAttribArray(0);
    }
    size_t need = (size_t)n * 6 * 4;
    if (need > g_cvcap) { g_cvcap = need * 2; g_cv = realloc(g_cv, g_cvcap * sizeof(float)); }
    float *o = g_cv, fx = 2.f / t->sw, fy = 2.f / t->sh;
    for (uint32_t i = 0; i < n; i++) {
        /* destination in clip space; source in scratch texels (texelFetch: integer, exact) */
        float x0 = r[i].dx * S * fx - 1, x1 = (r[i].dx + r[i].dw) * S * fx - 1, y0 = r[i].dy * S * fy - 1, y1 = (r[i].dy + r[i].dh) * S * fy - 1;
        float u0 = (float)((r[i].sx - bx0) * S), u1 = u0 + r[i].sw * S, v0 = (float)((r[i].sy - by0) * S), v1 = v0 + r[i].sh * S;
        float q[6][4] = { { x0, y0, u0, v0 }, { x1, y0, u1, v0 }, { x0, y1, u0, v1 }, { x1, y0, u1, v0 }, { x1, y1, u1, v1 }, { x0, y1, u0, v1 } };
        memcpy(o, q, sizeof q); o += 24;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->sw, t->sh);
    glDisable(GL_DEPTH_TEST); glDepthMask(0); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_POLYGON_OFFSET_FILL);
    glUseProgram(g_cprog); glUniform1i(g_cprog_tex, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_copy_tex);
    glBindVertexArray(g_cvao); glBindBuffer(GL_ARRAY_BUFFER, g_cvbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(need * sizeof(float)), g_cv, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(n * 6));
    gl_state_stale();
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
    glUniform4f(g_quad_u_uvrect, 0, 0, u1, v1);
    /* target textures are stored top-first: flip v when drawing to the window */
    quad(tex, (float)vx, (float)vy, (float)(vx + vw), (float)(vy + vh), ww, wh, 1, 0, 1);
    glUniform4f(g_quad_u_uvrect, 0, 0, 1, 1);
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
    { const char *e = getenv("GP3_TEX_PBO"); if (e) g_tex_pbo = atoi(e) != 0; }
    { const char *e = getenv("GP3_BATCHING"); if (e) g_gl_batching = atoi(e) != 0; }
    if (opt) { g_scale = opt->scale > 0 ? opt->scale : 1; g_aniso = opt->anisotropy; g_force_filter = opt->force_linear; g_gen_mips = opt->generate_mipmaps; }
    glog("GL: %s / %s / %s (internal scale %dx, aniso %d)", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), g_scale, g_aniso);
    if (!g_gl_batching) glog("GL: batching off (each drawing sent at once, as in 1.0.3)");
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
    g_quad_u_uvrect = glGetUniformLocation(g_quad_prog, "u_uvrect");
    glUseProgram(g_quad_prog);
    glUniform1i(g_quad_u_tex, 0);
    glUniform4f(g_quad_u_uvrect, 0, 0, 1, 1);
    GLenum e = glGetError();
    if (e) glog("GL: init error %x", e);
    return &g_gl;
}
