#pragma once

#include "types.h"

#ifdef UEE_HOST_FPU
#include <immintrin.h>
#endif

// Present only where UEE_HOST_FPU asked for -mavx2 -mfma.
namespace uee::arch {

#ifdef UEE_HOST_FPU

constexpr bool kHasFloatUnit = true;

inline u32 getFloatControl() { return _mm_getcsr(); }
inline void setFloatControl(u32 bits) { _mm_setcsr(bits); }

constexpr u32 kRoundingBits = 3u << 13;
inline u32 getRounding() { return getFloatControl() & kRoundingBits; }
inline void setRounding(u32 mode) { setFloatControl((getFloatControl() & ~kRoundingBits) | mode); }
inline void setRoundToNearest() { setRounding(0); }

// All four lanes or none: blending one at a time costs more than it saves.
class Lanes {
public:
    Lanes() : mValue(_mm_setzero_si128()) {}
    explicit Lanes(const u32 words[4])
        : mValue(_mm_loadu_si128(reinterpret_cast<const __m128i*>(words))) {}
    explicit Lanes(__m128i value) : mValue(value) {}

    static Lanes broadcast(u32 word) { return Lanes(_mm_set1_epi32(static_cast<int>(word))); }

    __m128i raw() const { return mValue; }
    void store(u32 words[4]) const {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(words), mValue);
    }

    Lanes exponents() const {
        return Lanes(_mm_and_si128(_mm_srli_epi32(mValue, 23), _mm_set1_epi32(0xFF)));
    }

private:
    __m128i mValue;
};

inline bool anyLaneSet(Lanes mask) {
    return _mm_movemask_ps(_mm_castsi128_ps(mask.raw())) != 0;
}

inline Lanes greaterThanSigned(Lanes a, Lanes b) {
    return Lanes(_mm_cmpgt_epi32(a.raw(), b.raw()));
}
inline Lanes equalTo(Lanes a, Lanes b) {
    return Lanes(_mm_cmpeq_epi32(a.raw(), b.raw()));
}
inline Lanes either(Lanes a, Lanes b) {
    return Lanes(_mm_or_si128(a.raw(), b.raw()));
}

inline Lanes maskWith(Lanes a, Lanes mask) {
    return Lanes(_mm_and_si128(a.raw(), mask.raw()));
}
inline Lanes flipBits(Lanes a, Lanes bits) {
    return Lanes(_mm_xor_si128(a.raw(), bits.raw()));
}
inline Lanes subtractEach(Lanes a, Lanes b) {
    return Lanes(_mm_sub_epi32(a.raw(), b.raw()));
}
inline Lanes absoluteEach(Lanes a) {
    return Lanes(_mm_abs_epi32(a.raw()));
}
inline Lanes maxUnsigned(Lanes a, Lanes b) {
    return Lanes(_mm_max_epu32(a.raw(), b.raw()));
}
inline Lanes shiftLeftEach(Lanes a, Lanes counts) {
    return Lanes(_mm_sllv_epi32(a.raw(), counts.raw()));
}
inline Lanes choose(Lanes whenClear, Lanes whenSet, Lanes mask) {
    return Lanes(_mm_blendv_epi8(whenClear.raw(), whenSet.raw(), mask.raw()));
}

inline Lanes multiplyLowEach(Lanes a, Lanes b) {
    return Lanes(_mm_mullo_epi32(a.raw(), b.raw()));
}

inline Lanes addFloat(Lanes a, Lanes b) {
    return Lanes(_mm_castps_si128(
        _mm_add_ps(_mm_castsi128_ps(a.raw()), _mm_castsi128_ps(b.raw()))));
}
inline Lanes multiplyFloat(Lanes a, Lanes b) {
    return Lanes(_mm_castps_si128(
        _mm_mul_ps(_mm_castsi128_ps(a.raw()), _mm_castsi128_ps(b.raw()))));
}
// One rounding, where a multiply and an add apart would take two.
inline Lanes multiplyAddFloat(Lanes a, Lanes b, Lanes addend) {
    return Lanes(_mm_castps_si128(_mm_fmadd_ps(_mm_castsi128_ps(a.raw()),
                                               _mm_castsi128_ps(b.raw()),
                                               _mm_castsi128_ps(addend.raw()))));
}
inline Lanes subtractFromFloat(Lanes a, Lanes b, Lanes minuend) {
    return Lanes(_mm_castps_si128(_mm_fnmadd_ps(_mm_castsi128_ps(a.raw()),
                                                _mm_castsi128_ps(b.raw()),
                                                _mm_castsi128_ps(minuend.raw()))));
}

#else

constexpr bool kHasFloatUnit = false;

inline u32 getFloatControl() { return 0; }
inline void setFloatControl(u32) {}
inline u32 getRounding() { return 0; }
inline void setRounding(u32) {}
inline void setRoundToNearest() {}

struct Lanes {
    Lanes() = default;
    explicit Lanes(const u32*) {}
    static Lanes broadcast(u32) { return Lanes(); }
};

#endif

}  // namespace uee::arch
