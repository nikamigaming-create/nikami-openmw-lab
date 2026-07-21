#include "loadmgef.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "reader.hpp"

bool ESM4::loadFalloutMagicEffectData(std::span<const std::uint8_t> bytes, MagicEffect::Data& data)
{
    // Every winning MGEF.DATA in the installed English Ultimate Edition corpus
    // (425 records across FalloutNV.esm and the official DLCs) uses this layout.
    if (bytes.size() != 72)
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
    value.flags = readUint32(0);
    value.baseCost = readFloat(4);
    value.associatedItem = ESM::FormId::fromUint32(readUint32(8));
    value.school = readInt32(12);
    value.resistanceActorValue = readInt32(16);
    value.counterEffectCount = readUint16(20);
    value.light = ESM::FormId::fromUint32(readUint32(24));
    value.projectileSpeed = readFloat(28);
    value.effectShader = ESM::FormId::fromUint32(readUint32(32));
    value.objectDisplayShader = ESM::FormId::fromUint32(readUint32(36));
    value.effectSound = ESM::FormId::fromUint32(readUint32(40));
    value.boltSound = ESM::FormId::fromUint32(readUint32(44));
    value.hitSound = ESM::FormId::fromUint32(readUint32(48));
    value.areaSound = ESM::FormId::fromUint32(readUint32(52));
    value.enchantmentFactor = readFloat(56);
    value.barterFactor = readFloat(60);
    const std::uint32_t archetype = readUint32(64);
    value.actorValue = readInt32(68);
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
                reader.adjustFormId(value.mData.associatedItem);
                reader.adjustFormId(value.mData.light);
                reader.adjustFormId(value.mData.effectShader);
                reader.adjustFormId(value.mData.objectDisplayShader);
                reader.adjustFormId(value.mData.effectSound);
                reader.adjustFormId(value.mData.boltSound);
                reader.adjustFormId(value.mData.hitSound);
                reader.adjustFormId(value.mData.areaSound);
                hasData = true;
                break;
            }
            case ESM::fourCC("ESCE"):
            {
                if (!hasData)
                    throw std::runtime_error("ESM4::MagicEffect::load - ESCE appears before DATA");
                ESM::FormId effect;
                if (!reader.getFormId(effect))
                    throw std::runtime_error("ESM4::MagicEffect::load - could not read ESCE");
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
