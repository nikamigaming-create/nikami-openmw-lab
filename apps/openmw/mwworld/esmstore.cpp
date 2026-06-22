#include "esmstore.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <variant>

#include <components/debug/debuglog.hpp>

#include <components/esm/records.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loaddial.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/reader.hpp>
#include <components/esm4/readerutils.hpp>
#include <components/esm4/records.hpp>
#include <components/esmloader/load.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/lua/configuration.hpp>
#include <components/misc/algorithm.hpp>

#include "../mwmechanics/spelllist.hpp"

namespace
{
    struct Ref
    {
        ESM::RefNum mRefNum;
        std::size_t mRefID;

        Ref(ESM::RefNum refNum, std::size_t refID)
            : mRefNum(refNum)
            , mRefID(refID)
        {
        }
    };

    constexpr std::size_t deletedRefID = std::numeric_limits<std::size_t>::max();

    struct Esm4RuntimeBridgeCounts
    {
        std::size_t mGameSettings = 0;
        std::size_t mGlobals = 0;
        std::size_t mScripts = 0;
        std::size_t mQuestDialogues = 0;
        std::size_t mTopicDialogues = 0;
        std::size_t mQuestInfos = 0;
        std::size_t mSkippedGameSettings = 0;
        std::size_t mSkippedGlobals = 0;
        std::size_t mSkippedScripts = 0;
        std::size_t mSkippedDialogues = 0;
        std::size_t mSkippedInfos = 0;
    };

    std::int32_t clampFloatToInt(float value)
    {
        if (!std::isfinite(value))
            return 0;
        if (value > static_cast<float>(std::numeric_limits<std::int32_t>::max()))
            return std::numeric_limits<std::int32_t>::max();
        if (value < static_cast<float>(std::numeric_limits<std::int32_t>::min()))
            return std::numeric_limits<std::int32_t>::min();
        return static_cast<std::int32_t>(std::lround(value));
    }

