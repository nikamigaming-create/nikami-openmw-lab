#ifndef OPENMW_COMPONENTS_ESM4_LOADAMEF_H
#define OPENMW_COMPONENTS_ESM4_LOADAMEF_H

#include <cstddef>
#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    struct AmmoEffect
    {
        enum class Type : std::uint32_t
        {
            Damage = 0,
            DamageResistance,
            DamageThreshold,
            Spread,
            WeaponCondition,
            Fatigue,
        };

        enum class Operation : std::uint32_t
        {
            Add = 0,
            Multiply,
            Subtract,
        };

        static constexpr std::size_t sDataSerializedSize
            = sizeof(std::uint32_t) + sizeof(std::uint32_t) + sizeof(float);

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        Type mType = Type::Damage;
        Operation mOperation = Operation::Add;
        float mValue = 0.f;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_AMEF4;
    };
}

#endif // OPENMW_COMPONENTS_ESM4_LOADAMEF_H
