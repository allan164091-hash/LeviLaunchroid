#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <dlfcn.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

namespace {

constexpr const char *kModuleId = "framegen_mod.main";

using SwapBuffersFn = unsigned int (*)(void *, void *);

SwapBuffersFn gOriginalSwap = nullptr;
std::atomic_bool gActive{true};
std::atomic_bool gHookInstalled{false};
std::atomic_bool gGlReady{false};
std::atomic_bool gHavePrev{false};
std::atomic<std::uint64_t> gGameFrames{0};
std::atomic<std::uint64_t> gPresents{0};
std::atomic<std::uint64_t> gTotalFrames{0};
std::atomic<std::int64_t> gWindowStartNs{0};

// ---------- API do OpenGL/EGL carregada por dlsym ----------
#define GL_FN(name) decltype(&gl##name) name
struct GlApi {
  GL_FN(GetIntegerv);
  GL_FN(GetBooleanv);
  GL_FN(IsEnabled);
  GL_FN(Enable);
  GL_FN(Disable);
  GL_FN(ColorMask);
  GL_FN(BindFramebuffer);
  GL_FN(Viewport);
  GL_FN(GenTextures);
  GL_FN(DeleteTextures);
  GL_FN(BindTexture);
  GL_FN(TexImage2D);
  GL_FN(TexParameteri);
  GL_FN(CopyTexSubImage2D);
  GL_FN(ActiveTexture);
  GL_FN(CreateShader);
  GL_FN(ShaderSource);
  GL_FN(CompileShader);
  GL_FN(GetShaderiv);
  GL_FN(DeleteShader);
  GL_FN(CreateProgram);
  GL_FN(AttachShader);
  GL_FN(LinkProgram);
  GL_FN(GetProgramiv);
  GL_FN(UseProgram);
  GL_FN(GetUniformLocation);
  GL_FN(Uniform1i);
  GL_FN(Uniform1f);
  GL_FN(DrawArrays);
  GL_FN(BindVertexArray);
  GL_FN(BindSampler);
  decltype(&eglQuerySurface) QuerySurface;
};

GlApi gGl{};

template <typename T> bool bindSym(void *lib, const char *name, T &out) {
  out = reinterpret_cast<T>(dlsym(lib, name));
  return out != nullptr;
}

bool loadGlApi() {
  void *gles = dlopen("libGLESv3.so", RTLD_NOW);
  if (!gles) {
    gles = dlopen("libGLESv2.so", RTLD_NOW);
  }
  void *egl = dlopen("libEGL.so", RTLD_NOW);
  if (!gles || !egl) {
    return false;
  }
  bool ok = true;
#define BIND(name) ok &= bindSym(gles, "gl" #name, gGl.name)
  BIND(GetIntegerv);
  BIND(GetBooleanv);
  BIND(IsEnabled);
  BIND(Enable);
  BIND(Disable);
  BIND(ColorMask);
  BIND(BindFramebuffer);
  BIND(Viewport);
  BIND(GenTextures);
  BIND(DeleteTextures);
  BIND(BindTexture);
  BIND(TexImage2D);
  BIND(TexParameteri);
  BIND(CopyTexSubImage2D);
  BIND(ActiveTexture);
  BIND(CreateShader);
  BIND(ShaderSource);
  BIND(CompileShader);
  BIND(GetShaderiv);
  BIND(DeleteShader);
  BIND(CreateProgram);
  BIND(AttachShader);
  BIND(LinkProgram);
  BIND(GetProgramiv);
  BIND(UseProgram);
  BIND(GetUniformLocation);
  BIND(Uniform1i);
  BIND(Uniform1f);
  BIND(DrawArrays);
  BIND(BindVertexArray);
  BIND(BindSampler);
#undef BIND
  ok &= bindSym(egl, "eglQuerySurface", gGl.QuerySurface);
  return ok;
}

// ---------- recursos (so mexem na thread de render) ----------
GLuint gProg = 0;
bool gProgFailed = false;
GLint gUPrev = -1;
GLint gUCur = -1;
GLint gUMix = -1;
GLuint gTex[2] = {0, 0};
int gW = 0;
int gH = 0;
int gIdx = 0; // textura que recebe o frame atual

const char *kVertSrc = R"(#version 300 es
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *kFragSrc = R"(#version 300 es
precision mediump float;
uniform sampler2D uPrev;
uniform sampler2D uCur;
uniform float uMix;
in vec2 vUv;
out vec4 oColor;
void main() {
  oColor = mix(texture(uPrev, vUv), texture(uCur, vUv), uMix);
}
)";

