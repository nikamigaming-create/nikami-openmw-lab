#include "loadclmt.hpp"

#include <array>
#include <sstream>
#include <string_view>

#include "reader.hpp"

namespace
{
    [[noreturn]] void failSize(
        ESM4::Reader& reader, std::string_view field, std::string_view expected, std::size_t actual)
    {
        std::ostringstream message;
        message << "CLMT " << field << " has " << actual << " bytes; expected " << expected;
        reader.fail(message.str());
    }

    void readFormIdPreservingNull(ESM4::Reader& reader, ESM::FormId& value)
    {
        ESM::FormId32 raw = 0;
        reader.get(raw);
        value = ESM::FormId::fromUint32(raw);
        if (raw != 0)
            reader.adjustFormId(value);
    }
}

namespace ESM4
{
    void Climate::load(Reader& reader)
    {
        mId = reader.getFormIdFromHeader();
        mFlags = reader.hdr().record.flags;

        while (reader.getSubRecordHeader())
        {
            const SubRecordHeader& subHeader = reader.subRecordHeader();
            switch (subHeader.typeId)
            {
                case ESM::fourCC("EDID"):
                    reader.getZString(mEditorId);
                    break;
                case ESM::fourCC("WLST"):
                {
                    constexpr std::size_t entrySize
                        = sizeof(ESM::FormId32) + sizeof(std::int32_t) + sizeof(ESM::FormId32);
                    if (subHeader.dataSize % entrySize != 0)
                        failSize(reader, "WLST", "a multiple of 12", subHeader.dataSize);

                    const std::size_t count = subHeader.dataSize / entrySize;
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
                case ESM::fourCC("FNAM"):
                    reader.getZString(mSunTexture);
                    break;
                case ESM::fourCC("GNAM"):
                    reader.getZString(mSunGlareTexture);
                    break;
                case ESM::fourCC("MODL"):
                    reader.getZString(mNightSkyModel);
                    break;
                case ESM::fourCC("TNAM"):
                {
                    constexpr std::size_t timingSize = 6;
                    if (subHeader.dataSize != timingSize)
                        failSize(reader, "TNAM", "6", subHeader.dataSize);
                    std::array<std::uint8_t, timingSize> raw{};
                    reader.get(raw.data(), raw.size());
                    mTiming.mSunriseBegin = raw[0];
                    mTiming.mSunriseEnd = raw[1];
                    mTiming.mSunsetBegin = raw[2];
                    mTiming.mSunsetEnd = raw[3];
                    mTiming.mVolatility = raw[4];
                    mTiming.mMoonInfo = raw[5];
                    mHasTiming = true;
                    break;
                }
                case ESM::fourCC("MODB"):
                case ESM::fourCC("MODT"):
                case ESM::fourCC("MODS"):
                case ESM::fourCC("MODD"):
                    reader.skipSubRecordData();
                    break;
                default:
                    reader.fail("Unknown CLMT subrecord " + ESM::printName(subHeader.typeId));
            }
        }
    }
}
