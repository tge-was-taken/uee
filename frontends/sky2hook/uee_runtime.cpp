#include "runtime.h"

#include "uee_hooks.h"

#include "config.h"
#include "game/config.h"
#include "guest.h"
#include "hooks.h"
#include "skygl_hooks.h"

#include "card_file.h"
#include "disc.h"
#include "elf.h"
#include "host.h"
#include "interpreter.h"
#include "iop_translate.h"
#include "kernel.h"
#include "log.h"
#include "memory.h"
#include "movie.h"
#include "movie_decoder.h"
#include "patches.h"
#include "run.h"
#include "ee_translate.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef UEE_TRANSLATED_P3FES
namespace uee {
bool runFunction(Machine& machine, u32 pc);
}  // namespace uee
#endif

#ifdef UEE_IOP_MODULES
namespace uee {
UEE_IOP_MODULE_DECLARATIONS
}  // namespace uee
#endif

namespace {

uee::LoggerChannel log("p3fes");

// What the build translated for this program, or nothing at all, in which case
// the interpreter runs it.
uee::FunctionRunner translatedRunner() {
#ifdef UEE_TRANSLATED_P3FES
    return uee::runFunction;
#else
    return nullptr;
#endif
}

std::span<uee::TranslatedIrx* const> translatedIopModules() {
#ifdef UEE_IOP_MODULES
    static uee::TranslatedIrx* const kModules[] = {UEE_IOP_MODULE_LIST};
    return kModules;
#else
    return {};
#endif
}


// libpad reports the buttons as one active-low halfword, the poll response's
// first data byte in the high half.
u16 toPadMask(core::PadButton bind) {
    switch (bind) {
        case core::PAD_LEFT: return 0x8000;
        case core::PAD_DOWN: return 0x4000;
        case core::PAD_RIGHT: return 0x2000;
        case core::PAD_UP: return 0x1000;
        case core::PAD_START: return 0x0800;
        case core::PAD_R3: return 0x0400;
        case core::PAD_L3: return 0x0200;
        case core::PAD_SELECT: return 0x0100;
        case core::PAD_SQUARE: return 0x0080;
        case core::PAD_CROSS: return 0x0040;
        case core::PAD_CIRCLE: return 0x0020;
        case core::PAD_TRIANGLE: return 0x0010;
        case core::PAD_R1: return 0x0008;
        case core::PAD_L1: return 0x0004;
        case core::PAD_R2: return 0x0002;
        case core::PAD_L2: return 0x0001;
        default: return 0;
    }
}

core::PadButton toPadButton(std::string_view name) {
    static const std::unordered_map<std::string_view, core::PadButton> kNames{
        {"up", core::PAD_UP},         {"down", core::PAD_DOWN},
        {"left", core::PAD_LEFT},     {"right", core::PAD_RIGHT},
        {"cross", core::PAD_CROSS},   {"circle", core::PAD_CIRCLE},
        {"square", core::PAD_SQUARE}, {"triangle", core::PAD_TRIANGLE},
        {"start", core::PAD_START},   {"select", core::PAD_SELECT},
        {"l1", core::PAD_L1},         {"r1", core::PAD_R1},
    };
    const auto found = kNames.find(name);
    if (found == kNames.end()) {
        throw std::runtime_error(std::format("{} is not a pad button", name));
    }
    return found->second;
}

// One step per line: how many polls to hold, then the buttons to hold for them.
// "60 down cross" holds both for a second; a line with no buttons holds nothing.
struct PadStep {
    u64 endsAt = 0;
    u16 mask = 0;
};

std::vector<PadStep> readPadScript(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error(std::format("cannot read pad script {}", path.string()));
    }
    std::vector<PadStep> steps;
    u64 endsAt = 0;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream fields(line.substr(0, line.find('#')));
        u64 polls = 0;
        if (!(fields >> polls)) {
            continue;
        }
        u16 mask = 0;
        for (std::string name; fields >> name;) {
            mask = static_cast<u16>(mask | toPadMask(toPadButton(name)));
        }
        endsAt += polls;
        steps.push_back({endsAt, mask});
    }
    return steps;
}

// The boot ELF is named after the disc serial, which is what the per-version
// address file is named after in the other spelling: SLUS_216.21 is SLUS-21621.
std::string serialOf(const std::filesystem::path& elf) {
    std::string serial;
    for (const char c : elf.filename().string()) {
        if (c == '_') {
            serial.push_back('-');
        } else if (c != '.') {
            serial.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }
    return serial;
}

std::vector<u8> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
}

