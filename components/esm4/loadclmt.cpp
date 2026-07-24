#include "loadclmt.hpp"

#include <stdexcept>
#include <utility>

#include "reader.hpp"

namespace ESM4
{
    void Climate::load(Reader& reader)
    {
        mId = reader.getFormIdFromHeader();
        mFlags = reader.hdr().record.flags;

        while (reader.getSubRecordHeader())
        {
            const SubRecordHeader& subHdr = reader.subRecordHeader();
            switch (subHdr.typeId)
            {
                case ESM::fourCC("EDID"):
                    reader.getZString(mEditorId);
                    break;
                case ESM::fourCC("WLST"):
                {
                    constexpr std::size_t entrySize = sizeof(ESM::FormId32) + sizeof(std::int32_t)
                        + sizeof(ESM::FormId32);
                    if (subHdr.dataSize % entrySize != 0)
                    {
                        reader.skipSubRecordData();
                        break;
                    }

                    const std::size_t count = subHdr.dataSize / entrySize;
                    mWeatherTypes.reserve(mWeatherTypes.size() + count);
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        WeatherType value;
                        reader.getFormId(value.mWeather);
                        reader.get(value.mChance);
                        reader.getFormId(value.mGlobal);
                        mWeatherTypes.push_back(std::move(value));
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
                    if (subHdr.dataSize == sizeof(mTiming))
                    {
                        reader.get(mTiming);
                        mHasTiming = true;
                    }
                    else
                        reader.skipSubRecordData();
                    break;
                case ESM::fourCC("MODB"):
                case ESM::fourCC("MODT"):
                case ESM::fourCC("MODS"):
                case ESM::fourCC("MODD"):
                    reader.skipSubRecordData();
                    break;
                default:
                    if (reader.skipUnknownStarfieldSubRecordData("loadclmt"))
                        break;
                    throw std::runtime_error(
                        "Unknown ESM4 CLMT (" + mId.toString() + ") subrecord " + ESM::printName(subHdr.typeId));
            }
        }
    }
}