    bool convertEsm4GameSettingValue(const ESM4::GameSetting::Data& source, ESM::Variant& target)
    {
        if (std::holds_alternative<std::monostate>(source))
            return false;

        if (const auto* value = std::get_if<bool>(&source))
        {
            target.setType(ESM::VT_Int);
            target.setInteger(*value ? 1 : 0);
            return true;
        }

        if (const auto* value = std::get_if<float>(&source))
        {
            target.setType(ESM::VT_Float);
            target.setFloat(*value);
            return true;
        }

        if (const auto* value = std::get_if<std::int32_t>(&source))
        {
            target.setType(ESM::VT_Int);
            target.setInteger(*value);
            return true;
        }

        if (const auto* value = std::get_if<std::string>(&source))
        {
            target.setType(ESM::VT_String);
            target.setString(*value);
            return true;
        }

        if (const auto* value = std::get_if<std::uint32_t>(&source))
        {
            if (*value <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
            {
                target.setType(ESM::VT_Int);
                target.setInteger(static_cast<std::int32_t>(*value));
            }
            else
            {
                target.setType(ESM::VT_Float);
                target.setFloat(static_cast<float>(*value));
            }
            return true;
        }

        return false;
    }

    ESM::VarType convertEsm4GlobalType(std::uint8_t type)
    {
        switch (type)
        {
            case 's':
            case 'S':
            case 0:
                return ESM::VT_Short;
            case 'l':
            case 'L':
            case 1:
                return ESM::VT_Long;
            case 'f':
            case 'F':
            case 2:
                return ESM::VT_Float;
            default:
                return ESM::VT_Unknown;
        }
    }

    ESM::Dialogue::Type convertEsm4DialogueType(std::uint8_t type)
    {
        switch (type)
        {
            case 1:
                return ESM::Dialogue::Voice;
            case 2:
                return ESM::Dialogue::Greeting;
            case 3:
                return ESM::Dialogue::Persuasion;
            case 4:
                return ESM::Dialogue::Journal;
            case 0:
            default:
                return ESM::Dialogue::Topic;
        }
    }

    ESM::RefId formIdRefId(ESM::FormId id)
    {
        return ESM::RefId::formIdRefId(id);
    }

    void bridgeEsm4QuestDialogueStores(MWWorld::ESMStore& store, Esm4RuntimeBridgeCounts& counts)
    {
        MWWorld::Store<ESM::Dialogue>& targetDialogues = store.getWritable<ESM::Dialogue>();
        std::unordered_map<ESM::FormId, ESM::RefId> questDialogueIds;

        for (const ESM4::Quest& source : store.get<ESM4::Quest>())
        {
            if (source.mEditorId.empty())
            {
                ++counts.mSkippedDialogues;
                continue;
            }

            ESM::Dialogue target;
            target.blank();
            target.mId = ESM::RefId::stringRefId(source.mEditorId);
            target.mStringId = source.mQuestName.empty() ? source.mEditorId : source.mQuestName;
            target.mType = ESM::Dialogue::Journal;

            targetDialogues.insert(target);
            questDialogueIds[source.mId] = target.mId;
            ++counts.mQuestDialogues;
        }

        for (const ESM4::Dialogue& source : store.get<ESM4::Dialogue>())
        {
            if (source.mEditorId.empty())
            {
                ++counts.mSkippedDialogues;
                continue;
            }

            ESM::Dialogue target;
            target.blank();
            target.mId = ESM::RefId::stringRefId(source.mEditorId);
            target.mStringId = source.mTopicName.empty() ? source.mEditorId : source.mTopicName;
            target.mType = convertEsm4DialogueType(source.mDialType);

            targetDialogues.insert(target);
            ++counts.mTopicDialogues;
        }

        for (const ESM4::DialogInfo& source : store.get<ESM4::DialogInfo>())
        {
            if (source.mQuest.isZeroOrUnset() || source.mResponse.empty())
            {
                ++counts.mSkippedInfos;
                continue;
            }

            const auto quest = questDialogueIds.find(source.mQuest);
            if (quest == questDialogueIds.end())
            {
                ++counts.mSkippedInfos;
                continue;
            }

            ESM::Dialogue* dialogue = targetDialogues.search(quest->second);
            if (dialogue == nullptr)
            {
                ++counts.mSkippedInfos;
                continue;
            }

            ESM::DialInfo info;
            info.blank();
            info.mId = formIdRefId(source.mId);
            // ESM3 INFO sound is a playable path, not a FormId. Keep journal entries silent until SNDR/SOUN
            // path resolution is bridged.
            info.mSound.clear();
            info.mResponse = source.mResponse;
            info.mResultScript = source.mScript.scriptSource;
            info.mData.mType = ESM::Dialogue::Journal;
            info.mData.mJournalIndex = static_cast<std::int32_t>(source.mResponseData.responseNo);

            dialogue->mInfo.push_back(std::move(info));
            ++counts.mQuestInfos;
        }

        targetDialogues.rebuildRuntimeIndex();
    }

    Esm4RuntimeBridgeCounts bridgeEsm4RuntimeStores(MWWorld::ESMStore& store)
    {
        Esm4RuntimeBridgeCounts counts;

        MWWorld::Store<ESM::GameSetting>& targetGameSettings = store.getWritable<ESM::GameSetting>();
        for (const ESM4::GameSetting& source : store.get<ESM4::GameSetting>())
        {
            if (source.mEditorId.empty())
            {
                ++counts.mSkippedGameSettings;
                continue;
            }

            ESM::GameSetting target;
            target.mRecordFlags = source.mFlags;
            target.mId = ESM::RefId::stringRefId(source.mEditorId);
            if (!convertEsm4GameSettingValue(source.mData, target.mValue))
            {
                ++counts.mSkippedGameSettings;
                continue;
            }

            targetGameSettings.insert(target);
            ++counts.mGameSettings;
        }

        MWWorld::Store<ESM::Global>& targetGlobals = store.getWritable<ESM::Global>();
        for (const ESM4::GlobalVariable& source : store.get<ESM4::GlobalVariable>())
        {
            if (source.mEditorId.empty())
            {
                ++counts.mSkippedGlobals;
                continue;
            }

            const ESM::VarType type = convertEsm4GlobalType(source.mType);
            if (type == ESM::VT_Unknown)
            {
                ++counts.mSkippedGlobals;
                continue;
            }

            ESM::Global target;
            target.mRecordFlags = source.mFlags;
            target.mId = ESM::RefId::stringRefId(source.mEditorId);
            target.mValue.setType(type);
            if (type == ESM::VT_Float)
                target.mValue.setFloat(source.mValue);
            else
                target.mValue.setInteger(clampFloatToInt(source.mValue));

            targetGlobals.insert(target);
            ++counts.mGlobals;
        }

        MWWorld::Store<ESM::Script>& targetScripts = store.getWritable<ESM::Script>();
        for (const ESM4::Script& source : store.get<ESM4::Script>())
        {
            if (source.mEditorId.empty() || source.mScript.scriptSource.empty())
            {
                ++counts.mSkippedScripts;
                continue;
            }

            ESM::Script target;
            target.blank();
            target.mRecordFlags = source.mFlags;
            target.mId = ESM::RefId::stringRefId(source.mEditorId);
            target.mNumShorts = 0;
            target.mNumLongs = 0;
            target.mNumFloats = 0;
            target.mScriptText = source.mScript.scriptSource;

            targetScripts.insert(target);
            ++counts.mScripts;
        }

        bridgeEsm4QuestDialogueStores(store, counts);

        return counts;
    }

    void readRefs(const ESM::Cell& cell, std::vector<Ref>& refs, std::vector<ESM::RefId>& refIDs,
        std::set<ESM::RefId>& keyIDs, ESM::ReadersCache& readers)
    {
        // TODO: we have many similar copies of this code.
        for (size_t i = 0; i < cell.mContextList.size(); i++)
        {
            const std::size_t index = static_cast<std::size_t>(cell.mContextList[i].index);
            const ESM::ReadersCache::BusyItem reader = readers.get(index);
            cell.restore(*reader, i);
            ESM::CellRef ref;
            bool deleted = false;
            while (cell.getNextRef(*reader, ref, deleted))
            {
                if (deleted)
                    refs.emplace_back(ref.mRefNum, deletedRefID);
                else if (std::find(cell.mMovedRefs.begin(), cell.mMovedRefs.end(), ref.mRefNum)
                    == cell.mMovedRefs.end())
                {
                    if (!ref.mKey.empty())
                        keyIDs.insert(std::move(ref.mKey));
                    refs.emplace_back(ref.mRefNum, refIDs.size());
                    refIDs.push_back(std::move(ref.mRefID));
                }
            }
        }
        for (const auto& [value, deleted] : cell.mLeasedRefs)
        {
            if (deleted)
                refs.emplace_back(value.mRefNum, deletedRefID);
            else
            {
                if (!value.mKey.empty())
                    keyIDs.insert(std::move(value.mKey));
                refs.emplace_back(value.mRefNum, refIDs.size());
                refIDs.push_back(value.mRefID);
            }
        }
    }

    const ESM::RefId& getDefaultClass(const MWWorld::Store<ESM::Class>& classes)
    {
        auto it = classes.begin();
        if (it != classes.end())
            return it->mId;
        throw std::runtime_error("List of NPC classes is empty!");
    }

    const ESM::RefId& getDefaultRace(const MWWorld::Store<ESM::Race>& races)
    {
        auto it = races.begin();
        if (it != races.end())
            return it->mId;
        throw std::runtime_error("List of NPC races is empty!");
    }

    void ensureFalloutCharacterDefaults(MWWorld::Store<ESM::Class>& classes, MWWorld::Store<ESM::Race>& races,
        MWWorld::Store<ESM::Skill>& skills, MWWorld::Store<ESM::MagicEffect>& magicEffects,
        MWWorld::Store<ESM::NPC>& npcs, MWWorld::Store<ESM::Weapon>& weapons, MWWorld::Store<ESM::Potion>& potions,
        MWWorld::Store<ESM::Miscellaneous>& miscItems)
    {
        const ESM::RefId classId = ESM::RefId::stringRefId("FNV_Courier");
        const ESM::RefId raceId = ESM::RefId::stringRefId("FNV_Wastelander");

        if (classes.begin() == classes.end())
        {
            ESM::Class courier;
            courier.mId = classId;
            courier.blank();
            courier.mName = "Courier";
            courier.mDescription = "FNV proof default player class";
            courier.mData.mIsPlayable = 1;
            courier.mData.mAttribute[0] = 0;
            courier.mData.mAttribute[1] = 3;
            courier.mData.mSpecialization = ESM::Class::Combat;
            courier.mData.mSkills = { { { 5, 10 }, { 12, 13 }, { 14, 15 }, { 16, 17 }, { 18, 19 } } };
            classes.insertStatic(courier);
            Log(Debug::Info) << "FNV/ESM4: inserted fallback ESM3 player class FNV_Courier";
        }

        if (races.begin() == races.end())
        {
            ESM::Race wastelander;
            wastelander.mId = raceId;
            wastelander.blank();
            wastelander.mName = "Wastelander";
            wastelander.mDescription = "FNV proof default player race";
            wastelander.mData.mFlags = ESM::Race::Playable;
            for (int i = 0; i < ESM::Attribute::Length; ++i)
            {
                wastelander.mData.mAttributeValues[i] = 50;
                wastelander.mData.mAttributeValues[i + ESM::Attribute::Length] = 50;
            }
            races.insertStatic(wastelander);
            Log(Debug::Info) << "FNV/ESM4: inserted fallback ESM3 player race FNV_Wastelander";
        }

        int insertedSkills = 0;
        for (int i = 0; i < ESM::Skill::Length; ++i)
        {
            const ESM::RefId id = ESM::Skill::indexToRefId(i);
            if (id.empty() || skills.searchStatic(id) != nullptr)
                continue;

            ESM::Skill skill;
            skill.mId = ESM::SkillId(id.serializeText());
            skill.mRecordFlags = 0;
            skill.mName = id.serializeText();
            skill.mDescription.clear();
            skill.mIcon.clear();
            skill.mWerewolfValue = 1.f;
            skill.mData.mAttribute = i % ESM::Attribute::Length;
            skill.mData.mSpecialization = i < 9 ? ESM::Class::Combat : (i < 18 ? ESM::Class::Magic : ESM::Class::Stealth);
            for (float& value : skill.mData.mUseValue)
                value = 1.f;
            skills.insertStatic(skill);
            ++insertedSkills;
        }
        if (insertedSkills > 0)
            Log(Debug::Info) << "FNV/ESM4: inserted fallback ESM3 skills count=" << insertedSkills;

        int insertedMagicEffects = 0;
        for (int i = 0; i < ESM::MagicEffect::Length; ++i)
        {
            const ESM::RefId id = ESM::MagicEffect::indexToRefId(i);
            if (magicEffects.search(id) != nullptr)
                continue;

            ESM::MagicEffect effect;
            effect.mId = id;
            effect.blank();
            effect.mData.mFlags = ESM::MagicEffect::NoDuration | ESM::MagicEffect::NoMagnitude;
            effect.mData.mRed = 128;
            effect.mData.mGreen = 128;
            effect.mData.mBlue = 128;
            magicEffects.insertStatic(effect);
            ++insertedMagicEffects;
        }
        if (insertedMagicEffects > 0)
            Log(Debug::Info) << "FNV/ESM4: inserted fallback ESM3 magic effects count=" << insertedMagicEffects;

        if (npcs.searchStatic(ESM::RefId::stringRefId("Player")) == nullptr)
        {
            ESM::NPC player;
            player.mId = ESM::RefId::stringRefId("Player");
            player.blank();
            player.mName = "Courier";
            player.mModel = "characters/_male/skeleton.nif";
            player.mRace = raceId;
            player.mClass = classId;
            player.mNpdt.mLevel = 1;
            player.mNpdt.mAttributes.fill(50);
            player.mNpdt.mSkills.fill(35);
            player.mNpdt.mHealth = 125;
            player.mNpdt.mMana = 60;
            player.mNpdt.mFatigue = 220;
            player.mNpdt.mDisposition = 50;
            player.mNpdt.mGold = 0;
            const auto addProofItem = [&player](std::string_view id, int count) {
                ESM::ContItem item;
                item.mCount = count;
                item.mItem = ESM::RefId::stringRefId(id);
                player.mInventory.mList.push_back(item);
            };
            addProofItem("FNV_PROOF_9MM_PISTOL", 1);
            addProofItem("FNV_PROOF_VARMINT_RIFLE", 1);
            addProofItem("FNV_PROOF_STIMPAK", 3);
            addProofItem("FNV_PROOF_9MM_AMMO", 48);
            addProofItem("FNV_PROOF_BOBBY_PIN", 5);
            addProofItem("FNV_PROOF_CAPS", 37);
            npcs.insertStatic(player);
            Log(Debug::Info) << "FNV/ESM4: inserted fallback ESM3 Player NPC for normal save/load with proof inventory";
        }

        const auto ensureWeapon = [&weapons](
                                      std::string_view id, std::string_view name, std::string_view icon, int value) {
            const ESM::RefId refId = ESM::RefId::stringRefId(id);
            if (weapons.searchStatic(refId) != nullptr)
                return;

            ESM::Weapon weapon;
            weapon.mId = refId;
            weapon.blank();
            weapon.mName = std::string(name);
            weapon.mIcon = std::string(icon);
            weapon.mData.mWeight = 2.f;
            weapon.mData.mValue = value;
            weapon.mData.mType = ESM::Weapon::MarksmanThrown;
            weapon.mData.mHealth = 100;
            weapon.mData.mSpeed = 1.f;
            weapon.mData.mReach = 1.f;
            weapon.mData.mChop = { 4, 12 };
            weapon.mData.mSlash = { 4, 12 };
            weapon.mData.mThrust = { 4, 12 };
            weapons.insertStatic(weapon);
            Log(Debug::Info) << "FNV/ESM4: inserted fallback inventory weapon " << id;
        };

        const auto ensurePotion = [&potions](
                                      std::string_view id, std::string_view name, std::string_view icon, int value) {
            const ESM::RefId refId = ESM::RefId::stringRefId(id);
            if (potions.searchStatic(refId) != nullptr)
                return;

            ESM::Potion potion;
            potion.mId = refId;
            potion.blank();
            potion.mName = std::string(name);
            potion.mIcon = std::string(icon);
            potion.mData.mWeight = 0.1f;
            potion.mData.mValue = value;
            potions.insertStatic(potion);
            Log(Debug::Info) << "FNV/ESM4: inserted fallback inventory potion " << id;
        };

        const auto ensureMisc = [&miscItems](std::string_view id, std::string_view name, std::string_view icon,
                                    float weight, int value) {
            const ESM::RefId refId = ESM::RefId::stringRefId(id);
            if (miscItems.searchStatic(refId) != nullptr)
                return;

            ESM::Miscellaneous item;
            item.mId = refId;
            item.blank();
            item.mName = std::string(name);
            item.mIcon = std::string(icon);
            item.mData.mWeight = weight;
            item.mData.mValue = value;
            miscItems.insertStatic(item);
            Log(Debug::Info) << "FNV/ESM4: inserted fallback inventory misc " << id;
        };

        constexpr std::string_view proofIcon = "nikami_proof_item.dds";
        ensureWeapon("FNV_PROOF_9MM_PISTOL", "9mm Pistol", proofIcon, 100);
        ensureWeapon("FNV_PROOF_VARMINT_RIFLE", "Varmint Rifle", proofIcon, 75);
        ensurePotion("FNV_PROOF_STIMPAK", "Stimpak", proofIcon, 25);
        ensureMisc("FNV_PROOF_9MM_AMMO", "9mm Round", proofIcon, 0.01f, 1);
        ensureMisc("FNV_PROOF_BOBBY_PIN", "Bobby Pin", proofIcon, 0.01f, 1);
        ensureMisc("FNV_PROOF_CAPS", "Bottle Cap", proofIcon, 0.f, 1);
    }

    bool shouldEnsureFalloutProofCharacterDefaults()
    {
        return std::getenv("OPENMW_FNV_BOOTSTRAP_LEVEL1_COURIER") != nullptr
            || std::getenv("OPENMW_FNV_BOOTSTRAP_DOC_SENT") != nullptr
            || std::getenv("OPENMW_FNV_PROOF_ENABLE_ESM3_FALLBACKS") != nullptr;
    }

    bool shouldEnsureFalloutCharacterDefaults(const MWWorld::Store<ESM::Class>& classes,
        const MWWorld::Store<ESM::Race>& races, const MWWorld::Store<ESM::NPC>& esm3Npcs,
        const MWWorld::Store<ESM::Creature>& esm3Creatures, const MWWorld::Store<ESM4::Npc>& esm4Npcs,
        const MWWorld::Store<ESM4::Creature>& esm4Creatures, const MWWorld::Store<ESM4::Race>& esm4Races)
    {
        if (shouldEnsureFalloutProofCharacterDefaults())
            return true;

        const bool missingEsm3CharacterBasis = classes.begin() == classes.end() || races.begin() == races.end();
        const bool hasCharacterData = esm3Npcs.begin() != esm3Npcs.end() || esm3Creatures.begin() != esm3Creatures.end()
            || esm4Npcs.begin() != esm4Npcs.end() || esm4Creatures.begin() != esm4Creatures.end()
            || esm4Races.begin() != esm4Races.end();
        return missingEsm3CharacterBasis && hasCharacterData;
    }

    std::vector<ESM::NPC> getNPCsToReplace(const MWWorld::Store<ESM::Faction>& factions,
        const MWWorld::Store<ESM::Class>& classes, const MWWorld::Store<ESM::Race>& races,
        const MWWorld::Store<ESM::Script>& scripts, const std::unordered_map<ESM::RefId, ESM::NPC>& npcs)
    {
        // Cache first class from store - we will use it if current class is not found
        const ESM::RefId& defaultCls = getDefaultClass(classes);
        // Same for races
        const ESM::RefId& defaultRace = getDefaultRace(races);

        // Validate NPCs for non-existing class and faction.
        // We will replace invalid entries by fixed ones
        std::vector<ESM::NPC> npcsToReplace;

        for (const auto& npcIter : npcs)
        {
            ESM::NPC npc = npcIter.second;
            bool changed = false;

            const ESM::RefId& npcFaction = npc.mFaction;
            if (!npcFaction.empty())
            {
                const ESM::Faction* fact = factions.search(npcFaction);
                if (!fact)
                {
                    Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent faction "
                                        << npc.mFaction << ", ignoring it.";
                    npc.mFaction = ESM::RefId();
                    npc.mNpdt.mRank = 0;
                    changed = true;
                }
            }

            const ESM::Class* cls = classes.search(npc.mClass);
            if (!cls)
            {
                Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent class "
                                    << npc.mClass << ", using " << defaultCls << " class as replacement.";
                npc.mClass = defaultCls;
                changed = true;
            }

            const ESM::Race* race = races.search(npc.mRace);
            if (!race)
            {
                Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent race " << npc.mRace
                                    << ", using " << defaultRace << " race as replacement.";
                npc.mRace = defaultRace;
                changed = true;
            }

            if (!npc.mScript.empty() && !scripts.search(npc.mScript))
            {
                Log(Debug::Verbose) << "NPC " << npc.mId << " (" << npc.mName << ") has nonexistent script "
                                    << npc.mScript << ", ignoring it.";
                npc.mScript = ESM::RefId();
                changed = true;
            }

            if (changed)
                npcsToReplace.push_back(std::move(npc));
        }

        return npcsToReplace;
    }

    template <class RecordType>
    std::vector<RecordType> getSpellsToReplace(
        const MWWorld::Store<RecordType>& spells, const MWWorld::Store<ESM::MagicEffect>& magicEffects)
    {
        std::vector<RecordType> spellsToReplace;

        for (RecordType spell : spells)
        {
            if (spell.mEffects.mList.empty())
                continue;

            bool changed = false;
            auto iter = spell.mEffects.mList.begin();
            while (iter != spell.mEffects.mList.end())
            {
                const ESM::MagicEffect* mgef = magicEffects.search(iter->mData.mEffectID);
                if (!mgef)
                {
                    Log(Debug::Verbose) << RecordType::getRecordType() << " " << spell.mId
                                        << ": dropping invalid effect (" << iter->mData.mEffectID << ")";
                    iter = spell.mEffects.mList.erase(iter);
                    changed = true;
                    continue;
                }

                if (!(mgef->mData.mFlags & ESM::MagicEffect::TargetAttribute) && !iter->mData.mAttribute.empty())
                {
                    iter->mData.mAttribute = ESM::RefId();
                    Log(Debug::Verbose) << RecordType::getRecordType() << " " << spell.mId
                                        << ": dropping unexpected attribute argument of " << iter->mData.mEffectID
                                        << " effect";
                    changed = true;
                }

                if (!(mgef->mData.mFlags & ESM::MagicEffect::TargetSkill) && !iter->mData.mSkill.empty())
                {
                    iter->mData.mSkill = ESM::RefId();
                    Log(Debug::Verbose) << RecordType::getRecordType() << " " << spell.mId
                                        << ": dropping unexpected skill argument of " << iter->mData.mEffectID
                                        << " effect";
                    changed = true;
                }

                ++iter;
            }

            if (changed)
                spellsToReplace.emplace_back(spell);
        }

        return spellsToReplace;
    }

    // Custom enchanted items can reference scripts that no longer exist, this doesn't necessarily mean the base item no
    // longer exists however. So instead of removing the item altogether, we're only removing the script.
    template <class MapT>
    void removeMissingScripts(const MWWorld::Store<ESM::Script>& scripts, MapT& items)
    {
        for (auto& [id, item] : items)
        {
            if (!item.mScript.empty() && !scripts.search(item.mScript))
            {
                Log(Debug::Verbose) << MapT::mapped_type::getRecordType() << ' ' << id << " (" << item.mName
                                    << ") has nonexistent script " << item.mScript << ", ignoring it.";
                item.mScript = ESM::RefId();
            }
        }
    }
}

namespace MWWorld
{
    using IDMap = std::unordered_map<ESM::RefId, int>;

