#include "journalimp.hpp"

#include <cstdlib>
#include <iterator>

#include <components/debug/debuglog.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/journalentry.hpp>
#include <components/esm3/loadinfo.hpp>
#include <components/esm3/questobjectivestate.hpp>
#include <components/esm3/queststate.hpp>

#include <components/misc/strings/algorithm.hpp>

#include "../mwbase/dialoguemanager.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

namespace MWDialogue
{
    namespace
    {
        const ESM::DialInfo* findJournalInfo(const ESM::RefId& topic, const ESM::RefId& infoId)
        {
            const ESM::Dialogue* dialogue = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>().find(topic);
            for (const ESM::DialInfo& info : dialogue->mInfo)
                if (info.mId == infoId)
                    return &info;
            return nullptr;
        }

        bool isQuestStageFragmentTraceEnabled()
        {
            const char* value = std::getenv("OPENMW_FNV_PROOF_QUEST_STAGE_FRAGMENT_TRACE");
            return value != nullptr && value[0] != '\0';
        }
    }

    Quest& Journal::getOrStartQuest(const ESM::RefId& id)
    {
        TQuestContainer::iterator iter = mQuests.find(id);

        if (iter == mQuests.end())
            iter = mQuests.emplace(id, Quest(id)).first;

        return iter->second;
    }

    Quest* Journal::getQuestOrNull(const ESM::RefId& id)
    {
        TQuestContainer::iterator iter = mQuests.find(id);
        if (iter == mQuests.end())
        {
            return nullptr;
        }

        return &(iter->second);
    }

    Topic& Journal::getTopic(const ESM::RefId& id)
    {
        TTopicContainer::iterator iter = mTopics.find(id);

        if (iter == mTopics.end())
        {
            std::pair<TTopicContainer::iterator, bool> result = mTopics.insert(std::make_pair(id, Topic(id)));

            iter = result.first;
        }

        return iter->second;
    }

    bool Journal::isThere(const ESM::RefId& topicId, const ESM::RefId& infoId) const
    {
        if (const ESM::Dialogue* dialogue
            = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>().search(topicId))
        {
            if (infoId.empty())
                return true;

            for (const ESM::DialInfo& info : dialogue->mInfo)
                if (info.mId == infoId)
                    return true;
        }

        return false;
    }

    Journal::Journal() {}

    void Journal::clear()
    {
        mJournal.clear();
        mQuests.clear();
        mTopics.clear();
        mQuestObjectiveStates.clear();
    }

    void Journal::addEntry(const ESM::RefId& id, int index, const MWWorld::Ptr& actor)
    {
        // bail out if we already have heard this...
        const ESM::RefId& infoId = JournalEntry::idFromIndex(id, index);
        for (const JournalEntry& entry : mJournal)
            if (entry.mTopic == id && entry.mInfoId == infoId)
            {
                if (getJournalIndex(id) < index)
                {
                    setJournalIndex(id, index);
                    MWBase::Environment::get().getWindowManager()->messageBox("#{sJournalEntry}");
                }
                return;
            }

        StampedJournalEntry entry = StampedJournalEntry::makeFromQuest(id, index, actor);
        const ESM::DialInfo* info = findJournalInfo(id, entry.mInfoId);

        Quest& quest = getOrStartQuest(id);
        const bool restarted = quest.addEntry(entry); // we are doing slicing on purpose here
        if (restarted)
        {
            // Restart all "other" quests with the same name as well
            std::string_view name = quest.getName();
            for (auto& it : mQuests)
            {
                if (it.second.isFinished() && Misc::StringUtils::ciEqual(it.second.getName(), name))
                    it.second.setFinished(false);
            }
        }

        if (info != nullptr && !info->mResultScript.empty())
        {
            MWWorld::Ptr scriptActor = actor;
            if (scriptActor.isEmpty())
                scriptActor = MWBase::Environment::get().getWorld()->getPlayerPtr();

            MWBase::Environment::get().getDialogueManager()->executeScript(info->mResultScript, scriptActor);

            if (isQuestStageFragmentTraceEnabled())
            {
                Log(Debug::Info) << "FNV/ESM4 proof: quest stage fragment runtime executed"
                                 << " quest=" << id.toDebugString()
                                 << " stageIndex=" << index
                                 << " info=" << entry.mInfoId.toDebugString()
                                 << " scriptBytes=" << info->mResultScript.size()
                                 << " runtimeBoundary=selected-stage-fragment-result-script-runtime-supported"
                                 << " setStageRuntime=runtime-supported"
                                 << " conditionRuntime=loaded-pending-runtime"
                                 << " fullQuestCompletionRuntime=loaded-pending-runtime";
            }
        }

        // there is no need to show empty entries in journal
        if (!entry.getText().empty())
        {
            mJournal.push_back(std::move(entry));
            MWBase::Environment::get().getWindowManager()->messageBox("#{sJournalEntry}");
        }
    }

