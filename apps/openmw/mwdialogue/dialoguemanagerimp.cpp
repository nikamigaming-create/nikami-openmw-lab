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
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadinfo.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/esm4/loadrefr.hpp>
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
#include "../mwclass/esm4creature.hpp"
#include "../mwclass/fnvaipackage.hpp"

#include "filter.hpp"
#include "esm4dialogueutils.hpp"
#include "esm4resultscript.hpp"
#include "hypertextparser.hpp"

namespace MWDialogue
{
    namespace
    {
        constexpr unsigned int maxEsm4SoundReferenceDepth = 8;

        ESM::RefId resolveEsm4MiscItemEditorId(const MWWorld::ESMStore& store, std::string_view editorId)
        {
            for (const ESM4::MiscItem& item : store.get<ESM4::MiscItem>())
            {
                if (Misc::StringUtils::ciEqual(item.mEditorId, editorId))
                    return ESM::RefId(item.mId);
            }
            return {};
        }

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
        mEsm4Picker.clear();
        mChoices.clear();
        mIsInChoice = false;
        mGoodbye = false;
        mEsm4SaidInfos.clear();
        mEsm4AddedTopics.clear();
        mEsm4VoicePaths.clear();
        mEsm4ResultReferenceIds.clear();
    }

    bool DialogueManager::matchesEsm4Info(const ESM4::DialogInfo& info) const
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return false;
        MWWorld::ESM4QuestRuntime& questRuntime = world->getESM4QuestRuntime();
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
                    return questRuntime.evaluateConditions({ condition });
                default:
                    break;
            }

            std::uint32_t runOn = condition.runOn;
            if (runOn == 0 && (condition.condition & ESM4::CTF_RunOnTarget) != 0)
                runOn = 1;

            MWWorld::Ptr actor;
            const bool playerFunction = condition.functionIndex == ESM4::FUN_GetPCIsClass
                || condition.functionIndex == ESM4::FUN_GetPCIsRace
                || condition.functionIndex == ESM4::FUN_GetPCIsSex;
            if (playerFunction)
                actor = world->getPlayerPtr();
            else if (runOn == 0)
                actor = mActor;
            else if (runOn == 1)
                actor = world->getPlayerPtr();
            else if (runOn == 2 && condition.reference != 0)
            {
                const ESM::FormId reference = ESM::FormId::fromUint32(condition.reference);
                // Fallout commonly encodes player checks as "Run On Reference: PlayerRef" rather than Run On
                // Target. PlayerRef is engine-owned and is not guaranteed to be discoverable as an ordinary
                // active-cell object, so route its canonical base/reference IDs directly to the live player.
                if (reference.mIndex == 0x7 || reference.mIndex == 0x14)
                    actor = world->getPlayerPtr();
                else
                {
                    try
                    {
                        actor = world->searchPtr(ESM::RefId(reference), false);
                    }
                    catch (const std::exception&)
                    {
                    }
                }
            }
            else if (runOn == 3)
                mActor.getClass().getCreatureStats(mActor).getAiSequence().getCombatTarget(actor);
            return evaluateEsm4ActorDialogueCondition(condition, actor, actor == world->getPlayerPtr());
        };

        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const ESM4::Quest* ownerQuest = nullptr;
        const MWWorld::ESM4QuestState* ownerState = nullptr;
        if (!info.mQuest.isZeroOrUnset() && store != nullptr)
        {
            ownerQuest = store->get<ESM4::Quest>().search(ESM::RefId(info.mQuest));
            ownerState = questRuntime.search(info.mQuest);
        }
        if (!matchesEsm4DialogueInfoConditions(info, ownerQuest, ownerState, evaluate))
            return false;

        if (!info.mSpeaker.isZeroOrUnset())
        {
            ESM::FormId baseId;
            if (const auto* actorRef
                = mActor.getType() == ESM4::Npc::sRecordId ? mActor.get<ESM4::Npc>() : nullptr)
                baseId = actorRef->mBase != nullptr ? actorRef->mBase->mId : ESM::FormId{};
            else if (const auto* actorRef
                = mActor.getType() == ESM4::Creature::sRecordId ? mActor.get<ESM4::Creature>() : nullptr)
                baseId = actorRef->mBase != nullptr ? actorRef->mBase->mId : ESM::FormId{};
            if (baseId.isZeroOrUnset() || baseId != info.mSpeaker)
                return false;
        }
        return true;
    }

    int DialogueManager::getEsm4InfoActorAffinity(const ESM4::DialogInfo& info) const
    {
        const ESM4::Npc* base = nullptr;
        const ESM4::Npc* traits = nullptr;
        const ESM4::Creature* creatureBase = nullptr;
        if (mActor.getType() == ESM4::Npc::sRecordId)
        {
            base = mActor.get<ESM4::Npc>()->mBase;
            traits = MWClass::ESM4Npc::getTraitsRecord(mActor);
        }
        else if (mActor.getType() == ESM4::Creature::sRecordId)
            creatureBase = mActor.get<ESM4::Creature>()->mBase;
        int affinity = 0;
        const ESM::FormId baseId = base != nullptr ? base->mId
            : creatureBase != nullptr                    ? creatureBase->mId
                                                        : ESM::FormId{};
        if (!baseId.isZeroOrUnset() && !info.mSpeaker.isZeroOrUnset() && info.mSpeaker == baseId)
            affinity += 10000;
        for (const ESM4::TargetCondition& condition : info.mTargetConditions)
        {
            const ESM::FormId parameter = ESM::FormId::fromUint32(condition.param1);
            if (condition.functionIndex == ESM4::FUN_GetIsID && !baseId.isZeroOrUnset() && parameter == baseId)
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

    const ESM4::DialogInfo* DialogueManager::resolveEsm4Selection(
        const Esm4DialogueSelection& selection) const
    {
        const ESM4::DialogInfo* info = MWBase::Environment::get()
                                               .getESMStore()
                                               ->get<ESM4::DialogInfo>()
                                               .search(ESM::RefId(selection.mInfo));
        if (info == nullptr || !matchesEsm4DialogueSelection(selection, *info))
            return nullptr;
        return info;
    }

    void DialogueManager::updateEsm4Topics()
    {
        mEsm4Picker.clearTopics();
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
                const Esm4DialogueSelection selection{ dialogue.mId, info->mId };
                if (mEsm4Picker.bindTopic(title, selection))
                {
                    if (std::getenv("OPENMW_PROOF_DIALOGUE_TOPIC") != nullptr)
                        Log(Debug::Info) << "FNV/ESM4 dialogue: available topic=\"" << title << "\" form="
                                         << ESM::RefId(dialogue.mId) << " info=" << ESM::RefId(info->mId);
                }
                else
                    Log(Debug::Verbose) << "FNV/ESM4 dialogue: duplicate visible topic skipped title=\"" << title
                                        << "\" form=" << ESM::RefId(dialogue.mId)
                                        << " info=" << ESM::RefId(info->mId);
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

    ESM::FormId DialogueManager::resolveEsm4ResultReferenceId(std::string_view editorId)
    {
        if (const auto cached = mEsm4ResultReferenceIds.find(std::string(editorId));
            cached != mEsm4ResultReferenceIds.end())
            return cached->second;

        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        ESM::FormId result;
        const auto search = [&](const auto& records) {
            for (const auto& record : records)
            {
                if (Misc::StringUtils::ciEqual(record.mEditorId, editorId))
                {
                    result = record.mId;
                    return true;
                }
            }
            return false;
        };
        if (store != nullptr && !search(store->get<ESM4::Reference>())
            && !search(store->get<ESM4::ActorCharacter>()))
            search(store->get<ESM4::ActorCreature>());

        mEsm4ResultReferenceIds.emplace(std::string(editorId), result);
        return result;
    }

    void DialogueManager::executeEsm4ResultSource(std::string_view source)
    {
        const Esm4ResultScript script = parseEsm4ResultScript(source);
        if (script.mMalformedControlFlow)
        {
            Log(Debug::Warning) << "FNV/ESM4 dialogue: skipped malformed result-script control flow";
            return;
        }

        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return;
        MWWorld::ESM4QuestRuntime& questRuntime = world->getESM4QuestRuntime();

        for (const Esm4ResultCommand& command : script.mCommands)
        {
            if (command.mType == Esm4ResultCommandType::Quest)
            {
                questRuntime.executeResultSource(command.mSource);
                continue;
            }
            if (command.mType == Esm4ResultCommandType::ShowBarterMenu)
            {
                MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
                if (windowManager != nullptr && !mActor.isEmpty() && !windowManager->containsMode(MWGui::GM_Barter))
                    windowManager->pushGuiMode(MWGui::GM_Barter, mActor);
                continue;
            }
            if (command.mType == Esm4ResultCommandType::AddItem
                || command.mType == Esm4ResultCommandType::RemoveItem)
            {
                const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
                const MWWorld::Ptr player = world->getPlayerPtr();
                const ESM::RefId itemId
                    = store == nullptr ? ESM::RefId() : resolveEsm4MiscItemEditorId(*store, command.mItem);
                if (player.isEmpty() || itemId.empty())
                {
                    Log(Debug::Warning) << "FNV/ESM4 dialogue: could not resolve player inventory item '"
                                        << command.mItem << "'";
                    continue;
                }

                MWWorld::ContainerStore& inventory = player.getClass().getContainerStore(player);
                if (command.mType == Esm4ResultCommandType::AddItem)
                {
                    inventory.add(itemId, command.mCount, false);
                    Log(Debug::Info) << "FNV/ESM4 dialogue: player AddItem item=" << command.mItem
                                     << " id=" << itemId << " count=" << command.mCount;
                }
                else
                {
                    const int removed = inventory.remove(itemId, command.mCount);
                    Log(removed == command.mCount ? Debug::Info : Debug::Warning)
                        << "FNV/ESM4 dialogue: player RemoveItem item=" << command.mItem
                        << " id=" << itemId << " requested=" << command.mCount << " removed=" << removed;
                }
                continue;
            }

            const ESM::FormId referenceId = resolveEsm4ResultReferenceId(command.mTarget);
            const MWWorld::Ptr target = referenceId.isZeroOrUnset()
                ? MWWorld::Ptr()
                : world->searchPtr(ESM::RefId(referenceId), false, false);
            if (target.isEmpty())
            {
                Log(Debug::Warning) << "FNV/ESM4 dialogue: could not resolve result-script reference '"
                                    << command.mTarget << "'";
                continue;
            }

            switch (command.mType)
            {
                case Esm4ResultCommandType::Enable:
                    world->enable(target);
                    Log(Debug::Info) << "FNV/ESM4 dialogue: enabled result-script reference '" << command.mTarget
                                     << "' id=" << target.getCellRef().getRefId();
                    break;
                case Esm4ResultCommandType::Disable:
                    world->disable(target);
                    Log(Debug::Info) << "FNV/ESM4 dialogue: disabled result-script reference '" << command.mTarget
                                     << "' id=" << target.getCellRef().getRefId();
                    break;
                case Esm4ResultCommandType::Unlock:
                    if (target.getClass().canLock(target))
                    {
                        target.getCellRef().unlock();
                        Log(Debug::Info) << "FNV/ESM4 dialogue: unlocked result-script reference '"
                                         << command.mTarget << "' id=" << target.getCellRef().getRefId()
                                         << " lockLevel=" << target.getCellRef().getLockLevel();
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 dialogue: Unlock rejected non-lockable reference '"
                                            << command.mTarget << "'";
                    break;
                case Esm4ResultCommandType::EvaluatePackage:
                    if (!MWClass::requestFnvAiPackageEvaluation(target))
                        Log(Debug::Warning) << "FNV/ESM4 dialogue: deferred unsafe EvaluatePackage for '"
                                            << command.mTarget << "'";
                    break;
                case Esm4ResultCommandType::StopCombat:
                    if (target.getClass().isActor())
                    {
                        MWBase::Environment::get().getMechanicsManager()->stopCombat(target);
                        Log(Debug::Info) << "FNV/ESM4 dialogue: stopped combat for result-script reference '"
                                         << command.mTarget << "' id=" << target.getCellRef().getRefId();
                    }
                    else
                        Log(Debug::Warning) << "FNV/ESM4 dialogue: StopCombat rejected non-actor reference '"
                                            << command.mTarget << "'";
                    break;
                case Esm4ResultCommandType::AddItem:
                case Esm4ResultCommandType::RemoveItem:
                case Esm4ResultCommandType::Quest:
                case Esm4ResultCommandType::ShowBarterMenu:
                    break;
            }
        }

        if (script.mSkippedConditionalCommands != 0)
            Log(Debug::Verbose) << "FNV/ESM4 dialogue: deferred " << script.mSkippedConditionalCommands
                                << " conditional result command(s) pending expression evaluation";
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
        std::vector<MWBase::FalloutDialogueVoiceMetadata> voiceMetadata;
        voices.reserve(orderedResponses.size());
        voiceMetadata.reserve(orderedResponses.size());
        const ESM4::DialogResponse* firstUnvoicedResponse = nullptr;
        for (std::size_t i = 0; i < orderedResponses.size(); ++i)
        {
            const ESM4::DialogResponse& item = *orderedResponses[i];
            if (item.mData.emoType != 0 || item.mData.emoValue != 0 || item.mData.flags != 0)
            {
                Log(Debug::Info) << "FNV/ESM4 dialogue: retail expression info=" << ESM::RefId(info->mId)
                                 << " response=" << (item.mData.responseNo != 0 ? item.mData.responseNo : i + 1)
                                 << " emotionType=" << item.mData.emoType << " emotionValue=" << item.mData.emoValue
                                 << " flags=0x" << std::hex << static_cast<unsigned int>(item.mData.flags) << std::dec
                                 << " speakerAnimation=" << ESM::RefId(item.mSpeakerAnimation)
                                 << " listenerAnimation=" << ESM::RefId(item.mListenerAnimation);
            }
            const std::string voice = resolveEsm4Voice(*info, item, i);
            if (voice.empty())
            {
                if (firstUnvoicedResponse == nullptr)
                    firstUnvoicedResponse = &item;
                continue;
            }
            Log(Debug::Info) << "FNV/ESM4 dialogue: resolved authored voice info=" << ESM::RefId(info->mId)
                             << " response="
                             << (item.mData.responseNo != 0 ? item.mData.responseNo : i + 1)
                             << " path=\"" << voice << "\"";
            voices.emplace_back(voice);
            voiceMetadata.push_back({ item.mData.emoType, item.mData.emoValue, item.mData.flags,
                ESM::RefId(item.mSpeakerAnimation), ESM::RefId(item.mListenerAnimation) });
        }
        if (!voices.empty())
            MWBase::Environment::get().getSoundManager()->saySequence(mActor, voices, voiceMetadata);
        else if (firstUnvoicedResponse != nullptr)
        {
            const ESM4::DialogResponse& item = *firstUnvoicedResponse;
            if ((item.mData.flags & 0x01) != 0)
                setEsm4DialogueExpression(mActor.mRef, item.mData.emoType, item.mData.emoValue);
            if (!item.mSpeakerAnimation.isZeroOrUnset())
                MWBase::Environment::get().getMechanicsManager()->playFalloutDialogueAnimation(
                    mActor, ESM::RefId(item.mSpeakerAnimation));
            if (!item.mListenerAnimation.isZeroOrUnset())
                MWBase::Environment::get().getMechanicsManager()->playFalloutDialogueAnimation(
                    MWBase::Environment::get().getWorld()->getPlayerPtr(), ESM::RefId(item.mListenerAnimation));
        }

        mChoices.clear();
        mEsm4Picker.clearChoices();
        for (ESM::FormId choice : info->mChoices)
        {
            const ESM4::Dialogue* choiceDialogue
                = MWBase::Environment::get().getESMStore()->get<ESM4::Dialogue>().search(ESM::RefId(choice));
            const ESM4::DialogInfo* choiceInfo = selectEsm4Info(choice);
            if (choiceDialogue == nullptr || choiceInfo == nullptr)
                continue;
            const std::string_view title = getEsm4DialoguePrompt(*choiceDialogue, *choiceInfo);
            const int choiceIndex = mEsm4Picker.bindChoice({ choice, choiceInfo->mId });
            mChoices.emplace_back(std::string(title), choiceIndex);
        }
        mIsInChoice = mEsm4Picker.hasChoices();
        if ((info->mInfoFlags & ESM4::INFO_Goodbye) != 0)
            goodbye();

        MWWorld::ESM4QuestRuntime& questRuntime
            = MWBase::Environment::get().getWorld()->getESM4QuestRuntime();
        const std::size_t unsupportedBefore = questRuntime.getUnsupportedStageCommands().size();
        if (!info->mScript.scriptSource.empty())
            executeEsm4ResultSource(info->mScript.scriptSource);
        if (!info->mEndScript.scriptSource.empty())
            executeEsm4ResultSource(info->mEndScript.scriptSource);
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

        mEsm4Dialogue = actor.getType() == ESM4::Npc::sRecordId
            || actor.getType() == ESM4::Creature::sRecordId;
        mLastEsm4Topic = {};
        mEsm4Picker.clear();
        if (mEsm4Dialogue)
        {
            if (std::getenv("OPENMW_FNV_PROOF_DIALOGUE_GESTURE_CATALOG") != nullptr)
            {
                const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
                std::size_t logged = 0;
                for (const ESM4::DialogInfo& info : store.get<ESM4::DialogInfo>())
                {
                    for (const ESM4::DialogResponse& response : info.mResponses)
                    {
                        if (response.mSpeakerAnimation.isZeroOrUnset()
                            && response.mListenerAnimation.isZeroOrUnset())
                            continue;

                        const ESM4::Dialogue* topic
                            = store.get<ESM4::Dialogue>().search(ESM::RefId(info.mTopic));
                        const ESM4::Npc* speaker
                            = info.mSpeaker.isZeroOrUnset() ? nullptr : store.get<ESM4::Npc>().search(ESM::RefId(info.mSpeaker));
                        const ESM4::ActorCharacter* placed = nullptr;
                        if (!info.mSpeaker.isZeroOrUnset())
                        {
                            for (const ESM4::ActorCharacter& candidate : store.get<ESM4::ActorCharacter>())
                            {
                                if (candidate.mBaseObj == info.mSpeaker)
                                {
                                    placed = &candidate;
                                    break;
                                }
                            }
                        }
                        Log(Debug::Info) << "FNV/ESM4 GESTURE CATALOG info=" << ESM::RefId(info.mId)
                                         << " topic=" << ESM::RefId(info.mTopic) << " topicEditor=\""
                                         << (topic != nullptr ? topic->mEditorId : std::string{}) << "\" prompt=\""
                                         << getEsm4DialoguePrompt(topic != nullptr ? *topic : ESM4::Dialogue{}, info)
                                         << "\" speaker=" << ESM::RefId(info.mSpeaker) << " speakerEditor=\""
                                         << (speaker != nullptr ? speaker->mEditorId : std::string{}) << "\" placed="
                                         << (placed != nullptr ? ESM::RefId(placed->mId) : ESM::RefId{}) << " parent="
                                         << (placed != nullptr ? placed->mParent : ESM::RefId{}) << " speakerAnimation="
                                         << ESM::RefId(response.mSpeakerAnimation) << " listenerAnimation="
                                         << ESM::RefId(response.mListenerAnimation) << " text=\"" << response.mResponse
                                         << "\"";
                        if (++logged >= 64)
                            break;
                    }
                    if (logged >= 64)
                        break;
                }
                Log(Debug::Info) << "FNV/ESM4 GESTURE CATALOG logged=" << logged;
                if (const char* baseText = std::getenv("OPENMW_FNV_PROOF_FIND_ACTOR_BASE");
                    baseText != nullptr && *baseText != '\0')
                {
                    const ESM::RefId requestedBase = ESM::RefId::deserializeText(baseText);
                    for (const ESM4::ActorCharacter& candidate : store.get<ESM4::ActorCharacter>())
                    {
                        if (candidate.mBaseObj != requestedBase)
                            continue;
                        Log(Debug::Info) << "FNV/ESM4 ACTOR BASE CATALOG base=" << requestedBase
                                         << " ref=" << ESM::RefId(candidate.mId) << " parent=" << candidate.mParent
                                         << " pos=(" << candidate.mPos.pos[0] << ',' << candidate.mPos.pos[1] << ','
                                         << candidate.mPos.pos[2] << ") editor=\"" << candidate.mEditorId << "\"";
                    }
                }
            }
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
            // Fallout INFO links are exclusive player responses. Present them through the dialogue history's
            // choice controls and keep the Morrowind topic sidebar inactive until the choice is answered.
            if (mEsm4Picker.hasChoices())
                return {};
            updateEsm4Topics();
            std::list<std::string> result;
            for (const auto& [title, _] : mEsm4Picker.getTopics())
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
            if (const std::optional<Esm4DialogueSelection> selection = mEsm4Picker.selectTopic(keyword))
                if (const ESM4::DialogInfo* info = resolveEsm4Selection(*selection))
                    executeEsm4Topic(selection->mTopic, callback, false, info);
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
        if (mEsm4Dialogue)
        {
            mEsm4Picker.clear();
            mChoices.clear();
            mIsInChoice = false;
            mGoodbye = false;
            mLastEsm4Topic = {};
            mEsm4Dialogue = false;
        }
    }

    void DialogueManager::questionAnswered(int answer, ResponseCallback* callback)
    {
        if (mEsm4Dialogue)
        {
            if (const std::optional<Esm4DialogueSelection> selection = mEsm4Picker.selectChoice(answer))
            {
                if (const ESM4::DialogInfo* info = resolveEsm4Selection(*selection))
                {
                    mChoices.clear();
                    mEsm4Picker.clearChoices();
                    mIsInChoice = false;
                    executeEsm4Topic(selection->mTopic, callback, false, info);
                }
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
        if (mEsm4Dialogue)
        {
            mChoices.clear();
            mEsm4Picker.clearChoices();
        }
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
        // Fallout actors can enter native combat/flee states without defining Morrowind's ambient voice topics.
        // A missing optional bark must be silent, not abort the AI package with an exception.
        const ESM::Dialogue* dial = store.get<ESM::Dialogue>().search(topic);
        if (dial == nullptr)
        {
            Log(Debug::Verbose) << "FNV/ESM4 dialogue: skipped unavailable voice topic \""
                                << topic.toDebugString() << "\" for actor " << actor.toString();
            return false;
        }

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

    bool DialogueManager::say(
        const MWWorld::Ptr& actor, const MWWorld::Ptr& listener, const ESM::FormId& topic)
    {
        MWBase::SoundManager* soundManager = MWBase::Environment::get().getSoundManager();
        if (actor.isEmpty() || soundManager->sayActive(actor))
            return false;
        if (actor.getClass().isNpc() && MWBase::Environment::get().getWorld()->isSwimming(actor))
            return false;
        if (actor.getClass().getCreatureStats(actor).getKnockedDown())
            return false;
        if (actor.getType() != ESM4::Npc::sRecordId && actor.getType() != ESM4::Creature::sRecordId)
            return false;

        const MWWorld::Ptr previousActor = mActor;
        mActor = actor;
        try
        {
            const ESM4::Dialogue* dialogue
                = MWBase::Environment::get().getESMStore()->get<ESM4::Dialogue>().search(ESM::RefId(topic));
            const ESM4::DialogInfo* info = selectEsm4Info(topic);
            if (dialogue == nullptr || info == nullptr)
            {
                mActor = previousActor;
                return false;
            }

            std::vector<const ESM4::DialogResponse*> responses;
            responses.reserve(info->mResponses.size());
            for (const ESM4::DialogResponse& response : info->mResponses)
                responses.push_back(&response);
            std::stable_sort(responses.begin(), responses.end(),
                [](const ESM4::DialogResponse* left, const ESM4::DialogResponse* right) {
                    const std::uint32_t leftNumber = left->mData.responseNo != 0
                        ? left->mData.responseNo
                        : std::numeric_limits<std::uint32_t>::max();
                    const std::uint32_t rightNumber = right->mData.responseNo != 0
                        ? right->mData.responseNo
                        : std::numeric_limits<std::uint32_t>::max();
                    return leftNumber < rightNumber;
                });

            std::string subtitle;
            std::vector<VFS::Path::Normalized> voices;
            std::vector<MWBase::FalloutDialogueVoiceMetadata> metadata;
            const ESM4::DialogResponse* firstUnvoicedResponse = nullptr;
            for (std::size_t i = 0; i < responses.size(); ++i)
            {
                const ESM4::DialogResponse& response = *responses[i];
                if (!response.mResponse.empty())
                {
                    if (!subtitle.empty())
                        subtitle += "\n";
                    subtitle += response.mResponse;
                }
                const std::string voice = resolveEsm4Voice(*info, response, i);
                if (voice.empty())
                {
                    if (firstUnvoicedResponse == nullptr)
                        firstUnvoicedResponse = &response;
                    continue;
                }
                voices.emplace_back(voice);
                metadata.push_back({ response.mData.emoType, response.mData.emoValue, response.mData.flags,
                    ESM::RefId(response.mSpeakerAnimation), ESM::RefId(response.mListenerAnimation) });
            }

            if (Settings::gui().mSubtitles && !subtitle.empty())
                MWBase::Environment::get().getWindowManager()->messageBox(subtitle);
            if (!voices.empty())
                soundManager->saySequence(actor, voices, metadata);
            else if (firstUnvoicedResponse != nullptr)
            {
                const ESM4::DialogResponse& response = *firstUnvoicedResponse;
                if ((response.mData.flags & 0x01) != 0)
                    setEsm4DialogueExpression(actor.mRef, response.mData.emoType, response.mData.emoValue);
                if (!response.mSpeakerAnimation.isZeroOrUnset())
                    MWBase::Environment::get().getMechanicsManager()->playFalloutDialogueAnimation(
                        actor, ESM::RefId(response.mSpeakerAnimation));
                if (!listener.isEmpty() && !response.mListenerAnimation.isZeroOrUnset())
                    MWBase::Environment::get().getMechanicsManager()->playFalloutDialogueAnimation(
                        listener, ESM::RefId(response.mListenerAnimation));
            }

            if ((info->mInfoFlags & ESM4::INFO_SayOnce) != 0)
                mEsm4SaidInfos.insert(info->mId);
            for (ESM::FormId addedTopic : info->mAddTopics)
                mEsm4AddedTopics.insert(addedTopic);
            if (!info->mScript.scriptSource.empty())
                executeEsm4ResultSource(info->mScript.scriptSource);
            if (!info->mEndScript.scriptSource.empty())
                executeEsm4ResultSource(info->mEndScript.scriptSource);
            Log(Debug::Info) << "FNV/ESM4 dialogue: SayTo actor=" << actor.toString()
                             << " listener=" << listener.toString() << " topic=" << dialogue->mEditorId
                             << " topicForm=" << ESM::RefId(topic) << " info=" << ESM::RefId(info->mId)
                             << " responses=" << responses.size() << " voices=" << voices.size();
            mActor = previousActor;
            return true;
        }
        catch (...)
        {
            mActor = previousActor;
            throw;
        }
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
