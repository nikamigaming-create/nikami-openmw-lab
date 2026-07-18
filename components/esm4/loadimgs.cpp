#include "loadimgs.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "reader.hpp"

namespace
{
    template <class T>
    bool readFixed(ESM4::Reader& reader, const ESM4::SubRecordHeader& header, T& value)
    {
        if (header.dataSize != sizeof(value))
        {
            reader.skipSubRecordData();
            return false;
        }
        reader.get(value);
        return true;
    }

    void mapCreationEngineTraits(ESM4::ImageSpace& imageSpace)
    {
        imageSpace.mTraits[ESM4::ImageSpace::Trait_EyeAdaptSpeed] = imageSpace.mHdr[0];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_BloomBlurRadius] = imageSpace.mHdr[1];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_SunlightDimmer] = imageSpace.mHdr[6];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicSaturation] = imageSpace.mCinematic[0];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness] = imageSpace.mCinematic[1];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicContrast] = imageSpace.mCinematic[2];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicTintStrength] = imageSpace.mTint[0];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicTintRed] = imageSpace.mTint[1];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicTintGreen] = imageSpace.mTint[2];
        imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicTintBlue] = imageSpace.mTint[3];
    }
}

void ESM4::ImageSpace::load(Reader& reader)
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
            case ESM::fourCC("DNAM"):
            {
                std::vector<std::uint8_t> raw(subHdr.dataSize);
                if (!raw.empty())
                    reader.get(raw.data(), raw.size());
                constexpr std::size_t legacyTraitCount = sTraitCount - 1;
                constexpr std::size_t legacyTraitBytes = legacyTraitCount * sizeof(float);
                if (reader.formVersion() < 14 && raw.size() >= legacyTraitBytes)
                {
                    // Older Fallout image spaces omit Skin Dimmer. Migrate their 32 authored traits into the
                    // 33-slot runtime layout instead of treating the first trailing flag word as Tint Strength.
                    constexpr std::size_t traitsBeforeSkin = Trait_SkinDimmer;
                    constexpr std::size_t traitsAfterSkin = sTraitCount - Trait_BloomBlurRadius;
                    std::memcpy(mTraits.data(), raw.data(), traitsBeforeSkin * sizeof(float));
                    mTraits[Trait_SkinDimmer] = 1.f;
                    std::memcpy(mTraits.data() + Trait_BloomBlurRadius, raw.data() + traitsBeforeSkin * sizeof(float),
                        traitsAfterSkin * sizeof(float));
                }
                else if (raw.size() >= sizeof(mTraits))
                {
                    // Fallout 3/New Vegas store the base image-space traits in one DNAM.
                    std::memcpy(mTraits.data(), raw.data(), sizeof(mTraits));
                }
                else
                {
                    // TES5/FO4 use DNAM for depth of field and split HDR/cinematic/tint
                    // into separate records. Never alias these bytes onto Fallout traits.
                    const std::size_t depthBytes = std::min(raw.size(), sizeof(mDepthOfField));
                    if (depthBytes != 0)
                        std::memcpy(&mDepthOfField, raw.data(), depthBytes);
                }
                break;
            }
            case ESM::fourCC("ENAM"):
            {
                std::array<float, 14> legacy{};
                if (readFixed(reader, subHdr, legacy))
                {
                    std::copy_n(legacy.begin(), 7, mHdr.begin());
                    std::copy_n(legacy.begin() + 7, 3, mCinematic.begin());
                    std::copy_n(legacy.begin() + 10, 4, mTint.begin());
                    mapCreationEngineTraits(*this);
                }
                break;
            }
            case ESM::fourCC("HNAM"):
                if (readFixed(reader, subHdr, mHdr))
                    mapCreationEngineTraits(*this);
                break;
            case ESM::fourCC("CNAM"):
                if (readFixed(reader, subHdr, mCinematic))
                    mapCreationEngineTraits(*this);
                break;
            case ESM::fourCC("TNAM"):
                if (readFixed(reader, subHdr, mTint))
                    mapCreationEngineTraits(*this);
                break;
            case ESM::fourCC("TX00"):
                reader.getZString(mLut);
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadimgs"))
                    break;
                throw std::runtime_error("ESM4::IMGS::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
