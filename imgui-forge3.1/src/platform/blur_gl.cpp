#include "platform/blur_gl.h"

#include <algorithm>
#include <cstdint>

#include <GL/gl.h>
#include <GL/glext.h>

namespace nl::blur_gl {
namespace {

constexpr int k_levels = 4;
constexpr float k_spread = 1.0f;

constexpr const char* k_vertex = R"(#version 150
out vec2 uv;
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// `flip` turns the bottom-up copy of the framebuffer into a top-down texture, which is what
// ImGui's UVs expect; only the first pass sets it.
constexpr const char* k_down = R"(#version 150
uniform sampler2D src;
uniform vec2 texel;
uniform float spread;
uniform float flip;
in vec2 uv;
out vec4 color;
vec3 tap(vec2 t) { return texture(src, vec2(t.x, mix(t.y, 1.0 - t.y, flip))).rgb; }
void main()
{
    vec2 o = texel * spread;
    vec3 c = tap(uv) * 4.0;
    c += tap(uv + vec2(-o.x, -o.y));
    c += tap(uv + vec2( o.x, -o.y));
    c += tap(uv + vec2(-o.x,  o.y));
    c += tap(uv + vec2( o.x,  o.y));
    color = vec4(c / 8.0, 1.0);
}
)";

constexpr const char* k_up = R"(#version 150
uniform sampler2D src;
uniform vec2 texel;
uniform float spread;
in vec2 uv;
out vec4 color;
void main()
{
    vec2 o = texel * spread;
    vec3 c = texture(src, uv + vec2(-o.x * 2.0, 0.0)).rgb;
    c += texture(src, uv + vec2(-o.x,  o.y)).rgb * 2.0;
    c += texture(src, uv + vec2( 0.0,  o.y * 2.0)).rgb;
    c += texture(src, uv + vec2( o.x,  o.y)).rgb * 2.0;
    c += texture(src, uv + vec2( o.x * 2.0, 0.0)).rgb;
    c += texture(src, uv + vec2( o.x, -o.y)).rgb * 2.0;
    c += texture(src, uv + vec2( 0.0, -o.y * 2.0)).rgb;
    c += texture(src, uv + vec2(-o.x, -o.y)).rgb * 2.0;
    color = vec4(c / 12.0, 1.0);
}
)";

struct Gl {
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLCREATESHADERPROC CreateShader;
    PFNGLSHADERSOURCEPROC ShaderSource;
    PFNGLCOMPILESHADERPROC CompileShader;
    PFNGLGETSHADERIVPROC GetShaderiv;
    PFNGLDELETESHADERPROC DeleteShader;
    PFNGLCREATEPROGRAMPROC CreateProgram;
    PFNGLATTACHSHADERPROC AttachShader;
    PFNGLLINKPROGRAMPROC LinkProgram;
    PFNGLGETPROGRAMIVPROC GetProgramiv;
    PFNGLDELETEPROGRAMPROC DeleteProgram;
    PFNGLUSEPROGRAMPROC UseProgram;
    PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation;
    PFNGLUNIFORM1IPROC Uniform1i;
    PFNGLUNIFORM1FPROC Uniform1f;
    PFNGLUNIFORM2FPROC Uniform2f;
    PFNGLGENVERTEXARRAYSPROC GenVertexArrays;
    PFNGLDELETEVERTEXARRAYSPROC DeleteVertexArrays;
    PFNGLBINDVERTEXARRAYPROC BindVertexArray;
    PFNGLACTIVETEXTUREPROC ActiveTexture;
};
Gl gl{};

struct Program {
    GLuint id = 0;
    GLint texel = -1, spread = -1, flip = -1;
};

struct Level {
    GLuint tex = 0, fbo = 0;
    int w = 0, h = 0;
};

struct State {
    bool ready = false;
    Program down, up;
    GLuint vao = 0;
    GLuint copy = 0;
    int copy_w = 0, copy_h = 0;
    Level levels[k_levels];
};
State g;

template <class F>
bool load(F& fn, GetProc get_proc, const char* name)
{
    fn = reinterpret_cast<F>(get_proc(name));
    return fn != nullptr;
}

GLuint compile(GLenum type, const char* source)
{
    const GLuint shader = gl.CreateShader(type);
    gl.ShaderSource(shader, 1, &source, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

bool link(Program& p, const char* fragment)
{
    const GLuint vs = compile(GL_VERTEX_SHADER, k_vertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        if (vs)
            gl.DeleteShader(vs);
        if (fs)
            gl.DeleteShader(fs);
        return false;
    }
    p.id = gl.CreateProgram();
    gl.AttachShader(p.id, vs);
    gl.AttachShader(p.id, fs);
    gl.LinkProgram(p.id);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok)
        return false;
    gl.UseProgram(p.id);
    gl.Uniform1i(gl.GetUniformLocation(p.id, "src"), 0);
    p.texel = gl.GetUniformLocation(p.id, "texel");
    p.spread = gl.GetUniformLocation(p.id, "spread");
    p.flip = gl.GetUniformLocation(p.id, "flip");
    gl.UseProgram(0);
    return true;
}

void storage(GLuint tex, int w, int h)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

GLuint make_texture()
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    storage(tex, 1, 1);
    return tex;
}

