#include "loadexpl.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

#include "reader.hpp"

bool ESM4::loadFalloutExplosionData(std::span<const std::uint8_t> bytes, Explosion::Data& data)
{
    // FalloutNV.esm serializes all 154 winning EXPL.DATA records in this exact 52-byte layout.
    if (bytes.size() != 52)
        return false;

    const auto readUint32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    const auto readFloat = [&](std::size_t offset) {
        float value = 0.f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };

    data.force = readFloat(0);
    data.damage = readFloat(4);
    data.radius = readFloat(8);
    data.light = ESM::FormId::fromUint32(readUint32(12));
    data.sound1 = ESM::FormId::fromUint32(readUint32(16));
    data.flags = readUint32(20);
    data.imageSpaceRadius = readFloat(24);
    data.impactDataSet = ESM::FormId::fromUint32(readUint32(28));
    data.sound2 = ESM::FormId::fromUint32(readUint32(32));
    data.radiationLevel = readFloat(36);
    data.radiationDissipationTime = readFloat(40);
    data.radiationRadius = readFloat(44);
    data.soundLevel = readUint32(48);
    data.present = true;
    return true;
}

void ESM4::Explosion::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHeader = reader.subRecordHeader();
        switch (subHeader.typeId)
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
            case ESM::fourCC("EITM"):
                reader.getFormId(mObjectEffect);
                break;
            case ESM::fourCC("MNAM"):
                reader.getFormId(mImageSpaceModifier);
                break;
            case ESM::fourCC("INAM"):
                reader.getFormId(mPlacedImpactObject);
                break;
            case ESM::fourCC("DATA"):
            {
                std::vector<std::uint8_t> bytes(subHeader.dataSize);
                reader.get(bytes.data(), bytes.size());
                if (!loadFalloutExplosionData(bytes, mData))
                    throw std::runtime_error("ESM4::Explosion::load - unsupported Fallout New Vegas DATA layout");
                reader.adjustFormId(mData.light);
                reader.adjustFormId(mData.sound1);
                reader.adjustFormId(mData.impactDataSet);
                reader.adjustFormId(mData.sound2);
                break;
            }
            case ESM::fourCC("OBND"):
            case ESM::fourCC("MODT"):
                reader.skipSubRecordData();
                break;
            default:
                throw std::runtime_error(
                    "ESM4::Explosion::load - unknown Fallout New Vegas subrecord "
                    + ESM::printName(subHeader.typeId));
        }
    }
}
