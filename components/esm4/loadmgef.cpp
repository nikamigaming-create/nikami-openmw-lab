#include "loadmgef.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "reader.hpp"

namespace
{
    void adjustFormIdIfSet(ESM4::Reader& reader, ESM::FormId& id)
    {
        if (!id.isZeroOrUnset())
            reader.adjustFormId(id);
    }
}

bool ESM4::loadFalloutMagicEffectData(std::span<const std::uint8_t> bytes, MagicEffect::Data& data)
{
    // Every winning MGEF.DATA in the installed English Ultimate Edition corpus
    // (425 records across FalloutNV.esm and the official DLCs) uses this layout.
    if (bytes.size() != MagicEffect::sDataSerializedSize)
        return false;

    const auto readUint16 = [&](std::size_t offset) {
        std::uint16_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const auto readUint32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const auto readInt32 = [&](std::size_t offset) {
        std::int32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const auto readFloat = [&](std::size_t offset) {
        float value = 0.f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };

    MagicEffect::Data value;
    value.flags = readUint32(MagicEffect::sDataFlagsOffset);
    value.baseCost = readFloat(MagicEffect::sDataBaseCostOffset);
    value.associatedItem = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataAssociatedItemOffset));
    value.school = readInt32(MagicEffect::sDataSchoolOffset);
    value.resistanceActorValue = readInt32(MagicEffect::sDataResistanceActorValueOffset);
    value.counterEffectCount = readUint16(MagicEffect::sDataCounterEffectCountOffset);
    value.light = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataLightOffset));
    value.projectileSpeed = readFloat(MagicEffect::sDataProjectileSpeedOffset);
    value.effectShader = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataEffectShaderOffset));
    value.objectDisplayShader = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataObjectDisplayShaderOffset));
    value.effectSound = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataEffectSoundOffset));
    value.boltSound = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataBoltSoundOffset));
    value.hitSound = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataHitSoundOffset));
    value.areaSound = ESM::FormId::fromUint32(readUint32(MagicEffect::sDataAreaSoundOffset));
    value.enchantmentFactor = readFloat(MagicEffect::sDataEnchantmentFactorOffset);
    value.barterFactor = readFloat(MagicEffect::sDataBarterFactorOffset);
    const std::uint32_t archetype = readUint32(MagicEffect::sDataArchetypeOffset);
    value.actorValue = readInt32(MagicEffect::sDataActorValueOffset);
    if (!std::isfinite(value.baseCost) || !std::isfinite(value.projectileSpeed)
        || !std::isfinite(value.enchantmentFactor) || !std::isfinite(value.barterFactor)
        || archetype > static_cast<std::uint32_t>(MagicEffect::Archetype::Turbo))
        return false;
    value.archetype = static_cast<MagicEffect::Archetype>(archetype);
    value.present = true;
    data = value;
    return true;
}

void ESM4::MagicEffect::load(Reader& reader)
{
    MagicEffect value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    bool hasEditorId = false;
    bool hasDescription = false;
    bool hasData = false;
    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case ESM::fourCC("EDID"):
                if (hasEditorId || header.dataSize == 0 || !reader.getZString(value.mEditorId))
                    throw std::runtime_error("ESM4::MagicEffect::load - invalid or duplicated EDID");
                hasEditorId = true;
                break;
            case ESM::fourCC("FULL"):
                reader.getLocalizedString(value.mFullName);
                break;
            case ESM::fourCC("DESC"):
                if (hasDescription)
                    throw std::runtime_error("ESM4::MagicEffect::load - duplicated DESC");
                reader.getLocalizedString(value.mDescription);
                hasDescription = true;
                break;
            case ESM::fourCC("ICON"):
                reader.getZString(value.mIcon);
                break;
            case ESM::fourCC("MODL"):
                reader.getZString(value.mModel);
                break;
            case ESM::fourCC("DATA"):
            {
                if (!hasEditorId || !hasDescription || hasData)
                    throw std::runtime_error("ESM4::MagicEffect::load - DATA is duplicated or out of order");
                std::vector<std::uint8_t> bytes(header.dataSize);
                if (!reader.get(bytes.data(), bytes.size()) || !loadFalloutMagicEffectData(bytes, value.mData))
                    throw std::runtime_error("ESM4::MagicEffect::load - unsupported Fallout New Vegas DATA layout");
                adjustFormIdIfSet(reader, value.mData.associatedItem);
                adjustFormIdIfSet(reader, value.mData.light);
                adjustFormIdIfSet(reader, value.mData.effectShader);
                adjustFormIdIfSet(reader, value.mData.objectDisplayShader);
                adjustFormIdIfSet(reader, value.mData.effectSound);
                adjustFormIdIfSet(reader, value.mData.boltSound);
                adjustFormIdIfSet(reader, value.mData.hitSound);
                adjustFormIdIfSet(reader, value.mData.areaSound);
                hasData = true;
                break;
            }
            case ESM::fourCC("ESCE"):
            {
                if (!hasData)
                    throw std::runtime_error("ESM4::MagicEffect::load - ESCE appears before DATA");
                ESM::FormId32 rawEffect = 0;
                if (!reader.getExact(rawEffect))
                    throw std::runtime_error("ESM4::MagicEffect::load - could not read ESCE");
                ESM::FormId effect = ESM::FormId::fromUint32(rawEffect);
                adjustFormIdIfSet(reader, effect);
                value.mCounterEffects.push_back(effect);
                break;
            }
            case ESM::fourCC("MODB"):
            case ESM::fourCC("MODT"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error("ESM4::MagicEffect::load - unknown Fallout New Vegas subrecord "
                    + ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasDescription || !hasData
        || value.mData.counterEffectCount != value.mCounterEffects.size())
        throw std::runtime_error("ESM4::MagicEffect::load - incomplete record or counter-effect count mismatch");
    *this = std::move(value);
}
