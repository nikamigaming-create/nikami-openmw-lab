#include "dialoguemanagerimp.hpp"

#include <algorithm>
#include <iomanip>
#include <list>
#include <limits>
#include <optional>
#include <sstream>

#include <components/debug/debuglog.hpp>

#include <components/esm3/dialoguestate.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loaddial.hpp>
#include <components/esm3/loadfact.hpp>
#include <components/esm3/loadinfo.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/loaddial.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadsoun.hpp>
#include <components/esm4/loadsndr.hpp>
#include <components/esm4/script.hpp>

#include <components/compiler/errorhandler.hpp>
#include <components/compiler/exception.hpp>
#include <components/compiler/locals.hpp>
#include <components/compiler/output.hpp>
#include <components/compiler/scanner.hpp>
#include <components/compiler/scriptparser.hpp>

#include <components/interpreter/defines.hpp>
#include <components/interpreter/interpreter.hpp>

#include <components/misc/resourcehelpers.hpp>

#include <components/resource/resourcesystem.hpp>

#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/journal.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/scriptmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/esm4questruntime.hpp"

#include "../mwscript/compilercontext.hpp"
#include "../mwscript/extensions.hpp"
#include "../mwscript/interpretercontext.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "../mwclass/esm4npc.hpp"

#include "filter.hpp"
#include "esm4dialogueutils.hpp"
#include "hypertextparser.hpp"

namespace MWDialogue
{
    namespace
    {
        constexpr unsigned int maxEsm4SoundReferenceDepth = 8;

        std::string resolveEsm4SoundFile(const MWWorld::ESMStore& store, ESM::FormId id, unsigned int depth = 0)
        {
            if (id.isZeroOrUnset() || depth >= maxEsm4SoundReferenceDepth)
                return {};
            if (const ESM4::Sound* sound = store.get<ESM4::Sound>().search(ESM::RefId(id)))
                return sound->mSoundFile;
            if (const ESM4::SoundReference* sound = store.get<ESM4::SoundReference>().search(ESM::RefId(id)))
            {
                if (!sound->mSoundFile.empty())
                    return sound->mSoundFile;
                return resolveEsm4SoundFile(store, sound->mSoundId, depth + 1);
            }
            return {};
        }
    }

    DialogueManager::DialogueManager(
        const Compiler::Extensions& extensions, Translation::Storage& translationDataStorage)
        : mTranslationDataStorage(translationDataStorage)
        , mCompilerContext(MWScript::CompilerContext::Type_Dialogue)
        , mErrorHandler()
        , mTalkedTo(false)
        , mOriginalDisposition(0)
        , mCurrentDisposition(0)
        , mPermanentDispositionChange(0)
    {
        mChoice = -1;
        mIsInChoice = false;
        mGoodbye = false;
        mCompilerContext.setExtensions(&extensions);
    }

    void DialogueManager::clear()
    {
        mKnownTopics.clear();
        mTalkedTo = false;
        mOriginalDisposition = 0;
        mCurrentDisposition = 0;
        mPermanentDispositionChange = 0;
        mEsm4Dialogue = false;
        mLastEsm4Topic = {};
        mEsm4TopicIds.clear();
        mEsm4ChoiceSelections.clear();
        mEsm4SaidInfos.clear();
        mEsm4AddedTopics.clear();
        mEsm4VoicePaths.clear();
    }