    struct ESMStoreImp
    {
        ESMStore::StoreTuple mStores;

        std::map<ESM::RecNameInts, DynamicStore*> mRecNameToStore;

        // Lookup of all IDs. Makes looking up references faster. Just
        // maps the id name to the record type.
        IDMap mIds;
        IDMap mStaticIds;

        template <typename T>
        static void assignStoreToIndex(ESMStore& stores, Store<T>& store)
        {
            const std::size_t storeIndex = ESMStore::getTypeIndex<T>();
            if (stores.mStores.size() <= storeIndex)
                stores.mStores.resize(storeIndex + 1);

            assert(&store == &std::get<Store<T>>(stores.mStoreImp->mStores));

            stores.mStores[storeIndex] = &store;
            if constexpr (std::is_convertible_v<Store<T>*, DynamicStore*>)
            {
                stores.mDynamicStores.push_back(&store);
                constexpr ESM::RecNameInts recName = T::sRecordId;
                if constexpr (recName != ESM::REC_INTERNAL_PLAYER)
                {
                    stores.mStoreImp->mRecNameToStore[recName] = &store;
                }
            }
        }

        template <typename T>
        static bool typedReadRecordESM4(ESM4::Reader& reader, Store<T>& store)
        {
            auto recordType = static_cast<ESM4::RecordTypes>(reader.hdr().record.typeId);

            ESM::RecNameInts esm4RecName = static_cast<ESM::RecNameInts>(ESM::esm4Recname(recordType));
            if constexpr (HasRecordId<T>::value)
            {
                if constexpr (ESM::isESM4Rec(T::sRecordId))
                {
                    if (T::sRecordId == esm4RecName)
                    {
                        reader.getRecordData();
                        T value;
                        value.load(reader);
                        store.insertStatic(value);
                        return true;
                    }
                }
            }
            return false;
        }