    void Journal::setJournalIndex(const ESM::RefId& id, int index)
    {
        Quest& quest = getOrStartQuest(id);

        quest.setIndex(index);
    }

    void Journal::addTopic(const ESM::RefId& topicId, const ESM::RefId& infoId, const MWWorld::Ptr& actor)
    {
        Topic& topic = getTopic(topicId);

        JournalEntry entry(topicId, infoId, actor);
        entry.mActorName = actor.getClass().getName(actor);
        topic.addEntry(entry);
    }

    void Journal::removeLastAddedTopicResponse(const ESM::RefId& topicId, std::string_view actorName)
    {
        Topic& topic = getTopic(topicId);

        topic.removeLastAddedResponse(actorName);

        if (topic.begin() == topic.end())
            mTopics.erase(mTopics.find(topicId)); // All responses removed -> remove topic
    }

    int Journal::getJournalIndex(const ESM::RefId& id) const
    {
        TQuestContainer::const_iterator iter = mQuests.find(id);

        if (iter == mQuests.end())
            return 0;

        return iter->second.getIndex();
    }

    bool Journal::isQuestStarted(const ESM::RefId& id) const
    {
        return mQuests.find(id) != mQuests.end();
    }

    void Journal::setQuestFinished(const ESM::RefId& id, bool finished)
    {
        getOrStartQuest(id).setFinished(finished);
    }

    bool Journal::getQuestFinished(const ESM::RefId& id) const
    {
        TQuestContainer::const_iterator iter = mQuests.find(id);
        return iter != mQuests.end() && iter->second.isFinished();
    }

    void Journal::setQuestObjectiveDisplayed(const ESM::RefId& id, int objective, bool displayed)
    {
        const QuestObjectiveKey key(id, objective);
        QuestObjectiveState& state = mQuestObjectiveStates[key];
        state.mDisplayed = displayed;
        if (!state.mDisplayed && !state.mCompleted)
            mQuestObjectiveStates.erase(key);
    }

    bool Journal::getQuestObjectiveDisplayed(const ESM::RefId& id, int objective) const
    {
        const auto it = mQuestObjectiveStates.find(QuestObjectiveKey(id, objective));
        return it != mQuestObjectiveStates.end() && it->second.mDisplayed;
    }

    void Journal::setQuestObjectiveCompleted(const ESM::RefId& id, int objective, bool completed)
    {
        const QuestObjectiveKey key(id, objective);
        QuestObjectiveState& state = mQuestObjectiveStates[key];
        state.mCompleted = completed;
        if (!state.mDisplayed && !state.mCompleted)
            mQuestObjectiveStates.erase(key);
    }

    bool Journal::getQuestObjectiveCompleted(const ESM::RefId& id, int objective) const
    {
        const auto it = mQuestObjectiveStates.find(QuestObjectiveKey(id, objective));
        return it != mQuestObjectiveStates.end() && it->second.mCompleted;
    }

