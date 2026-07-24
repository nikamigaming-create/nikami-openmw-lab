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

    void applyFalloutCinematicFlags(ESM4::ImageSpace& imageSpace)
    {
        if (!imageSpace.mHasCinematicFlags)
            return;

        // Fallout 3/New Vegas stores values for all four cinematic controls even when their
        // corresponding GECK checkbox is disabled. Disabled controls are shader identities.
        if ((imageSpace.mCinematicFlags & ESM4::ImageSpace::Cinematic_Saturation) == 0)
            imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicSaturation] = 1.f;
        if ((imageSpace.mCinematicFlags & ESM4::ImageSpace::Cinematic_Contrast) == 0)
        {
            imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicContrastAverageLuminance] = 0.f;
            imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicContrast] = 1.f;
        }
        if ((imageSpace.mCinematicFlags & ESM4::ImageSpace::Cinematic_Tint) == 0)
            imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicTintStrength] = 0.f;
        if ((imageSpace.mCinematicFlags & ESM4::ImageSpace::Cinematic_Brightness) == 0)
            imageSpace.mTraits[ESM4::ImageSpace::Trait_CinematicBrightness] = 1.f;
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
                if (raw.size() >= sizeof(mTraits))
                {
                    // Fallout 3/New Vegas store the base image-space traits in one DNAM.
                    std::memcpy(mTraits.data(), raw.data(), sizeof(mTraits));
                    // Form version 13 added the GECK enable flags after four reserved dwords.
                    // Use the size as the compatibility gate because Reader already handed us
                    // the exact DNAM payload and older records legitimately omit this tail.
                    constexpr std::size_t cinematicFlagsOffset = sizeof(mTraits) + 4 * sizeof(std::uint32_t);
                    if (raw.size() > cinematicFlagsOffset)
                    {
                        mCinematicFlags = raw[cinematicFlagsOffset];
                        mHasCinematicFlags = true;
                        applyFalloutCinematicFlags(*this);
                    }
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