        static bool readRecord(ESM4::Reader& reader, ESMStore& store)
        {
            return std::apply(
                [&reader](auto&... x) { return (typedReadRecordESM4(reader, x) || ...); }, store.mStoreImp->mStores);
        }
    };

    int ESMStore::find(const ESM::RefId& id) const
    {
        IDMap::const_iterator it = mStoreImp->mIds.find(id);
        if (it == mStoreImp->mIds.end())
        {
            return 0;
        }
        return it->second;
    }

    int ESMStore::findStatic(const ESM::RefId& id) const
    {
        IDMap::const_iterator it = mStoreImp->mStaticIds.find(id);
        if (it == mStoreImp->mStaticIds.end())
        {
            return 0;
        }
        return it->second;
    }

    ESMStore::ESMStore()
    {
        mStoreImp = std::make_unique<ESMStoreImp>();
        std::apply([this](auto&... x) { (ESMStoreImp::assignStoreToIndex(*this, x), ...); }, mStoreImp->mStores);
        mDynamicCount = 0;
        getWritable<ESM::Pathgrid>().setCells(getWritable<ESM::Cell>());
    }

    ESMStore::~ESMStore() = default;

    void ESMStore::clearDynamic()
    {
        for (const auto& store : mDynamicStores)
            store->clearDynamic();
        mStoreImp->mIds = mStoreImp->mStaticIds;

        movePlayerRecord();
    }

