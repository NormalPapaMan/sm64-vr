// VR overlay: the floating menu screen (title, file select, act select are rendered exactly like
// the desktop game into a texture and shown on a panel that stays put in the room), the
// on-panel START button, and the controller laser pointers.

#ifdef _WIN32
#include <windows.h>
#endif
#define GLEW_STATIC
#include <GL/glew.h>

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "vr.h"
#include "vr_math.h"

struct VrPanel gVrPanel;
struct VrButton gVrStartButton;
bool gVrLaserActive[2];
float gVrLaserHit[2][3];

#define SCREEN_W 1280
#define SCREEN_H 960

static bool sInited;
static GLuint sProg, sScreenFbo, sScreenTex, sScreenDepth, sStartTex, sWhiteTex;
static GLint uMvp, uColor, uTex;

void vr_panel_matrix(float out[4][4]) {
    vrm_rot_y(out, gVrPanel.yaw);
    out[3][0] = gVrPanel.pos[0];
    out[3][1] = gVrPanel.pos[1];
    out[3][2] = gVrPanel.pos[2];
}

// ---- tiny 5x7 font for the button label --------------------------------------------------------

static const char *glyph(char c) {
    switch (c) {
        case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
        case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
        case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
        case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
        default:  return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    }
}

static GLuint make_button_texture(const char *label) {
    enum { W = 128, H = 48, SCALE = 3 };
    static unsigned char px[W * H * 4];
    int len = (int) strlen(label);
    int textW = (len * 6 - 1) * SCALE, textH = 7 * SCALE;
    int ox = (W - textW) / 2, oy = (H - textH) / 2;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned char *p = &px[(y * W + x) * 4];
            bool border = x < 3 || y < 3 || x >= W - 3 || y >= H - 3;
            bool text = false;
            int tx = x - ox, ty = y - oy;
            if (tx >= 0 && ty >= 0 && tx < textW && ty < textH) {
                int ch = tx / (6 * SCALE), cx = (tx % (6 * SCALE)) / SCALE, cy = ty / SCALE;
                // texture rows go bottom-up in GL; the glyphs are written top-down
                if (cx < 5 && ch < len) text = glyph(label[ch])[(6 - cy) * 5 + cx] == '1';
            }
            if (border || text) {
                p[0] = p[1] = p[2] = 255;
            } else {
                p[0] = 200; p[1] = 20; p[2] = 30;
            }
            p[3] = 255;
        }
    }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[vr] overlay shader: %s\n", log);
    }
    return s;
}

