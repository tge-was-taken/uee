#pragma once

#include "fpu.h"
#include "opcodes.h"
#include "single.h"

namespace uee {

struct Instruction;

class FpuInterpreter {
public:
    explicit FpuInterpreter(Fpu& state) : mState(state) {}

    // false if this backend does not model the op
    bool execute(Op op, Instruction insn);

private:
    u32 store(u32 result, u32 cause);
    u32 arithmetic(single::Rounded result);
    u32 divide(u32 numerator, u32 denominator);
    u32 accumulate(u32 a, u32 b, bool subtract);
    u32 squareRoot(u32 value);
    u32 reciprocalSquareRoot(u32 numerator, u32 radicand);
    void compare(bool set);

    Fpu& mState;
};

}  // namespace uee
