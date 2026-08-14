#ifndef OPENMW_COMPONENTS_ESM4_LOADAMEF_H
#define OPENMW_COMPONENTS_ESM4_LOADAMEF_H

#include <cstdint>
#include <string>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

namespace ESM4
{
    class Reader;

    /// Fallout: New Vegas AMEF record. AMMO.RCIL entries point at these records and the engine applies matching
    /// effects in authored list order to damage, DR, DT, spread, weapon-condition loss, or fatigue.
    struct AmmoEffect
    {
        enum class Type : std::uint32_t
        {
            Damage = 0,
            DamageResistance = 1,
            DamageThreshold = 2,
            Spread = 3,
            WeaponCondition = 4,
            Fatigue = 5,
        };

        enum class Operation : std::uint32_t
        {
            Add = 0,
            Multiply = 1,
            Subtract = 2,
        };

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

#endif
