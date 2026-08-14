#ifndef ESM4_PROJECTILE_H
#define ESM4_PROJECTILE_H

#include <cstdint>
#include <span>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct Projectile
    {
        enum Type : std::uint16_t
        {
            Missile = 1,
            Lobber = 2,
            Beam = 4,
            Flame = 8,
        };

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
            std::uint16_t flags = 0;
            std::uint16_t type = 0;
            float gravity = 0.f;
            float speed = 0.f;
            float range = 0.f;
            ESM::FormId projectileLight;
            ESM::FormId muzzleFlashLight;
            float tracerChance = 0.f;
            float alternateProximity = 0.f;
            float alternateTimer = 0.f;
            ESM::FormId explosion;
            ESM::FormId sound;
            float muzzleFlashDuration = 0.f;
            float fadeDuration = 0.f;
            float impactForce = 0.f;
            ESM::FormId countdownSound;
            ESM::FormId disableSound;
            ESM::FormId defaultWeapon;
            float rotationX = 0.f;
            float rotationY = 0.f;
            float rotationZ = 0.f;
            float bounciness = 0.f;
            bool present = false;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        std::string mModel;
        std::string mMuzzleFlashModel;
        Data mData;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_PROJ4;
    };

    [[nodiscard]] bool loadFalloutProjectileData(std::span<const std::uint8_t> bytes, Projectile::Data& data);
}

#endif