// Runs the game on the uee interpreter, which HLEs the PS2 kernel and so boots
// the ELF directly. The emulation thread owns the machine; this drives boot and
class UeeRuntime final : public core::Runtime, private uee::PadSource, private uee::StopRequest {
public:
    bool init(int argc, char** argv) override {
        uee::configureLogging();
        std::vector<std::string> positional;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "-surfaceless") == 0) {
                mUseWindow = false;
            } else if (std::strcmp(argv[i], "-advance") == 0) {
                mAdvance = true;
            } else if (std::strcmp(argv[i], "-walk") == 0) {
                mWalk = true;
            } else if (std::strncmp(argv[i], "-script=", 8) == 0) {
                mScript = readPadScript(argv[i] + 8);
            } else if (argv[i][0] != '-') {
                positional.emplace_back(argv[i]);
            }
        }
        if (positional.empty()) {
            log.fatal("usage: p3fes <elf> [disc] [patches.toml] [card.mc2] "
                      "[-surfaceless] [-advance]");
            return false;
        }
        mElf = positional[0];
        mDisc = positional.size() > 1 ? positional[1] : "";
        mPatches = positional.size() > 2 ? positional[2] : "";
        mCard = positional.size() > 3 ? positional[3] : "";
        return true;
    }

    bool windowRequested() const override { return mUseWindow; }

    void start(std::atomic<int>& exitCode) override {
        mThread = std::thread([this, &exitCode] { exitCode.store(runGame()); });
    }

    bool finished() const override { return mStopped.load(std::memory_order_relaxed); }

    void join() override {
        if (mThread.joinable()) {
            mThread.join();
        }
    }

    void shutdown() override {}

    void setPadButton(core::PadButton bind, bool pressed) override {
        const u16 mask = toPadMask(bind);
        if (pressed) {
            mHeld.fetch_or(mask);
        } else {
            mHeld.fetch_and(static_cast<u16>(~mask));
        }
    }

    // The interpreter runs as fast as it can and the guest paces itself off the
    // display fields, so there is no speed to limit.
    void setSpeedLimit(bool) override {}

