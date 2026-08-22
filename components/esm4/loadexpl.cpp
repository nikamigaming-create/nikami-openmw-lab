#include "loadexpl.hpp"

#include <stdexcept>

#include "reader.hpp"

namespace
{
    void loadFalloutData(ESM4::Reader& reader, ESM4::Explosion::Data& data, std::uint32_t size, bool isFONV)
    {
        if (!isFONV || size != 52)
        {
            reader.skipSubRecordData();
            return;
        }

        data = {};
        reader.get(data.mForce);
        reader.get(data.mDamage);
        reader.get(data.mRadius);
        reader.getFormId(data.mLight);
        reader.getFormId(data.mSound1);
        reader.get(data.mFlags);
        reader.get(data.mImageSpaceRadius);
        reader.getFormId(data.mImpactDataSet);
        reader.getFormId(data.mSound2);
        reader.get(data.mRadiationLevel);
        reader.get(data.mRadiationDissipationTime);
        reader.get(data.mRadiationRadius);
        reader.get(data.mSoundLevel);
        data.mPresent = true;
    }
}

void ESM4::Explosion::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    const std::uint32_t esmVer = reader.esmVersion();
    const bool isFONV = esmVer == ESM::VER_132 || esmVer == ESM::VER_133 || esmVer == ESM::VER_134;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subRecord = reader.subRecordHeader();
        switch (subRecord.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(mFullName);
                break;
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("EITM"):
                reader.getFormId(mObjectEffect);
                break;
            case ESM::fourCC("MNAM"):
                reader.getFormId(mImageSpaceModifier);
                break;
            case ESM::fourCC("DATA"):
                loadFalloutData(reader, mData, subRecord.dataSize, isFONV);
                break;
            case ESM::fourCC("INAM"):
                reader.getFormId(mPlacedImpactObject);
                break;
            case ESM::fourCC("OBND"):
            case ESM::fourCC("MODT"):
            case ESM::fourCC("MODC"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODF"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error(
                    "ESM4::Explosion::load - Unknown subrecord " + ESM::printName(subRecord.typeId));
        }
    }
}
