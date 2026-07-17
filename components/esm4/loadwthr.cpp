#include "loadwthr.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "reader.hpp"

namespace
{
    bool getExtendedCloudTextureLayer(std::uint32_t typeId, std::size_t& layer)
    {
        // TES5 and later encode their 32 cloud texture layers as 00TX through O0TX.
        // The first byte advances continuously from ASCII '0' to 'O', including punctuation.
        constexpr std::uint32_t suffixMask = 0xffffff00u;
        constexpr std::uint32_t suffix = ESM::fourCC("00TX") & suffixMask;
        if ((typeId & suffixMask) != suffix)
            return false;

        const std::uint32_t first = typeId & 0xffu;
        if (first < static_cast<std::uint32_t>('0') || first > static_cast<std::uint32_t>('O'))
            return false;

        layer = first - static_cast<std::uint32_t>('0');
        return true;
    }

    template <class T>
    void readFixedOrSkip(ESM4::Reader& reader, const ESM4::SubRecordHeader& header, T& value)
    {
        if (header.dataSize == sizeof(value))
            reader.get(value);
        else
            reader.skipSubRecordData();
    }

    template <class T>
    std::vector<T> readPodArray(ESM4::Reader& reader, const ESM4::SubRecordHeader& header)
    {
        if (header.dataSize % sizeof(T) != 0)
        {
            reader.skipSubRecordData();
            return {};
        }

        std::vector<T> values(header.dataSize / sizeof(T));
        if (!values.empty())
            reader.get(values.data(), header.dataSize);
        return values;
    }

    std::size_t getWeatherSampleCount(
        std::size_t valueCount, std::size_t minimumRows, std::size_t maximumRows, bool preferEightSamples)
    {
        const std::array<std::size_t, 3> sampleCounts
            = preferEightSamples ? std::array<std::size_t, 3>{ 8, 6, 4 }
                                 : std::array<std::size_t, 3>{ 6, 4, 8 };
        for (const std::size_t sampleCount : sampleCounts)
        {
            if (valueCount % sampleCount != 0)
                continue;
            const std::size_t rows = valueCount / sampleCount;
            if (rows >= minimumRows && rows <= maximumRows)
                return sampleCount;
        }
        return 0;
    }

    void readCloudColors(ESM4::Reader& reader, const ESM4::SubRecordHeader& header,
        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sTimeCount>,
            ESM4::Weather::sCloudLayerCount>& output)
    {
        const std::vector<ESM4::Weather::Color> values = readPodArray<ESM4::Weather::Color>(reader, header);
        const std::size_t samples = getWeatherSampleCount(
            values.size(), ESM4::Weather::sCloudLayerCount, 32, reader.formVersion() >= 111);
        if (samples == 0)
            return;

        const std::size_t layers = values.size() / samples;
        for (std::size_t layer = 0; layer < std::min(layers, output.size()); ++layer)
        {
            const std::size_t copiedSamples = std::min(samples, output[layer].size());
            std::copy_n(values.begin() + layer * samples, copiedSamples, output[layer].begin());
        }
    }

    void readCloudAlphas(ESM4::Reader& reader, const ESM4::SubRecordHeader& header, ESM4::Weather& weather)
    {
        const std::vector<float> values = readPodArray<float>(reader, header);
        const std::size_t samples = getWeatherSampleCount(
            values.size(), ESM4::Weather::sCloudLayerCount, 32, reader.formVersion() >= 111);
        if (samples == 0)
            return;

        const std::size_t layers = values.size() / samples;
        for (std::size_t layer = 0; layer < std::min(layers, weather.mCloudAlphas.size()); ++layer)
        {
            const std::size_t copiedSamples = std::min(samples, weather.mCloudAlphas[layer].size());
            std::copy_n(values.begin() + layer * samples, copiedSamples, weather.mCloudAlphas[layer].begin());
        }
        weather.mHasCloudAlphas = true;
    }

    void readWeatherColors(ESM4::Reader& reader, const ESM4::SubRecordHeader& header,
        std::array<std::array<ESM4::Weather::Color, ESM4::Weather::sTimeCount>,
            ESM4::Weather::sColorTypeCount>& output)
    {
        const std::vector<ESM4::Weather::Color> values = readPodArray<ESM4::Weather::Color>(reader, header);
        const std::size_t samples = getWeatherSampleCount(
            values.size(), ESM4::Weather::sColorTypeCount, 32, reader.formVersion() >= 111);
        if (samples == 0)
            return;

        const std::size_t colorTypes = values.size() / samples;
        for (std::size_t type = 0; type < std::min(colorTypes, output.size()); ++type)
        {
            const std::size_t copiedSamples = std::min(samples, output[type].size());
            std::copy_n(values.begin() + type * samples, copiedSamples, output[type].begin());
        }
    }

