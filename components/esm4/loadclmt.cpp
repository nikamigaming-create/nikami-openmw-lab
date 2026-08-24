#include "loadclmt.hpp"

#include <array>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <components/esm/fourcc.hpp>

#include "reader.hpp"

namespace
{
    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sWeatherList = ESM::fourCC("WLST");
    constexpr std::uint32_t sSunTexture = ESM::fourCC("FNAM");
    constexpr std::uint32_t sSunGlareTexture = ESM::fourCC("GNAM");
    constexpr std::uint32_t sNightSkyModel = ESM::fourCC("MODL");
    constexpr std::uint32_t sTiming = ESM::fourCC("TNAM");
    constexpr std::uint32_t sModelBounds = ESM::fourCC("MODB");
    constexpr std::uint32_t sModelTexture = ESM::fourCC("MODT");
    constexpr std::uint32_t sModelAlternateTextures = ESM::fourCC("MODS");
    constexpr std::uint32_t sModelData = ESM::fourCC("MODD");
    constexpr std::size_t sTimingSunriseBegin = 0;
    constexpr std::size_t sTimingSunriseEnd = 1;
    constexpr std::size_t sTimingSunsetBegin = 2;
    constexpr std::size_t sTimingSunsetEnd = 3;
    constexpr std::size_t sTimingVolatility = 4;
    constexpr std::size_t sTimingMoonInfo = 5;

    [[noreturn]] void failSize(
        ESM4::Reader& reader, std::string_view field, std::size_t expected, std::size_t actual)
    {
        std::ostringstream message;
        message << "CLMT " << field << " has " << actual << " bytes; expected " << expected;
        reader.fail(message.str());
    }

    void readFormIdPreservingNull(ESM4::Reader& reader, ESM::FormId& value)
    {
        ESM::FormId32 raw = {};
        reader.get(raw);
        value = ESM::FormId::fromUint32(raw);
        if (raw != ESM::FormId32{})
            reader.adjustFormId(value);
    }
}

void ESM4::Climate::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case sEditorId:
                reader.getZString(mEditorId);
                break;
            case sWeatherList:
            {
                if (subHdr.dataSize % sWeatherTypeSerializedSize != 0)
                    failSize(reader, "WLST", sWeatherTypeSerializedSize, subHdr.dataSize);

                const std::size_t count = subHdr.dataSize / sWeatherTypeSerializedSize;
                mWeatherTypes.reserve(mWeatherTypes.size() + count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    WeatherType weather;
                    readFormIdPreservingNull(reader, weather.mWeather);
                    reader.get(weather.mChance);
                    readFormIdPreservingNull(reader, weather.mGlobal);
                    mWeatherTypes.push_back(weather);
                }
                break;
            }
            case sSunTexture:
                reader.getZString(mSunTexture);
                break;
            case sSunGlareTexture:
                reader.getZString(mSunGlareTexture);
                break;
            case sNightSkyModel:
                reader.getZString(mNightSkyModel);
                break;
            case sTiming:
            {
                if (subHdr.dataSize != sTimingSerializedSize)
                    failSize(reader, "TNAM", sTimingSerializedSize, subHdr.dataSize);

                std::array<std::uint8_t, sTimingSerializedSize> raw{};
                reader.get(raw.data(), raw.size());
                mTiming.mSunriseBegin = raw[sTimingSunriseBegin];
                mTiming.mSunriseEnd = raw[sTimingSunriseEnd];
                mTiming.mSunsetBegin = raw[sTimingSunsetBegin];
                mTiming.mSunsetEnd = raw[sTimingSunsetEnd];
                mTiming.mVolatility = raw[sTimingVolatility];
                mTiming.mMoonInfo = raw[sTimingMoonInfo];
                mHasTiming = true;
                break;
            }
            case sModelBounds:
            case sModelTexture:
            case sModelAlternateTextures:
            case sModelData:
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error("ESM4::CLMT::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
