#include "esm4dialogueutils.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#include <components/esm3/loadnpc.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrepu.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/esm4/script.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwclass/esm4npc.hpp"
#include "../mwclass/esm4creature.hpp"

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/falloutactorstate.hpp"

#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esm4questruntime.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/ptr.hpp"

namespace MWDialogue
{
    namespace
    {
        std::mutex sDialogueExpressionMutex;
        std::unordered_map<const void*, Esm4DialogueExpression> sDialogueExpressions;
    }

    void setEsm4DialogueExpression(const void* actorRef, std::uint32_t type, std::int32_t value)
    {
        if (actorRef == nullptr)
            return;
        std::lock_guard lock(sDialogueExpressionMutex);
        sDialogueExpressions[actorRef]
            = { type, std::clamp(static_cast<float>(value) / 100.f, 0.f, 1.f) };
    }

    std::optional<Esm4DialogueExpression> getEsm4DialogueExpression(const void* actorRef)
    {
        std::lock_guard lock(sDialogueExpressionMutex);
        const auto found = sDialogueExpressions.find(actorRef);
        return found == sDialogueExpressions.end() ? std::nullopt : std::optional(found->second);
    }

    std::optional<bool> evaluateEsm4ActorDialogueCondition(
        const ESM4::TargetCondition& condition, const MWWorld::Ptr& actor, bool isPlayer)
    {
        if (actor.isEmpty())
            return std::nullopt;

        const auto* actorRef
            = actor.getType() == ESM4::Npc::sRecordId ? actor.get<ESM4::Npc>() : nullptr;
        const auto* creatureRef
            = actor.getType() == ESM4::Creature::sRecordId ? actor.get<ESM4::Creature>() : nullptr;
        const ESM4::Npc* base = actorRef != nullptr ? actorRef->mBase : nullptr;
        const ESM4::Creature* creatureBase = creatureRef != nullptr ? creatureRef->mBase : nullptr;
        const ESM4::Npc* traits = actorRef != nullptr ? MWClass::ESM4Npc::getTraitsRecord(actor) : nullptr;
        const ESM4::Npc* stats = actorRef != nullptr ? MWClass::ESM4Npc::getStatsRecord(actor) : nullptr;
        const MWWorld::FalloutPlayerRuntimeState* playerState = nullptr;
        if (isPlayer)
            if (MWBase::World* const world = MWBase::Environment::tryGetWorld())
                playerState = &world->getFalloutPlayerRuntimeState();
        const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
        float actual = 0.f;
        switch (condition.functionIndex)
        {
            case ESM4::FUN_GetIsID:
                actual = (base != nullptr && base->mId == parameter)
                        || (creatureBase != nullptr && creatureBase->mId == parameter)
                        || actor.getCellRef().getRefId() == ESM::RefId(parameter)
                        || (isPlayer && (parameter.mIndex == 0x7 || parameter.mIndex == 0x14))
                    ? 1.f
                    : 0.f;
                break;
            case ESM4::FUN_GetIsReference:
                actual = actor.getCellRef().getRefNum() == parameter ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetIsRace:
                actual = traits != nullptr && traits->mRace == parameter ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetIsVoiceType:
                actual = traits != nullptr && traits->mVoiceType == parameter ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetIsClass:
                actual = stats != nullptr && stats->mClass == parameter ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetIsSex:
            case ESM4::FUN_GetPCIsSex:
                if (actorRef != nullptr)
                    actual = static_cast<std::uint32_t>(MWClass::ESM4Npc::isFemale(actor)) == condition.param1 ? 1.f : 0.f;
                else if (const auto* npc
                    = actor.getType() == ESM::NPC::sRecordId ? actor.get<ESM::NPC>() : nullptr)
                    actual = static_cast<std::uint32_t>((npc->mBase->mFlags & ESM::NPC::Female) != 0)
                            == condition.param1
                        ? 1.f
                        : 0.f;
                break;
            case ESM4::FUN_GetInFaction:
            case ESM4::FUN_GetFactionRank:
            {
                const MWMechanics::FalloutFactionMembership membership
                    = MWMechanics::getFalloutFactionMembership(actor, parameter, playerState);
                actual = condition.functionIndex == ESM4::FUN_GetInFaction
                    ? (membership.mMember ? 1.f : 0.f)
                    : (membership.mMember ? static_cast<float>(membership.mRank) : -1.f);
                break;
            }
            case ESM4::FUN_GetActorValue:
            case ESM4::FUN_GetBaseActorValue:
            case ESM4::FUN_GetPermanentActorValue:
            {
                if (condition.param1 >= MWWorld::FalloutPlayerRuntimeState::ActorValueCount)
                    return std::nullopt;
                const std::optional<float> value = MWMechanics::getFalloutActorValue(
                    actor, static_cast<std::uint8_t>(condition.param1), playerState);
                if (!value)
                    return std::nullopt;
                actual = *value;
                break;
            }
            case ESM4::FUN_GetInCell:
                actual = actor.isInCell() && actor.getCell()->getCell()->getId() == ESM::RefId(parameter) ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetInWorldspace:
                actual = actor.isInCell() && actor.getCell()->getCell()->getWorldSpace() == ESM::RefId(parameter)
                    ? 1.f
                    : 0.f;
                break;
            case ESM4::FUN_GetTalkedToPC:
                actual = actor.getClass().getCreatureStats(actor).hasTalkedToPlayer() ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetDead:
                actual = actor.getClass().getCreatureStats(actor).isDead() ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetDestroyed:
                actual = actor.getRefData().isDestroyed() ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetMapMarkerVisible:
            {
                MWBase::World* world = MWBase::Environment::tryGetWorld();
                if (world == nullptr)
                    return std::nullopt;
                const ESM::RefNum marker = actor.getCellRef().getRefNum();
                if (!marker.isSet())
                    return std::nullopt;
                actual = static_cast<float>(world->getFalloutMapMarkerState(marker));
                break;
            }
            case ESM4::FUN_GetLevel:
                actual = static_cast<float>(actor.getClass().getCreatureStats(actor).getLevel());
                break;
            case ESM4::FUN_GetHealthPercentage:
                actual = actor.getClass().getCreatureStats(actor).getHealth().getRatio();
                break;
            case ESM4::FUN_GetIsCreature:
                actual = actor.getClass().isActor() && !actor.getClass().isNpc() ? 1.f : 0.f;
                break;
            case ESM4::FUN_Exists:
                actual = 1.f;
                break;
            case ESM4::FUN_GetItemCount:
                try
                {
                    actual = static_cast<float>(actor.getClass().getContainerStore(actor).count(ESM::RefId(parameter)));
                }
                catch (const std::exception&)
                {
                    return std::nullopt;
                }
                break;
            case ESM4::FUN_HasPerk:
            {
                if (!isPlayer)
                    return std::nullopt;
                MWBase::World* world = MWBase::Environment::tryGetWorld();
                if (world == nullptr)
                    return std::nullopt;
                actual = world->getFalloutPlayerRuntimeState().hasPerk(parameter) ? 1.f : 0.f;
                break;
            }
            case ESM4::FUN_GetReputation:
            case ESM4::FUN_GetReputationPct:
            case ESM4::FUN_GetReputationThreshold:
            {
                if (!isPlayer)
                    return std::nullopt;
                MWBase::World* world = MWBase::Environment::tryGetWorld();
                if (world == nullptr)
                    return std::nullopt;
                const ESM4::Reputation* reputation
                    = world->getStore().get<ESM4::Reputation>().search(ESM::RefId(parameter));
                if (reputation == nullptr || condition.param2 > 2)
                    return std::nullopt;
                if (condition.functionIndex == ESM4::FUN_GetReputationThreshold)
                {
                    const std::optional<int> threshold
                        = world->getFalloutPlayerRuntimeState().getReputationThreshold(
                            parameter, reputation->mMaximum, condition.param2);
                    if (!threshold)
                        return std::nullopt;
                    actual = static_cast<float>(*threshold);
                    break;
                }
                if (condition.param2 > 1)
                    return std::nullopt;
                const std::optional<MWWorld::FalloutReputationValue> value
                    = world->getFalloutPlayerRuntimeState().getReputation(parameter);
                if (!value)
                    return std::nullopt;
                actual = condition.param2 == 0 ? value->mInfamy : value->mFame;
                if (condition.functionIndex == ESM4::FUN_GetReputationPct)
                    actual = std::clamp(actual / reputation->mMaximum, 0.f, 1.f);
                break;
            }
            case ESM4::FUN_GetEquipped:
                if (actorRef != nullptr)
                {
                    const ESM4::Weapon* weapon = MWClass::ESM4Npc::getEquippedWeapon(actor);
                    actual = weapon != nullptr && weapon->mId == parameter ? 1.f : 0.f;
                    if (actual == 0.f)
                        for (const ESM4::Armor* armor : MWClass::ESM4Npc::getEquippedArmor(actor))
                            if (armor != nullptr && armor->mId == parameter)
                                actual = 1.f;
                    if (actual == 0.f)
                        for (const ESM4::Clothing* clothing : MWClass::ESM4Npc::getEquippedClothing(actor))
                            if (clothing != nullptr && clothing->mId == parameter)
                                actual = 1.f;
                }
                break;
            default:
                return std::nullopt;
        }

        if ((condition.condition & ESM4::CTF_UseGlobal) != 0)
            return std::nullopt;
        const float expected = condition.comparison;
        switch (condition.condition & 0xe0)
        {
            case ESM4::CTF_EqualTo:
                return actual == expected;
            case ESM4::CTF_NotEqualTo:
                return actual != expected;
            case ESM4::CTF_GreaterThan:
                return actual > expected;
            case ESM4::CTF_GrThOrEqTo:
                return actual >= expected;
            case ESM4::CTF_LessThan:
                return actual < expected;
            case ESM4::CTF_LeThOrEqTo:
                return actual <= expected;
            default:
                return false;
        }
    }

