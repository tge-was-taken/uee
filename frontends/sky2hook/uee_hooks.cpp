#include "uee_hooks.h"

#include "ee_context.h"
#include "ee_hooks.h"
#include "guest.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

struct HookPair {
    Hooks::Handler pre = nullptr;
    Hooks::PostHandler post = nullptr;
};

// $sp at entry is $sp again on return, which is how nested calls find their
// own frame.
struct PendingReturn {
    std::uint32_t callee;
    Hooks::PostHandler post;
    std::uint32_t realRa;
    std::uint32_t sp;
};

std::unordered_map<std::uint32_t, HookPair> s_hooks;
// Addresses the port made up rather than found in the game, so nothing was
// translated for them and nothing should have been.
std::vector<std::uint32_t> s_stubs;
std::vector<PendingReturn> s_returnStack;
Hooks::Observer s_observer = nullptr;
std::uint32_t s_returnSentinel = 0;

// The run loop asks about every address it executes, so the answer has to cost
// less than a hash lookup: one bit per guest instruction slot in main RAM.
constexpr std::uint32_t RAM_WORDS = guest::RAM_SIZE / 4;
std::vector<std::uint64_t> s_hookedWords(RAM_WORDS / 64);

void markHooked(std::uint32_t pc) {
    const std::uint32_t word = (pc & guest::RAM_MASK) / 4;
    s_hookedWords[word / 64] |= std::uint64_t(1) << (word % 64);
}

// gpr[] is Reg128[32] and fpr[] is u32[32], which is the stride pair
// Hooks::Context expects.
Hooks::Context makeContext(uee::EeContext& ee, std::uint32_t pc) {
    return Hooks::Context{reinterpret_cast<std::uint32_t*>(ee.cpu.gpr),
                          reinterpret_cast<float*>(ee.fpu.fpr), pc};
}

// A `jr $ra; nop` the guest never runs: the address is registered, so the hook
// dispatch takes it before the instructions do.
void plantStub(std::uint32_t addr) {
    auto* words = static_cast<std::uint32_t*>(guest::host(addr, 8));
    words[0] = 0x03E00008;  // jr $ra
    words[1] = 0x00000000;  // nop
    s_hooks[addr];
    s_stubs.push_back(addr);
    markHooked(addr);
}

class UeeHook final : public uee::Hook {
public:
    // A mirror of a hooked address answers yes and then misses the map, which
    // costs one lookup and no correctness: the hooks are all game addresses.
    bool isHooked(u32 pc) const override {
        const std::uint32_t word = (pc & guest::RAM_MASK) / 4;
        return ((s_hookedWords[word / 64] >> (word % 64)) & 1) != 0;
    }

    bool enter(uee::EeContext& ee) override {
        const std::uint32_t pc = ee.cpu.pc;
        if (s_returnSentinel != 0 && pc == s_returnSentinel) {
            return leaveHookedCall(ee, pc);
        }

        const auto found = s_hooks.find(pc);
        if (found == s_hooks.end()) {
            return false;
        }
        const HookPair& hook = found->second;

        Hooks::Context ctx = makeContext(ee, pc);
        if (s_observer != nullptr) {
            s_observer(ctx);
        }
        if (hook.pre != nullptr && hook.pre(ctx) == Hooks::Result::Exit) {
            ee.cpu.pc = ctx.ra;
            return true;
        }
        if (hook.post != nullptr && s_returnSentinel != 0) {
            s_returnStack.push_back({pc, hook.post, ctx.ra, ctx.sp});
            ctx.ra = s_returnSentinel;
        }
        return false;
    }

private:
    bool leaveHookedCall(uee::EeContext& ee, std::uint32_t pc) {
        Hooks::Context here = makeContext(ee, pc);
        for (std::size_t i = s_returnStack.size(); i-- > 0;) {
            if (s_returnStack[i].sp != here.sp) {
                continue;
            }
            const PendingReturn pending = s_returnStack[i];
            s_returnStack.erase(s_returnStack.begin() + i);
            // Restored before the handler runs, so it reads the entry $ra
            // rather than the sentinel. $v0 already holds the return value.
            here.ra = pending.realRa;
            if (pending.post != nullptr) {
                Hooks::Context ctx = makeContext(ee, pending.callee);
                pending.post(ctx);
            }
            ee.cpu.pc = pending.realRa;
            return true;
        }
        ee.cpu.pc = here.ra;
        return true;
    }
};

UeeHook s_ueeHook;

}  // namespace

uee::Hook& sky2::getHook() {
    return s_ueeHook;
}

void Hooks::Register(u32 pc, Handler pre, PostHandler post) {
    s_hooks[pc] = {pre, post};
    markHooked(pc);
}

void Hooks::Clear() {
    s_hooks.clear();
    s_stubs.clear();
    s_returnStack.clear();
    s_hookedWords.assign(s_hookedWords.size(), 0);
}

bool Hooks::IsHooked(u32 pc) {
    return s_hooks.find(pc) != s_hooks.end();
}

void Hooks::SetReturnSentinel(u32 addr) {
    s_returnSentinel = addr;
    plantStub(addr);
}

void Hooks::SetCallMarker(u32 addr) {
    plantStub(addr);
}

void Hooks::SetObserver(Observer observer) {
    s_observer = observer;
}

// Calling back into the guest needs a nested run loop the hook dispatch has no
// handle on, and nothing in the renderer does it.
Hooks::u32 Hooks::CallEE(u32, std::initializer_list<u32>, std::initializer_list<float>) {
    return 0;
}

int Hooks::Dispatch() {
    return 0;
}

// Hardware watches the port's diagnostics arm through PCSX2 patches; here they
// record nothing.
void Hooks::SetDmaKickSink(DmaKickSink) {}
void Hooks::OnDmaKick(u32, u32, u32, u32) {}
void Hooks::SetGsVSyncSink(GsVSyncSink) {}
void Hooks::OnGsVSync() {}
void Hooks::SetGsDrawSink(GsDrawSink) {}
void Hooks::OnGsDraw(const GsDraw&) {}
void Hooks::SetVu1UploadSink(Vu1UploadSink) {}
void Hooks::OnVu1Upload(u32, const u32*, u32) {}
void Hooks::SetVu1ExecSink(Vu1ExecSink) {}
void Hooks::OnVu1Exec(u32, const u8*, u32) {}
void Hooks::SetVu1JrSink(Vu1JrSink) {}
void Hooks::OnVu1Jr(u32, u32, u32) {}
void Hooks::SetVu1StepSink(Vu1StepSink) {}
bool Hooks::HasVu1StepSink() {
    return false;
}
void Hooks::OnVu1Step(const Vu1Step&) {}
void Hooks::SetVifTagSink(VifTagSink) {}
void Hooks::OnVifTag(u32, u32, u32, u32) {}
void Hooks::SetVifCodeSink(VifCodeSink) {}
void Hooks::OnVifCode(u32, u32) {}

void Hooks::RunOnGsThread(std::function<void()> task) {
    task();
}

char* Hooks::DisasmVu1Upper(u32, u32) {
    return nullptr;
}

char* Hooks::DisasmVu1Lower(u32, u32) {
    return nullptr;
}