    size_t Journal::countSavedGameRecords() const
    {
        std::size_t count = mQuests.size();

        for (const auto& [_, quest] : mQuests)
            count += quest.size();

        count += mJournal.size();

        for (const auto& [_, topic] : mTopics)
            count += topic.size();

        count += mQuestObjectiveStates.size();

        return count;
    }

    void Journal::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        for (const auto& [_, quest] : mQuests)
        {
            ESM::QuestState state;
            quest.write(state);
            writer.startRecord(ESM::REC_QUES);
            state.save(writer);
            writer.endRecord(ESM::REC_QUES);

            for (const Entry& questEntry : quest)
            {
                ESM::JournalEntry entry;
                entry.mType = ESM::JournalEntry::Type_Quest;
                entry.mTopic = quest.getTopic();
                questEntry.write(entry);
                writer.startRecord(ESM::REC_JOUR);
                entry.save(writer);
                writer.endRecord(ESM::REC_JOUR);
            }
        }

        for (const StampedJournalEntry& journalEntry : mJournal)
        {
            ESM::JournalEntry entry;
            entry.mType = ESM::JournalEntry::Type_Journal;
            journalEntry.write(entry);
            writer.startRecord(ESM::REC_JOUR);
            entry.save(writer);
            writer.endRecord(ESM::REC_JOUR);
        }

        for (const auto& [_, topic] : mTopics)
        {
            for (const Entry& topicEntry : topic)
            {
                ESM::JournalEntry entry;
                entry.mType = ESM::JournalEntry::Type_Topic;
                entry.mTopic = topic.getTopic();
                topicEntry.write(entry);
                writer.startRecord(ESM::REC_JOUR);
                entry.save(writer);
                writer.endRecord(ESM::REC_JOUR);
            }
        }

        for (const auto& [key, state] : mQuestObjectiveStates)
        {
            ESM::QuestObjectiveState record;
            record.mTopic = key.first;
            record.mObjective = key.second;
            record.mDisplayed = state.mDisplayed ? 1 : 0;
            record.mCompleted = state.mCompleted ? 1 : 0;
            writer.startRecord(ESM::REC_QOBJ);
            record.save(writer);
            writer.endRecord(ESM::REC_QOBJ);
        }
    }

    void Journal::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_JOUR)
        {
            ESM::JournalEntry record;
            record.load(reader);

            if (isThere(record.mTopic, record.mInfo))
                switch (record.mType)
                {
                    case ESM::JournalEntry::Type_Quest:

                        getOrStartQuest(record.mTopic).insertEntry(record);
                        break;

                    case ESM::JournalEntry::Type_Journal:

                        mJournal.push_back(record);
                        break;

                    case ESM::JournalEntry::Type_Topic:

                        getTopic(record.mTopic).insertEntry(record);
                        break;
                }
        }
        else if (type == ESM::REC_QUES)
        {
            ESM::QuestState record;
            record.load(reader);

            if (isThere(record.mTopic))
            {
                std::pair<TQuestContainer::iterator, bool> result
                    = mQuests.insert(std::make_pair(record.mTopic, record));
                // reapply quest index, this is to handle users upgrading from only
                // Morrowind.esm (no quest states) to Morrowind.esm + Tribunal.esm
                result.first->second.setIndex(record.mState);
            }
        }
        else if (type == ESM::REC_QOBJ)
        {
            ESM::QuestObjectiveState record;
            record.load(reader);

            if (record.mObjective >= 0 && isThere(record.mTopic))
            {
                QuestObjectiveState state;
                state.mDisplayed = record.mDisplayed != 0;
                state.mCompleted = record.mCompleted != 0;
                if (state.mDisplayed || state.mCompleted)
                    mQuestObjectiveStates[QuestObjectiveKey(record.mTopic, record.mObjective)] = state;
            }
        }
    }
}