    bool matchesEsm4DialogueConditions(
        const std::vector<ESM4::TargetCondition>& conditions, const Esm4DialogueConditionEvaluator& evaluate)
    {
        for (std::size_t i = 0; i < conditions.size(); ++i)
        {
            std::optional<bool> value = evaluate(conditions[i]);
            bool groupResult = value.value_or(false);
            while ((conditions[i].condition & ESM4::CTF_Combine) != 0 && i + 1 < conditions.size())
            {
                ++i;
                value = evaluate(conditions[i]);
                groupResult = groupResult || value.value_or(false);
            }
            if (!groupResult)
                return false;
        }
        return true;
    }

    bool matchesEsm4DialogueInfoConditions(const ESM4::DialogInfo& info, const ESM4::Quest* ownerQuest,
        const MWWorld::ESM4QuestState* ownerState, const Esm4DialogueConditionEvaluator& evaluate)
    {
        if (!info.mQuest.isZeroOrUnset())
        {
            if (ownerQuest == nullptr || ownerQuest->mId != info.mQuest || ownerState == nullptr
                || (ownerState->mFlags & MWWorld::ESM4QuestState::Flag_Running) == 0)
                return false;
            if (!matchesEsm4DialogueConditions(ownerQuest->mTargetConditions, evaluate))
                return false;
        }
        return matchesEsm4DialogueConditions(info.mTargetConditions, evaluate);
    }
}