GLuint compileShader(GLenum type, const char *src) {
  GLuint s = gGl.CreateShader(type);
  gGl.ShaderSource(s, 1, &src, nullptr);
  gGl.CompileShader(s);
  GLint ok = 0;
  gGl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    gGl.DeleteShader(s);
    return 0;
  }
  return s;
}

bool buildProgram() {
  GLuint vs = compileShader(GL_VERTEX_SHADER, kVertSrc);
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragSrc);
  if (!vs || !fs) {
    return false;
  }
  GLuint p = gGl.CreateProgram();
  gGl.AttachShader(p, vs);
  gGl.AttachShader(p, fs);
  gGl.LinkProgram(p);
  gGl.DeleteShader(vs);
  gGl.DeleteShader(fs);
  GLint ok = 0;
  gGl.GetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    return false;
  }
  gProg = p;
  gUPrev = gGl.GetUniformLocation(p, "uPrev");
  gUCur = gGl.GetUniformLocation(p, "uCur");
  gUMix = gGl.GetUniformLocation(p, "uMix");
  return true;
}

bool ensureResources(int w, int h) {
  if (!gProg && !gProgFailed) {
    if (!buildProgram()) {
      gProgFailed = true;
    }
  }
  if (!gProg) {
    return false;
  }
  if (w != gW || h != gH) {
    gGl.ActiveTexture(GL_TEXTURE0);
    if (gTex[0]) {
      gGl.DeleteTextures(2, gTex);
    }
    gGl.GenTextures(2, gTex);
    for (int i = 0; i < 2; i++) {
      gGl.BindTexture(GL_TEXTURE_2D, gTex[i]);
      gGl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB,
                     GL_UNSIGNED_BYTE, nullptr);
      gGl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      gGl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      gGl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      gGl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gW = w;
    gH = h;
    gIdx = 0;
    gHavePrev = false;
  }
  return true;
}

// ---------- salvar / restaurar estado do OpenGL ----------
struct SavedState {
  GLint program = 0;
  GLint vao = 0;
  GLint activeTex = 0;
  GLint tex0 = 0;
  GLint tex1 = 0;
  GLint samp0 = 0;
  GLint samp1 = 0;
  GLint drawFb = 0;
  GLint readFb = 0;
  GLint viewport[4] = {0, 0, 0, 0};
  GLboolean blend = GL_FALSE;
  GLboolean depth = GL_FALSE;
  GLboolean cull = GL_FALSE;
  GLboolean scissor = GL_FALSE;
  GLboolean stencil = GL_FALSE;
  GLboolean mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
};

void saveState(SavedState &s) {
  gGl.GetIntegerv(GL_CURRENT_PROGRAM, &s.program);
  gGl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
  gGl.GetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
  gGl.ActiveTexture(GL_TEXTURE0);
  gGl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex0);
  gGl.GetIntegerv(GL_SAMPLER_BINDING, &s.samp0);
  gGl.ActiveTexture(GL_TEXTURE1);
  gGl.GetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex1);
  gGl.GetIntegerv(GL_SAMPLER_BINDING, &s.samp1);
  gGl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFb);
  gGl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFb);
  gGl.GetIntegerv(GL_VIEWPORT, s.viewport);
  s.blend = gGl.IsEnabled(GL_BLEND);
  s.depth = gGl.IsEnabled(GL_DEPTH_TEST);
  s.cull = gGl.IsEnabled(GL_CULL_FACE);
  s.scissor = gGl.IsEnabled(GL_SCISSOR_TEST);
  s.stencil = gGl.IsEnabled(GL_STENCIL_TEST);
  gGl.GetBooleanv(GL_COLOR_WRITEMASK, s.mask);
}

void setEnabled(GLenum cap, GLboolean on) {
  if (on) {
    gGl.Enable(cap);
  } else {
    gGl.Disable(cap);
  }
}

