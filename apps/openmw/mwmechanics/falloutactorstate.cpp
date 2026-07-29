#include "falloutactorstate.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include <components/esm/attr.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadnpc.hpp>

#include "../mwclass/esm4creature.hpp"
#include "../mwclass/esm4npc.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/ptr.hpp"

#include "aisetting.hpp"
#include "creaturestats.hpp"

namespace MWMechanics
{
    namespace
    {
        std::string normalizeActorValueName(std::string_view name)
        {
            std::string result;
            result.reserve(name.size());
            for (const unsigned char c : name)
            {
                if (std::isalnum(c) != 0)
                    result.push_back(static_cast<char>(std::tolower(c)));
            }
            return result;
        }

        std::optional<ESM::RefId> attributeId(std::uint8_t actorValue)
        {
            switch (actorValue)
            {
                case 5:
                    return ESM::Attribute::Strength;
                case 6:
                    return ESM::Attribute::Willpower; // Fallout Perception compatibility channel.
                case 7:
                    return ESM::Attribute::Endurance;
                case 8:
                    return ESM::Attribute::Personality; // Fallout Charisma compatibility channel.
                case 9:
                    return ESM::Attribute::Intelligence;
                case 10:
                    return ESM::Attribute::Agility;
                case 11:
                    return ESM::Attribute::Luck;
                default:
                    return std::nullopt;
            }
        }

        std::optional<float> getNpcSkill(const ESM4::Npc* npc, std::uint8_t actorValue)
        {
            if (npc == nullptr || !npc->mHasFNVSkills || actorValue < 32 || actorValue > 45)
                return std::nullopt;
            const ESM4::Npc::FNVSkillValues& value = npc->mFNVSkills.values;
            switch (actorValue)
            {
                case 32:
                    return value.barter;
                case 33:
                    return value.bigGuns;
                case 34:
                    return value.energyWeapons;
                case 35:
                    return value.explosives;
                case 36:
                    return value.lockpick;
                case 37:
                    return value.medicine;
                case 38:
                    return value.meleeWeapons;
                case 39:
                    return value.repair;
                case 40:
                    return value.science;
                case 41:
                    return value.smallGuns;
                case 42:
                    return value.sneak;
                case 43:
                    return value.speech;
                case 44:
                    return value.survivalOrThrowing;
                case 45:
                    return value.unarmed;
                default:
                    return std::nullopt;
            }
        }

        const ESM4::Npc* getNpcStats(const MWWorld::Ptr& actor)
        {
            return actor.getType() == ESM4::Npc::sRecordId ? MWClass::ESM4Npc::getStatsRecord(actor) : nullptr;
        }

        const ESM4::Creature* getCreatureStatsRecord(const MWWorld::Ptr& actor)
        {
            return actor.getType() == ESM4::Creature::sRecordId
                ? MWClass::ESM4Creature::getStatsRecord(actor)
                : nullptr;
        }

