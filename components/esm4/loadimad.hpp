#ifndef OPENMW_COMPONENTS_ESM4_LOADIMAD_H
#define OPENMW_COMPONENTS_ESM4_LOADIMAD_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct ImageSpaceModifier
    {
        static constexpr std::size_t sMultiplyAddChannelCount = 21;

        enum MultiplyAddChannel : std::size_t
        {
            Channel_EyeAdaptSpeed,
            Channel_HdrBlurRadius,
            Channel_SkinDimmer,
            Channel_EmissiveMultiplier,
            Channel_TargetLuminance,
            Channel_UpperLuminanceClamp,
            Channel_BrightScale,
            Channel_BrightClamp,
            Channel_LuminanceRampNoTexture,
            Channel_LuminanceRampMin,
            Channel_LuminanceRampMax,
            Channel_SunlightDimmer,
            Channel_GrassDimmer,
            Channel_TreeDimmer,
            Channel_BloomBlurRadius,
            Channel_BloomAlphaInterior,
            Channel_BloomAlphaExterior,
            Channel_CinematicSaturation,
            Channel_CinematicContrast,
            Channel_CinematicContrastAverageLuminance,
            Channel_CinematicBrightness,
        };

        struct FloatKey
        {
            float time = 0.f;
            float value = 0.f;
        };

        struct ColorKey
        {
            float time = 0.f;
            std::array<float, 4> value{};
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::uint32_t mAdapterFlags = 0;
        float mDuration = 0.f;
        std::vector<std::uint8_t> mData;
        std::array<std::vector<FloatKey>, sMultiplyAddChannelCount> mMultiply;
        std::array<std::vector<FloatKey>, sMultiplyAddChannelCount> mAdd;
        std::vector<FloatKey> mBlurRadius;
        std::vector<FloatKey> mDoubleVisionStrength;
        std::vector<ColorKey> mTint;
        std::vector<ColorKey> mFade;
        std::vector<FloatKey> mRadialBlurStrength;
        std::vector<FloatKey> mRadialBlurRampUp;
        std::vector<FloatKey> mRadialBlurStart;
        std::vector<FloatKey> mRadialBlurRampDown;
        std::vector<FloatKey> mRadialBlurDownStart;
        std::vector<FloatKey> mDepthOfFieldStrength;
        std::vector<FloatKey> mDepthOfFieldDistance;
        std::vector<FloatKey> mDepthOfFieldRange;
        std::vector<FloatKey> mMotionBlurStrength;
        ESM::FormId mIntroSound;
        ESM::FormId mOutroSound;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_IMAD4;
    };

    static_assert(sizeof(ImageSpaceModifier::FloatKey) == 8);
    static_assert(sizeof(ImageSpaceModifier::ColorKey) == 20);
}

#endif