    static bool isCacheableRecord(int id)
    {
        switch (id)
        {
            case ESM::REC_ACTI:
            case ESM::REC_ALCH:
            case ESM::REC_APPA:
            case ESM::REC_ARMO:
            case ESM::REC_BOOK:
            case ESM::REC_CLOT:
            case ESM::REC_CONT:
            case ESM::REC_CREA:
            case ESM::REC_DOOR:
            case ESM::REC_INGR:
            case ESM::REC_LEVC:
            case ESM::REC_LEVI:
            case ESM::REC_LIGH:
            case ESM::REC_LOCK:
            case ESM::REC_MISC:
            case ESM::REC_NPC_:
            case ESM::REC_PROB:
            case ESM::REC_REPA:
            case ESM::REC_STAT:
            case ESM::REC_WEAP:
            case ESM::REC_BODY:
            case ESM::REC_ACTI4:
            case ESM::REC_ALCH4:
            case ESM::REC_AMMO4:
            case ESM::REC_ARMO4:
            case ESM::REC_BOOK4:
            case ESM::REC_CONT4:
            case ESM::REC_CREA4:
            case ESM::REC_DOOR4:
            case ESM::REC_FLOR4:
            case ESM::REC_FURN4:
            case ESM::REC_IMOD4:
            case ESM::REC_INGR4:
            case ESM::REC_LIGH4:
            case ESM::REC_LVLI4:
            case ESM::REC_LVLC4:
            case ESM::REC_LVLN4:
            case ESM::REC_MISC4:
            case ESM::REC_MSTT4:
            case ESM::REC_NPC_4:
            case ESM::REC_SCOL4:
            case ESM::REC_STAT4:
            case ESM::REC_TERM4:
            case ESM::REC_TREE4:
            case ESM::REC_WEAP4:
                return true;
                break;
        }
        return false;
    }

    void ESMStore::load(ESM::ESMReader& esm, Loading::Listener* listener, ESM::Dialogue*& dialogue)
    {
        if (listener != nullptr)
            listener->setProgressRange(::EsmLoader::fileProgress);

        // Loop through all records
        while (esm.hasMoreRecs())
        {
            ESM::NAME n = esm.getRecName();
            esm.getRecHeader();
            if (esm.getRecordFlags() & ESM::FLAG_Ignored)
            {
                esm.skipRecord();
                continue;
            }

            // Look up the record type.
            ESM::RecNameInts recName = static_cast<ESM::RecNameInts>(n.toInt());
            const auto& it = mStoreImp->mRecNameToStore.find(recName);

            if (it == mStoreImp->mRecNameToStore.end())
            {
                if (recName == ESM::REC_INFO)
                {
                    if (dialogue)
                    {
                        dialogue->readInfo(esm);
                    }
                    else
                    {
                        Log(Debug::Error) << "Error: info record without dialog";
                        esm.skipRecord();
                    }
                }
                else if (n.toInt() == ESM::REC_MGEF)
                {
                    getWritable<ESM::MagicEffect>().load(esm);
                }
                else if (n.toInt() == ESM::REC_SKIL)
                {
                    getWritable<ESM::Skill>().load(esm);
                }
                else if (n.toInt() == ESM::REC_FILT || n.toInt() == ESM::REC_DBGP)
                {
                    // ignore project file only records
                    esm.skipRecord();
                }
                else if (n.toInt() == ESM::REC_LUAL)
                {
                    ESM::LuaScriptsCfg cfg;
                    cfg.load(esm);
                    cfg.adjustRefNums(esm);
                    mLuaContent.push_back(std::move(cfg));
                }
                else
                {
                    throw std::runtime_error("Unknown record: " + n.toString());
                }
            }
            else
            {
                RecordId id = it->second->load(esm);
                if (id.mIsDeleted)
                {
                    it->second->eraseStatic(id.mId);
                    continue;
                }

                if (n.toInt() == ESM::REC_DIAL)
                {
                    dialogue = const_cast<ESM::Dialogue*>(getWritable<ESM::Dialogue>().find(id.mId));
                }
                else
                {
                    dialogue = nullptr;
                }
            }
            if (listener != nullptr)
                listener->setProgress(::EsmLoader::fileProgress * esm.getFileOffset() / esm.getFileSize());
        }
    }

