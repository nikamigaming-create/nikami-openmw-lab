#include "loadimad.hpp"

#include <cstring>
#include <stdexcept>

#include "reader.hpp"

namespace
{
    template <class T>
    std::vector<T> readKeys(ESM4::Reader& reader, std::uint32_t byteCount)
    {
        std::vector<std::uint8_t> raw(byteCount);
        if (!raw.empty())
            reader.get(raw.data(), raw.size());
        if (byteCount % sizeof(T) != 0)
            return {};
        std::vector<T> result(byteCount / sizeof(T));
        if (!result.empty())
            std::memcpy(result.data(), raw.data(), raw.size());
        return result;
    }
}

void ESM4::ImageSpaceModifier::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHdr = reader.subRecordHeader();
        const std::uint32_t iadSuffix = ESM::fourCC("\0IAD") & 0xffffff00u;
        if ((subHdr.typeId & 0xffffff00u) == iadSuffix)
        {
            const std::uint8_t channel = static_cast<std::uint8_t>(subHdr.typeId & 0xffu);
            if (channel < sMultiplyAddChannelCount)
                mMultiply[channel] = readKeys<FloatKey>(reader, subHdr.dataSize);
            else if (channel >= 0x40 && channel < 0x40 + sMultiplyAddChannelCount)
                mAdd[channel - 0x40] = readKeys<FloatKey>(reader, subHdr.dataSize);
            else
                reader.skipSubRecordData();
            continue;
        }

        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("DNAM"):
                mData.resize(subHdr.dataSize);
                if (!mData.empty())
                    reader.get(mData.data(), mData.size());
                if (mData.size() >= 8)
                {
                    std::memcpy(&mAdapterFlags, mData.data(), sizeof(mAdapterFlags));
                    std::memcpy(&mDuration, mData.data() + 4, sizeof(mDuration));
                }
                break;
            case ESM::fourCC("BNAM"):
                mBlurRadius = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("VNAM"):
                mDoubleVisionStrength = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("TNAM"):
                mTint = readKeys<ColorKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("NAM3"):
                mFade = readKeys<ColorKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("RNAM"):
                mRadialBlurStrength = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("SNAM"):
                mRadialBlurRampUp = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("UNAM"):
                mRadialBlurStart = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("NAM1"):
                mRadialBlurRampDown = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("NAM2"):
                mRadialBlurDownStart = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("WNAM"):
                mDepthOfFieldStrength = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("XNAM"):
                mDepthOfFieldDistance = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("YNAM"):
                mDepthOfFieldRange = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("NAM4"):
                mMotionBlurStrength = readKeys<FloatKey>(reader, subHdr.dataSize);
                break;
            case ESM::fourCC("RDSD"):
                if (subHdr.dataSize == sizeof(ESM::FormId))
                    reader.getFormId(mIntroSound);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("RDSI"):
                if (subHdr.dataSize == sizeof(ESM::FormId))
                    reader.getFormId(mOutroSound);
                else
                    reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadimad"))
                    break;
                throw std::runtime_error("ESM4::IMAD::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