        std::optional<float> authoredFalloutActorValue(const MWWorld::Ptr& actor,
            const CreatureStats& stats, std::uint8_t actorValue)
        {
            if (const std::optional<ESM::RefId> attribute = attributeId(actorValue))
                return stats.getAttribute(*attribute).getModified();

            switch (actorValue)
            {
                case 0:
                    return static_cast<float>(stats.getAiSetting(AiSetting::Fight).getModified());
                case 1:
                    return static_cast<float>(100 - stats.getAiSetting(AiSetting::Flee).getModified());
                case 3:
                    return static_cast<float>(stats.getAiSetting(AiSetting::Alarm).getModified());
                case 13:
                    return 150.f + 10.f * stats.getAttribute(ESM::Attribute::Strength).getModified();
                case 16:
                    return stats.getHealth().getCurrent();
                case 21:
                {
                    if (const ESM4::Npc* npc = getNpcStats(actor); npc != nullptr && npc->mIsFONV)
                        return static_cast<float>(std::max<std::uint16_t>(npc->mBaseConfig.fo3.speedMultiplier, 1));
                    if (const ESM4::Creature* creature = getCreatureStatsRecord(actor);
                        creature != nullptr && creature->mIsFONV)
                        return static_cast<float>(
                            std::max<std::uint16_t>(creature->mBaseConfig.fo3.speedMultiplier, 1));
                    return 100.f;
                }
                case 22:
                    return stats.getFatigue().getCurrent();
                case 23:
                {
                    if (const ESM4::Npc* npc = getNpcStats(actor); npc != nullptr && npc->mIsFONV)
                        return npc->mBaseConfig.fo3.karma;
                    if (const ESM4::Creature* creature = getCreatureStatsRecord(actor);
                        creature != nullptr && creature->mIsFONV)
                        return creature->mBaseConfig.fo3.karma;
                    return 0.f;
                }
                case 25:
                case 26:
                case 27:
                case 28:
                case 29:
                case 30:
                case 31:
                    // CreatureStats stores accumulated native hit-point damage. The scripting AV exposes the
                    // remaining condition channel; 100 is the undamaged quest-script baseline.
                    return std::max(0.f,
                        100.f - stats.getFalloutLimbDamage(static_cast<std::int8_t>(actorValue)));
                case 57:
                {
                    if (actor.getType() == ESM4::Npc::sRecordId)
                    {
                        if (const ESM4::Npc* ai = MWClass::ESM4Npc::getAIDataRecord(actor);
                            ai != nullptr && ai->mHasFNVAIData)
                            return static_cast<float>(ai->mFNVAIData.assistance);
                    }
                    else if (actor.getType() == ESM4::Creature::sRecordId)
                    {
                        if (const ESM4::Creature* ai = MWClass::ESM4Creature::getAIDataRecord(actor);
                            ai != nullptr && ai->mHasFNVAIData)
                            return static_cast<float>(ai->mFNVAIData.assistance);
                    }
                    return 0.f;
                }
                default:
                    break;
            }

            if (actorValue == 2 || actorValue == 4)
            {
                if (actor.getType() == ESM4::Npc::sRecordId)
                {
                    if (const ESM4::Npc* ai = MWClass::ESM4Npc::getAIDataRecord(actor);
                        ai != nullptr && ai->mHasFNVAIData)
                        return actorValue == 2 ? static_cast<float>(ai->mFNVAIData.energyLevel)
                                               : static_cast<float>(ai->mFNVAIData.mood);
                }
                else if (actor.getType() == ESM4::Creature::sRecordId)
                {
                    if (const ESM4::Creature* ai = MWClass::ESM4Creature::getAIDataRecord(actor);
                        ai != nullptr && ai->mHasFNVAIData)
                        return actorValue == 2 ? static_cast<float>(ai->mFNVAIData.energyLevel)
                                               : static_cast<float>(ai->mFNVAIData.mood);
                }
            }

            if (actorValue >= 32 && actorValue <= 45)
                if (const std::optional<float> skill = getNpcSkill(getNpcStats(actor), actorValue))
                    return skill;

            // Every native index accepted by the Fallout runtime has a defined zero baseline even when the
            // corresponding immutable AVIF payload is not yet projected into the shared Morrowind stat model.
            return actorValue < 96 ? std::optional<float>(0.f) : std::nullopt;
        }
    }

