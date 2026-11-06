#include <string_view>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

namespace {

constexpr const char *kModuleId = "framegen_mod.main";

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
    const bool registered =
        pl::modmenu::ModuleBuilder(kModuleId, "Frame Gen (teste)")
            .modId(self.getId())
            .description("Base do mod de frame generation. Por enquanto so "
                         "registra o modulo e escreve no log.")
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
    pl::modmenu::unregisterModule(kModuleId);
    getSelf().getLogger().info("Disabled");
    return true;
  }

  bool unload() {
    pl::modmenu::unregisterModule(kModuleId);
    getSelf().getLogger().info("Unloaded");
    return true;
  }

private:
  ll::mod::NativeMod &mSelf;

  static void onModuleToggle(std::string_view moduleId, bool enabled) {
    instance().getSelf().getLogger().info("Module {} {}", moduleId,
                                          enabled ? "enabled" : "disabled");
  }
};

} // namespace

PL_REGISTER_MOD(FramegenMod, FramegenMod::instance())