void restoreState(const SavedState &s) {
  gGl.ActiveTexture(GL_TEXTURE1);
  gGl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.tex1));
  gGl.BindSampler(1, static_cast<GLuint>(s.samp1));
  gGl.ActiveTexture(GL_TEXTURE0);
  gGl.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s.tex0));
  gGl.BindSampler(0, static_cast<GLuint>(s.samp0));
  gGl.ActiveTexture(static_cast<GLenum>(s.activeTex));
  gGl.UseProgram(static_cast<GLuint>(s.program));
  gGl.BindVertexArray(static_cast<GLuint>(s.vao));
  gGl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(s.drawFb));
  gGl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(s.readFb));
  gGl.Viewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
  setEnabled(GL_BLEND, s.blend);
  setEnabled(GL_DEPTH_TEST, s.depth);
  setEnabled(GL_CULL_FACE, s.cull);
  setEnabled(GL_SCISSOR_TEST, s.scissor);
  setEnabled(GL_STENCIL_TEST, s.stencil);
  gGl.ColorMask(s.mask[0], s.mask[1], s.mask[2], s.mask[3]);
}

// Desenha mix(prev, cur, amount) na tela (framebuffer 0).
void drawPass(GLuint prevTex, GLuint curTex, float amount) {
  gGl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  gGl.Viewport(0, 0, gW, gH);
  gGl.Disable(GL_BLEND);
  gGl.Disable(GL_DEPTH_TEST);
  gGl.Disable(GL_CULL_FACE);
  gGl.Disable(GL_SCISSOR_TEST);
  gGl.Disable(GL_STENCIL_TEST);
  gGl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gGl.UseProgram(gProg);
  gGl.BindVertexArray(0);
  gGl.ActiveTexture(GL_TEXTURE0);
  gGl.BindTexture(GL_TEXTURE_2D, prevTex);
  gGl.BindSampler(0, 0);
  gGl.ActiveTexture(GL_TEXTURE1);
  gGl.BindTexture(GL_TEXTURE_2D, curTex);
  gGl.BindSampler(1, 0);
  gGl.Uniform1i(gUPrev, 0);
  gGl.Uniform1i(gUCur, 1);
  gGl.Uniform1f(gUMix, amount);
  gGl.DrawArrays(GL_TRIANGLES, 0, 3);
}

// Faz o frame gen 2x e apresenta os frames. Devolve o resultado do swap.
unsigned int frameGenSwap(void *display, void *surface) {
  EGLint w = 0;
  EGLint h = 0;
  gGl.QuerySurface(display, surface, EGL_WIDTH, &w);
  gGl.QuerySurface(display, surface, EGL_HEIGHT, &h);
  if (w <= 0 || h <= 0) {
    return gOriginalSwap(display, surface);
  }

  SavedState saved;
  saveState(saved);

  if (!ensureResources(w, h)) {
    restoreState(saved);
    return gOriginalSwap(display, surface);
  }

  const int cur = gIdx;
  const int prev = 1 - cur;

  // Copia o frame atual (que o jogo acabou de desenhar) para a textura.
  gGl.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  gGl.ActiveTexture(GL_TEXTURE0);
  gGl.BindTexture(GL_TEXTURE_2D, gTex[cur]);
  gGl.CopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

  if (gHavePrev.load()) {
    // 1) frame do meio: mistura de anterior e atual
    drawPass(gTex[prev], gTex[cur], 0.5f);
    gOriginalSwap(display, surface);
    gPresents++;
    // 2) redesenha o frame atual (o buffer fica indefinido depois do swap)
    drawPass(gTex[prev], gTex[cur], 1.0f);
  }

  restoreState(saved);
  const unsigned int result = gOriginalSwap(display, surface);
  gPresents++;

  gIdx = prev;
  gHavePrev = true;
  return result;
}

// ---------- arquivo de status ----------
std::string readPackageName() {
  std::FILE *f = std::fopen("/proc/self/cmdline", "r");
  if (!f) {
    return {};
  }
  char buf[256] = {0};
  std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  return std::string(buf);
}

void writeProbe(const std::string &text) {
  const std::string pkg = readPackageName();
  const std::string paths[] = {
      "/storage/emulated/0/Download/framegen_fps.txt",
      "/storage/emulated/0/Android/data/" + pkg + "/files/framegen_fps.txt",
  };
  for (const auto &p : paths) {
    std::FILE *f = std::fopen(p.c_str(), "w");
    if (f) {
      std::fputs(text.c_str(), f);
      std::fclose(f);
      return;
    }
  }
}

unsigned int swapDetour(void *display, void *surface);

