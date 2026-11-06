#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <dlfcn.h>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>

namespace {

constexpr const char *kModuleId = "framegen_mod.main";

using SwapBuffersFn = unsigned int (*)(void *, void *);

SwapBuffersFn gOriginalSwap = nullptr;
std::atomic_bool gCounting{true};
std::atomic_bool gHookInstalled{false};
std::atomic<std::uint64_t> gFrameCount{0};
std::atomic<std::uint64_t> gTotalFrames{0};
std::atomic<std::int64_t> gWindowStartNs{0};

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
            .description("Etapa 1: mede o FPS do jogo e escreve no log.")
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
    gCounting = enabled;
    instance().getSelf().getLogger().info("Module {} {}", moduleId,
                                          enabled ? "enabled" : "disabled");
  }
};

unsigned int swapDetour(void *display, void *surface) {
  if (gCounting.load(std::memory_order_relaxed)) {
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
      FramegenMod::instance().getSelf().getLogger().info(
          "FPS (eglSwapBuffers): {}", fpsInt);
      writeProbe(std::string("hook: ") +
                 (gHookInstalled.load() ? "OK" : "FALHOU") +
                 "\nfps: " + std::to_string(fpsInt) +
                 "\nframes_total: " + std::to_string(gTotalFrames.load()) +
                 "\n");
    }
  }
  return gOriginalSwap ? gOriginalSwap(display, surface) : 1u;
}

} // namespace

PL_REGISTER_MOD(FramegenMod, FramegenMod::instance())
