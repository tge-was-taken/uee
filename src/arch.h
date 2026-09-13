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

// One bit per lane whose mask is set, lane 0 lowest.
inline u32 getSetLanes(Lanes mask) {
    return static_cast<u32>(_mm_movemask_ps(_mm_castsi128_ps(mask.raw())));
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
inline Lanes addEach(Lanes a, Lanes b) {
    return Lanes(_mm_add_epi32(a.raw(), b.raw()));
}
inline Lanes subtractEach(Lanes a, Lanes b) {
    return Lanes(_mm_sub_epi32(a.raw(), b.raw()));
}
inline Lanes absoluteEach(Lanes a) {
    return Lanes(_mm_abs_epi32(a.raw()));
}
template <int count>
inline Lanes shiftLeftBy(Lanes a) {
    return Lanes(_mm_slli_epi32(a.raw(), count));
}
template <int count>
inline Lanes shiftRightBy(Lanes a) {
    return Lanes(_mm_srli_epi32(a.raw(), count));
}
inline Lanes clearBits(Lanes a, Lanes bits) {
    return Lanes(_mm_andnot_si128(bits.raw(), a.raw()));
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

// Both round the way the control register says, which is toward zero here.
inline Lanes convertToFloat(Lanes a) {
    return Lanes(_mm_castps_si128(_mm_cvtepi32_ps(a.raw())));
}
inline Lanes truncateToInt(Lanes a) {
    return Lanes(_mm_cvttps_epi32(_mm_castsi128_ps(a.raw())));
}

inline Lanes addFloat(Lanes a, Lanes b) {
    return Lanes(_mm_castps_si128(
        _mm_add_ps(_mm_castsi128_ps(a.raw()), _mm_castsi128_ps(b.raw()))));
}
inline Lanes multiplyFloat(Lanes a, Lanes b) {
    return Lanes(_mm_castps_si128(
        _mm_mul_ps(_mm_castsi128_ps(a.raw()), _mm_castsi128_ps(b.raw()))));
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
    void store(u32*) const {}
};

#endif

}  // namespace uee::arch
