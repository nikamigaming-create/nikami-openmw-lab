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
#include <components/esm4/loadweap.hpp>
#include <components/esm4/script.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwclass/esm4npc.hpp"
#include "../mwclass/esm4creature.hpp"

#include "../mwmechanics/creaturestats.hpp"

#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esm4questruntime.hpp"
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
        sDialogueExpressions[actorRef] = { type, std::clamp(static_cast<float>(value) / 100.f, 0.f, 1.f) };
    }

    std::optional<Esm4DialogueExpression> getEsm4DialogueExpression(const void* actorRef)
    {
        std::lock_guard lock(sDialogueExpressionMutex);
        const auto found = sDialogueExpressions.find(actorRef);
        if (found == sDialogueExpressions.end())
            return std::nullopt;
        return found->second;
    }

    void Esm4DialoguePicker::clear()
    {
        mTopics.clear();
        mChoices.clear();
    }

    void Esm4DialoguePicker::clearTopics()
    {
        mTopics.clear();
    }

    void Esm4DialoguePicker::clearChoices()
    {
        mChoices.clear();
    }

    bool Esm4DialoguePicker::bindTopic(std::string_view title, Esm4DialogueSelection selection)
    {
        if (title.empty())
            return false;
        return mTopics.emplace(std::string(title), selection).second;
    }

    int Esm4DialoguePicker::bindChoice(Esm4DialogueSelection selection)
    {
        mChoices.push_back(selection);
        return static_cast<int>(mChoices.size() - 1);
    }

    std::optional<Esm4DialogueSelection> Esm4DialoguePicker::selectTopic(std::string_view title) const
    {
        if (hasChoices())
            return std::nullopt;
        const auto found = mTopics.find(title);
        if (found == mTopics.end())
            return std::nullopt;
        return found->second;
    }

    std::optional<Esm4DialogueSelection> Esm4DialoguePicker::selectChoice(int index) const
    {
        if (index < 0 || static_cast<std::size_t>(index) >= mChoices.size())
            return std::nullopt;
        return mChoices[static_cast<std::size_t>(index)];
    }

    bool matchesEsm4DialogueSelection(
        const Esm4DialogueSelection& selection, const ESM4::DialogInfo& info)
    {
        return info.mTopic == selection.mTopic && info.mId == selection.mInfo;
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
        const ESM4::Npc* factions
            = actorRef != nullptr ? MWClass::ESM4Npc::getFactionsRecord(actor) : nullptr;
        const ESM4::Npc* stats = actorRef != nullptr ? MWClass::ESM4Npc::getStatsRecord(actor) : nullptr;
        const ESM4::Creature* creatureFactions
            = creatureRef != nullptr ? MWClass::ESM4Creature::getFactionsRecord(actor) : nullptr;
        const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
        float actual = 0.f;
        switch (condition.functionIndex)
        {
            case ESM4::FUN_GetActorValue:
            {
                // Every GetActorValue CTDA owned by the Goodsprings dialogue quest runs on the Player. Its exact
                // operands cover SPECIAL and FNV skills (INT, Barter, Explosives, Medicine, Science, Sneak, Speech),
                // all of which are retained by the native player runtime state. Non-player and unsupported values
                // remain fail-closed rather than being projected through Morrowind stats.
                MWBase::World* world = isPlayer ? MWBase::Environment::tryGetWorld() : nullptr;
                const std::optional<MWWorld::FalloutRuntimeActorValue> value
                    = world != nullptr
                    ? world->getFalloutPlayerRuntimeState().getCurrentActorValue(condition.param1)
                    : std::nullopt;
                if (!value)
                    return std::nullopt;
                actual = value->mValue;
                break;
            }
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
            case ESM4::FUN_GetPCIsRace:
                actual = traits != nullptr && traits->mRace == parameter ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetIsVoiceType:
                actual = traits != nullptr && traits->mVoiceType == parameter ? 1.f : 0.f;
                break;
            case ESM4::FUN_GetIsClass:
            case ESM4::FUN_GetPCIsClass:
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
                if (condition.functionIndex == ESM4::FUN_GetFactionRank)
                    actual = -1.f;
                const std::vector<ESM4::ActorFaction>* actorFactions
                    = factions != nullptr ? &factions->mFactions : nullptr;
                if (isPlayer)
                {
                    MWBase::World* world = MWBase::Environment::tryGetWorld();
                    if (world != nullptr)
                    {
                        const std::optional<MWWorld::FalloutPlayerState>& state
                            = world->getFalloutPlayerRuntimeState().getBaseState();
                        if (state)
                            actorFactions = &state->mFactions;
                    }
                }
                if (actorFactions != nullptr)
                    for (const ESM4::ActorFaction& faction : *actorFactions)
                        if (ESM::FormId::fromUint32(faction.faction) == parameter)
                        {
                            actual = condition.functionIndex == ESM4::FUN_GetInFaction ? 1.f : faction.rank;
                            break;
                        }
                if (creatureFactions != nullptr)
                    for (const ESM4::ActorFaction& faction : creatureFactions->mFactions)
                        if (ESM::FormId::fromUint32(faction.faction) == parameter)
                        {
                            actual = condition.functionIndex == ESM4::FUN_GetInFaction ? 1.f : faction.rank;
                            break;
                        }
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
