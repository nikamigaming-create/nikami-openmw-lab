#ifndef OPENMW_COMPONENTS_ESM4_LOADIMGS_H
#define OPENMW_COMPONENTS_ESM4_LOADIMGS_H

#include <array>
#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct ImageSpace
    {
        static constexpr std::size_t sTraitCount = 33;

        enum CinematicFlag : std::uint8_t
        {
            Cinematic_Saturation = 1 << 0,
            Cinematic_Contrast = 1 << 1,
            Cinematic_Tint = 1 << 2,
            Cinematic_Brightness = 1 << 3,
        };

        enum Trait : std::size_t
        {
            Trait_EyeAdaptSpeed,
            Trait_HdrBlurRadius,
            Trait_HdrBlurPasses,
            Trait_EmissiveMultiplier,
            Trait_TargetLuminance,
            Trait_UpperLuminanceClamp,
            Trait_BrightScale,
            Trait_BrightClamp,
            Trait_LuminanceRampNoTexture,
            Trait_LuminanceRampMin,
            Trait_LuminanceRampMax,
            Trait_SunlightDimmer,
            Trait_GrassDimmer,
            Trait_TreeDimmer,
            Trait_SkinDimmer,
            Trait_BloomBlurRadius,
            Trait_BloomAlphaInterior,
            Trait_BloomAlphaExterior,
            Trait_GetHitBlurRadius,
            Trait_GetHitBlurDamping,
            Trait_GetHitDamping,
            Trait_NightEyeTintRed,
            Trait_NightEyeTintGreen,
            Trait_NightEyeTintBlue,
            Trait_NightEyeBrightness,
            Trait_CinematicSaturation,
            Trait_CinematicContrastAverageLuminance,
            Trait_CinematicContrast,
            Trait_CinematicBrightness,
            Trait_CinematicTintRed,
            Trait_CinematicTintGreen,
            Trait_CinematicTintBlue,
            Trait_CinematicTintStrength,
        };

        struct DepthOfField
        {
            float strength = 0.f;
            float distance = 0.f;
            float range = 0.f;
            std::uint16_t unused = 0;
            std::uint16_t skyBlurRadius = 0;
            float vignetteRadius = 0.f;
            float vignetteStrength = 0.f;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::array<float, sTraitCount> mTraits{};
        std::uint8_t mCinematicFlags = 0;
        bool mHasCinematicFlags = false;
        // TES5/FO4 split image-space records. Keep the authored values distinct from
        // Fallout 3/New Vegas' monolithic DNAM trait array.
        std::array<float, 9> mHdr{};
        std::array<float, 3> mCinematic{};
        std::array<float, 4> mTint{};
        DepthOfField mDepthOfField;
        std::string mLut;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_IMGS4;
    };

    static_assert(sizeof(ImageSpace::DepthOfField) == 24);
}

#endif
