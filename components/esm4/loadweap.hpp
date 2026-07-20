/*
  Copyright (C) 2016, 2018-2020 cc9cii

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  cc9cii cc9c@iinet.net.au

  Much of the information on the data structures are based on the information
  from Tes4Mod:Mod_File_Format and Tes5Mod:File_Formats but also refined by
  trial & error.  See http://en.uesp.net/wiki for details.

*/
#ifndef ESM4_WEAP_H
#define ESM4_WEAP_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;
    class Writer;

    struct Weapon
    {
        struct Data
        {
            enum WeaponFlags1 : std::uint8_t
            {
                Automatic = 0x02,
            };

            // type
            // 0 = Blade One Hand
            // 1 = Blade Two Hand
            // 2 = Blunt One Hand
            // 3 = Blunt Two Hand
            // 4 = Staff
            // 5 = Bow
            std::uint32_t type;
            float speed;
            float reach;
            std::uint32_t flags;
            std::uint32_t value; // gold
            std::uint32_t health;
            float weight;
            std::uint16_t damage;
            std::uint8_t clipSize; // FO3/FONV only

            // FO3/FONV WEAP.DNAM animation selectors. These choose the authored animation family and the
            // optional HandGrip overlay; they are not part of the TES4 DATA subrecord above.
            std::uint8_t animationType;
            float animationMultiplier;
            std::uint8_t weaponFlags1;
            std::uint8_t handGrip;
            std::uint8_t ammoUse;
            std::uint8_t reloadAnim;

            // FO3/FNV WEAP.DNAM ballistic contract. These fields are stored directly after the animation
            // selectors above and are consumed by the retail firing path. Keep the serialized values intact;
            // gameplay code must not invent a range, projectile count, or fire cadence when the contract is absent.
            float minSpread;
            float spread;
            float sightFov;
            ESM::FormId projectile;
            std::uint8_t baseVatsChance;
            std::uint8_t attackAnim;
            std::uint8_t numProjectiles;
            std::uint8_t embedWeaponActorValue;
            float minRange;
            float maxRange;
            std::uint32_t onHit;
            std::uint32_t flags2;
            float animAttackMult;
            float fireRate;
            float animShotsPerSec;
            float semiAutoFireDelayMin;
            float semiAutoFireDelayMax;
            bool hasBallistics;

            Data()
                : type(0)
                , speed(0.f)
                , reach(0.f)
                , flags(0)
                , value(0)
                , health(0)
                , weight(0.f)
                , damage(0)
                , clipSize(0)
                , animationType(0xff)
                , animationMultiplier(0.f)
                , weaponFlags1(0)
                , handGrip(0xff)
                , ammoUse(0)
                , reloadAnim(0)
                , minSpread(0.f)
                , spread(0.f)
                , sightFov(0.f)
                , baseVatsChance(0)
                , attackAnim(0)
                , numProjectiles(0)
                , embedWeaponActorValue(0)
                , minRange(0.f)
                , maxRange(0.f)
                , onHit(0)
                , flags2(0)
                , animAttackMult(0.f)
                , fireRate(0.f)
                , animShotsPerSec(0.f)
                , semiAutoFireDelayMin(0.f)
                , semiAutoFireDelayMax(0.f)
                , hasBallistics(false)
            {
            }

            [[nodiscard]] bool isAutomatic() const noexcept { return (weaponFlags1 & Automatic) != 0; }
        };

        struct SoundRef
        {
            std::uint32_t mType = 0;
            ESM::FormId mSound;
        };

        ESM::FormId mId; // from the header
        std::uint32_t mFlags; // from the header, see enum type RecordFlag for details

        std::string mEditorId;
        std::string mFullName;
        std::string mModel;
        // FO3/FNV WEAP.MOD4 is the camera-space weapon mesh used with the
        // _1stperson skeleton.  It is distinct from MODL, which remains the
        // third-person/world actor model.
        std::string mFirstPersonModel;
        std::string mText;
        std::string mIcon;
        std::string mMiniIcon;

        // FONV specific
        std::string mModModel[7];
        ESM::FormId mModItem[3];
        ESM::FormId mAmmo;
        ESM::FormId mRepairList;
        ESM::FormId mEquipType;
        ESM::FormId mImpactDataSet;
        ESM::FormId mWorldModel;
        ESM::FormId mModdedWeapon[7];
        std::vector<SoundRef> mSoundRefs;

        ESM::FormId mPickUpSound;
        ESM::FormId mDropSound;

        float mBoundRadius;

        ESM::FormId mScriptId;
        std::uint16_t mEnchantmentPoints;
        ESM::FormId mEnchantment;

        Data mData;

        void load(ESM4::Reader& reader);
        // void save(ESM4::Writer& writer) const;

        // void blank();
        static constexpr ESM::RecNameInts sRecordId = ESM::RecNameInts::REC_WEAP4;
    };

    // Parse the stable FO3/FNV WEAP.DNAM prefix. The first 16 bytes contain the animation selectors and the
    // 68-byte prefix contains the complete primary ballistic contract. Later games reuse DNAM incompatibly.
    [[nodiscard]] bool loadFalloutWeaponDnam(std::span<const std::uint8_t> dnam, Weapon::Data& data);
}

#endif // ESM4_WEAP_H