// Texture names stay the same when the size changes, so the ImTextureID handed to ImGui does too.
void resize(int w, int h)
{
    if (w == g.copy_w && h == g.copy_h)
        return;
    storage(g.copy, w, h);
    g.copy_w = w;
    g.copy_h = h;
    for (Level& l : g.levels) {
        w = std::max(1, (w + 1) / 2);
        h = std::max(1, (h + 1) / 2);
        storage(l.tex, w, h);
        l.w = w;
        l.h = h;
    }
}

void pass(const Program& p, GLuint src, int src_w, int src_h, const Level& dst, float flip)
{
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    gl.UseProgram(p.id);
    gl.Uniform2f(p.texel, 1.0f / static_cast<float>(src_w), 1.0f / static_cast<float>(src_h));
    gl.Uniform1f(p.spread, k_spread);
    if (p.flip >= 0)
        gl.Uniform1f(p.flip, flip);
    glBindTexture(GL_TEXTURE_2D, src);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void capture(const ImDrawList*, const ImDrawCmd*)
{
    GLint draw_fbo = 0, read_fbo = 0, viewport[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_VIEWPORT, viewport);
    const int w = viewport[2], h = viewport[3];
    if (w <= 0 || h <= 0)
        return;

    gl.ActiveTexture(GL_TEXTURE0);
    resize(w, h);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
    glBindTexture(GL_TEXTURE_2D, g.copy);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], w, h);

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    gl.BindVertexArray(g.vao);
    pass(g.down, g.copy, w, h, g.levels[0], 1.0f);
    for (int i = 1; i < k_levels; ++i)
        pass(g.down, g.levels[i - 1].tex, g.levels[i - 1].w, g.levels[i - 1].h, g.levels[i], 0.0f);
    for (int i = k_levels - 2; i >= 0; --i)
        pass(g.up, g.levels[i + 1].tex, g.levels[i + 1].w, g.levels[i + 1].h, g.levels[i], 0.0f);

    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}

} // namespace

bool init(GetProc get_proc)
{
    bool ok = true;
    ok &= load(gl.GenFramebuffers, get_proc, "glGenFramebuffers");
    ok &= load(gl.DeleteFramebuffers, get_proc, "glDeleteFramebuffers");
    ok &= load(gl.BindFramebuffer, get_proc, "glBindFramebuffer");
    ok &= load(gl.FramebufferTexture2D, get_proc, "glFramebufferTexture2D");
    ok &= load(gl.CreateShader, get_proc, "glCreateShader");
    ok &= load(gl.ShaderSource, get_proc, "glShaderSource");
    ok &= load(gl.CompileShader, get_proc, "glCompileShader");
    ok &= load(gl.GetShaderiv, get_proc, "glGetShaderiv");
    ok &= load(gl.DeleteShader, get_proc, "glDeleteShader");
    ok &= load(gl.CreateProgram, get_proc, "glCreateProgram");
    ok &= load(gl.AttachShader, get_proc, "glAttachShader");
    ok &= load(gl.LinkProgram, get_proc, "glLinkProgram");
    ok &= load(gl.GetProgramiv, get_proc, "glGetProgramiv");
    ok &= load(gl.DeleteProgram, get_proc, "glDeleteProgram");
    ok &= load(gl.UseProgram, get_proc, "glUseProgram");
    ok &= load(gl.GetUniformLocation, get_proc, "glGetUniformLocation");
    ok &= load(gl.Uniform1i, get_proc, "glUniform1i");
    ok &= load(gl.Uniform1f, get_proc, "glUniform1f");
    ok &= load(gl.Uniform2f, get_proc, "glUniform2f");
    ok &= load(gl.GenVertexArrays, get_proc, "glGenVertexArrays");
    ok &= load(gl.DeleteVertexArrays, get_proc, "glDeleteVertexArrays");
    ok &= load(gl.BindVertexArray, get_proc, "glBindVertexArray");
    ok &= load(gl.ActiveTexture, get_proc, "glActiveTexture");
    if (!ok || !link(g.down, k_down) || !link(g.up, k_up)) {
        shutdown();
        return false;
    }

    gl.GenVertexArrays(1, &g.vao);
    g.copy = make_texture();
    for (Level& l : g.levels) {
        l.tex = make_texture();
        gl.GenFramebuffers(1, &l.fbo);
        gl.BindFramebuffer(GL_FRAMEBUFFER, l.fbo);
        gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l.tex, 0);
    }
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    g.ready = true;
    return true;
}

void shutdown()
{
    if (gl.DeleteFramebuffers)
        for (Level& l : g.levels)
            if (l.fbo)
                gl.DeleteFramebuffers(1, &l.fbo);
    for (Level& l : g.levels)
        if (l.tex)
            glDeleteTextures(1, &l.tex);
    if (g.copy)
        glDeleteTextures(1, &g.copy);
    if (g.vao && gl.DeleteVertexArrays)
        gl.DeleteVertexArrays(1, &g.vao);
    if (gl.DeleteProgram) {
        if (g.down.id)
            gl.DeleteProgram(g.down.id);
        if (g.up.id)
            gl.DeleteProgram(g.up.id);
    }
    g = State{};
}

blur::Backend backend()
{
    if (!g.ready)
        return {};
    return { &capture, static_cast<ImTextureID>(g.levels[0].tex) };
}

} // namespace nl::blur_gl
