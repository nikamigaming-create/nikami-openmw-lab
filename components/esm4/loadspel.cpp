#include "loadspel.hpp"

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

bool ESM4::loadFalloutSpellData(std::span<const std::uint8_t> bytes, Spell::Data& data)
{
    if (bytes.size() != Spell::sDataSerializedSize)
        return false;

    const auto readUint32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const std::uint32_t type = readUint32(Spell::sDataTypeOffset);
    if (type > static_cast<std::uint32_t>(Spell::Type::Addiction))
        return false;

    Spell::Data value;
    value.type = static_cast<Spell::Type>(type);
    value.cost = readUint32(Spell::sDataCostOffset);
    value.level = readUint32(Spell::sDataLevelOffset);
    value.flags = bytes[Spell::sDataFlagsOffset];
    value.present = true;
    data = value;
    return true;
}

bool ESM4::loadFalloutSpellEffectData(std::span<const std::uint8_t> bytes, Spell::Effect& effect)
{
    if (bytes.size() != Spell::sEffectSerializedSize)
        return false;

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
    const std::uint32_t range = readUint32(Spell::sEffectRangeOffset);
    if (range > static_cast<std::uint32_t>(Spell::Range::Target))
        return false;

    Spell::Effect value = effect;
    value.magnitude = readUint32(Spell::sEffectMagnitudeOffset);
    value.area = readUint32(Spell::sEffectAreaOffset);
    value.duration = readUint32(Spell::sEffectDurationOffset);
    value.range = static_cast<Spell::Range>(range);
    value.actorValue = readInt32(Spell::sEffectActorValueOffset);
    effect = std::move(value);
    return true;
}

void ESM4::Spell::load(Reader& reader)
{
    Spell value;
    value.mId = reader.getFormIdFromHeader();
    value.mFlags = reader.hdr().record.flags;

    bool hasEditorId = false;
    bool hasData = false;
    bool effectNeedsData = false;
    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& header = reader.subRecordHeader();
        switch (header.typeId)
        {
            case ESM::fourCC("EDID"):
                if (hasEditorId || header.dataSize == 0 || !reader.getZString(value.mEditorId))
                    throw std::runtime_error("ESM4::Spell::load - invalid or duplicated EDID");
                hasEditorId = true;
                break;
            case ESM::fourCC("FULL"):
                if (!hasEditorId || hasData)
                    throw std::runtime_error("ESM4::Spell::load - FULL is out of order");
                reader.getLocalizedString(value.mFullName);
                break;
            case ESM::fourCC("SPIT"):
            {
                if (!hasEditorId || hasData)
                    throw std::runtime_error("ESM4::Spell::load - SPIT is duplicated or out of order");
                std::vector<std::uint8_t> bytes(header.dataSize);
                if (!reader.get(bytes.data(), bytes.size()) || !loadFalloutSpellData(bytes, value.mData))
                    throw std::runtime_error("ESM4::Spell::load - unsupported Fallout New Vegas SPIT layout");
                hasData = true;
                break;
            }
            case ESM::fourCC("EFID"):
            {
                if (!hasData || effectNeedsData)
                    throw std::runtime_error("ESM4::Spell::load - EFID is out of order or prior EFIT is missing");
                value.mEffects.emplace_back();
                ESM::FormId32 rawBaseEffect = 0;
                if (!reader.getExact(rawBaseEffect))
                    throw std::runtime_error("ESM4::Spell::load - could not read EFID");
                value.mEffects.back().baseEffect = ESM::FormId::fromUint32(rawBaseEffect);
                adjustFormIdIfSet(reader, value.mEffects.back().baseEffect);
                effectNeedsData = true;
                break;
            }
            case ESM::fourCC("EFIT"):
            {
                if (!effectNeedsData || value.mEffects.empty())
                    throw std::runtime_error("ESM4::Spell::load - EFIT appears without EFID");
                std::vector<std::uint8_t> bytes(header.dataSize);
                if (!reader.get(bytes.data(), bytes.size())
                    || !loadFalloutSpellEffectData(bytes, value.mEffects.back()))
                    throw std::runtime_error("ESM4::Spell::load - unsupported Fallout New Vegas EFIT layout");
                effectNeedsData = false;
                break;
            }
            case ESM::fourCC("CTDA"):
            {
                if (effectNeedsData || value.mEffects.empty())
                    throw std::runtime_error("ESM4::Spell::load - CTDA appears before a complete effect");
                TargetCondition condition;
                if (!loadTargetCondition(reader, condition))
                    throw std::runtime_error("ESM4::Spell::load - unsupported CTDA layout");
                value.mEffects.back().conditions.push_back(condition);
                break;
            }
            default:
                throw std::runtime_error("ESM4::Spell::load - unknown Fallout New Vegas subrecord "
                    + ESM::printName(header.typeId));
        }
    }

    if (!hasEditorId || !hasData || effectNeedsData || value.mEffects.empty())
        throw std::runtime_error("ESM4::Spell::load - incomplete actor-effect record");
    *this = std::move(value);
}