    bool DialogueManager::matchesEsm4Info(const ESM4::DialogInfo& info) const
    {
        if (!info.mSpeaker.isZeroOrUnset())
        {
            const auto* actorRef
                = mActor.getType() == ESM4::Npc::sRecordId ? mActor.get<ESM4::Npc>() : nullptr;
            const ESM4::Npc* base = actorRef != nullptr ? actorRef->mBase : nullptr;
            if (base == nullptr || base->mId != info.mSpeaker)
                return false;
        }

        const auto evaluate = [&](const ESM4::TargetCondition& condition) -> std::optional<bool> {
            switch (condition.functionIndex)
            {
                case ESM4::FUN_GetQuestRunning:
                case ESM4::FUN_GetStage:
                case ESM4::FUN_GetStageDone:
                case ESM4::FUN_GetGlobalValue:
                case ESM4::FUN_GetQuestVariable:
                case ESM4::FUN_GetQuestCompleted:
                case ESM4::FUN_GetObjectiveCompleted:
                case ESM4::FUN_GetObjectiveDisplayed:
                    return MWBase::Environment::get().getWorld()->getESM4QuestRuntime().evaluateConditions({ condition });
                default:
                    break;
            }

            std::uint32_t runOn = condition.runOn;
            if (runOn == 0 && (condition.condition & ESM4::CTF_RunOnTarget) != 0)
                runOn = 1;

            MWWorld::Ptr actor;
            if (runOn == 0)
                actor = mActor;
            else if (runOn == 1)
                actor = MWBase::Environment::get().getWorld()->getPlayerPtr();
            else if (runOn == 2 && condition.reference != 0)
            {
                try
                {
                    actor = MWBase::Environment::get().getWorld()->searchPtr(
                        ESM::RefId(ESM::FormId::fromUint32(condition.reference)), false);
                }
                catch (const std::exception&)
                {
                }
            }
            else if (runOn == 3)
                mActor.getClass().getCreatureStats(mActor).getAiSequence().getCombatTarget(actor);
            if (actor.isEmpty())
                return std::nullopt;

            const auto* actorRef
                = actor.getType() == ESM4::Npc::sRecordId ? actor.get<ESM4::Npc>() : nullptr;
            const ESM4::Npc* base = actorRef != nullptr ? actorRef->mBase : nullptr;
            const ESM4::Npc* traits = actorRef != nullptr ? MWClass::ESM4Npc::getTraitsRecord(actor) : nullptr;
            const ESM4::Npc* factions
                = actorRef != nullptr ? MWClass::ESM4Npc::getFactionsRecord(actor) : nullptr;
            const ESM4::Npc* stats = actorRef != nullptr ? MWClass::ESM4Npc::getStatsRecord(actor) : nullptr;
            const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
            float actual = 0.f;
            switch (condition.functionIndex)
            {
                case ESM4::FUN_GetIsID:
                    actual = (base != nullptr && base->mId == parameter)
                            || actor.getCellRef().getRefId() == ESM::RefId(parameter)
                            || (actor == MWBase::Environment::get().getWorld()->getPlayerPtr()
                                && (parameter.mIndex == 0x7 || parameter.mIndex == 0x14))
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
                    if (actorRef != nullptr)
                        actual = static_cast<std::uint32_t>(MWClass::ESM4Npc::isFemale(actor)) == condition.param1 ? 1.f : 0.f;
                    else if (const auto* npc = actor.get<ESM::NPC>())
                        actual = static_cast<std::uint32_t>((npc->mBase->mFlags & ESM::NPC::Female) != 0)
                                == condition.param1
                            ? 1.f
                            : 0.f;
                    break;
                case ESM4::FUN_GetInFaction:
                case ESM4::FUN_GetFactionRank:
                    if (condition.functionIndex == ESM4::FUN_GetFactionRank)
                        actual = -1.f;
                    if (factions != nullptr)
                        for (const ESM4::ActorFaction& faction : factions->mFactions)
                            if (ESM::FormId::fromUint32(faction.faction) == parameter)
                            {
                                actual = condition.functionIndex == ESM4::FUN_GetInFaction ? 1.f : faction.rank;
                                break;
                            }
                    break;
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
            float expected = condition.comparison;
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
        };

        for (std::size_t i = 0; i < info.mTargetConditions.size(); ++i)
        {
            std::optional<bool> value = evaluate(info.mTargetConditions[i]);
            bool groupResult = value.value_or(false);
            while ((info.mTargetConditions[i].condition & ESM4::CTF_Combine) != 0
                && i + 1 < info.mTargetConditions.size())
            {
                ++i;
                value = evaluate(info.mTargetConditions[i]);
                groupResult = groupResult || value.value_or(false);
            }
            if (!groupResult)
                return false;
        }
        return true;
    }

    int DialogueManager::getEsm4InfoActorAffinity(const ESM4::DialogInfo& info) const
    {
        const ESM4::Npc* base = mActor.get<ESM4::Npc>()->mBase;
        const ESM4::Npc* traits = MWClass::ESM4Npc::getTraitsRecord(mActor);
        int affinity = 0;
        if (base != nullptr && !info.mSpeaker.isZeroOrUnset() && info.mSpeaker == base->mId)
            affinity += 10000;
        for (const ESM4::TargetCondition& condition : info.mTargetConditions)
        {
            const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
            if (condition.functionIndex == ESM4::FUN_GetIsID && base != nullptr && parameter == base->mId)
                affinity += 10000;
            else if (condition.functionIndex == ESM4::FUN_GetIsRace && traits != nullptr
                && parameter == traits->mRace)
                affinity += 1000;
        }
        return affinity;
    }

    const ESM4::DialogInfo* DialogueManager::selectEsm4Info(ESM::FormId topic) const
    {
        const ESM4::DialogInfo* selected = nullptr;
        const auto& infos = MWBase::Environment::get().getESMStore()->get<ESM4::DialogInfo>();
        for (const ESM4::DialogInfo& info : infos)
        {
            if (info.mTopic != topic || !matchesEsm4Info(info))
                continue;
            if ((info.mInfoFlags & ESM4::INFO_SayOnce) != 0 && mEsm4SaidInfos.contains(info.mId))
                continue;
            // The ESM4 store preserves physical topic-child order. Fallout
            // uses that order as response priority when multiple INFOs pass;
            // FormID order is unrelated and reverses real topics such as
            // Chet's reputation-specific barter response and its fallback.
            // Actor affinity remains an explicit override for shared/generic
            // responses. Equal-affinity candidates retain authored order.
            const int affinity = getEsm4InfoActorAffinity(info);
            const int selectedAffinity = selected == nullptr ? -1 : getEsm4InfoActorAffinity(*selected);
            if (selected == nullptr || affinity > selectedAffinity)
                selected = &info;
        }
        return selected;
    }

    void DialogueManager::updateEsm4Topics()
    {
        mEsm4TopicIds.clear();
        const auto& dialogues = MWBase::Environment::get().getESMStore()->get<ESM4::Dialogue>();
        for (const ESM4::Dialogue& dialogue : dialogues)
        {
            if (dialogue.mDialType != 0 || (dialogue.mDialFlags & 0x02) == 0
                || Misc::StringUtils::ciEqual(dialogue.mEditorId, "GREETING")
                || Misc::StringUtils::ciEqual(dialogue.mEditorId, "GOODBYE"))
                continue;
            const ESM4::DialogInfo* info = selectEsm4Info(dialogue.mId);
            if (info == nullptr
                || (getEsm4InfoActorAffinity(*info) == 0 && !mEsm4AddedTopics.contains(dialogue.mId)))
                continue;
            const std::string_view title = getEsm4DialoguePrompt(dialogue, *info);
            if (!title.empty())
            {
                mEsm4TopicIds.emplace(std::string(title), dialogue.mId);
                if (std::getenv("OPENMW_PROOF_DIALOGUE_TOPIC") != nullptr)
                    Log(Debug::Info) << "FNV/ESM4 dialogue: available topic=\"" << title << "\" form="
                                     << ESM::RefId(dialogue.mId) << " info=" << ESM::RefId(info->mId);
            }
        }
    }

    std::string DialogueManager::resolveEsm4Voice(
        const ESM4::DialogInfo& info, const ESM4::DialogResponse& response, std::size_t responseIndex)
    {
        const std::uint32_t authoredResponseNumber = response.mData.responseNo;
        const std::uint32_t responseNumber = authoredResponseNumber != 0 ? authoredResponseNumber
                                                                         : static_cast<std::uint32_t>(responseIndex + 1);
        const auto key = std::pair{ info.mId, responseNumber };
        if (const auto found = mEsm4VoicePaths.find(key); found != mEsm4VoicePaths.end())
            return found->second;

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        std::string soundFile;
        if (response.mData.sound != 0)
            soundFile = resolveEsm4SoundFile(store, ESM::FormId::fromUint32(response.mData.sound));
        if (soundFile.empty() && !info.mSound.isZeroOrUnset())
            soundFile = resolveEsm4SoundFile(store, info.mSound);

        std::string path;
        if (!soundFile.empty())
            path = Misc::ResourceHelpers::correctResourcePath({ { "sound" } }, soundFile, vfs);

        if (path.empty() || !vfs->exists(VFS::Path::Normalized(path)))
        {
            std::ostringstream suffix;
            suffix << '_' << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << info.mId.mIndex
                   << '_' << std::dec << responseNumber;
            const std::string stem = suffix.str();
            for (const VFS::Path::Normalized& candidate : vfs->getRecursiveDirectoryIterator("sound/voice"))
            {
                const std::string_view value = candidate.view();
                if ((value.ends_with(stem + ".ogg") || value.ends_with(stem + ".mp3")
                        || value.ends_with(stem + ".wav")))
                {
                    path = candidate.value();
                    break;
                }
            }
        }

        mEsm4VoicePaths.emplace(key, path);
        return path;
    }

    void DialogueManager::executeEsm4Topic(
        ESM::FormId topic, ResponseCallback* callback, bool greeting, const ESM4::DialogInfo* retainedInfo)
    {
        const ESM4::Dialogue* dialogue
            = MWBase::Environment::get().getESMStore()->get<ESM4::Dialogue>().search(ESM::RefId(topic));
        const ESM4::DialogInfo* info = retainedInfo != nullptr ? retainedInfo : selectEsm4Info(topic);
        if (dialogue == nullptr || info == nullptr)
            return;

        std::vector<const ESM4::DialogResponse*> orderedResponses;
        orderedResponses.reserve(info->mResponses.size());
        for (const ESM4::DialogResponse& item : info->mResponses)
            orderedResponses.push_back(&item);
        std::stable_sort(orderedResponses.begin(), orderedResponses.end(),
            [](const ESM4::DialogResponse* left, const ESM4::DialogResponse* right) {
                const std::uint32_t leftAuthoredNumber = left->mData.responseNo;
                const std::uint32_t rightAuthoredNumber = right->mData.responseNo;
                const std::uint32_t leftNumber = leftAuthoredNumber != 0 ? leftAuthoredNumber
                                                                         : std::numeric_limits<std::uint32_t>::max();
                const std::uint32_t rightNumber = rightAuthoredNumber != 0 ? rightAuthoredNumber
                                                                           : std::numeric_limits<std::uint32_t>::max();
                return leftNumber < rightNumber;
            });

        std::string response;
        for (const ESM4::DialogResponse* item : orderedResponses)
        {
            if (item->mResponse.empty())
                continue;
            if (!response.empty())
                response += "\n";
            response += item->mResponse;
        }
        if (response.empty())
            response = info->mResponse;

        callback->addResponse(greeting ? std::string_view{} : getEsm4DialoguePrompt(*dialogue, *info), response);
        mLastEsm4Topic = topic;
        if ((info->mInfoFlags & ESM4::INFO_SayOnce) != 0)
            mEsm4SaidInfos.insert(info->mId);

        for (ESM::FormId addedTopic : info->mAddTopics)
            mEsm4AddedTopics.insert(addedTopic);

        std::vector<VFS::Path::Normalized> voices;
        voices.reserve(orderedResponses.size());
        for (std::size_t i = 0; i < orderedResponses.size(); ++i)
        {
            const ESM4::DialogResponse& item = *orderedResponses[i];
            const std::string voice = resolveEsm4Voice(*info, item, i);
            if (voice.empty())
                continue;
            Log(Debug::Info) << "FNV/ESM4 dialogue: resolved authored voice info=" << ESM::RefId(info->mId)
                             << " response="
                             << (item.mData.responseNo != 0 ? item.mData.responseNo : i + 1)
                             << " path=\"" << voice << "\"";
            voices.emplace_back(voice);
        }
        if (!voices.empty())
            MWBase::Environment::get().getSoundManager()->saySequence(mActor, voices);

        mChoices.clear();
        mEsm4ChoiceSelections.clear();
        for (ESM::FormId choice : info->mChoices)
        {
            const ESM4::Dialogue* choiceDialogue
                = MWBase::Environment::get().getESMStore()->get<ESM4::Dialogue>().search(ESM::RefId(choice));
            const ESM4::DialogInfo* choiceInfo = selectEsm4Info(choice);
            if (choiceDialogue == nullptr || choiceInfo == nullptr)
                continue;
            const std::string_view title = getEsm4DialoguePrompt(*choiceDialogue, *choiceInfo);
            mEsm4ChoiceSelections.emplace_back(choice, choiceInfo->mId);
            mChoices.emplace_back(std::string(title), static_cast<int>(mEsm4ChoiceSelections.size() - 1));
        }
        mIsInChoice = !mChoices.empty();
        if ((info->mInfoFlags & ESM4::INFO_Goodbye) != 0)
            goodbye();

        MWWorld::ESM4QuestRuntime& questRuntime
            = MWBase::Environment::get().getWorld()->getESM4QuestRuntime();
        const std::size_t unsupportedBefore = questRuntime.getUnsupportedStageCommands().size();
        if (!info->mScript.scriptSource.empty())
            questRuntime.executeResultSource(info->mScript.scriptSource);
        if (!info->mEndScript.scriptSource.empty())
            questRuntime.executeResultSource(info->mEndScript.scriptSource);
        const std::size_t unsupportedAfter = questRuntime.getUnsupportedStageCommands().size();
        if (!info->mScript.scriptSource.empty() || !info->mEndScript.scriptSource.empty()
            || !info->mScript.compiledData.empty() || !info->mEndScript.compiledData.empty())
            Log(Debug::Info) << "FNV/ESM4 dialogue: executed result source info=" << ESM::RefId(info->mId)
                             << " beginSource=" << info->mScript.scriptSource.size()
                             << " endSource=" << info->mEndScript.scriptSource.size()
                             << " unsupportedAdded=" << (unsupportedAfter - unsupportedBefore)
                             << " compiledOnly="
                             << ((!info->mScript.compiledData.empty() && info->mScript.scriptSource.empty())
                                     || (!info->mEndScript.compiledData.empty() && info->mEndScript.scriptSource.empty()));

        Log(Debug::Info) << "FNV/ESM4 dialogue: selected topic=" << dialogue->mEditorId
                         << " topicForm=" << ESM::RefId(dialogue->mId) << " info=" << ESM::RefId(info->mId)
                         << " responses=" << info->mResponses.size() << " choices=" << mChoices.size();
    }

    void DialogueManager::addTopic(const ESM::RefId& topic)
    {
        mKnownTopics.insert(topic);
    }

    std::vector<ESM::RefId> DialogueManager::parseTopicIdsFromText(const std::string& text)
    {
        std::vector<ESM::RefId> topicIdList;

        std::vector<HyperTextParser::Token> hypertext = HyperTextParser::parseHyperText(text);

        for (std::vector<HyperTextParser::Token>::iterator tok = hypertext.begin(); tok != hypertext.end(); ++tok)
        {
            std::string topicId = Misc::StringUtils::lowerCase(tok->mText);

            if (tok->isExplicitLink())
            {
                // calculation of standard form for all hyperlinks
                size_t asteriskCount = HyperTextParser::removePseudoAsterisks(topicId);
                for (; asteriskCount > 0; --asteriskCount)
                    topicId.append("*");

                topicId = mTranslationDataStorage.topicStandardForm(topicId);
            }

            topicIdList.push_back(ESM::RefId::stringRefId(topicId));
        }

        return topicIdList;
    }

    void DialogueManager::addTopicsFromText(const std::string& text)
    {
        updateActorKnownTopics();

        for (const auto& topicId : parseTopicIdsFromText(text))
        {
            if (mActorKnownTopics.count(topicId))
                mKnownTopics.insert(topicId);
        }
    }

    void DialogueManager::updateOriginalDisposition()
    {
        if (mActor.getClass().isNpc())
        {
            const auto& stats = mActor.getClass().getNpcStats(mActor);
            // Disposition changed by script; discard our preconceived notions
            if (stats.getBaseDisposition() != mCurrentDisposition)
            {
                mCurrentDisposition = stats.getBaseDisposition();
                mOriginalDisposition = mCurrentDisposition;
            }
        }
    }

    bool DialogueManager::startDialogue(const MWWorld::Ptr& actor, ResponseCallback* callback)
    {
        updateGlobals();

        // Dialogue with dead actor (e.g. through script) should not be allowed.
        if (actor.getClass().getCreatureStats(actor).isDead())
            return false;

        mLastTopic = ESM::RefId();
        // Note that we intentionally don't reset mPermanentDispositionChange

        mChoice = -1;
        mIsInChoice = false;
        mGoodbye = false;
        mChoices.clear();

        mActor = actor;

        MWMechanics::CreatureStats& creatureStats = actor.getClass().getCreatureStats(actor);
        mTalkedTo = creatureStats.hasTalkedToPlayer();

        mActorKnownTopics.clear();

        mEsm4Dialogue = actor.getType() == ESM4::Npc::sRecordId;
        mLastEsm4Topic = {};
        mEsm4TopicIds.clear();
        mEsm4ChoiceSelections.clear();
        if (mEsm4Dialogue)
        {
            const auto& esm4Dialogues = MWBase::Environment::get().getESMStore()->get<ESM4::Dialogue>();
            const ESM4::Dialogue* greeting = nullptr;
            for (const ESM4::Dialogue& dialogue : esm4Dialogues)
            {
                if (Misc::StringUtils::ciEqual(dialogue.mEditorId, "GREETING"))
                {
                    greeting = &dialogue;
                    break;
                }
            }
            if (greeting == nullptr || selectEsm4Info(greeting->mId) == nullptr)
                return false;

            executeEsm4Topic(greeting->mId, callback, true);
            creatureStats.talkedToPlayer();
            mTalkedTo = true;
            updateEsm4Topics();
            return true;
        }

        // greeting
        const MWWorld::Store<ESM::Dialogue>& dialogs = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>();

        Filter filter(actor, mChoice, mTalkedTo);

        for (const ESM::Dialogue& dialogue : dialogs)
        {
            if (dialogue.mType == ESM::Dialogue::Greeting)
            {
                // Search a response (we do not accept a fallback to "Info refusal" here)
                if (const ESM::DialInfo* info = filter.search(dialogue, false).second)
                {
                    creatureStats.talkedToPlayer();

                    if (!info->mSound.empty())
                    {
                        Log(Debug::Info) << "FNV/ESM4 dialogue: playing greeting voice \"" << info->mSound << "\"";
                        MWBase::Environment::get().getSoundManager()->say(
                            actor, Misc::ResourceHelpers::correctSoundPath(VFS::Path::Normalized(info->mSound)));
                    }

                    MWScript::InterpreterContext interpreterContext(&mActor.getRefData().getLocals(), mActor);
                    callback->addResponse({}, Interpreter::fixDefinesDialog(info->mResponse, interpreterContext));
                    executeScript(info->mResultScript, mActor);
                    mLastTopic = dialogue.mId;

                    addTopicsFromText(info->mResponse);

                    return true;
                }
            }
        }
        return false;
    }

    std::optional<Interpreter::Program> DialogueManager::compile(const std::string& cmd, const MWWorld::Ptr& actor)
    {
        bool success = true;
        std::optional<Interpreter::Program> program;

        try
        {
            mErrorHandler.reset();

            mErrorHandler.setContext("[dialogue script]");

            std::istringstream input(cmd + "\n");

            Compiler::Scanner scanner(mErrorHandler, input, mCompilerContext.getExtensions());

            Compiler::Locals locals;

            const ESM::RefId& actorScript = actor.getClass().getScript(actor);

            if (!actorScript.empty())
            {
                // grab local variables from actor's script, if available.
                locals = MWBase::Environment::get().getScriptManager()->getLocals(actorScript);
            }

            Compiler::ScriptParser parser(mErrorHandler, mCompilerContext, locals, false);

            scanner.scan(parser);

            if (!mErrorHandler.isGood())
                success = false;

            if (success)
                program = parser.getProgram();
        }
        catch (const Compiler::SourceException& /* error */)
        {
            // error has already been reported via error handler
            success = false;
        }
        catch (const std::exception& error)
        {
            Log(Debug::Error) << std::string("Dialogue error: An exception has been thrown: ") + error.what();
            success = false;
        }

        if (!success)
        {
            Log(Debug::Error) << "Error: compiling failed (dialogue script): \n" << cmd << "\n";
        }

        return program;
    }

    void DialogueManager::executeScript(const std::string& script, const MWWorld::Ptr& actor)
    {
        if (const std::optional<Interpreter::Program> program = compile(script, actor))
        {
            try
            {
                MWScript::InterpreterContext interpreterContext(&actor.getRefData().getLocals(), actor);
                Interpreter::Interpreter interpreter;
                MWScript::installOpcodes(interpreter);
                interpreter.run(*program, interpreterContext);
            }
            catch (const std::exception& error)
            {
                Log(Debug::Error) << std::string("Dialogue error: An exception has been thrown: ") + error.what();
            }
        }
    }

    bool DialogueManager::inJournal(const ESM::RefId& topicId, const ESM::RefId& infoId) const
    {
        MWBase::Journal* journal = MWBase::Environment::get().getJournal();
        const auto topic = journal->getTopics().find(topicId);
        if (topic != journal->getTopics().end())
        {
            return std::ranges::find_if(topic->second, [&](const MWDialogue::Entry& entry) {
                return entry.mInfoId == infoId;
            }) != topic->second.end();
        }
        return false;
    }

    void DialogueManager::executeTopic(const ESM::RefId& topic, ResponseCallback* callback)
    {
        Filter filter(mActor, mChoice, mTalkedTo);

        const MWWorld::Store<ESM::Dialogue>& dialogues = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>();

        const ESM::Dialogue& dialogue = *dialogues.find(topic);

        const auto [responseTopic, info] = filter.search(dialogue, true);

        if (info)
        {
            std::string_view title;
            if (dialogue.mType == ESM::Dialogue::Persuasion)
            {
                // Determine GMST from dialogue topic. GMSTs are:
                // sAdmireSuccess, sAdmireFail, sIntimidateSuccess, sIntimidateFail,
                // sTauntSuccess, sTauntFail, sBribeSuccess, sBribeFail
                std::string modifiedTopic = "s" + topic.getRefIdString();

                modifiedTopic.erase(std::remove(modifiedTopic.begin(), modifiedTopic.end(), ' '), modifiedTopic.end());

                const MWWorld::Store<ESM::GameSetting>& gmsts
                    = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();

                title = gmsts.find(modifiedTopic)->mValue.getString();
            }
            else
                title = dialogue.mStringId;

            MWScript::InterpreterContext interpreterContext(&mActor.getRefData().getLocals(), mActor);
            callback->addResponse(title, Interpreter::fixDefinesDialog(info->mResponse, interpreterContext));

            if (dialogue.mType == ESM::Dialogue::Topic)
            {
                // Make sure the returned DialInfo is from the Dialogue we supplied. If could also be from the Info
                // refusal group, in which case it should not be added to the journal.
                if (responseTopic == &dialogue)
                    MWBase::Environment::get().getJournal()->addTopic(topic, info->mId, mActor);
            }

            mLastTopic = topic;

            executeScript(info->mResultScript, mActor);

            addTopicsFromText(info->mResponse);
        }
    }

    const ESM::Dialogue* DialogueManager::searchDialogue(const ESM::RefId& id)
    {
        return MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>().search(id);
    }

    void DialogueManager::updateGlobals()
    {
        MWBase::Environment::get().getWorld()->updateDialogueGlobals();
    }

    void DialogueManager::updateActorKnownTopics()
    {
        updateGlobals();

        mActorKnownTopics.clear();

        const auto& dialogs = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>();

        Filter filter(mActor, -1, mTalkedTo);

        for (const auto& dialog : dialogs)
        {
            if (dialog.mType == ESM::Dialogue::Topic)
            {
                const auto* answer = filter.search(dialog, true).second;
                const auto& topicId = dialog.mId;

                if (answer != nullptr)
                {
                    int topicFlags = 0;
                    if (!inJournal(topicId, answer->mId))
                    {
                        // Does this dialogue contains some actor-specific answer?
                        if (answer->mActor == mActor.getCellRef().getRefId())
                            topicFlags |= MWBase::DialogueManager::TopicType::Specific;
                    }
                    else
                        topicFlags |= MWBase::DialogueManager::TopicType::Exhausted;
                    mActorKnownTopics.insert(std::make_pair(dialog.mId, ActorKnownTopicInfo{ topicFlags, answer }));
                }
            }
        }

        // If response to a topic leads to a new topic, the original topic is not exhausted.

        for (auto& [dialogId, topicInfo] : mActorKnownTopics)
        {
            // If the topic is not marked as exhausted, we don't need to do anything about it.
            // If the topic will not be shown to the player, the flag actually does not matter.

            if (!(topicInfo.mFlags & MWBase::DialogueManager::TopicType::Exhausted) || !mKnownTopics.count(dialogId))
                continue;

            for (const auto& topicId : parseTopicIdsFromText(topicInfo.mInfo->mResponse))
            {
                if (mActorKnownTopics.count(topicId) && !mKnownTopics.count(topicId))
                {
                    topicInfo.mFlags &= ~MWBase::DialogueManager::TopicType::Exhausted;
                    break;
                }
            }
        }
    }

    std::list<std::string> DialogueManager::getAvailableTopics()
    {
        if (mEsm4Dialogue)
        {
            updateEsm4Topics();
            std::list<std::string> result;
            for (const auto& [title, _] : mEsm4TopicIds)
                result.push_back(title);
            return result;
        }
        updateActorKnownTopics();

        std::list<std::string> keywordList;
        const auto& store = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>();
        for (const auto& [topic, topicInfo] : mActorKnownTopics)
        {
            // does the player know the topic?
            if (mKnownTopics.contains(topic))
                keywordList.push_back(store.find(topic)->mStringId);
        }

        // sort again, because the previous sort was case-sensitive
        keywordList.sort(Misc::StringUtils::ciLess);
        return keywordList;
    }

    int DialogueManager::getTopicFlag(const ESM::RefId& topicId) const
    {
        if (mEsm4Dialogue)
            return 0;
        auto known = mActorKnownTopics.find(topicId);
        if (known != mActorKnownTopics.end())
            return known->second.mFlags;
        return 0;
    }

    void DialogueManager::keywordSelected(std::string_view keyword, ResponseCallback* callback)
    {
        if (mEsm4Dialogue)
        {
            if (const auto found = mEsm4TopicIds.find(keyword); found != mEsm4TopicIds.end())
                executeEsm4Topic(found->second, callback);
            updateEsm4Topics();
            return;
        }
        if (!mIsInChoice)
        {
            const ESM::Dialogue* dialogue = searchDialogue(ESM::RefId::stringRefId(keyword));
            if (dialogue && dialogue->mType == ESM::Dialogue::Topic)
            {
                executeTopic(dialogue->mId, callback);
            }
        }
    }

    bool DialogueManager::isInChoice() const
    {
        return mIsInChoice;
    }

    void DialogueManager::goodbyeSelected()
    {
        // Apply disposition change to NPC's base disposition if we **think** we need to change something
        if ((mPermanentDispositionChange || mOriginalDisposition != mCurrentDisposition) && mActor.getClass().isNpc())
        {
            updateOriginalDisposition();
            MWMechanics::NpcStats& npcStats = mActor.getClass().getNpcStats(mActor);

            // Get the sum of disposition effects minus charm (shouldn't be made permanent)
            npcStats.setBaseDisposition(0);
            int zero = MWBase::Environment::get().getMechanicsManager()->getDerivedDisposition(mActor, false)
                - npcStats.getMagicEffects().getOrDefault(ESM::MagicEffect::Charm).getMagnitude();

            // Clamp new permanent disposition to avoid negative derived disposition (can be caused by intimidate)
            int disposition = std::clamp(mOriginalDisposition + mPermanentDispositionChange, -zero, 100 - zero);
            npcStats.setBaseDisposition(disposition);
        }
        mPermanentDispositionChange = 0;
        mOriginalDisposition = 0;
        mCurrentDisposition = 0;
    }

    void DialogueManager::questionAnswered(int answer, ResponseCallback* callback)
    {
        if (mEsm4Dialogue)
        {
            if (answer >= 0 && static_cast<std::size_t>(answer) < mEsm4ChoiceSelections.size())
            {
                const auto [topic, infoId] = mEsm4ChoiceSelections[answer];
                const ESM4::DialogInfo* info
                    = MWBase::Environment::get().getESMStore()->get<ESM4::DialogInfo>().search(ESM::RefId(infoId));
                mChoices.clear();
                mEsm4ChoiceSelections.clear();
                mIsInChoice = false;
                if (info != nullptr && info->mTopic == topic)
                    executeEsm4Topic(topic, callback, false, info);
            }
            updateEsm4Topics();
            return;
        }
        mChoice = answer;

        const ESM::Dialogue* dialogue = searchDialogue(mLastTopic);
        if (dialogue)
        {
            Filter filter(mActor, mChoice, mTalkedTo);

            if (dialogue->mType == ESM::Dialogue::Topic || dialogue->mType == ESM::Dialogue::Greeting)
            {
                const auto [responseTopic, info] = filter.search(*dialogue, true);
                if (info)
                {
                    const std::string& text = info->mResponse;
                    addTopicsFromText(text);

                    mChoice = -1;
                    mIsInChoice = false;
                    mChoices.clear();

                    MWScript::InterpreterContext interpreterContext(&mActor.getRefData().getLocals(), mActor);
                    callback->addResponse({}, Interpreter::fixDefinesDialog(text, interpreterContext));

                    if (dialogue->mType == ESM::Dialogue::Topic)
                    {
                        // Make sure the returned DialInfo is from the Dialogue we supplied. If could also be from the
                        // Info refusal group, in which case it should not be added to the journal
                        if (responseTopic == dialogue)
                            MWBase::Environment::get().getJournal()->addTopic(mLastTopic, info->mId, mActor);
                    }

                    executeScript(info->mResultScript, mActor);
                }
                else
                {
                    mChoice = -1;
                    mIsInChoice = false;
                    mChoices.clear();
                }
            }
        }

        updateActorKnownTopics();
    }

    void DialogueManager::addChoice(std::string_view text, int choice)
    {
        mIsInChoice = true;
        mChoices.emplace_back(text, choice);
    }

    const std::vector<std::pair<std::string, int>>& DialogueManager::getChoices() const
    {
        return mChoices;
    }

    bool DialogueManager::isGoodbye() const
    {
        return mGoodbye;
    }

    void DialogueManager::goodbye()
    {
        mIsInChoice = false;
        mGoodbye = true;
    }

    void DialogueManager::persuade(int type, ResponseCallback* callback)
    {
        bool success;
        int temp, perm;
        MWBase::Environment::get().getMechanicsManager()->getPersuasionDispositionChange(
            mActor, MWBase::MechanicsManager::PersuasionType(type), success, temp, perm);
        updateOriginalDisposition();
        if (temp > 0 && perm > 0 && mOriginalDisposition + perm + mPermanentDispositionChange < 0)
            perm = -(mOriginalDisposition + mPermanentDispositionChange);
        mCurrentDisposition += temp;
        mActor.getClass().getNpcStats(mActor).setBaseDisposition(mCurrentDisposition);
        mPermanentDispositionChange += perm;

        MWWorld::Ptr player = MWMechanics::getPlayer();
        player.getClass().skillUsageSucceeded(
            player, ESM::Skill::Speechcraft, success ? ESM::Skill::Speechcraft_Success : ESM::Skill::Speechcraft_Fail);

        if (success)
        {
            int gold = 0;
            if (type == MWBase::MechanicsManager::PT_Bribe10)
                gold = 10;
            else if (type == MWBase::MechanicsManager::PT_Bribe100)
                gold = 100;
            else if (type == MWBase::MechanicsManager::PT_Bribe1000)
                gold = 1000;

            if (gold)
            {
                player.getClass().getContainerStore(player).remove(MWWorld::ContainerStore::sGoldId, gold);
                mActor.getClass().getContainerStore(mActor).add(MWWorld::ContainerStore::sGoldId, gold);
            }
        }

        std::string text;

        if (type == MWBase::MechanicsManager::PT_Admire)
            text = "Admire";
        else if (type == MWBase::MechanicsManager::PT_Taunt)
            text = "Taunt";
        else if (type == MWBase::MechanicsManager::PT_Intimidate)
            text = "Intimidate";
        else
        {
            text = "Bribe";
        }

        executeTopic(ESM::RefId::stringRefId(text + (success ? " Success" : " Fail")), callback);
    }

    void DialogueManager::applyBarterDispositionChange(int delta)
    {
        if (!mActor.isEmpty() && mActor.getClass().isNpc())
        {
            updateOriginalDisposition();
            mCurrentDisposition += delta;
            mActor.getClass().getNpcStats(mActor).setBaseDisposition(mCurrentDisposition);
            if (Settings::game().mBarterDispositionChangeIsPermanent)
                mPermanentDispositionChange += delta;
        }
    }

    bool DialogueManager::checkServiceRefused(ResponseCallback* callback, ServiceType service)
    {
        Filter filter(mActor, service, mTalkedTo);

        const MWWorld::Store<ESM::Dialogue>& dialogues = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>();

        const ESM::Dialogue& dialogue = *dialogues.find(ESM::RefId::stringRefId("Service Refusal"));

        std::vector<Filter::Response> infos = filter.list(dialogue, false, false, true);
        if (!infos.empty())
        {
            const ESM::DialInfo* info = infos[0].second;

            addTopicsFromText(info->mResponse);

            const MWWorld::Store<ESM::GameSetting>& gmsts
                = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();

            MWScript::InterpreterContext interpreterContext(&mActor.getRefData().getLocals(), mActor);

            callback->addResponse(gmsts.find("sServiceRefusal")->mValue.getString(),
                Interpreter::fixDefinesDialog(info->mResponse, interpreterContext));

            executeScript(info->mResultScript, mActor);
            return true;
        }
        return false;
    }

    bool DialogueManager::say(const MWWorld::Ptr& actor, const ESM::RefId& topic)
    {
        MWBase::SoundManager* sndMgr = MWBase::Environment::get().getSoundManager();
        if (sndMgr->sayActive(actor))
        {
            // Actor is already saying something.
            return false;
        }

        if (actor.getClass().isNpc() && MWBase::Environment::get().getWorld()->isSwimming(actor))
        {
            // NPCs don't talk while submerged
            return false;
        }

        if (actor.getClass().getCreatureStats(actor).getKnockedDown())
        {
            // Unconscious actors can not speak
            return false;
        }

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Dialogue* dial = store.get<ESM::Dialogue>().find(topic);

        const MWMechanics::CreatureStats& creatureStats = actor.getClass().getCreatureStats(actor);
        Filter filter(actor, 0, creatureStats.hasTalkedToPlayer());
        const ESM::DialInfo* info = filter.search(*dial, false).second;
        if (info != nullptr)
        {
            Log(Debug::Info) << "FNV/ESM4 dialogue: actor " << actor.toString() << " topic \""
                             << topic.toDebugString() << "\" selected sound \"" << info->mSound << "\"";
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            if (Settings::gui().mSubtitles)
                winMgr->messageBox(info->mResponse);
            if (!info->mSound.empty())
                sndMgr->say(actor, Misc::ResourceHelpers::correctSoundPath(VFS::Path::Normalized(info->mSound)));
            if (!info->mResultScript.empty())
                executeScript(info->mResultScript, actor);
        }
        return info != nullptr;
    }

    int DialogueManager::countSavedGameRecords() const
    {
        return 1; // known topics
    }

    void DialogueManager::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        ESM::DialogueState state;

        state.mKnownTopics.reserve(mKnownTopics.size());
        std::copy(mKnownTopics.begin(), mKnownTopics.end(), std::back_inserter(state.mKnownTopics));

        state.mChangedFactionReaction = mChangedFactionReaction;

        writer.startRecord(ESM::REC_DIAS);
        state.save(writer);
        writer.endRecord(ESM::REC_DIAS);
    }