    std::optional<std::uint8_t> resolveFalloutActorValue(std::string_view name)
    {
        static constexpr std::array<std::pair<std::string_view, std::uint8_t>, 103> Values{ {
            { "aggression", 0 }, { "confidence", 1 }, { "energy", 2 }, { "energylevel", 2 },
            { "responsibility", 3 }, { "mood", 4 }, { "strength", 5 }, { "perception", 6 },
            { "endurance", 7 }, { "charisma", 8 }, { "intelligence", 9 }, { "agility", 10 },
            { "luck", 11 }, { "actionpoints", 12 }, { "carryweight", 13 }, { "critchance", 14 },
            { "healrate", 15 }, { "health", 16 }, { "meleedamage", 17 }, { "damageresistance", 18 },
            { "poisonresistance", 19 }, { "radresistance", 20 }, { "speedmultiplier", 21 },
            { "speedmult", 21 }, { "fatigue", 22 }, { "karma", 23 }, { "xp", 24 },
            { "experience", 24 }, { "head", 25 }, { "headcondition", 25 },
            { "perceptioncondition", 25 }, { "torso", 26 }, { "torsocondition", 26 },
            { "endurancecondition", 26 }, { "leftarm", 27 }, { "leftattackcondition", 27 },
            { "rightarm", 28 }, { "rightattackcondition", 28 }, { "leftleg", 29 },
            { "leftmobilitycondition", 29 }, { "rightleg", 30 }, { "rightmobilitycondition", 30 },
            { "brain", 31 }, { "braincondition", 31 }, { "barter", 32 }, { "bigguns", 33 },
            { "energyweapons", 34 }, { "explosives", 35 }, { "lockpick", 36 }, { "medicine", 37 },
            { "meleeweapons", 38 }, { "repair", 39 }, { "science", 40 }, { "guns", 41 },
            { "smallguns", 41 }, { "sneak", 42 }, { "speech", 43 }, { "survival", 44 },
            { "throwing", 44 }, { "unarmed", 45 }, { "inventoryweight", 46 }, { "paralysis", 47 },
            { "invisibility", 48 }, { "chameleon", 49 }, { "nighteye", 50 }, { "turbo", 51 },
            { "detectliferange", 51 }, { "fireresistance", 52 }, { "waterbreathing", 53 },
            { "radlevel", 54 }, { "radiationrads", 54 }, { "bloodymess", 55 },
            { "unarmeddamage", 56 }, { "assistance", 57 }, { "electricresistance", 58 },
            { "frostresistance", 59 }, { "energyresistance", 60 }, { "empresistance", 61 },
            { "variable01", 62 }, { "variable1", 62 }, { "var1medical", 62 },
            { "variable02", 63 }, { "variable2", 63 }, { "variable03", 64 }, { "variable3", 64 },
            { "variable04", 65 }, { "variable4", 65 }, { "variable05", 66 }, { "variable5", 66 },
            { "variable06", 67 }, { "variable6", 67 }, { "variable07", 68 }, { "variable7", 68 },
            { "variable08", 69 }, { "variable8", 69 }, { "variable09", 70 }, { "variable9", 70 },
            { "variable10", 71 }, { "ignorecrippledlimbs", 72 }, { "dehydration", 73 },
            { "hunger", 74 }, { "sleepdeprivation", 75 }, { "damagethreshold", 76 },
        } };

        const std::string normalized = normalizeActorValueName(name);
        const auto found = std::ranges::find_if(Values,
            [&](const auto& value) { return value.first == normalized; });
        return found != Values.end() ? std::optional<std::uint8_t>(found->second) : std::nullopt;
    }

    std::optional<float> getFalloutActorValue(const MWWorld::Ptr& actor, std::uint8_t actorValue,
        const MWWorld::FalloutPlayerRuntimeState* playerState)
    {
        if (actor.isEmpty() || !actor.getClass().isActor() || actorValue >= 96)
            return std::nullopt;

        if (playerState != nullptr)
        {
            if (actorValue == 23)
                if (const std::optional<float> karma = playerState->getKarma())
                    return karma;
            if (const std::optional<MWWorld::FalloutRuntimeActorValue> value
                = playerState->getCurrentActorValue(actorValue))
                return value->mValue;
            if (actorValue == 13)
                if (const std::optional<float> capacity = playerState->getCarryCapacity())
                    if (!actor.getClass().getCreatureStats(actor).getFalloutActorValueOverride(actorValue))
                        return capacity;
        }

        const CreatureStats& stats = actor.getClass().getCreatureStats(actor);
        if (const std::optional<float> value = stats.getFalloutActorValueOverride(actorValue))
            return value;
        return authoredFalloutActorValue(actor, stats, actorValue);
    }

