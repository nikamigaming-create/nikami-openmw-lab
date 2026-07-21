#ifndef OPENMW_COMPONENTS_ESM4_LOADSPEL_H
#define OPENMW_COMPONENTS_ESM4_LOADSPEL_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <components/esm/defs.hpp>
#include <components/esm/formid.hpp>

#include "script.hpp"

namespace ESM4
{
    class Reader;

    /// Fallout 3/New Vegas SPEL actor effect. Native WEAP.CRDT records point at
    /// these records for critical effects.
    struct Spell
    {
        enum class Type : std::uint32_t
        {
            ActorEffect = 0,
            Disease = 1,
            Power = 2,
            LesserPower = 3,
            Ability = 4,
            Poison = 5,
            Addiction = 10,
        };

        enum Flags : std::uint8_t
        {
            NoAutoCalculate = 1u << 0,
            PlayerStartEffect = 1u << 2,
            AreaIgnoresLineOfSight = 1u << 4,
            ScriptEffectAlwaysApplies = 1u << 5,
            DisableAbsorbReflect = 1u << 6,
            ForceTouchExplode = 1u << 7,
        };

        enum class Range : std::uint32_t
        {
            Self = 0,
            Touch = 1,
            Target = 2,
        };

        struct Data
        {
            Type type = Type::ActorEffect;
            std::uint32_t cost = 0;
            std::uint32_t level = 0;
            std::uint8_t flags = 0;
            bool present = false;
        };

        struct Effect
        {
            ESM::FormId baseEffect;
            std::uint32_t magnitude = 0;
            std::uint32_t area = 0;
            std::uint32_t duration = 0;
            Range range = Range::Self;
            std::int32_t actorValue = -1;
            std::vector<TargetCondition> conditions;
        };

        ESM::FormId mId;
        std::uint32_t mFlags = 0;
        std::string mEditorId;
        std::string mFullName;
        Data mData;
        std::vector<Effect> mEffects;

        void load(Reader& reader);

        static constexpr ESM::RecNameInts sRecordId = ESM::REC_SPEL4;
    };

    [[nodiscard]] bool loadFalloutSpellData(std::span<const std::uint8_t> bytes, Spell::Data& data);
    [[nodiscard]] bool loadFalloutSpellEffectData(std::span<const std::uint8_t> bytes, Spell::Effect& effect);
}

#endif