    void ESMStore::loadESM4(ESM4::Reader& reader, Loading::Listener* listener)
    {
        if (listener != nullptr)
            listener->setProgressRange(::EsmLoader::fileProgress);

        std::map<ESM::RecNameInts, std::size_t> loadedRecordCounts;
        std::map<ESM::RecNameInts, std::size_t> skippedRecordCounts;

        auto visitorRec = [this, listener, &loadedRecordCounts, &skippedRecordCounts](ESM4::Reader& r) {
            const auto recordType = static_cast<ESM4::RecordTypes>(r.hdr().record.typeId);
            const auto recName = static_cast<ESM::RecNameInts>(ESM::esm4Recname(recordType));
            bool result = ESMStoreImp::readRecord(r, *this);
            if (result)
                ++loadedRecordCounts[recName];
            else
                ++skippedRecordCounts[recName];
            if (listener != nullptr)
                listener->setProgress(::EsmLoader::fileProgress * r.getFileOffset() / r.getFileSize());
            return result;
        };
        ESM4::ReaderUtils::readAll(reader, visitorRec, [](ESM4::Reader&) {});

        std::size_t loadedRecords = 0;
        std::size_t skippedRecords = 0;
        for (const auto& [_, count] : loadedRecordCounts)
            loadedRecords += count;
        for (const auto& [_, count] : skippedRecordCounts)
            skippedRecords += count;

        Log(Debug::Info) << "FNV/ESM4 inventory: loaded record total=" << loadedRecords
                         << " types=" << loadedRecordCounts.size() << " skipped record total=" << skippedRecords
                         << " types=" << skippedRecordCounts.size();
        for (const auto& [recName, count] : loadedRecordCounts)
            Log(Debug::Info) << "FNV/ESM4 inventory loaded: " << ESM::getRecNameString(recName).toStringView()
                             << " count=" << count;
        for (const auto& [recName, count] : skippedRecordCounts)
            Log(Debug::Warning) << "FNV/ESM4 inventory skipped unsupported: "
                                << ESM::getRecNameString(recName).toStringView() << " count=" << count;

        if (std::getenv("OPENMW_FNV_STRICT_ESM4_RECORDS") != nullptr && !skippedRecordCounts.empty())
        {
            std::string message = "FNV strict ESM4 record gate failed; unsupported record types:";
            for (const auto& [recName, count] : skippedRecordCounts)
            {
                message += " ";
                message += ESM::getRecNameString(recName).toStringView();
                message += "=" + std::to_string(count);
            }
            throw std::runtime_error(message);
        }
    }

    void ESMStore::setIdType(const ESM::RefId& id, ESM::RecNameInts type)
    {
        mStoreImp->mIds[id] = type;
    }

    ESM::LuaScriptsCfg ESMStore::getLuaScriptsCfg() const
    {
        ESM::LuaScriptsCfg cfg;
        for (const LuaContent& c : mLuaContent)
        {
            if (std::holds_alternative<std::filesystem::path>(c))
            {
                // *.omwscripts are intentionally reloaded every time when `getLuaScriptsCfg` is called.
                // It is important for the `reloadlua` console command.
                try
                {
                    auto file = std::ifstream(std::get<std::filesystem::path>(c));
                    std::string fileContent(std::istreambuf_iterator<char>(file), {});
                    LuaUtil::parseOMWScripts(cfg, fileContent);
                }
                catch (std::exception& e)
                {
                    Log(Debug::Error) << e.what();
                }
            }
            else
            {
                const ESM::LuaScriptsCfg& addition = std::get<ESM::LuaScriptsCfg>(c);
                cfg.mScripts.insert(cfg.mScripts.end(), addition.mScripts.begin(), addition.mScripts.end());
            }
        }
        return cfg;
    }

    void ESMStore::setUp()
    {
        if (mIsSetUpDone)
            throw std::logic_error("ESMStore::setUp() is called twice");
        mIsSetUpDone = true;

        for (const auto& [_, store] : mStoreImp->mRecNameToStore)
            store->setUp();

        const Esm4RuntimeBridgeCounts bridgeCounts = bridgeEsm4RuntimeStores(*this);
        Log(Debug::Info) << "FNV/ESM4 proof: bridged runtime records gmst=" << bridgeCounts.mGameSettings
                         << " globals=" << bridgeCounts.mGlobals
                         << " scripts=" << bridgeCounts.mScripts
                         << " questDialogues=" << bridgeCounts.mQuestDialogues
                         << " topicDialogues=" << bridgeCounts.mTopicDialogues
                         << " questInfos=" << bridgeCounts.mQuestInfos
                         << " skippedGmst=" << bridgeCounts.mSkippedGameSettings
                         << " skippedGlobals=" << bridgeCounts.mSkippedGlobals
                         << " skippedScripts=" << bridgeCounts.mSkippedScripts
                         << " skippedDialogues=" << bridgeCounts.mSkippedDialogues
                         << " skippedInfos=" << bridgeCounts.mSkippedInfos;

        getWritable<ESM::Skill>().setUp(get<ESM::GameSetting>());
        getWritable<ESM::Attribute>().setUp(get<ESM::GameSetting>());
        getWritable<ESM4::Land>().updateLandPositions(get<ESM4::Cell>());
        getWritable<ESM4::Reference>().preprocessReferences(get<ESM4::Cell>());
        getWritable<ESM4::ActorCharacter>().preprocessReferences(get<ESM4::Cell>());
        getWritable<ESM4::ActorCreature>().preprocessReferences(get<ESM4::Cell>());

        rebuildIdsIndex();
        mStoreImp->mStaticIds = mStoreImp->mIds;
    }

