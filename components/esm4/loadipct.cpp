#include "loadipct.hpp"

#include <stdexcept>

#include "reader.hpp"

namespace
{
    void loadDecalData(ESM4::Reader& reader, ESM4::DecalData& decal)
    {
        if (reader.subRecordHeader().dataSize != 36)
            throw std::runtime_error("ESM4::ImpactData::load - unsupported Fallout New Vegas DODT layout");

        reader.get(decal.mMinWidth);
        reader.get(decal.mMaxWidth);
        reader.get(decal.mMinHeight);
        reader.get(decal.mMaxHeight);
        reader.get(decal.mDepth);
        reader.get(decal.mShininess);
        reader.get(decal.mParallaxScale);
        reader.get(decal.mParallaxPasses);
        reader.get(decal.mFlags);
        std::uint16_t unused = 0;
        reader.get(unused);
        for (std::uint8_t& channel : decal.mColor)
            reader.get(channel);
        std::uint8_t unusedColorByte = 0;
        reader.get(unusedColorByte);
        decal.mPresent = true;
    }
}

void ESM4::ImpactData::load(Reader& reader)
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
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("DNAM"):
                reader.getFormId(mTextureSet);
                break;
            case ESM::fourCC("SNAM"):
                reader.getFormId(mSound);
                break;
            case ESM::fourCC("DATA"):
                if (subHeader.dataSize != 24)
                    throw std::runtime_error(
                        "ESM4::ImpactData::load - unsupported Fallout New Vegas DATA layout");
                reader.get(mData.mEffectDuration);
                reader.get(mData.mOrientation);
                reader.get(mData.mAngleThreshold);
                reader.get(mData.mPlacementRadius);
                reader.get(mData.mSoundLevel);
                reader.get(mData.mFlags);
                mData.mPresent = true;
                break;
            case ESM::fourCC("DODT"):
                loadDecalData(reader, mDecal);
                break;
            case ESM::fourCC("MODT"):
            case ESM::fourCC("NAM1"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error(
                    "ESM4::ImpactData::load - unknown Fallout New Vegas subrecord "
                    + ESM::printName(subHeader.typeId));
        }
    }
}
