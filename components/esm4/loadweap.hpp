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
            std::uint8_t handGrip;
            std::uint8_t ammoUse;
            std::uint8_t reloadAnim;

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
                , handGrip(0xff)
                , ammoUse(0)
                , reloadAnim(0)
            {
            }
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

    // Parse the stable 16-byte FO3/FNV prefix of WEAP.DNAM. Later games reuse DNAM with incompatible layouts.
    [[nodiscard]] bool loadFalloutWeaponDnam(std::span<const std::uint8_t> dnam, Weapon::Data& data);
}

#endif // ESM4_WEAP_H
