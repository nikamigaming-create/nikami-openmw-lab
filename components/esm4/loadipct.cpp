#include "loadipct.hpp"

#include <stdexcept>

#include "falloutformat.hpp"
#include "reader.hpp"

namespace
{
    void loadFalloutData(ESM4::Reader& reader, ESM4::ImpactData::Data& data, std::uint32_t size, bool isFONV)
    {
        if (!isFONV || size != ESM4::Fallout::kImpactDataBytes)
        {
            reader.skipSubRecordData();
            return;
        }

        data = {};
        reader.get(data.mEffectDuration);
        reader.get(data.mOrientation);
        reader.get(data.mAngleThreshold);
        reader.get(data.mPlacementRadius);
        reader.get(data.mSoundLevel);
        reader.get(data.mFlags);
        data.mPresent = true;
    }
}

void ESM4::ImpactData::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    const std::uint32_t esmVer = reader.esmVersion();
    const bool isFONV = ESM4::Fallout::isNewVegasVersion(esmVer);

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subRecord = reader.subRecordHeader();
        switch (subRecord.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("DNAM"):
                if (isFONV)
                    reader.getFormId(mTextureSet);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("SNAM"):
                if (isFONV)
                    reader.getFormId(mSound1);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("NAM1"):
                if (isFONV)
                    reader.getFormId(mSound2);
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("DATA"):
                loadFalloutData(reader, mData, subRecord.dataSize, isFONV);
                break;
            case ESM::fourCC("DODT"):
            case ESM::fourCC("OBND"):
            case ESM::fourCC("MODT"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error(
                    "ESM4::ImpactData::load - Unknown subrecord " + ESM::printName(subRecord.typeId));
        }
    }
}
