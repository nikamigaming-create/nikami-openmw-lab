#include "projectilestate.hpp"

#include "esmreader.hpp"
#include "esmwriter.hpp"

namespace ESM
{

    void BaseProjectileState::save(ESMWriter& esm) const
    {
        esm.writeHNRefId("ID__", mId);
        esm.writeHNT("VEC3", mPosition);
        esm.writeHNT("QUAT", mOrientation);
        esm.writeHNT("ACTO", mActorId);
    }

    void BaseProjectileState::load(ESMReader& esm)
    {
        mId = esm.getHNRefId("ID__");
        esm.getHNT("VEC3", mPosition.mValues);
        esm.getHNT("QUAT", mOrientation.mValues);
        esm.getHNT(mActorId, "ACTO");
    }

    void MagicBoltState::save(ESMWriter& esm) const
    {
        BaseProjectileState::save(esm);

        esm.writeHNRefId("SPEL", mSpellId);
        esm.writeHNT("SPED", mSpeed);
        if (mItem.isSet())
            esm.writeFormId(mItem, true, "ITEM");
    }

    void MagicBoltState::load(ESMReader& esm)
    {
        BaseProjectileState::load(esm);

        mSpellId = esm.getHNRefId("SPEL");
        esm.getHNT(mSpeed, "SPED");
        if (esm.peekNextSub("ITEM"))
            mItem = esm.getFormId(true, "ITEM");
        if (esm.isNextSub("SLOT")) // for backwards compatibility
            esm.skipHSub();
    }

    void ProjectileState::save(ESMWriter& esm) const
    {
        BaseProjectileState::save(esm);

        esm.writeHNRefId("BOW_", mBowId);
        esm.writeHNT("VEL_", mVelocity);
        esm.writeHNT("STR_", mAttackStrength);
    }

    void ProjectileState::load(ESMReader& esm)
    {
        BaseProjectileState::load(esm);

        mBowId = esm.getHNRefId("BOW_");
        esm.getHNT("VEL_", mVelocity.mValues);

        mAttackStrength = 1.f;
        esm.getHNOT(mAttackStrength, "STR_");
    }

    void FalloutProjectileState::save(ESMWriter& esm) const
    {
        BaseProjectileState::save(esm);

        esm.writeHNT("VEL_", mVelocity);
        esm.writeHNT("RVEL", mRotationVelocity);
        esm.writeHNT("PREV", mPreviousPosition);
        esm.writeHNT("GRAV", mGravity);
        esm.writeHNT("RANG", mMaximumRange);
        esm.writeHNT("DIST", mDistanceTravelled);
        esm.writeHNRefId("WEAP", mWeapon);
        esm.writeHNT("DAMG", mRawDamage);
        esm.writeHNT("LDAM", mLimbDamageMultiplier);
        if (!mExplosion.empty())
            esm.writeHNRefId("EXPL", mExplosion);
        esm.writeHNT("EDMG", mExplosionDamageMultiplier);
        esm.writeHNT("FLAG", mFlags);
        for (const RefId& effect : mAmmoEffects)
            esm.writeHNRefId("AMEF", effect);

        if ((mFlags & HasVatsAction) != 0)
        {
            esm.writeHNRefId("VTGT", mVats.mTarget);
            esm.writeHNT("VPRT", mVats.mBodyPart);
            esm.writeHNT("VCHN", mVats.mDisplayedHitChance);
            esm.writeHNT("VHPC", mVats.mHealthPercent);
            esm.writeHNT("VAV_", mVats.mActorValue);
            esm.writeHNT("VAPC", mVats.mActionPointCost);
            esm.writeHNT("VHDM", mVats.mHealthDamageMultiplier);
            esm.writeHNT("VLDM", mVats.mLimbDamageMultiplier);
            esm.writeHNString("VBNM", mVats.mBodyPartName);
            esm.writeHNString("VTND", mVats.mTargetNode);
        }
    }

    void FalloutProjectileState::load(ESMReader& esm)
    {
        BaseProjectileState::load(esm);

        esm.getHNT("VEL_", mVelocity.mValues);
        esm.getHNT("RVEL", mRotationVelocity.mValues);
        esm.getHNT("PREV", mPreviousPosition.mValues);
        esm.getHNT(mGravity, "GRAV");
        esm.getHNT(mMaximumRange, "RANG");
        esm.getHNT(mDistanceTravelled, "DIST");
        mWeapon = esm.getHNRefId("WEAP");
        esm.getHNT(mRawDamage, "DAMG");
        esm.getHNT(mLimbDamageMultiplier, "LDAM");
        if (esm.isNextSub("EXPL"))
            mExplosion = esm.getRefId();
        esm.getHNOT(mExplosionDamageMultiplier, "EDMG");
        esm.getHNT(mFlags, "FLAG");
        while (esm.isNextSub("AMEF"))
            mAmmoEffects.push_back(esm.getRefId());

        if ((mFlags & HasVatsAction) != 0)
        {
            mVats.mTarget = esm.getHNRefId("VTGT");
            esm.getHNT(mVats.mBodyPart, "VPRT");
            esm.getHNT(mVats.mDisplayedHitChance, "VCHN");
            esm.getHNT(mVats.mHealthPercent, "VHPC");
            esm.getHNT(mVats.mActorValue, "VAV_");
            esm.getHNT(mVats.mActionPointCost, "VAPC");
            esm.getHNT(mVats.mHealthDamageMultiplier, "VHDM");
            esm.getHNT(mVats.mLimbDamageMultiplier, "VLDM");
            mVats.mBodyPartName = esm.getHNString("VBNM");
            mVats.mTargetNode = esm.getHNString("VTND");
        }
    }

}
