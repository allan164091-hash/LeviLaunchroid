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
std::atomic<std::uint64_t> gFrameCount{0};
std::atomic<std::uint64_t> gTotalFrames{0};
std::atomic<std::int64_t> gWindowStartNs{0};

// Funcoes do OpenGL/EGL carregadas por dlsym (nao precisa mexer no CMake).
struct GlApi {
  void (*GetIntegerv)(GLenum, GLint *);
  void (*GetFloatv)(GLenum, GLfloat *);
  void (*GetBooleanv)(GLenum, GLboolean *);
  GLboolean (*IsEnabled)(GLenum);
  void (*Enable)(GLenum);
  void (*Disable)(GLenum);
  void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
  void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
  void (*Clear)(GLbitfield);
  void (*ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
  void (*BindFramebuffer)(GLenum, GLuint);
  EGLBoolean (*QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
};

GlApi gGl{};
std::atomic_bool gGlReady{false};

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
  ok &= bindSym(gles, "glGetIntegerv", gGl.GetIntegerv);
  ok &= bindSym(gles, "glGetFloatv", gGl.GetFloatv);
  ok &= bindSym(gles, "glGetBooleanv", gGl.GetBooleanv);
  ok &= bindSym(gles, "glIsEnabled", gGl.IsEnabled);
  ok &= bindSym(gles, "glEnable", gGl.Enable);
  ok &= bindSym(gles, "glDisable", gGl.Disable);
  ok &= bindSym(gles, "glScissor", gGl.Scissor);
  ok &= bindSym(gles, "glClearColor", gGl.ClearColor);
  ok &= bindSym(gles, "glClear", gGl.Clear);
  ok &= bindSym(gles, "glColorMask", gGl.ColorMask);
  ok &= bindSym(gles, "glBindFramebuffer", gGl.BindFramebuffer);
  ok &= bindSym(egl, "eglQuerySurface", gGl.QuerySurface);
  return ok;
}

// Desenha um quadrado magenta no canto inferior esquerdo e restaura o estado.
void drawMarker(void *display, void *surface) {
  if (!gGlReady.load()) {
    return;
  }
  EGLint w = 0;
  EGLint h = 0;
  gGl.QuerySurface(display, surface, EGL_WIDTH, &w);
  gGl.QuerySurface(display, surface, EGL_HEIGHT, &h);
  if (w <= 0 || h <= 0) {
    return;
  }
  const GLint side = (w < h ? w : h) / 12 > 32 ? (w < h ? w : h) / 12 : 32;

  GLint prevDraw = 0;
  GLint prevRead = 0;
  GLint prevScissor[4] = {0, 0, 0, 0};
  GLfloat prevClear[4] = {0, 0, 0, 0};
  GLboolean prevMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  const GLboolean scissorWas = gGl.IsEnabled(GL_SCISSOR_TEST);
  gGl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
  gGl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
  gGl.GetIntegerv(GL_SCISSOR_BOX, prevScissor);
  gGl.GetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
  gGl.GetBooleanv(GL_COLOR_WRITEMASK, prevMask);

  gGl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  gGl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gGl.Enable(GL_SCISSOR_TEST);
  gGl.Scissor(0, 0, side, side);
  gGl.ClearColor(1.0f, 0.0f, 1.0f, 1.0f);
  gGl.Clear(GL_COLOR_BUFFER_BIT);

  gGl.ClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
  gGl.ColorMask(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);
  gGl.Scissor(prevScissor[0], prevScissor[1], prevScissor[2], prevScissor[3]);
  if (!scissorWas) {
    gGl.Disable(GL_SCISSOR_TEST);
  }
  gGl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
  gGl.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
}

unsigned int swapDetour(void *display, void *surface);

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

// Grava o status num arquivo de texto (sobrescreve a cada chamada).
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
        pl::modmenu::ModuleBuilder(kModuleId, "Frame Gen (teste)")
            .modId(self.getId())
            .description("Etapa 2a: desenha um quadrado magenta no canto da "
                         "tela e mede o FPS.")
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
    instance().getSelf().getLogger().info("Module {} {}", moduleId,
                                          enabled ? "enabled" : "disabled");
  }
};

unsigned int swapDetour(void *display, void *surface) {
  if (gActive.load(std::memory_order_relaxed)) {
    drawMarker(display, surface);

    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const std::int64_t start = gWindowStartNs.load();
    if (start == 0) {
      gWindowStartNs = now;
    }
    gFrameCount++;
    gTotalFrames++;
    if (start != 0 && now - start >= 1000000000LL) {
      const std::uint64_t frames = gFrameCount.exchange(0);
      const double fps =
          static_cast<double>(frames) * 1e9 / static_cast<double>(now - start);
      gWindowStartNs = now;
      const int fpsInt = static_cast<int>(fps + 0.5);
      writeProbe(std::string("hook: ") +
                 (gHookInstalled.load() ? "OK" : "FALHOU") +
                 "\ngl: " + (gGlReady.load() ? "OK" : "FALHOU") +
                 "\nfps: " + std::to_string(fpsInt) +
                 "\nframes_total: " + std::to_string(gTotalFrames.load()) +
                 "\n");
    }
  }
  return gOriginalSwap ? gOriginalSwap(display, surface) : 1u;
}

} // namespace

PL_REGISTER_MOD(FramegenMod, FramegenMod::instance())
