#ifndef OPENMW_ESM4_DECALDATA_H
#define OPENMW_ESM4_DECALDATA_H

#include <array>
#include <cstdint>

namespace ESM4
{
    struct DecalData
    {
        float mMinWidth = 0.f;
        float mMaxWidth = 0.f;
        float mMinHeight = 0.f;
        float mMaxHeight = 0.f;
        float mDepth = 0.f;
        float mShininess = 0.f;
        float mParallaxScale = 0.f;
        std::uint8_t mParallaxPasses = 0;
        std::uint8_t mFlags = 0;
        std::array<std::uint8_t, 3> mColor{};
        bool mPresent = false;

        static constexpr std::uint8_t Parallax = 1u << 0;
        static constexpr std::uint8_t AlphaBlending = 1u << 1;
        static constexpr std::uint8_t AlphaTesting = 1u << 2;
    };
}

#endif
