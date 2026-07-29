#ifndef OPENMW_COMPONENTS_ESM4_LOADWTHR_H
#define OPENMW_COMPONENTS_ESM4_LOADWTHR_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct Weather
    {
        static constexpr std::size_t sTimeCount = 6;
        static constexpr std::size_t sColorTypeCount = 10;
        static constexpr std::size_t sCloudLayerCount = 4;

        enum Time : std::size_t
        {
            Time_Sunrise,
            Time_Day,
            Time_Sunset,
            Time_Night,
            Time_HighNoon,
            Time_Midnight,
        };

        enum ColorType : std::size_t
        {
            Color_SkyUpper,
            Color_Fog,
            Color_Unused2,
            Color_Ambient,
            Color_Sunlight,
            Color_Sun,
            Color_Stars,
            Color_SkyLower,
            Color_Horizon,
            Color_Unused9,
        };

        struct Color
        {
            std::uint8_t r = 0;
            std::uint8_t g = 0;
            std::uint8_t b = 0;
            std::uint8_t unused = 0;
        };

        struct Data
        {
            std::uint8_t windSpeed = 0;
            std::uint8_t lowerCloudSpeed = 0;
            std::uint8_t upperCloudSpeed = 0;
            std::uint8_t transitionDelta = 0;
            std::uint8_t sunGlare = 0;
            std::uint8_t sunDamage = 0;
            std::uint8_t precipitationBeginFadeIn = 0;
            std::uint8_t precipitationEndFadeOut = 0;
            std::uint8_t lightningBeginFadeIn = 0;
            std::uint8_t lightningEndFadeOut = 0;
            std::uint8_t lightningFrequency = 0;
            std::uint8_t classification = 0;
            Color lightningColor;
        };

        struct Sound
        {
            ESM::FormId sound;
            std::uint32_t type = 0;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::array<ESM::FormId, sTimeCount> mImageSpaceModifiers;
        std::array<std::string, sCloudLayerCount> mCloudTextures;
        std::string mModel;
        std::array<std::uint8_t, sCloudLayerCount> mCloudSpeeds{};
        std::array<std::array<Color, sTimeCount>, sCloudLayerCount> mCloudColors{};
        std::array<std::array<float, sTimeCount>, sCloudLayerCount> mCloudAlphas{};
        bool mHasCloudAlphas = false;
        bool mUsesExtendedCloudSpeeds = false;
        std::array<std::array<Color, sTimeCount>, sColorTypeCount> mColors{};
        std::array<float, 32> mUnknownCloudLayerValues{};
        std::array<float, 6> mFogDistance{};
        Data mData;
        std::vector<Sound> mSounds;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_WTHR4;
    };

    static_assert(sizeof(Weather::Color) == 4);
    static_assert(sizeof(Weather::Data) == 16);
}

#endif
