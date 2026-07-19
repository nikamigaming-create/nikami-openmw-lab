#include "loadwthr.hpp"

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
        message << "WTHR " << field << " has " << actual << " bytes; expected " << expected;
        reader.fail(message.str());
    }

    void requireSize(ESM4::Reader& reader, const ESM4::SubRecordHeader& header, std::string_view field,
        std::size_t expected)
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
        std::array<std::uint8_t, 15> raw{};
        reader.get(raw.data(), raw.size());
        output.windSpeed = raw[0];
        output.lowerCloudSpeed = raw[1];
        output.upperCloudSpeed = raw[2];
        output.transitionDelta = raw[3];
        output.sunGlare = raw[4];
        output.sunDamage = raw[5];
        output.precipitationBeginFadeIn = raw[6];
        output.precipitationEndFadeOut = raw[7];
        output.lightningBeginFadeIn = raw[8];
        output.lightningEndFadeOut = raw[9];
        output.lightningFrequency = raw[10];
        output.classification = raw[11];
        output.lightningColor = { raw[12], raw[13], raw[14], 0 };
    }
}

namespace ESM4
{
    void Weather::load(Reader& reader)
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
                case ESM::fourCC("\0IAD"):
                case ESM::fourCC("\1IAD"):
                case ESM::fourCC("\2IAD"):
                case ESM::fourCC("\3IAD"):
                case ESM::fourCC("\4IAD"):
                case ESM::fourCC("\5IAD"):
                {
                    requireSize(reader, subHeader, "image-space modifier", sizeof(ESM::FormId32));
                    const std::size_t index = subHeader.typeId & 0xffu;
                    reader.getFormId(mImageSpaceModifiers[index]);
                    break;
                }
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
                case ESM::fourCC("LNAM"):
                    requireSize(reader, subHeader, "LNAM", sizeof(mMaxCloudLayers));
                    reader.get(mMaxCloudLayers);
                    mHasMaxCloudLayers = true;
                    break;
                case ESM::fourCC("ONAM"):
                    requireSize(reader, subHeader, "ONAM", sizeof(mCloudSpeeds));
                    reader.get(mCloudSpeeds);
                    mHasCloudSpeeds = true;
                    break;
                case ESM::fourCC("PNAM"):
                    mCloudColorSampleCount = readColors(reader, subHeader, "PNAM", mCloudColors);
                    mHasCloudColors = true;
                    break;
                case ESM::fourCC("NAM0"):
                    mColorSampleCount = readColors(reader, subHeader, "NAM0", mColors);
                    mHasColors = true;
                    break;
                case ESM::fourCC("FNAM"):
                    requireSize(reader, subHeader, "FNAM", sizeof(mFogDistance));
                    reader.get(mFogDistance);
                    mHasFogDistance = true;
                    break;
                case ESM::fourCC("INAM"):
                    requireSize(reader, subHeader, "INAM", mUnusedImageSpaceData.size());
                    reader.get(mUnusedImageSpaceData.data(), mUnusedImageSpaceData.size());
                    mHasUnusedImageSpaceData = true;
                    break;
                case ESM::fourCC("DATA"):
                    requireSize(reader, subHeader, "DATA", 15);
                    readData(reader, mData);
                    mHasData = true;
                    break;
                case ESM::fourCC("SNAM"):
                {
                    requireSize(reader, subHeader, "SNAM", 8);
                    Sound sound;
                    reader.getFormId(sound.sound);
                    reader.get(sound.type);
                    mSounds.push_back(sound);
                    break;
                }
                case ESM::fourCC("MODB"):
                case ESM::fourCC("MODT"):
                case ESM::fourCC("MODS"):
                case ESM::fourCC("MODD"):
                    reader.skipSubRecordData();
                    break;
                default:
                    reader.fail("Unknown WTHR subrecord " + ESM::printName(subHeader.typeId));
            }
        }
    }
}
