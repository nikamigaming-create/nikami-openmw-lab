#ifndef OPENMW_COMPONENTS_ESM4_LOADCLMT_H
#define OPENMW_COMPONENTS_ESM4_LOADCLMT_H

#include <cstdint>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct Climate
    {
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

            std::uint8_t getMoonPhaseLength() const { return mMoonInfo & 0x3f; }
            bool hasSecunda() const { return (mMoonInfo & 0x40) != 0; }
            bool hasMasser() const { return (mMoonInfo & 0x80) != 0; }
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::vector<WeatherType> mWeatherTypes;
        std::string mSunTexture;
        std::string mSunGlareTexture;
        std::string mNightSkyModel;
        Timing mTiming;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_CLMT4;
    };

    static_assert(sizeof(Climate::Timing) == 6);
}

#endif
