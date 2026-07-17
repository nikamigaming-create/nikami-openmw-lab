#include "loadproj.hpp"

#include <cstring>
#include <stdexcept>
#include <vector>

#include "reader.hpp"

bool ESM4::loadFalloutProjectileData(std::span<const std::uint8_t> bytes, Projectile::Data& data)
{
    // FNV serializes two explicit PROJ.DATA layouts: 68 bytes through defaultWeapon and 84 bytes when the four
    // rotation/bounciness floats are present. Reject every other size instead of interpreting it by coincidence.
    if (bytes.size() != 68 && bytes.size() != 84)
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
    const auto readFloat = [&](std::size_t offset) {
        float value = 0.f;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };

    data.flags = readUint16(0);
    data.type = readUint16(2);
    data.gravity = readFloat(4);
    data.speed = readFloat(8);
    data.range = readFloat(12);
    data.projectileLight = ESM::FormId::fromUint32(readUint32(16));
    data.muzzleFlashLight = ESM::FormId::fromUint32(readUint32(20));
    data.tracerChance = readFloat(24);
    data.alternateProximity = readFloat(28);
    data.alternateTimer = readFloat(32);
    data.explosion = ESM::FormId::fromUint32(readUint32(36));
    data.sound = ESM::FormId::fromUint32(readUint32(40));
    data.muzzleFlashDuration = readFloat(44);
    data.fadeDuration = readFloat(48);
    data.impactForce = readFloat(52);
    data.countdownSound = ESM::FormId::fromUint32(readUint32(56));
    data.disableSound = ESM::FormId::fromUint32(readUint32(60));
    data.defaultWeapon = ESM::FormId::fromUint32(readUint32(64));
    if (bytes.size() == 84)
    {
        data.rotationX = readFloat(68);
        data.rotationY = readFloat(72);
        data.rotationZ = readFloat(76);
        data.bounciness = readFloat(80);
    }
    data.present = true;
    return true;
}

void ESM4::Projectile::load(Reader& reader)
{
    mId = reader.getFormIdFromHeader();
    mFlags = reader.hdr().record.flags;

    while (reader.getSubRecordHeader())
    {
        const SubRecordHeader& subHdr = reader.subRecordHeader();
        switch (subHdr.typeId)
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
            case ESM::fourCC("NAM1"):
                reader.getZString(mMuzzleFlashModel);
                break;
            case ESM::fourCC("DATA"):
            {
                std::vector<std::uint8_t> bytes(subHdr.dataSize);
                reader.get(bytes.data(), bytes.size());
                if (!loadFalloutProjectileData(bytes, mData))
                    throw std::runtime_error("ESM4::Projectile::load - unsupported DATA layout");
                break;
            }
            case ESM::fourCC("OBND"):
            case ESM::fourCC("MODT"):
            case ESM::fourCC("NAM2"):
            case ESM::fourCC("VNAM"):
            case ESM::fourCC("DEST"):
            case ESM::fourCC("DSTD"):
            case ESM::fourCC("DMDL"):
            case ESM::fourCC("DMDT"):
            case ESM::fourCC("DSTA"):
            case ESM::fourCC("DSTF"):
                reader.skipSubRecordData();
                break;
            default:
                if (reader.skipUnknownStarfieldSubRecordData("loadproj"))
                    break;
                throw std::runtime_error(
                    "ESM4::Projectile::load - Unknown subrecord " + ESM::printName(subHdr.typeId));
        }
    }
}
