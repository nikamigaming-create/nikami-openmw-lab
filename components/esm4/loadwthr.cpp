#include "loadwthr.hpp"

#include <algorithm>
#include <stdexcept>

#include "reader.hpp"

namespace
{
    template <class T>
    void readFixedOrSkip(ESM4::Reader& reader, const ESM4::SubRecordHeader& header, T& value)
    {
        if (header.dataSize == sizeof(value))
            reader.get(value);
        else
            reader.skipSubRecordData();
    }
}

void ESM4::Weather::load(Reader& reader)
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
            case ESM::fourCC("\0IAD"):
                reader.getFormId(mImageSpaceModifiers[Time_Sunrise]);
                break;
            case ESM::fourCC("\1IAD"):
                reader.getFormId(mImageSpaceModifiers[Time_Day]);
                break;
            case ESM::fourCC("\2IAD"):
                reader.getFormId(mImageSpaceModifiers[Time_Sunset]);
                break;
            case ESM::fourCC("\3IAD"):
                reader.getFormId(mImageSpaceModifiers[Time_Night]);
                break;
            case ESM::fourCC("\4IAD"):
                reader.getFormId(mImageSpaceModifiers[Time_HighNoon]);
                break;
            case ESM::fourCC("\5IAD"):
                reader.getFormId(mImageSpaceModifiers[Time_Midnight]);
                break;
            case ESM::fourCC("DNAM"):
                reader.getZString(mCloudTextures[0]);
                break;
            case ESM::fourCC("CNAM"):
                reader.getZString(mCloudTextures[1]);
                break;
            case ESM::fourCC("ANAM"):
                reader.getZString(mCloudTextures[2]);
                break;
            case ESM::fourCC("BNAM"):
                reader.getZString(mCloudTextures[3]);
                break;
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("ONAM"):
                readFixedOrSkip(reader, subHdr, mCloudSpeeds);
                break;
            case ESM::fourCC("PNAM"):
                readFixedOrSkip(reader, subHdr, mCloudColors);
                break;
            case ESM::fourCC("NAM0"):
                if (subHdr.dataSize == 4 * sTimeCount * sColorTypeCount)
                    reader.get(mColors);
                else if (subHdr.dataSize == 4 * 4 * sColorTypeCount)
                {
                    std::array<std::array<Color, 4>, sColorTypeCount> fo3Colors{};
                    reader.get(fo3Colors);
                    for (std::size_t type = 0; type < sColorTypeCount; ++type)
                        std::copy(fo3Colors[type].begin(), fo3Colors[type].end(), mColors[type].begin());
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("FNAM"):
                readFixedOrSkip(reader, subHdr, mFogDistance);
                break;
            case ESM::fourCC("DATA"):
                if (subHdr.dataSize == 15)
                    reader.get(&mData, 15);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("SNAM"):
            {
                if (subHdr.dataSize != 8)
                {
                    reader.skipSubRecordData();
                    break;
                }
                Sound sound;
                reader.getFormId(sound.sound);
                reader.get(sound.type);
                mSounds.push_back(sound);
                break;
            }
            case ESM::fourCC("LNAM"):
            case ESM::fourCC("INAM"):
            case ESM::fourCC("MODB"):
            case ESM::fourCC("MODT"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODD"):
                reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadwthr"))
                    break;
                throw std::runtime_error("ESM4::WTHR::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