static void overlay_init(void) {
    if (sInited) return;
    sInited = true;
    const char *vs =
        "#version 110\n"
        "attribute vec3 aPos; attribute vec2 aUv; uniform mat4 uMvp; varying vec2 vUv;\n"
        "void main() { vUv = aUv; gl_Position = uMvp * vec4(aPos, 1.0); }\n";
    const char *fs =
        "#version 110\n"
        "uniform sampler2D uTex; uniform vec4 uColor; varying vec2 vUv;\n"
        "void main() { gl_FragColor = texture2D(uTex, vUv) * uColor; }\n";
    sProg = glCreateProgram();
    glAttachShader(sProg, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(sProg, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(sProg, 0, "aPos");
    glBindAttribLocation(sProg, 1, "aUv");
    glLinkProgram(sProg);
    uMvp = glGetUniformLocation(sProg, "uMvp");
    uColor = glGetUniformLocation(sProg, "uColor");
    uTex = glGetUniformLocation(sProg, "uTex");

    glGenTextures(1, &sScreenTex);
    glBindTexture(GL_TEXTURE_2D, sScreenTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SCREEN_W, SCREEN_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &sScreenDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, sScreenDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, SCREEN_W, SCREEN_H);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &sScreenFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sScreenFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sScreenTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sScreenDepth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    sStartTex = make_button_texture("START");
    static const unsigned char white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &sWhiteTex);
    glBindTexture(GL_TEXTURE_2D, sWhiteTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void vr_screen_capture(void *displayList) {
    overlay_init();
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sScreenFbo);
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    gfx_vr_render_flat(displayList, SCREEN_W, SCREEN_H);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

// ---- drawing ------------------------------------------------------------------------------------

static VrMat sViewProj; // tracking space -> clip for the current eye

static void draw(GLenum prim, const float *xyz, const float *uv, int n, const VrMat model, GLuint tex,
                 float r, float g, float b, float a) {
    VrMat mvp;
    vrm_mul(mvp, model, sViewProj);
    glUniformMatrix4fv(uMvp, 1, GL_FALSE, &mvp[0][0]);
    glUniform4f(uColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, xyz);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(prim, 0, n);
}

// Axis-aligned rectangle in panel-local space (z = small offset toward the viewer).
static void draw_rect(const VrMat panel, float cx, float cy, float hw, float hh, float z, GLuint tex,
                      float r, float g, float b, float a) {
    const float xyz[] = {
        cx - hw, cy - hh, z, cx + hw, cy - hh, z, cx + hw, cy + hh, z,
        cx - hw, cy - hh, z, cx + hw, cy + hh, z, cx - hw, cy + hh, z,
    };
    const float uv[] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
    draw(GL_TRIANGLES, xyz, uv, 6, panel, tex, r, g, b, a);
}

void vr_overlay_draw(int eye, bool screenMode) {
    overlay_init();
    const struct VrTracking *t = &gVrTrack;
    uint32_t w, h;
    vr_eye_size(&w, &h);

    // eye view-projection in tracking space (meters)
    VrMat eyeM, view, proj, ident;
    vrm_from_pose(eyeM, t->eye[eye].quat, t->eye[eye].pos);
    vrm_rigid_inverse(view, eyeM);
    vrm_projection(proj, t->fov[eye][0], t->fov[eye][1], t->fov[eye][2], t->fov[eye][3], 0.05f, 100.0f);
    vrm_mul(sViewProj, view, proj);
    vrm_identity(ident);

    // save the state the game renderer caches
    GLint prevProg = 0, prevTex = 0, prevActive = 0, prevBuf = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glPushAttrib(GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_VIEWPORT_BIT
                 | GL_SCISSOR_BIT | GL_POLYGON_BIT | GL_LINE_BIT);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);

    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUseProgram(sProg);
    glUniform1i(uTex, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    VrMat panel;
    vr_panel_matrix(panel);

    if (screenMode) {
        glClearColor(0.03f, 0.03f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // dim floor so the room doesn't feel like a void
        const float floorXyz[] = { -6, 0, -6, 6, 0, -6, 6, 0, 6, -6, 0, -6, 6, 0, 6, -6, 0, 6 };
        const float floorUv[] = { 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1 };
        VrMat floorM;
        vrm_translate(floorM, t->head.pos[0], t->head.pos[1] - 1.6f, t->head.pos[2]);
        draw(GL_TRIANGLES, floorXyz, floorUv, 6, floorM, sWhiteTex, 0.08f, 0.08f, 0.12f, 1.0f);
        // frame + picture
        draw_rect(panel, 0, 0, gVrPanel.halfW + 0.04f, gVrPanel.halfH + 0.04f, -0.002f, sWhiteTex, 0.12f, 0.12f, 0.14f, 1);
        draw_rect(panel, 0, 0, gVrPanel.halfW, gVrPanel.halfH, 0, sScreenTex, 1, 1, 1, 1);
    }

    if (gVrStartButton.visible) {
        const struct VrButton *b = &gVrStartButton;
        float k = b->hovered ? 1.0f : 0.8f;
        float grow = b->hovered ? 1.08f : 1.0f;
        draw_rect(panel, b->cx, b->cy, b->hw * grow, b->hh * grow, 0.003f, sStartTex, k, k, k, b->hovered ? 1.0f : 0.85f);
    }

    for (int hnd = 0; hnd < 2; hnd++) {
        if (!gVrLaserActive[hnd] || !t->aimValid[hnd]) continue;
        const float *o = t->aim[hnd].pos, *e = gVrLaserHit[hnd];
        const float line[] = { o[0], o[1], o[2], e[0], e[1], e[2] };
        const float luv[] = { 0, 0, 1, 1 };
        glLineWidth(3.0f);
        draw(GL_LINES, line, luv, 2, ident, sWhiteTex, 0.4f, 0.8f, 1.0f, 0.8f);
        // cursor dot on the panel
        VrMat dot;
        vrm_copy(dot, panel);
        dot[3][0] = e[0]; dot[3][1] = e[1]; dot[3][2] = e[2];
        draw_rect(dot, 0, 0, 0.012f, 0.012f, 0.004f, sWhiteTex, 1, 1, 1, 1);
    }

    glPopClientAttrib();
    glPopAttrib();
    glUseProgram(prevProg);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glActiveTexture(prevActive);
    glBindBuffer(GL_ARRAY_BUFFER, prevBuf);
}