    void ESMStore::rebuildIdsIndex()
    {
        mStoreImp->mIds.clear();
        for (const auto& [recordType, store] : mStoreImp->mRecNameToStore)
        {
            if (isCacheableRecord(recordType))
            {
                std::vector<ESM::RefId> identifiers;
                store->listIdentifier(identifiers);
                for (auto& record : identifiers)
                    mStoreImp->mIds[record] = recordType;
            }
        }
    }

    void ESMStore::validateRecords(ESM::ReadersCache& readers)
    {
        validate();
        countAllCellRefsAndMarkKeys(readers);
    }

    void ESMStore::countAllCellRefsAndMarkKeys(ESM::ReadersCache& readers)
    {
        // TODO: We currently need to read entire files here again.
        // We should consider consolidating or deferring this reading.
        if (!mRefCount.empty())
            return;
        std::vector<Ref> refs;
        std::set<ESM::RefId> keyIDs;
        std::vector<ESM::RefId> refIDs;
        const Store<ESM::Cell>& cells = get<ESM::Cell>();
        for (auto it = cells.intBegin(); it != cells.intEnd(); ++it)
            readRefs(*it, refs, refIDs, keyIDs, readers);
        for (auto it = cells.extBegin(); it != cells.extEnd(); ++it)
            readRefs(*it, refs, refIDs, keyIDs, readers);
        const auto lessByRefNum = [](const Ref& l, const Ref& r) { return l.mRefNum < r.mRefNum; };
        std::stable_sort(refs.begin(), refs.end(), lessByRefNum);
        const auto equalByRefNum = [](const Ref& l, const Ref& r) { return l.mRefNum == r.mRefNum; };
        const auto incrementRefCount = [&](const Ref& value) {
            if (value.mRefID != deletedRefID)
            {
                ESM::RefId& refId = refIDs[value.mRefID];
                ++mRefCount[std::move(refId)];
            }
        };
        Misc::forEachUnique(refs.rbegin(), refs.rend(), equalByRefNum, incrementRefCount);
        auto& store = getWritable<ESM::Miscellaneous>().mStatic;
        for (const auto& id : keyIDs)
        {
            auto it = store.find(id);
            if (it != store.end())
                it->second.mData.mFlags |= ESM::Miscellaneous::Key;
        }
    }

    int ESMStore::getRefCount(const ESM::RefId& id) const
    {
        auto it = mRefCount.find(id);
        if (it == mRefCount.end())
            return 0;
        return it->second;
    }

    void ESMStore::validate()
    {
        auto& npcs = getWritable<ESM::NPC>();
        if (shouldEnsureFalloutCharacterDefaults(getWritable<ESM::Class>(), getWritable<ESM::Race>(), npcs,
                getWritable<ESM::Creature>(), getWritable<ESM4::Npc>(), getWritable<ESM4::Creature>(),
                getWritable<ESM4::Race>()))
            ensureFalloutCharacterDefaults(
                getWritable<ESM::Class>(), getWritable<ESM::Race>(), getWritable<ESM::Skill>(),
                getWritable<ESM::MagicEffect>(), npcs, getWritable<ESM::Weapon>(), getWritable<ESM::Potion>(),
                getWritable<ESM::Miscellaneous>());
        rebuildIdsIndex();
        mStoreImp->mStaticIds = mStoreImp->mIds;

        std::vector<ESM::NPC> npcsToReplace = getNPCsToReplace(getWritable<ESM::Faction>(), getWritable<ESM::Class>(),
            getWritable<ESM::Race>(), getWritable<ESM::Script>(), npcs.mStatic);

        for (const ESM::NPC& npc : npcsToReplace)
        {
            npcs.eraseStatic(npc.mId);
            npcs.insertStatic(npc);
        }

        removeMissingScripts(getWritable<ESM::Script>(), getWritable<ESM::Creature>().mStatic);

        // Validate spell effects and enchantments for invalid arguments
        auto& spells = getWritable<ESM::Spell>();
        auto& enchantments = getWritable<ESM::Enchantment>();
        auto& magicEffects = getWritable<ESM::MagicEffect>();

        std::vector<ESM::Spell> spellsToReplace = getSpellsToReplace(spells, magicEffects);
        for (const ESM::Spell& spell : spellsToReplace)
        {
            spells.eraseStatic(spell.mId);
            spells.insertStatic(spell);
        }

        std::vector<ESM::Enchantment> enchantmentsToReplace = getSpellsToReplace(enchantments, magicEffects);
        for (const ESM::Enchantment& enchantment : enchantmentsToReplace)
        {
            enchantments.eraseStatic(enchantment.mId);
            enchantments.insertStatic(enchantment);
        }
    }

    void ESMStore::movePlayerRecord()
    {
        auto& npcs = getWritable<ESM::NPC>();
        auto player = npcs.find(ESM::RefId::stringRefId("Player"));
        npcs.insert(*player);
    }

    void ESMStore::validateDynamic()
    {
        auto& npcs = getWritable<ESM::NPC>();
        if (shouldEnsureFalloutCharacterDefaults(getWritable<ESM::Class>(), getWritable<ESM::Race>(), npcs,
                getWritable<ESM::Creature>(), getWritable<ESM4::Npc>(), getWritable<ESM4::Creature>(),
                getWritable<ESM4::Race>()))
            ensureFalloutCharacterDefaults(
                getWritable<ESM::Class>(), getWritable<ESM::Race>(), getWritable<ESM::Skill>(),
                getWritable<ESM::MagicEffect>(), npcs, getWritable<ESM::Weapon>(), getWritable<ESM::Potion>(),
                getWritable<ESM::Miscellaneous>());
        rebuildIdsIndex();

        auto& scripts = getWritable<ESM::Script>();

        std::vector<ESM::NPC> npcsToReplace = getNPCsToReplace(getWritable<ESM::Faction>(), getWritable<ESM::Class>(),
            getWritable<ESM::Race>(), getWritable<ESM::Script>(), npcs.mDynamic);

        for (const ESM::NPC& npc : npcsToReplace)
            npcs.insert(npc);

        removeMissingScripts(scripts, getWritable<ESM::Armor>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Book>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Clothing>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Creature>().mDynamic);
        removeMissingScripts(scripts, getWritable<ESM::Weapon>().mDynamic);

        removeMissingObjects(getWritable<ESM::CreatureLevList>());
        removeMissingObjects(getWritable<ESM::ItemLevList>());
    }

