#include <atomic>
#include <chrono>
#include <cstdint>
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
std::atomic<std::uint64_t> gFrameCount{0};
std::atomic<std::int64_t> gWindowStartNs{0};

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

    void *egl = dlopen("libEGL.so", RTLD_NOW);
    void *target = egl ? dlsym(egl, "eglSwapBuffers") : nullptr;
    if (!target) {
      self.getLogger().error("eglSwapBuffers not found");
    } else {
      mHook = pl::memory::HookHandle(
          target, reinterpret_cast<pl::memory::FuncPtr>(&swapDetour),
          reinterpret_cast<pl::memory::FuncPtr *>(&gOriginalSwap));
      if (mHook.installed()) {
        self.getLogger().info("Hook on eglSwapBuffers installed");
      } else {
        self.getLogger().error("Failed to install hook on eglSwapBuffers");
      }
    }

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
    pl::modmenu::unregisterModule(kModuleId);
    getSelf().getLogger().info("Disabled");
    return true;
  }

  bool unload() {
    mHook.reset();
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
    if (start != 0 && now - start >= 1000000000LL) {
      const std::uint64_t frames = gFrameCount.exchange(0);
      const double fps =
          static_cast<double>(frames) * 1e9 / static_cast<double>(now - start);
      gWindowStartNs = now;
      FramegenMod::instance().getSelf().getLogger().info(
          "FPS (eglSwapBuffers): {}", static_cast<int>(fps + 0.5));
    }
  }
  return gOriginalSwap ? gOriginalSwap(display, surface) : 1u;
}

} // namespace

PL_REGISTER_MOD(FramegenMod, FramegenMod::instance())
