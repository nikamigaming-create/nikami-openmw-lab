#include "loadproj.hpp"

#include <stdexcept>

#include "reader.hpp"

namespace
{
    void loadFalloutData(ESM4::Reader& reader, ESM4::Projectile::Data& data, std::uint32_t size)
    {
        if (size != 68 && size != 84)
        {
            reader.skipSubRecordData();
            return;
        }

        data = {};
        reader.get(data.mFlags);
        reader.get(data.mType);
        reader.get(data.mGravity);
        reader.get(data.mSpeed);
        reader.get(data.mRange);
        reader.getFormId(data.mProjectileLight);
        reader.getFormId(data.mMuzzleFlashLight);
        reader.get(data.mTracerChance);
        reader.get(data.mAlternateProximity);
        reader.get(data.mAlternateTimer);
        reader.getFormId(data.mExplosion);
        reader.getFormId(data.mSound);
        reader.get(data.mMuzzleFlashDuration);
        reader.get(data.mFadeDuration);
        reader.get(data.mImpactForce);
        reader.getFormId(data.mCountdownSound);
        reader.getFormId(data.mDisableSound);
        reader.getFormId(data.mDefaultWeapon);
        if (size == 84)
        {
            for (float& value : data.mRotation)
                reader.get(value);
            reader.get(data.mBounciness);
        }
        data.mPresent = true;
    }
}

void ESM4::Projectile::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

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
            case ESM::fourCC("DATA"):
                loadFalloutData(reader, mData, subRecord.dataSize);
                break;
            case ESM::fourCC("NAM1"):
                reader.getZString(mMuzzleFlashModel);
                break;
            case ESM::fourCC("VNAM"):
                reader.get(mSoundLevel);
                break;
            case ESM::fourCC("OBND"):
            case ESM::fourCC("MODT"):
            case ESM::fourCC("MODC"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODF"):
            case ESM::fourCC("NAM2"):
            case ESM::fourCC("DAMC"):
            case ESM::fourCC("DEST"):
            case ESM::fourCC("DMDC"):
            case ESM::fourCC("DMDL"):
            case ESM::fourCC("DMDT"):
            case ESM::fourCC("DMDS"):
            case ESM::fourCC("DSTA"):
            case ESM::fourCC("DSTD"):
            case ESM::fourCC("DSTF"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error(
                    "ESM4::Projectile::load - Unknown subrecord " + ESM::printName(subRecord.typeId));
        }
    }
}