    bool readFogDistances(ESM4::Reader& reader, const ESM4::SubRecordHeader& header,
        std::array<float, 6>& output)
    {
        const std::vector<float> values = readPodArray<float>(reader, header);
        if (values.size() < output.size())
            return false;
        std::copy_n(values.begin(), output.size(), output.begin());
        return true;
    }

    void readWeatherData(ESM4::Reader& reader, const ESM4::SubRecordHeader& header, ESM4::Weather::Data& output)
    {
        const std::vector<std::uint8_t> values = readPodArray<std::uint8_t>(reader, header);
        if (values.size() < 15)
            return;

        output.windSpeed = values[0];
        output.lowerCloudSpeed = values[1];
        output.upperCloudSpeed = values[2];
        output.transitionDelta = values[3];
        output.sunGlare = values[4];
        output.sunDamage = values[5];
        output.precipitationBeginFadeIn = values[6];
        output.precipitationEndFadeOut = values[7];
        output.lightningBeginFadeIn = values[8];
        output.lightningEndFadeOut = values[9];
        output.lightningFrequency = values[10];
        output.classification = values[11];
        output.lightningColor = { values[12], values[13], values[14], 0 };
    }
}

void ESM4::Weather::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHdr = reader.subRecordHeader();

        std::size_t extendedCloudLayer = 0;
        if (getExtendedCloudTextureLayer(subHdr.typeId, extendedCloudLayer))
        {
            // The current flat renderer exposes four cloud layers. Preserve those layers and
            // consume the remaining documented TES5+/FO4 fields without aborting the world load.
            if (extendedCloudLayer < mCloudTextures.size())
                reader.getZString(mCloudTextures[extendedCloudLayer]);
            else
                reader.skipSubRecordData();
            continue;
        }

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
                readCloudColors(reader, subHdr, mCloudColors);
                break;
            case ESM::fourCC("NAM0"):
                readWeatherColors(reader, subHdr, mColors);
                break;
            case ESM::fourCC("NAM4"):
                readFixedOrSkip(reader, subHdr, mUnknownCloudLayerValues);
                break;
            case ESM::fourCC("FNAM"):
                if (readFogDistances(reader, subHdr, mFogDistance))
                    mHasFogDistance = true;
                break;
            case ESM::fourCC("DATA"):
                readWeatherData(reader, subHdr, mData);
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
            case ESM::fourCC("HNAM"): // Oblivion HDR lighting parameters; not consumed by the renderer yet.
            case ESM::fourCC("MNAM"): // TES5+ precipitation type.
            case ESM::fourCC("NNAM"): // TES5+ visual effect.
            case ESM::fourCC("QNAM"): // TES5+ cloud X speeds.
            case ESM::fourCC("NAM1"): // TES5+ disabled cloud-layer mask.
            case ESM::fourCC("TNAM"): // TES5+ sky statics.
            case ESM::fourCC("DALC"): // TES5+ directional ambient lighting.
            case ESM::fourCC("NAM2"): // TES5 sun glare colors.
            case ESM::fourCC("NAM3"): // TES5 moon glare colors.
            case ESM::fourCC("GNAM"): // SSE/VR sun-glare lens flare.
            case ESM::fourCC("WGDR"): // FO4 god-ray records.
            case ESM::fourCC("UNAM"): // FO4 weather activation/lightning spells.
            case ESM::fourCC("VNAM"): // FO4 volatility multiplier.
            case ESM::fourCC("WNAM"): // FO4 visibility multiplier.
            case ESM::fourCC("MODB"):
            case ESM::fourCC("MODT"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODD"):
                reader.skipSubRecordData();
                break;
            case ESM::fourCC("RNAM"): // TES5+ cloud Y speeds.
            {
                const std::vector<std::uint8_t> speeds = readPodArray<std::uint8_t>(reader, subHdr);
                std::copy_n(speeds.begin(), std::min(speeds.size(), mCloudSpeeds.size()), mCloudSpeeds.begin());
                mUsesExtendedCloudSpeeds = !speeds.empty();
                break;
            }
            case ESM::fourCC("JNAM"): // TES5+ cloud alphas.
                readCloudAlphas(reader, subHdr, *this);
                break;
            case ESM::fourCC("IMSP"): // TES5+ image spaces.
            {
                if (subHdr.dataSize % sizeof(ESM::FormId32) != 0)
                {
                    reader.skipSubRecordData();
                    break;
                }
                const std::size_t count = subHdr.dataSize / sizeof(ESM::FormId32);
                for (std::size_t index = 0; index < count; ++index)
                {
                    ESM::FormId value;
                    reader.getFormId(value);
                    if (index < 4)
                        mImageSpaceModifiers[index] = value;
                }
                break;
            }
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadwthr"))
                    break;
                throw std::runtime_error("ESM4::WTHR::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
