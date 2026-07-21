/*
  Copyright (C) 2016, 2018-2021 cc9cii

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
#include "loadweap.hpp"

#include <array>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "reader.hpp"
//#include "writer.hpp"

bool ESM4::loadFalloutWeaponDnam(std::span<const std::uint8_t> dnam, Weapon::Data& data)
{
    if (dnam.size() < 16)
        return false;

    const auto readFloat = [&](std::size_t offset) {
        float value = 0.f;
        std::memcpy(&value, dnam.data() + offset, sizeof(value));
        return value;
    };
    const auto readUint32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, dnam.data() + offset, sizeof(value));
        return value;
    };
    const auto readInt32 = [&](std::size_t offset) {
        std::int32_t value = 0;
        std::memcpy(&value, dnam.data() + offset, sizeof(value));
        return value;
    };

    data.animationType = dnam[0];
    data.animationMultiplier = readFloat(4);
    data.reach = readFloat(8);
    data.weaponFlags1 = dnam[12];
    data.handGrip = dnam[13];
    data.ammoUse = dnam[14];
    data.reloadAnim = dnam[15];

    if (dnam.size() < 68)
        return true;

    data.minSpread = readFloat(16);
    data.spread = readFloat(20);
    data.sightFov = readFloat(28);
    data.projectile = ESM::FormId::fromUint32(readUint32(36));
    data.baseVatsChance = dnam[40];
    data.attackAnim = dnam[41];
    data.numProjectiles = dnam[42];
    data.embedWeaponActorValue = dnam[43];
    data.minRange = readFloat(44);
    data.maxRange = readFloat(48);
    data.onHit = readUint32(52);
    data.flags2 = readUint32(56);
    data.animAttackMult = readFloat(60);
    data.fireRate = readFloat(64);
    data.hasBallistics = true;

    if (dnam.size() >= 72)
        data.overrideActionPoints = readFloat(68);
    if (dnam.size() >= 88)
        data.damageToWeaponMult = readFloat(84);
    if (dnam.size() >= 92)
        data.animShotsPerSec = readFloat(88);
    if (dnam.size() >= 108)
        data.skillActorValue = readInt32(104);
    if (dnam.size() >= 120)
        data.limbDamageMult = readFloat(116);
    if (dnam.size() >= 132)
        data.semiAutoFireDelayMin = readFloat(128);
    if (dnam.size() >= 136)
        data.semiAutoFireDelayMax = readFloat(132);
    return true;
}

bool ESM4::loadFalloutWeaponCrdt(std::span<const std::uint8_t> crdt, Weapon::CriticalData& data)
{
    constexpr std::size_t serializedSize = 16;
    if (crdt.size() != serializedSize)
        return false;

    std::uint32_t effect = 0;
    std::memcpy(&data.damage, crdt.data(), sizeof(data.damage));
    std::memcpy(&data.chanceMultiplier, crdt.data() + 4, sizeof(data.chanceMultiplier));
    data.flags = crdt[8];
    std::memcpy(&effect, crdt.data() + 12, sizeof(effect));
    data.effect = ESM::FormId::fromUint32(effect);
    data.present = true;
    return true;
}

void ESM4::Weapon::load(ESM4::Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;
    std::uint32_t esmVer = reader.esmVersion();
    bool isFONV = esmVer == ESM::VER_132 || esmVer == ESM::VER_133 || esmVer == ESM::VER_134;
    bool isFalloutWeapon = isFONV;

    while (reader.getSubRecordHeader())
    {
        const ESM4::SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
        {
            case ESM::fourCC("EDID"):
                reader.getZString(mEditorId);
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(mFullName);
                break;
            case ESM::fourCC("DATA"):
            {
                // if (reader.esmVersion() == ESM::VER_094 || reader.esmVersion() == ESM::VER_170)
                if (subHdr.dataSize == 10) // FO3 has 15 bytes even though VER_094
                {
                    reader.get(mData.value);
                    reader.get(mData.weight);
                    reader.get(mData.damage);
                }
                else if (isFONV || subHdr.dataSize == 15)
                {
                    isFalloutWeapon = true;
                    reader.get(mData.value);
                    reader.get(mData.health);
                    reader.get(mData.weight);
                    reader.get(mData.damage);
                    reader.get(mData.clipSize);
                }
                else
                {
                    reader.get(mData.type);
                    reader.get(mData.speed);
                    reader.get(mData.reach);
                    reader.get(mData.flags);
                    reader.get(mData.value);
                    reader.get(mData.health);
                    reader.get(mData.weight);
                    reader.get(mData.damage);
                }
                break;
            }
            case ESM::fourCC("MODL"):
                reader.getZString(mModel);
                break;
            case ESM::fourCC("MOD4"):
                reader.getZString(mFirstPersonModel);
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(mIcon);
                break;
            case ESM::fourCC("MICO"):
                reader.getZString(mMiniIcon);
                break; // FO3
            case ESM::fourCC("SCRI"):
                reader.getFormId(mScriptId);
                break;
            case ESM::fourCC("ANAM"):
                reader.get(mEnchantmentPoints);
                break;
            case ESM::fourCC("ENAM"):
                reader.getFormId(mEnchantment);
                break;
            case ESM::fourCC("MODB"):
                reader.get(mBoundRadius);
                break;
            case ESM::fourCC("DESC"):
                reader.getLocalizedString(mText);
                break;
            case ESM::fourCC("YNAM"):
                reader.getFormId(mPickUpSound);
                break;
            case ESM::fourCC("ZNAM"):
                reader.getFormId(mDropSound);
                break;
            case ESM::fourCC("NAM0"):
                reader.getFormId(mAmmo);
                break;
            case ESM::fourCC("REPL"):
                reader.getFormId(mRepairList);
                break;
            case ESM::fourCC("ETYP"):
                reader.getFormId(mEquipType);
                break;
            case ESM::fourCC("INAM"):
                reader.getFormId(mImpactDataSet);
                break;
            case ESM::fourCC("DNAM"):
                if (isFalloutWeapon && subHdr.dataSize >= 16)
                {
                    // FO3/FNV DNAM begins with the runtime TESObjectWEAP fields documented by xNVSE. Preserve the
                    // whole serialized record so the firing path receives range/projectile/cadence bytes too.
                    std::vector<std::uint8_t> dnam(subHdr.dataSize);
                    reader.get(dnam.data(), dnam.size());
                    loadFalloutWeaponDnam(dnam, mData);
                    if (mData.hasBallistics)
                        reader.adjustFormId(mData.projectile);
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("CRDT"):
                if (isFalloutWeapon && subHdr.dataSize == 16)
                {
                    std::array<std::uint8_t, 16> crdt{};
                    reader.get(crdt.data(), crdt.size());
                    loadFalloutWeaponCrdt(crdt, mCriticalData);
                    reader.adjustFormId(mCriticalData.effect);
                }
                else
                    reader.skipSubRecordData();
                break;
            case ESM::fourCC("WNAM"):
                reader.getFormId(mWorldModel);
                break;
            case ESM::fourCC("SNAM"):
            case ESM::fourCC("XNAM"):
            case ESM::fourCC("TNAM"):
            case ESM::fourCC("NAM8"):
            case ESM::fourCC("NAM9"):
            case ESM::fourCC("WMS1"):
            case ESM::fourCC("WMS2"):
            {
                SoundRef sound;
                sound.mType = subHdr.typeId;
                reader.getFormId(sound.mSound);
                mSoundRefs.push_back(sound);
                break;
            }
            case ESM::fourCC("MODT"): // Model data
            case ESM::fourCC("MODC"):
            case ESM::fourCC("MODS"):
            case ESM::fourCC("MODF"): // Model data end
            case ESM::fourCC("BAMT"):
            case ESM::fourCC("BIDS"):
            case ESM::fourCC("CNAM"):
            case ESM::fourCC("EAMT"):
            case ESM::fourCC("EITM"):
            case ESM::fourCC("KSIZ"):
            case ESM::fourCC("KWDA"):
            case ESM::fourCC("OBND"):
            case ESM::fourCC("UNAM"):
            case ESM::fourCC("VMAD"):
            case ESM::fourCC("VNAM"):
            case ESM::fourCC("NNAM"):
            case ESM::fourCC("MOD2"): // FO3
            case ESM::fourCC("MO2T"): // FO3
            case ESM::fourCC("MO2S"): // FO3
            case ESM::fourCC("NAM6"): // FO3
            case ESM::fourCC("MO4T"):
            case ESM::fourCC("MO4S"):
            case ESM::fourCC("MO4C"):
            case ESM::fourCC("MO4F"): // First person model data end
            case ESM::fourCC("BIPL"): // FO3
            case ESM::fourCC("NAM7"): // FO3
            case ESM::fourCC("MOD3"): // FO3
            case ESM::fourCC("MO3T"): // FO3
            case ESM::fourCC("MO3S"): // FO3
            case ESM::fourCC("MODD"): // FO3
                                      // case ESM::fourCC("MOSD"): // FO3
            case ESM::fourCC("DAMC"): // Destructible
            case ESM::fourCC("DEST"):
            case ESM::fourCC("DMDC"):
            case ESM::fourCC("DMDL"):
            case ESM::fourCC("DMDT"):
            case ESM::fourCC("DMDS"):
            case ESM::fourCC("DSTA"):
            case ESM::fourCC("DSTD"):
            case ESM::fourCC("DSTF"): // Destructible end
            case ESM::fourCC("VATS"): // FONV
            case ESM::fourCC("VANM"): // FONV
                reader.skipSubRecordData();
                break;
            case ESM::fourCC("MWD1"): reader.getZString(mModModel[0]); break;
            case ESM::fourCC("MWD2"): reader.getZString(mModModel[1]); break;
            case ESM::fourCC("MWD3"): reader.getZString(mModModel[2]); break;
            case ESM::fourCC("MWD4"): reader.getZString(mModModel[3]); break;
            case ESM::fourCC("MWD5"): reader.getZString(mModModel[4]); break;
            case ESM::fourCC("MWD6"): reader.getZString(mModModel[5]); break;
            case ESM::fourCC("MWD7"): reader.getZString(mModModel[6]); break;
            case ESM::fourCC("WMI1"): reader.getFormId(mModItem[0]); break;
            case ESM::fourCC("WMI2"): reader.getFormId(mModItem[1]); break;
            case ESM::fourCC("WMI3"): reader.getFormId(mModItem[2]); break;
            case ESM::fourCC("WNM1"): reader.getFormId(mModdedWeapon[0]); break;
            case ESM::fourCC("WNM2"): reader.getFormId(mModdedWeapon[1]); break;
            case ESM::fourCC("WNM3"): reader.getFormId(mModdedWeapon[2]); break;
            case ESM::fourCC("WNM4"): reader.getFormId(mModdedWeapon[3]); break;
            case ESM::fourCC("WNM5"): reader.getFormId(mModdedWeapon[4]); break;
            case ESM::fourCC("WNM6"): reader.getFormId(mModdedWeapon[5]); break;
            case ESM::fourCC("WNM7"): reader.getFormId(mModdedWeapon[6]); break;
            case ESM::fourCC("EFSD"): // FONV DeadMoney
            case ESM::fourCC("APPR"): // FO4
            case ESM::fourCC("DAMA"): // FO4
            case ESM::fourCC("FLTR"): // FO4
            case ESM::fourCC("FNAM"): // FO4
            case ESM::fourCC("INRD"): // FO4
            case ESM::fourCC("LNAM"): // FO4
            case ESM::fourCC("MASE"): // FO4
            case ESM::fourCC("PTRN"): // FO4
            case ESM::fourCC("STCP"): // FO4
            case ESM::fourCC("WAMD"): // FO4
            case ESM::fourCC("WZMD"): // FO4
            case ESM::fourCC("OBTE"): // FO4 object template start
            case ESM::fourCC("OBTF"):
            case ESM::fourCC("OBTS"):
            case ESM::fourCC("STOP"): // FO4 object template end
                reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadweap"))
                    break;
                throw std::runtime_error("ESM4::WEAP::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}

// void ESM4::Weapon::save(ESM4::Writer& writer) const
//{
// }

// void ESM4::Weapon::blank()
//{
// }
