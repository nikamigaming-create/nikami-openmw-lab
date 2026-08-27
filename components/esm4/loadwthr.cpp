#include "loadwthr.hpp"

#include <array>
#include <sstream>
#include <string_view>

#include <components/esm/fourcc.hpp>

#include "falloutformat.hpp"
#include "reader.hpp"

namespace
{
    // SNAM stores a compact ESM4 FormID, not the in-memory adjusted FormId pair.
    constexpr std::size_t sSoundDataSize = sizeof(ESM::FormId32) + sizeof(std::uint32_t);
    constexpr std::uint32_t sImageSpaceIndexMask = 0xffu;

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

    [[noreturn]] void failSize(
        ESM4::Reader& reader, std::string_view field, std::string_view expected, std::size_t actual)
    {
        std::ostringstream message;
        message << "WTHR " << field << " has " << actual << " bytes; expected " << expected;
        reader.fail(message.str());
    }

    void requireSize(
        ESM4::Reader& reader, const ESM4::SubRecordHeader& header, std::string_view field, std::size_t expected)
    {
        if (header.dataSize != expected)
            failSize(reader, field, std::to_string(expected), header.dataSize);
    }

    template <std::size_t Rows>
    std::size_t readColors(ESM4::Reader& reader, const ESM4::SubRecordHeader& header, std::string_view field,
        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sTimeCount>, Rows>& output)
    {
        constexpr std::size_t legacySize
            = Rows * ESM4::Weather::sLegacyTimeCount * sizeof(ESM4::Weather::Color);
        constexpr std::size_t currentSize = Rows * ESM4::Weather::sTimeCount * sizeof(ESM4::Weather::Color);

        std::size_t sampleCount = 0;
        if (header.dataSize == legacySize)
            sampleCount = ESM4::Weather::sLegacyTimeCount;
        else if (header.dataSize == currentSize)
            sampleCount = ESM4::Weather::sTimeCount;
        else
            failSize(reader, field, std::to_string(legacySize) + " or " + std::to_string(currentSize),
                header.dataSize);

        for (auto& row : output)
            row.fill(ESM4::Weather::Color{});
        for (auto& row : output)
            reader.get(row.data(), sampleCount * sizeof(ESM4::Weather::Color));
        return sampleCount;
    }

    void readData(ESM4::Reader& reader, ESM4::Weather::Data& output)
    {
        std::array<std::uint8_t, ESM4::Fallout::kWeatherDataSerializedSize> raw{};
        reader.get(raw.data(), raw.size());
        output.windSpeed = raw[ESM4::Fallout::kWeatherDataWindSpeedOffset];
        output.lowerCloudSpeed = raw[ESM4::Fallout::kWeatherDataLowerCloudSpeedOffset];
        output.upperCloudSpeed = raw[ESM4::Fallout::kWeatherDataUpperCloudSpeedOffset];
        output.transitionDelta = raw[ESM4::Fallout::kWeatherDataTransitionDeltaOffset];
        output.sunGlare = raw[ESM4::Fallout::kWeatherDataSunGlareOffset];
        output.sunDamage = raw[ESM4::Fallout::kWeatherDataSunDamageOffset];
        output.precipitationBeginFadeIn = raw[ESM4::Fallout::kWeatherDataPrecipitationBeginFadeInOffset];
        output.precipitationEndFadeOut = raw[ESM4::Fallout::kWeatherDataPrecipitationEndFadeOutOffset];
        output.lightningBeginFadeIn = raw[ESM4::Fallout::kWeatherDataLightningBeginFadeInOffset];
        output.lightningEndFadeOut = raw[ESM4::Fallout::kWeatherDataLightningEndFadeOutOffset];
        output.lightningFrequency = raw[ESM4::Fallout::kWeatherDataLightningFrequencyOffset];
        output.classification = raw[ESM4::Fallout::kWeatherDataClassificationOffset];
        output.lightningColor
            = { raw[ESM4::Fallout::kWeatherDataLightningColorRedOffset],
                raw[ESM4::Fallout::kWeatherDataLightningColorGreenOffset],
                raw[ESM4::Fallout::kWeatherDataLightningColorBlueOffset], 0 };
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
            case sDayImageSpace:
            case sSunsetImageSpace:
            case sNightImageSpace:
            case sHighNoonImageSpace:
            case sMidnightImageSpace:
                requireSize(reader, subHdr, "image-space modifier", sizeof(ESM::FormId32));
                reader.getFormId(mImageSpaceModifiers[subHdr.typeId & sImageSpaceIndexMask]);
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
            case sMaxCloudLayers:
                requireSize(reader, subHdr, "LNAM", sizeof(mMaxCloudLayers));
                reader.get(mMaxCloudLayers);
                mHasMaxCloudLayers = true;
                break;
            case sCloudSpeeds:
                requireSize(reader, subHdr, "ONAM", sizeof(mCloudSpeeds));
                reader.get(mCloudSpeeds);
                mHasCloudSpeeds = true;
                break;
            case sCloudColors:
                mCloudColorSampleCount = readColors(reader, subHdr, "PNAM", mCloudColors);
                mHasCloudColors = true;
                break;
            case sColors:
                mColorSampleCount = readColors(reader, subHdr, "NAM0", mColors);
                mHasColors = true;
                break;
            case sFogDistance:
                requireSize(reader, subHdr, "FNAM", sizeof(mFogDistance));
                reader.get(mFogDistance);
                mHasFogDistance = true;
                break;
            case sImageSpace:
                requireSize(reader, subHdr, "INAM", mUnusedImageSpaceData.size());
                reader.get(mUnusedImageSpaceData.data(), mUnusedImageSpaceData.size());
                mHasUnusedImageSpaceData = true;
                break;
            case sData:
                requireSize(reader, subHdr, "DATA", sDataSerializedSize);
                readData(reader, mData);
                mHasData = true;
                break;
            case sSound:
            {
                requireSize(reader, subHdr, "SNAM", sSoundDataSize);
                Sound sound;
                reader.getFormId(sound.sound);
                reader.get(sound.type);
                mSounds.push_back(sound);
                break;
            }
            case sModelBounds:
            case sModelTexture:
            case sModelAlternateTextures:
            case sModelData:
                reader.skipSubRecordData();
                break;
            default:
                reader.fail("Unknown WTHR subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
