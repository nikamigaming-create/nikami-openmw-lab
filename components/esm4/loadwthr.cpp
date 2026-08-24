#include "loadwthr.hpp"

#include <algorithm>
#include <stdexcept>

#include <components/esm/fourcc.hpp>

#include "reader.hpp"

namespace
{
    constexpr std::size_t sColorChannelCount = sizeof(ESM4::Weather::Color);
    constexpr std::size_t sFullColorDataSize
        = ESM4::Weather::sTimeCount * ESM4::Weather::sColorTypeCount * sColorChannelCount;
    constexpr std::size_t sLegacyColorDataSize
        = ESM4::Weather::sLegacyTimeCount * ESM4::Weather::sColorTypeCount * sColorChannelCount;
    constexpr std::size_t sSoundDataSize = sizeof(ESM::FormId) + sizeof(std::uint32_t);

    constexpr std::uint32_t sEditorId = ESM::fourCC("EDID");
    constexpr std::uint32_t sSunriseImageSpace = ESM::fourCC("\0IAD");
    constexpr std::uint32_t sDayImageSpace = ESM::fourCC("\1IAD");
    constexpr std::uint32_t sSunsetImageSpace = ESM::fourCC("\2IAD");
    constexpr std::uint32_t sNightImageSpace = ESM::fourCC("\3IAD");
    constexpr std::uint32_t sHighNoonImageSpace = ESM::fourCC("\4IAD");
    constexpr std::uint32_t sMidnightImageSpace = ESM::fourCC("\5IAD");
    constexpr std::uint32_t sLowerCloudTexture = ESM::fourCC("DNAM");
    constexpr std::uint32_t sCloudTexture = ESM::fourCC("CNAM");
    constexpr std::uint32_t sUpperCloudTexture = ESM::fourCC("ANAM");
    constexpr std::uint32_t sSkyTexture = ESM::fourCC("BNAM");
    constexpr std::uint32_t sModel = ESM::fourCC("MODL");
    constexpr std::uint32_t sCloudSpeeds = ESM::fourCC("ONAM");
    constexpr std::uint32_t sCloudColors = ESM::fourCC("PNAM");
    constexpr std::uint32_t sColors = ESM::fourCC("NAM0");
    constexpr std::uint32_t sFogDistance = ESM::fourCC("FNAM");
    constexpr std::uint32_t sData = ESM::fourCC("DATA");
    constexpr std::uint32_t sSound = ESM::fourCC("SNAM");
    constexpr std::uint32_t sMaxCloudLayers = ESM::fourCC("LNAM");
    constexpr std::uint32_t sImageSpace = ESM::fourCC("INAM");
    constexpr std::uint32_t sModelBounds = ESM::fourCC("MODB");
    constexpr std::uint32_t sModelTexture = ESM::fourCC("MODT");
    constexpr std::uint32_t sModelAlternateTextures = ESM::fourCC("MODS");
    constexpr std::uint32_t sModelData = ESM::fourCC("MODD");

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
            case sEditorId:
                reader.getZString(mEditorId);
                break;
            case sSunriseImageSpace:
                reader.getFormId(mImageSpaceModifiers[Time_Sunrise]);
                break;
            case sDayImageSpace:
                reader.getFormId(mImageSpaceModifiers[Time_Day]);
                break;
            case sSunsetImageSpace:
                reader.getFormId(mImageSpaceModifiers[Time_Sunset]);
                break;
            case sNightImageSpace:
                reader.getFormId(mImageSpaceModifiers[Time_Night]);
                break;
            case sHighNoonImageSpace:
                reader.getFormId(mImageSpaceModifiers[Time_HighNoon]);
                break;
            case sMidnightImageSpace:
                reader.getFormId(mImageSpaceModifiers[Time_Midnight]);
                break;
            case sLowerCloudTexture:
                reader.getZString(mCloudTextures[CloudLayer_Lower]);
                break;
            case sCloudTexture:
                reader.getZString(mCloudTextures[CloudLayer_Middle]);
                break;
            case sUpperCloudTexture:
                reader.getZString(mCloudTextures[CloudLayer_Upper]);
                break;
            case sSkyTexture:
                reader.getZString(mCloudTextures[CloudLayer_Sky]);
                break;
            case sModel:
                reader.getZString(mModel);
                break;
            case sCloudSpeeds:
                readFixedOrSkip(reader, subHdr, mCloudSpeeds);
                break;
            case sCloudColors:
                readFixedOrSkip(reader, subHdr, mCloudColors);
                break;
            case sColors:
                if (subHdr.dataSize == sFullColorDataSize)
                    reader.get(mColors);
                else if (subHdr.dataSize == sLegacyColorDataSize)
                {
                    std::array<std::array<Color, sLegacyTimeCount>, sColorTypeCount> legacyColors{};
                    reader.get(legacyColors);
                    for (std::size_t type = 0; type < sColorTypeCount; ++type)
                        std::copy(legacyColors[type].begin(), legacyColors[type].end(), mColors[type].begin());
                }
                else
                    reader.skipSubRecordData();
                break;
            case sFogDistance:
                readFixedOrSkip(reader, subHdr, mFogDistance);
                break;
            case sData:
                if (subHdr.dataSize == sDataSerializedSize)
                    reader.get(&mData, sDataSerializedSize);
                else
                    reader.skipSubRecordData();
                break;
            case sSound:
            {
                if (subHdr.dataSize != sSoundDataSize)
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
            case sMaxCloudLayers:
            case sImageSpace:
            case sModelBounds:
            case sModelTexture:
            case sModelAlternateTextures:
            case sModelData:
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error("ESM4::WTHR::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