private:
    // Cross, over and over, which is what walks the boot prompts through with
    // nobody at the pad. It is held for a few polls because the game reads the
    // pad once a field and wants to see an edge. No direction is pressed: a
    // character that walks makes every run a different scene, and a run that
    // is measured has to be the same one twice.
    u16 heldForAdvance() const {
        constexpr u16 kPollsPerStep = 4;
        const u16 step = static_cast<u16>(mPolls / kPollsPerStep % 4);
        return step == 0 ? toPadMask(core::PAD_CROSS) : 0;
    }

    // A direction makes every run a different scene, which is the point when
    // the question is what the renderer draws rather than how fast. Cross still
    // answers the boot prompts, since the save will not load without it.
    u16 heldForWalk() const {
        constexpr u64 kPollsToLoad = 2400;
        if (mPolls < kPollsToLoad) {
            return heldForAdvance();
        }
        constexpr u64 kPollsPerLeg = 240;
        constexpr core::PadButton kLegs[] = {core::PAD_UP, core::PAD_RIGHT, core::PAD_DOWN,
                                             core::PAD_LEFT};
        return toPadMask(kLegs[(mPolls - kPollsToLoad) / kPollsPerLeg % 4]);
    }

    // Steps run in order and the last one holds once the script runs out, so a
    // script that ends on nothing leaves the pad alone.
    u16 heldForScript() {
        while (mStep < mScript.size() && mPolls >= mScript[mStep].endsAt) {
            ++mStep;
        }
        return mStep < mScript.size() ? mScript[mStep].mask : 0;
    }

    u16 buttons() override {
        ++mPolls;
        const u16 driven = !mScript.empty() ? heldForScript()
                                            : (mWalk ? heldForWalk()
                                                     : (mAdvance ? heldForAdvance() : 0));
        const u16 held = mHeld.load(std::memory_order_relaxed) | driven;
        return static_cast<u16>(~held);
    }

    bool isStopRequested() override { return skygl::hooks::closeRequested(); }

    int runGame() {
        const int code = boot();
        mStopped.store(true, std::memory_order_relaxed);
        return code;
    }

    int boot() {
        const std::vector<u8> image = readFile(mElf);
        if (image.empty()) {
            log.fatal("cannot read {}", mElf.string());
            return EXIT_FAILURE;
        }

        uee::GameConfig config;
        if (!mPatches.empty()) {
            try {
                config = uee::loadGameConfig(mPatches.string());
            } catch (const std::exception& error) {
                log.fatal("{}", error.what());
                return EXIT_FAILURE;
            }
        }

        uee::Memory memory = uee::makeMainMemory();
        uee::Interpreter interpreter(memory);
        uee::Kernel kernel(interpreter.ee, memory, config, translatedIopModules());
        interpreter.ee.cpu.pc = uee::loadElf(image, memory).entry;
        uee::applyPatches(memory, config.patches);
        guest::setRam(memory.regionAt(0).data(), guest::RAM_SIZE);

        const std::filesystem::path root = mPatches.parent_path();
        for (const std::string& module : config.preload) {
            const std::filesystem::path path = root / module;
            const std::vector<u8> bytes = readFile(path);
            if (bytes.empty() || !kernel.iop().loadModule(path.stem().string(), bytes)) {
                log.fatal("preloaded module {} did not start", module);
                return EXIT_FAILURE;
            }
        }

        const std::unique_ptr<uee::Disc> disc = uee::openDisc(mDisc.string());
        if (!mDisc.empty() && !disc->isOpen()) {
            log.fatal("cannot open disc {}", mDisc.string());
            return EXIT_FAILURE;
        }
        if (disc->isOpen()) {
            kernel.attachDisc(*disc);
        }
        kernel.attachPad(this);

        std::optional<uee::CardFile> card;
        if (!mCard.empty()) {
            card.emplace(mCard.string());
            if (!card->isOpen()) {
                log.fatal("cannot open memory card image {}", mCard.string());
                return EXIT_FAILURE;
            }
            kernel.attachCard(&*card);
        }

        if (!armRenderer()) {
            return EXIT_FAILURE;
        }
        // The render thread reads guest memory directly, so it has to be joined
        // before the memory declared above it goes away.
        struct StopRenderer {
            bool armed;
            ~StopRenderer() {
                if (armed) {
                    skygl::hooks::close();
                }
            }
        } stopRenderer{app::Config::get().nativeRenderer};

        sky2::MovieDecoder video;
        uee::Movie movie{kernel.mmio().ipu(), config.mpegGetPicture};
        movie.attach(&video);
        // The port's own sceMpegGetPicture only records where the picture was
        // going and lets the call continue, and the renderer draws nothing
        // until it has. Answering the call first would skip it.
        uee::Hook* hooks[] = {&sky2::getHook(), &movie};

        uee::Machine machine{.interpreter = interpreter,
                             .memory = memory,
                             .mmio = kernel.mmio(),
                             .kernel = kernel,
                             .budget = kernel.budget(),
                             .functions = translatedRunner(),
                             .hooks = hooks,
                             .stopRequest = this};
        // No time limit: the run ends when the guest exits or the window closes.
        const uee::Stop stop = uee::run(machine);
        std::fputs(kernel.readOutput().c_str(), stdout);
        if (stop.reason == uee::StopReason::Exited || stop.reason == uee::StopReason::Stopped) {
            log.info("{}", uee::describe(stop));
            return EXIT_SUCCESS;
        }
        log.error("{}", uee::describe(stop));
        kernel.reportThreads();
        return EXIT_FAILURE;
    }

    bool armRenderer() {
        game::loadAddresses(serialOf(mElf));
        if (!app::Config::get().nativeRenderer) {
            return true;
        }
        skygl::hooks::setGameResolution(skygl::GAME_WIDTH, skygl::GAME_HEIGHT);
        if (!skygl::hooks::open()) {
            log.fatal("the renderer could not open its device");
            return false;
        }
        hooks::install(/*hookRender=*/true, /*renderActive=*/true);
        return true;
    }

    std::filesystem::path mElf;
    std::filesystem::path mDisc;
    std::filesystem::path mPatches;
    std::filesystem::path mCard;
    std::thread mThread;
    std::atomic<bool> mStopped{false};
    std::atomic<u16> mHeld{0};
    bool mAdvance = false;
    bool mWalk = false;
    std::vector<PadStep> mScript;
    usize mStep = 0;
    u32 mPolls = 0;
    bool mUseWindow = true;
};

}  // namespace

std::unique_ptr<core::Runtime> core::makeRuntime() {
    return std::make_unique<UeeRuntime>();
}
