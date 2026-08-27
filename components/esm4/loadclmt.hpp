#ifndef OPENMW_COMPONENTS_ESM4_LOADCLMT_H
#define OPENMW_COMPONENTS_ESM4_LOADCLMT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

#include "falloutformat.hpp"

namespace ESM4
{
    class Reader;

    struct Climate
    {
        static constexpr std::size_t sWeatherTypeSerializedSize
            = Fallout::kClimateWeatherTypeSerializedSize;
        static constexpr std::size_t sTimingSerializedSize = Fallout::kClimateTimingSerializedSize;
        static constexpr std::uint8_t sMoonPhaseLengthMask = Fallout::kClimateMoonPhaseLengthMask;
        static constexpr std::uint8_t sSecundaFlag = Fallout::kClimateSecundaFlag;
        static constexpr std::uint8_t sMasserFlag = Fallout::kClimateMasserFlag;

        struct WeatherType
        {
            ESM::FormId mWeather;
            std::int32_t mChance = 0;
            ESM::FormId mGlobal;
        };

        struct Timing
        {
            std::uint8_t mSunriseBegin = 0;
            std::uint8_t mSunriseEnd = 0;
            std::uint8_t mSunsetBegin = 0;
            std::uint8_t mSunsetEnd = 0;
            std::uint8_t mVolatility = 0;
            std::uint8_t mMoonInfo = 0;

            std::uint8_t getMoonPhaseLength() const { return mMoonInfo & sMoonPhaseLengthMask; }
            bool hasSecunda() const { return (mMoonInfo & sSecundaFlag) != 0; }
            bool hasMasser() const { return (mMoonInfo & sMasserFlag) != 0; }
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::vector<WeatherType> mWeatherTypes;
        std::string mSunTexture;
        std::string mSunGlareTexture;
        std::string mNightSkyModel;
        Timing mTiming;
        bool mHasTiming = false;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_CLMT4;
    };

    static_assert(sizeof(Climate::Timing) == Climate::sTimingSerializedSize);
}

#endif // OPENMW_COMPONENTS_ESM4_LOADCLMT_H