    void DialogueManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_DIAS)
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();

            ESM::DialogueState state;
            state.load(reader);

            for (const auto& knownTopic : state.mKnownTopics)
                if (store.get<ESM::Dialogue>().search(knownTopic))
                    mKnownTopics.insert(knownTopic);

            mChangedFactionReaction = state.mChangedFactionReaction;
        }
    }

    void DialogueManager::modFactionReaction(const ESM::RefId& faction1, const ESM::RefId& faction2, int diff)
    {
        // Make sure the factions exist
        MWBase::Environment::get().getESMStore()->get<ESM::Faction>().find(faction1);
        MWBase::Environment::get().getESMStore()->get<ESM::Faction>().find(faction2);

        int newValue = getFactionReaction(faction1, faction2) + diff;

        auto& map = mChangedFactionReaction[faction1];
        map[faction2] = newValue;
    }

    void DialogueManager::setFactionReaction(const ESM::RefId& faction1, const ESM::RefId& faction2, int absolute)
    {
        // Make sure the factions exist
        MWBase::Environment::get().getESMStore()->get<ESM::Faction>().find(faction1);
        MWBase::Environment::get().getESMStore()->get<ESM::Faction>().find(faction2);

        auto& map = mChangedFactionReaction[faction1];
        map[faction2] = absolute;
    }

    int DialogueManager::getFactionReaction(const ESM::RefId& faction1, const ESM::RefId& faction2) const
    {
        ModFactionReactionMap::const_iterator map = mChangedFactionReaction.find(faction1);
        if (map != mChangedFactionReaction.end())
        {
            auto it = map->second.find(faction2);
            if (it != map->second.end())
                return it->second;
        }

        const ESM::Faction* faction = MWBase::Environment::get().getESMStore()->get<ESM::Faction>().find(faction1);

        auto it = faction->mReactions.begin();
        for (; it != faction->mReactions.end(); ++it)
        {
            if (it->first == faction2)
                return it->second;
        }
        return 0;
    }

    const std::map<ESM::RefId, int>* DialogueManager::getFactionReactionOverrides(const ESM::RefId& faction) const
    {
        // Make sure the faction exists
        MWBase::Environment::get().getESMStore()->get<ESM::Faction>().find(faction);

        const auto found = mChangedFactionReaction.find(faction);
        if (found != mChangedFactionReaction.end())
            return &found->second;
        return nullptr;
    }

    void DialogueManager::clearInfoActor(const MWWorld::Ptr& actor) const
    {
        if (actor == mActor && !mLastTopic.empty())
        {
            MWBase::Environment::get().getJournal()->removeLastAddedTopicResponse(
                mLastTopic, actor.getClass().getName(actor));
        }
    }
}
