#ifndef OPENMW_COMPONENTS_ESM4_LOADPROJ_H
#define OPENMW_COMPONENTS_ESM4_LOADPROJ_H

#include <array>
#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>
#include <components/esm/path.hpp>

namespace ESM4
{
    class Reader;

    struct Projectile
    {
        enum Flags : std::uint16_t
        {
            Hitscan = 1u << 0,
            Explosion = 1u << 1,
            AlternateTrigger = 1u << 2,
            MuzzleFlash = 1u << 3,
            CanBeDisabled = 1u << 5,
            CanBePickedUp = 1u << 6,
            Supersonic = 1u << 7,
            PinsLimbs = 1u << 8,
            PassesSmallTransparent = 1u << 9,
            Detonates = 1u << 10,
            Rotates = 1u << 11,
        };

        struct Data
        {
            std::uint16_t mFlags = 0;
            std::uint16_t mType = 0;
            float mGravity = 0.f;
            float mSpeed = 0.f;
            float mRange = 0.f;
            ESM::FormId mProjectileLight;
            ESM::FormId mMuzzleFlashLight;
            float mTracerChance = 0.f;
            float mAlternateProximity = 0.f;
            float mAlternateTimer = 0.f;
            ESM::FormId mExplosion;
            ESM::FormId mSound;
            float mMuzzleFlashDuration = 0.f;
            float mFadeDuration = 0.f;
            float mImpactForce = 0.f;
            ESM::FormId mCountdownSound;
            ESM::FormId mDisableSound;
            ESM::FormId mDefaultWeapon;
            std::array<float, 3> mRotation{};
            float mBounciness = 0.f;
            bool mPresent = false;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        ESM::Path mModel;
        ESM::Path mMuzzleFlashModel;
        std::uint32_t mSoundLevel = 0;
        Data mData;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_PROJ4;
    };
}

#endif