class FramegenMod {
public:
  static FramegenMod &instance() {
    static FramegenMod mod;
    return mod;
  }

  FramegenMod() : mSelf(*ll::mod::NativeMod::current()) {}

  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

  bool load() {
    getSelf().getLogger().info("FramegenMod loaded");
    return true;
  }

  bool enable() {
    auto &self = getSelf();

    std::string status = "enable chamado\n";

    gGlReady = loadGlApi();
    status += gGlReady ? "gl: OK\n" : "gl: FALHOU\n";

    void *egl = dlopen("libEGL.so", RTLD_NOW);
    void *target = egl ? dlsym(egl, "eglSwapBuffers") : nullptr;
    if (!target) {
      self.getLogger().error("eglSwapBuffers not found");
      status += "eglSwapBuffers: NAO ENCONTRADA\n";
    } else {
      mHook = pl::memory::HookHandle(
          target, reinterpret_cast<pl::memory::FuncPtr>(&swapDetour),
          reinterpret_cast<pl::memory::FuncPtr *>(&gOriginalSwap));
      gHookInstalled = mHook.installed();
      if (mHook.installed()) {
        self.getLogger().info("Hook on eglSwapBuffers installed");
        status += "hook: OK\n";
      } else {
        self.getLogger().error("Failed to install hook on eglSwapBuffers");
        status += "hook: FALHOU\n";
      }
    }
    writeProbe(status);

    const bool registered =
        pl::modmenu::ModuleBuilder(kModuleId, "Frame Gen 2x (teste)")
            .modId(self.getId())
            .description("Etapa 2b: gera 1 frame no meio (mistura 50/50) "
                         "entre cada frame real.")
            .defaultEnabled(true)
            .onToggle(onModuleToggle)
            .registerModule();

    if (registered) {
      self.getLogger().info("Registered Mod Menu module {}", kModuleId);
    } else {
      self.getLogger().error("Failed to register Mod Menu module {}",
                             kModuleId);
    }
    return registered;
  }

  bool disable() {
    mHook.reset();
    gHookInstalled = false;
    pl::modmenu::unregisterModule(kModuleId);
    getSelf().getLogger().info("Disabled");
    return true;
  }

  bool unload() {
    mHook.reset();
    gHookInstalled = false;
    pl::modmenu::unregisterModule(kModuleId);
    getSelf().getLogger().info("Unloaded");
    return true;
  }

private:
  ll::mod::NativeMod &mSelf;
  pl::memory::HookHandle mHook;

  static void onModuleToggle(std::string_view moduleId, bool enabled) {
    gActive = enabled;
    gHavePrev = false;
    instance().getSelf().getLogger().info("Module {} {}", moduleId,
                                          enabled ? "enabled" : "disabled");
  }
};

unsigned int swapDetour(void *display, void *surface) {
  if (!gOriginalSwap) {
    return 1u;
  }
  if (!gActive.load(std::memory_order_relaxed)) {
    return gOriginalSwap(display, surface);
  }

  gGameFrames++;
  gTotalFrames++;

  unsigned int result;
  if (gGlReady.load()) {
    result = frameGenSwap(display, surface);
  } else {
    result = gOriginalSwap(display, surface);
    gPresents++;
  }

  const std::int64_t now =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const std::int64_t start = gWindowStartNs.load();
  if (start == 0) {
    gWindowStartNs = now;
  } else if (now - start >= 1000000000LL) {
    const double secs = static_cast<double>(now - start) / 1e9;
    const int gameFps =
        static_cast<int>(static_cast<double>(gGameFrames.exchange(0)) / secs +
                         0.5);
    const int presentFps =
        static_cast<int>(static_cast<double>(gPresents.exchange(0)) / secs +
                         0.5);
    gWindowStartNs = now;
    writeProbe(std::string("hook: ") +
               (gHookInstalled.load() ? "OK" : "FALHOU") +
               "\ngl: " + (gGlReady.load() ? "OK" : "FALHOU") +
               "\nshader: " + (gProg ? "OK" : (gProgFailed ? "FALHOU" : "-")) +
               "\ngame_fps: " + std::to_string(gameFps) +
               "\npresent_fps: " + std::to_string(presentFps) +
               "\nframes_total: " + std::to_string(gTotalFrames.load()) + "\n");
  }
  return result;
}

} // namespace

PL_REGISTER_MOD(FramegenMod, FramegenMod::instance())