    bool applyFalloutActorValue(const MWWorld::Ptr& actor, std::uint8_t actorValue,
        FalloutActorValueOperation operation, float value, MWWorld::FalloutPlayerRuntimeState* playerState)
    {
        if (actor.isEmpty() || !actor.getClass().isActor() || actorValue >= 96 || !std::isfinite(value))
            return false;

        CreatureStats& stats = actor.getClass().getCreatureStats(actor);
        const std::optional<float> current = getFalloutActorValue(actor, actorValue, playerState);
        if (!current)
            return false;
        float target = operation == FalloutActorValueOperation::Set ? value : *current + value;
        if (!std::isfinite(target))
            return false;

        if (actorValue >= 25 && actorValue <= 31)
        {
            float damage = stats.getFalloutLimbDamage(static_cast<std::int8_t>(actorValue));
            if (operation == FalloutActorValueOperation::Set)
                damage = std::max(0.f, 100.f - value);
            else
                damage = std::max(0.f, damage - value);
            return stats.setFalloutLimbDamage(static_cast<std::int8_t>(actorValue), damage);
        }

        if (actorValue == 54 && operation == FalloutActorValueOperation::Restore)
            target = std::max(0.f, *current - value);

        if (actorValue == 0)
        {
            stats.setAiSetting(AiSetting::Fight,
                std::clamp(static_cast<int>(std::lround(target)), 0, 3));
            return true;
        }
        if (actorValue == 1)
        {
            const int confidence = std::clamp(static_cast<int>(std::lround(target)), 0, 4);
            stats.setAiSetting(AiSetting::Flee, 100 - confidence);
            return true;
        }
        if (actorValue == 3)
        {
            if (target < static_cast<float>(std::numeric_limits<int>::min())
                || target > static_cast<float>(std::numeric_limits<int>::max()))
                return false;
            stats.setAiSetting(AiSetting::Alarm, static_cast<int>(std::lround(target)));
            return true;
        }

        if (const std::optional<ESM::RefId> attribute = attributeId(actorValue))
        {
            if (operation == FalloutActorValueOperation::Restore)
            {
                AttributeValue restored = stats.getAttribute(*attribute);
                restored.restore(value);
                target = restored.getModified();
                stats.setAttribute(*attribute, restored);
            }
            else
                stats.setAttribute(*attribute, target);

            if (playerState != nullptr
                && playerState->setCurrentActorValue(actorValue, target)
                    != MWWorld::FalloutActorValueMutationResult::Applied)
                return false;
            return true;
        }

        if (actorValue == 16 || actorValue == 22)
        {
            DynamicStat<float> dynamic = actorValue == 16 ? stats.getHealth() : stats.getFatigue();
            if (operation == FalloutActorValueOperation::Restore)
                dynamic.setCurrent(dynamic.getCurrent() + value);
            else
            {
                const float priorCurrent = dynamic.getCurrent();
                const float priorBase = dynamic.getBase();
                const float newBase = operation == FalloutActorValueOperation::Set
                    ? std::max(0.f, value)
                    : std::max(0.f, priorBase + value);
                const float newCurrent = operation == FalloutActorValueOperation::Set
                    ? newBase
                    : std::max(0.f, priorCurrent + value);
                dynamic.setBase(newBase);
                dynamic.setCurrent(newCurrent, false, true);
            }
            target = dynamic.getCurrent();
            if (actorValue == 16)
                stats.setHealth(dynamic);
            else
                stats.setFatigue(dynamic);

            if (playerState != nullptr && actorValue == 16
                && playerState->setCurrentActorValue(actorValue, target)
                    != MWWorld::FalloutActorValueMutationResult::Applied)
                return false;
            return true;
        }

        if (playerState != nullptr)
        {
            if (actorValue == 23)
                return playerState->setKarma(target);
            const MWWorld::FalloutActorValueMutationResult result
                = playerState->setCurrentActorValue(actorValue, target);
            if (result == MWWorld::FalloutActorValueMutationResult::Applied)
                return true;
            if (result != MWWorld::FalloutActorValueMutationResult::Unsupported)
                return false;
        }

        return stats.setFalloutActorValueOverride(actorValue, target);
    }