    // Leveled lists can be modified by scripts. This removes items that no longer exist (presumably because the
    // plugin was removed) from modified lists
    template <class T>
    void ESMStore::removeMissingObjects(Store<T>& store)
    {
        for (auto& entry : store.mDynamic)
        {
            auto first = std::remove_if(entry.second.mList.begin(), entry.second.mList.end(), [&](const auto& item) {
                if (!find(item.mId))
                {
                    Log(Debug::Verbose) << "Leveled list " << entry.first << " has nonexistent object " << item.mId
                                        << ", ignoring it.";
                    return true;
                }
                return false;
            });
            entry.second.mList.erase(first, entry.second.mList.end());
        }
    }

    size_t ESMStore::countSavedGameRecords() const
    {
        return 1 // DYNA (dynamic name counter)
            + get<ESM::Potion>().getDynamicSize() + get<ESM::Armor>().getDynamicSize()
            + get<ESM::Book>().getDynamicSize() + get<ESM::Class>().getDynamicSize()
            + get<ESM::Clothing>().getDynamicSize() + get<ESM::Enchantment>().getDynamicSize()
            + get<ESM::NPC>().getDynamicSize() + get<ESM::Spell>().getDynamicSize()
            + get<ESM::Activator>().getDynamicSize() + get<ESM::Miscellaneous>().getDynamicSize()
            + get<ESM::Weapon>().getDynamicSize() + get<ESM::CreatureLevList>().getDynamicSize()
            + get<ESM::ItemLevList>().getDynamicSize() + get<ESM::Creature>().getDynamicSize()
            + get<ESM::Container>().getDynamicSize() + get<ESM::Light>().getDynamicSize()
            + get<ESM::Static>().getDynamicSize() + get<ESM::Door>().getDynamicSize()
            + get<ESM::Probe>().getDynamicSize();
    }

    void ESMStore::write(ESM::ESMWriter& writer, Loading::Listener& progress) const
    {
        writer.startRecord(ESM::REC_DYNA);
        writer.startSubRecord("COUN");
        writer.writeT(mDynamicCount);
        writer.endRecord("COUN");
        writer.endRecord(ESM::REC_DYNA);

        get<ESM::Potion>().write(writer, progress);
        get<ESM::Armor>().write(writer, progress);
        get<ESM::Book>().write(writer, progress);
        get<ESM::Class>().write(writer, progress);
        get<ESM::Clothing>().write(writer, progress);
        get<ESM::Enchantment>().write(writer, progress);
        get<ESM::NPC>().write(writer, progress);
        get<ESM::Miscellaneous>().write(writer, progress);
        get<ESM::Activator>().write(writer, progress);
        get<ESM::Spell>().write(writer, progress);
        get<ESM::Weapon>().write(writer, progress);
        get<ESM::CreatureLevList>().write(writer, progress);
        get<ESM::ItemLevList>().write(writer, progress);
        get<ESM::Creature>().write(writer, progress);
        get<ESM::Container>().write(writer, progress);
        get<ESM::Light>().write(writer, progress);
        get<ESM::Static>().write(writer, progress);
        get<ESM::Door>().write(writer, progress);
        get<ESM::Probe>().write(writer, progress);
    }

    bool ESMStore::readRecord(ESM::ESMReader& reader, uint32_t typeId)
    {
        ESM::RecNameInts type = static_cast<ESM::RecNameInts>(typeId);
        switch (type)
        {
            case ESM::REC_ALCH:
            case ESM::REC_ARMO:
            case ESM::REC_BOOK:
            case ESM::REC_CLAS:
            case ESM::REC_CLOT:
            case ESM::REC_ENCH:
            case ESM::REC_SPEL:
            case ESM::REC_WEAP:
                mStoreImp->mRecNameToStore[type]->read(reader);
                return true;
            case ESM::REC_NPC_:
            case ESM::REC_CREA:
            case ESM::REC_CONT:
            case ESM::REC_MISC:
            case ESM::REC_ACTI:
            case ESM::REC_LEVI:
            case ESM::REC_LEVC:
            case ESM::REC_LIGH:
            case ESM::REC_STAT:
            case ESM::REC_DOOR:
            case ESM::REC_PROB:
                mStoreImp->mRecNameToStore[type]->read(reader, true);
                return true;

            case ESM::REC_DYNA:
                reader.getSubNameIs("COUN");
                if (reader.getFormatVersion() <= ESM::MaxActiveSpellTypeVersion)
                {
                    uint32_t dynamicCount32 = 0;
                    reader.getHT(dynamicCount32);
                    mDynamicCount = dynamicCount32;
                }
                else
                {
                    reader.getHT(mDynamicCount);
                }
                return true;

            default:

                return false;
        }
    }

    void ESMStore::checkPlayer()
    {
        const ESM::NPC* player = get<ESM::NPC>().find(ESM::RefId::stringRefId("Player"));

        if (!get<ESM::Race>().find(player->mRace) || !get<ESM::Class>().find(player->mClass))
            throw std::runtime_error("Invalid player record (race or class unavailable");
    }

    std::pair<std::shared_ptr<MWMechanics::SpellList>, bool> ESMStore::getSpellList(const ESM::RefId& id) const
    {
        auto result = mSpellListCache.find(id);
        std::shared_ptr<MWMechanics::SpellList> ptr;
        if (result != mSpellListCache.end())
            ptr = result->second.lock();
        if (!ptr)
        {
            int type = find(id);
            ptr = std::make_shared<MWMechanics::SpellList>(id, type);
            if (result != mSpellListCache.end())
                result->second = ptr;
            else
                mSpellListCache.insert({ id, ptr });
            return { ptr, false };
        }
        return { ptr, true };
    }

    template <>
    const ESM::Cell* ESMStore::insert<ESM::Cell>(const ESM::Cell& cell)
    {
        return getWritable<ESM::Cell>().insert(cell);
    }

    template <>
    const ESM::NPC* ESMStore::insert<ESM::NPC>(const ESM::NPC& npc)
    {

        auto& npcs = getWritable<ESM::NPC>();
        if (npc.mId == "Player")
        {
            return npcs.insert(npc);
        }
        const ESM::RefId id = ESM::RefId::generated(mDynamicCount++);
        if (npcs.search(id) != nullptr)
            throw std::runtime_error("Try to override existing record: " + id.toDebugString());
        ESM::NPC record = npc;

        record.mId = id;

        ESM::NPC* ptr = npcs.insert(record);
        mStoreImp->mIds[ptr->mId] = ESM::REC_NPC_;
        return ptr;
    }

} // end namespace