    std::vector<ESM4::ActorFaction> getFalloutActorFactions(const MWWorld::Ptr& actor,
        const MWWorld::FalloutPlayerRuntimeState* playerState)
    {
        if (actor.isEmpty() || !actor.getClass().isActor())
            return {};

        std::vector<ESM4::ActorFaction> authored;
        if (playerState != nullptr && playerState->getBaseState())
            authored = playerState->getBaseState()->mFactions;
        else if (actor.getType() == ESM4::Npc::sRecordId)
        {
            if (const ESM4::Npc* factions = MWClass::ESM4Npc::getFactionsRecord(actor))
                authored = factions->mFactions;
        }
        else if (actor.getType() == ESM4::Creature::sRecordId)
        {
            if (const ESM4::Creature* factions = MWClass::ESM4Creature::getFactionsRecord(actor))
            {
                authored = factions->mFactions;
                if (authored.empty() && factions->mFaction.faction != 0)
                    authored.push_back(factions->mFaction);
            }
        }

        std::map<ESM::FormId, ESM4::ActorFaction> merged;
        for (const ESM4::ActorFaction& membership : authored)
        {
            const ESM::FormId faction = ESM::FormId::fromUint32(membership.faction);
            if (!faction.isZeroOrUnset())
                merged.insert_or_assign(faction, membership);
        }

        const CreatureStats& stats = actor.getClass().getCreatureStats(actor);
        for (const auto& [faction, rank] : stats.getFalloutFactionOverrides())
        {
            if (rank == CreatureStats::FalloutFactionRemoved)
                merged.erase(faction);
            else
                merged.insert_or_assign(faction,
                    ESM4::ActorFaction{ faction.toUint32(), static_cast<std::int8_t>(rank), 0, 0, 0 });
        }

        std::vector<ESM4::ActorFaction> result;
        result.reserve(merged.size());
        for (const auto& [faction, membership] : merged)
        {
            (void)faction;
            result.push_back(membership);
        }
        return result;
    }

    FalloutFactionMembership getFalloutFactionMembership(const MWWorld::Ptr& actor,
        ESM::FormId faction, const MWWorld::FalloutPlayerRuntimeState* playerState)
    {
        if (faction.isZeroOrUnset())
            return {};
        const std::vector<ESM4::ActorFaction> factions = getFalloutActorFactions(actor, playerState);
        const auto found = std::ranges::find_if(factions, [&](const ESM4::ActorFaction& value) {
            return ESM::FormId::fromUint32(value.faction) == faction;
        });
        return found != factions.end() ? FalloutFactionMembership{ true, found->rank }
                                       : FalloutFactionMembership{};
    }

    bool setFalloutActorFaction(const MWWorld::Ptr& actor, ESM::FormId faction, std::optional<int> rank)
    {
        return !actor.isEmpty() && actor.getClass().isActor()
            && actor.getClass().getCreatureStats(actor).setFalloutFactionOverride(faction, rank);
    }

    bool getFalloutActorFlag(const MWWorld::Ptr& actor, FalloutActorFlag flag)
    {
        return !actor.isEmpty() && actor.getClass().isActor()
            && actor.getClass().getCreatureStats(actor).getFalloutRuntimeFlag(
                static_cast<std::uint32_t>(flag));
    }

    bool setFalloutActorFlag(const MWWorld::Ptr& actor, FalloutActorFlag flag, bool enabled)
    {
        return !actor.isEmpty() && actor.getClass().isActor()
            && actor.getClass().getCreatureStats(actor).setFalloutRuntimeFlag(
                static_cast<std::uint32_t>(flag), enabled);
    }
}
