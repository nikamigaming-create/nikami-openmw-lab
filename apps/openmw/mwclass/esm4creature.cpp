#include "esm4creature.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <type_traits>
#include <vector>

#include <components/debug/debuglog.hpp>
#include <components/esm/attr.hpp>
#include <components/esm/defs.hpp>
#include <components/esm3/creaturestate.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadlvlc.hpp>
#include <components/esm4/loadlvli.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/esm4/script.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/aitravel.hpp"
#include "../mwmechanics/aiwander.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/movement.hpp"

#include "../mwworld/containerstore.hpp"
#include "../mwworld/customdata.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/esm4questruntime.hpp"
#include "../mwworld/worldmodel.hpp"

#include "../mwgui/tooltips.hpp"

#include "esm4base.hpp"
#include "fnvactorstate.hpp"

namespace MWClass
{
    class ESM4CreatureContainerStore final : public MWWorld::ContainerStore
    {
        using PlannedItems = std::map<ESM::RefId, int>;

        static constexpr int sMaxLevelledItemDepth = 16;

        enum class AddResult
        {
            Stored,
            Missing,
            InvalidCount,
            Overflow,
        };

        static bool checkedMultiply(int left, int right, int& result)
        {
            if (left < 0 || right <= 0 || left > std::numeric_limits<int>::max() / right)
                return false;
            result = left * right;
            return true;
        }

        static bool addPlannedCount(
            PlannedItems& items, const ESM::RefId& id, int count, std::string_view& failure)
        {
            if (count <= 0)
            {
                failure = "invalid-entry-count";
                return false;
            }

            const auto [it, inserted] = items.emplace(id, count);
            if (!inserted)
            {
                if (it->second > std::numeric_limits<int>::max() - count)
                {
                    failure = "aggregate-count-overflow";
                    return false;
                }
                it->second += count;
            }
            return true;
        }

        static bool isSupportedLevelledTerminal(int recordType)
        {
            switch (recordType)
            {
                case ESM::REC_ALCH4:
                case ESM::REC_AMMO4:
                case ESM::REC_ARMO4:
                case ESM::REC_BOOK4:
                case ESM::REC_IMOD4:
                case ESM::REC_KEYM4:
                case ESM::REC_MISC4:
                case ESM::REC_WEAP4:
                    return true;
                default:
                    return false;
            }
        }

        static bool resolveLevelledItem(const MWWorld::ESMStore& store, const ESM::RefId& listId, int actorLevel,
            int depth, std::set<ESM::RefId>& path, PlannedItems& result, std::string_view& failure)
        {
            if (depth > sMaxLevelledItemDepth)
            {
                failure = "maximum-depth-exceeded";
                return false;
            }
            if (path.contains(listId))
            {
                failure = "cycle";
                return false;
            }

            const ESM4::LevelledItem* list = store.get<ESM4::LevelledItem>().search(listId);
            if (list == nullptr)
            {
                failure = "missing-levelled-list";
                return false;
            }
            if (!list->mHasChanceNone || !list->mHasLvlItemFlags)
            {
                failure = "missing-required-lvld-or-lvlf";
                return false;
            }
            if (list->chanceNone() != 0)
            {
                failure = "chance-none-requires-rng";
                return false;
            }
            if (!list->mChanceGlobal.isZeroOrUnset())
            {
                failure = "chance-global-requires-runtime-evaluation";
                return false;
            }
            if ((list->mLvlItemFlags & ~std::uint8_t{ 0x07 }) != 0
                || (list->useAll() && (list->calcAllLvlLessThanPlayer() || list->calcEachItemInCount())))
            {
                failure = "unsupported-levelled-list-flags";
                return false;
            }
            if (!list->mLvlObjectExtra.empty())
            {
                if (list->mLvlObjectExtra.size() != list->mLvlObject.size())
                {
                    failure = "malformed-entry-extra-data";
                    return false;
                }
                if (std::any_of(list->mLvlObjectExtra.begin(), list->mLvlObjectExtra.end(),
                        [](const auto& value) { return value.has_value(); }))
                {
                    failure = "entry-condition-requires-runtime-evaluation";
                    return false;
                }
            }

            std::vector<const ESM4::LVLO*> eligible;
            if (list->useAll() || list->calcAllLvlLessThanPlayer())
            {
                for (const ESM4::LVLO& entry : list->mLvlObject)
                {
                    if (entry.level <= actorLevel)
                        eligible.push_back(&entry);
                }
            }
            else
            {
                std::optional<std::int16_t> highestLevel;
                for (const ESM4::LVLO& entry : list->mLvlObject)
                {
                    if (entry.level <= actorLevel && (!highestLevel || entry.level > *highestLevel))
                        highestLevel = entry.level;
                }
                if (highestLevel)
                {
                    for (const ESM4::LVLO& entry : list->mLvlObject)
                    {
                        if (entry.level == *highestLevel)
                            eligible.push_back(&entry);
                    }
                }
            }

            if (!list->useAll() && eligible.size() > 1)
            {
                failure = "ambiguous-random-selection";
                return false;
            }
            if (eligible.empty())
                return true;

            path.insert(listId);
            bool resolved = true;
            for (const ESM4::LVLO* entry : eligible)
            {
                if (entry->level <= 0 || entry->count <= 0 || entry->item == 0)
                {
                    failure = "invalid-levelled-list-entry";
                    resolved = false;
                    break;
                }

                const ESM::RefId itemId(ESM::FormId::fromUint32(entry->item));
                PlannedItems child;
                const int recordType = store.find(itemId);
                if (recordType == ESM::REC_LVLI4)
                {
                    if (!resolveLevelledItem(store, itemId, actorLevel, depth + 1, path, child, failure))
                    {
                        resolved = false;
                        break;
                    }
                }
                else if (recordType == 0)
                {
                    failure = "missing-terminal-record";
                    resolved = false;
                    break;
                }
                else if (!isSupportedLevelledTerminal(recordType))
                {
                    failure = "unsupported-terminal-record";
                    resolved = false;
                    break;
                }
                else
                    child.emplace(itemId, 1);

                for (const auto& [childId, childCount] : child)
                {
                    int multiplied = 0;
                    if (!checkedMultiply(childCount, entry->count, multiplied))
                    {
                        failure = "recursive-entry-count-overflow";
                        resolved = false;
                        break;
                    }
                    if (!addPlannedCount(result, childId, multiplied, failure))
                    {
                        resolved = false;
                        break;
                    }
                }
                if (!resolved)
                    break;
            }
            path.erase(listId);
            return resolved;
        }

        template <class Record>
        AddResult addInitialRecord(const MWWorld::ESMStore& store, const ESM::RefId& id, int count)
        {
            if (count <= 0)
                return AddResult::InvalidCount;

            const Record* record = store.get<Record>().search(id);
            if (record == nullptr)
                return AddResult::Missing;

            ESM::CellRef cellRef = ESM::makeBlankCellRef();
            cellRef.mRefID = ESM::RefId::formIdRefId(record->mId);
            MWWorld::LiveCellRef<Record> liveRef(cellRef, record);
            const MWWorld::ConstPtr ptr(&liveRef);
            const int type = getType(ptr);
            for (MWWorld::ContainerStoreIterator item = begin(type); item != end(); ++item)
            {
                if (item->getCellRef().getRefId() != cellRef.mRefID)
                    continue;

                const int oldCount = item->getCellRef().getCount(false);
                if (oldCount < 0 || oldCount > std::numeric_limits<int>::max() - count)
                    return AddResult::Overflow;
                item->getCellRef().setCount(oldCount + count);
                flagAsModified();
                return AddResult::Stored;
            }

            addNewStack(ptr, count);
            return AddResult::Stored;
        }

        template <class Record>
        AddResult validateInitialRecord(const MWWorld::ESMStore& store, const ESM::RefId& id, int count)
        {
            if (count <= 0)
                return AddResult::InvalidCount;

            const Record* record = store.get<Record>().search(id);
            if (record == nullptr)
                return AddResult::Missing;

            ESM::CellRef cellRef = ESM::makeBlankCellRef();
            cellRef.mRefID = ESM::RefId::formIdRefId(record->mId);
            MWWorld::LiveCellRef<Record> liveRef(cellRef, record);
            const MWWorld::ConstPtr ptr(&liveRef);
            const int type = getType(ptr);
            for (MWWorld::ContainerStoreIterator item = begin(type); item != end(); ++item)
            {
                if (item->getCellRef().getRefId() != cellRef.mRefID)
                    continue;

                const int oldCount = item->getCellRef().getCount(false);
                if (oldCount < 0 || oldCount > std::numeric_limits<int>::max() - count)
                    return AddResult::Overflow;
                break;
            }
            return AddResult::Stored;
        }

        AddResult validatePlannedRecord(const MWWorld::ESMStore& store, const ESM::RefId& id, int count)
        {
            switch (store.find(id))
            {
                case ESM::REC_AMMO4:
                    return validateInitialRecord<ESM4::Ammunition>(store, id, count);
                case ESM::REC_ARMO4:
                    return validateInitialRecord<ESM4::Armor>(store, id, count);
                case ESM::REC_MISC4:
                    return validateInitialRecord<ESM4::MiscItem>(store, id, count);
                case ESM::REC_WEAP4:
                    return validateInitialRecord<ESM4::Weapon>(store, id, count);
                case ESM::REC_ALCH4:
                    return validateInitialRecord<ESM4::Potion>(store, id, count);
                case ESM::REC_BOOK4:
                    return validateInitialRecord<ESM4::Book>(store, id, count);
                case ESM::REC_IMOD4:
                    return validateInitialRecord<ESM4::ItemMod>(store, id, count);
                case ESM::REC_KEYM4:
                    return validateInitialRecord<ESM4::Key>(store, id, count);
                default:
                    return AddResult::Missing;
            }
        }

        AddResult addPlannedRecord(const MWWorld::ESMStore& store, const ESM::RefId& id, int count)
        {
            switch (store.find(id))
            {
                case ESM::REC_AMMO4:
                    return addInitialRecord<ESM4::Ammunition>(store, id, count);
                case ESM::REC_ARMO4:
                    return addInitialRecord<ESM4::Armor>(store, id, count);
                case ESM::REC_MISC4:
                    return addInitialRecord<ESM4::MiscItem>(store, id, count);
                case ESM::REC_WEAP4:
                    return addInitialRecord<ESM4::Weapon>(store, id, count);
                case ESM::REC_ALCH4:
                    return addInitialRecord<ESM4::Potion>(store, id, count);
                case ESM::REC_BOOK4:
                    return addInitialRecord<ESM4::Book>(store, id, count);
                case ESM::REC_IMOD4:
                    return addInitialRecord<ESM4::ItemMod>(store, id, count);
                case ESM::REC_KEYM4:
                    return addInitialRecord<ESM4::Key>(store, id, count);
                default:
                    return AddResult::Missing;
            }
        }

        static std::string_view getAddFailure(AddResult result)
        {
            switch (result)
            {
                case AddResult::Missing:
                    return "missing-record";
                case AddResult::InvalidCount:
                    return "invalid-count";
                case AddResult::Overflow:
                    return "duplicate-count-overflow";
                case AddResult::Stored:
                    break;
            }
            return "stored";
        }

        bool commitLevelledPlan(
            const MWWorld::ESMStore& store, const PlannedItems& plan, std::string_view& failure)
        {
            for (const auto& [id, count] : plan)
            {
                const AddResult result = validatePlannedRecord(store, id, count);
                if (result != AddResult::Stored)
                {
                    failure = getAddFailure(result);
                    return false;
                }
            }

            for (const auto& [id, count] : plan)
            {
                const AddResult result = addPlannedRecord(store, id, count);
                if (result != AddResult::Stored)
                {
                    // The complete plan was preflighted against this single-threaded store immediately above.
                    // Reaching this branch would mean an internal mutation violated that precondition.
                    failure = "commit-precondition-changed";
                    return false;
                }
            }
            return true;
        }

    public:
        void fill(
            const ESM4::Creature& creature, const ESM4::Creature* statsProvider, const MWWorld::ESMStore& store)
        {
            std::optional<int> fixedActorLevel;
            std::string_view actorLevelFailure;
            if (statsProvider == nullptr || !statsProvider->mIsFONV)
                actorLevelFailure = "missing-fnv-stats-provider";
            else if ((statsProvider->mBaseConfig.fo3.flags & ESM4::Creature::FO3_PCLevelMult) != 0)
                actorLevelFailure = "pc-level-multiplier-is-unimplemented";
            else if (statsProvider->mBaseConfig.fo3.levelOrMult <= 0)
                actorLevelFailure = "invalid-fixed-actor-level";
            else
                fixedActorLevel = statsProvider->mBaseConfig.fo3.levelOrMult;

            for (const ESM4::InventoryItem& item : creature.mInventory)
            {
                const ESM::RefId ownerId(creature.mId);
                if (item.item == 0)
                {
                    Log(Debug::Warning) << "Ignoring zero FormID in fixed FNV creature inventory for " << ownerId;
                    continue;
                }
                if (item.count == 0 || item.count > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
                {
                    // CNTO stores the count as a signed 32-bit value on disk. The parser preserves its bits in
                    // uint32_t, so values above INT_MAX also cover authored negative counts without narrowing.
                    Log(Debug::Warning) << "Ignoring invalid fixed FNV creature inventory count " << item.count
                                        << " for item " << ESM::RefId(ESM::FormId::fromUint32(item.item)) << " in "
                                        << ownerId;
                    continue;
                }

                const ESM::RefId itemId(ESM::FormId::fromUint32(item.item));
                const int recordType = store.find(itemId);
                if (recordType == 0)
                {
                    Log(Debug::Warning) << "Ignoring missing fixed FNV creature inventory item " << itemId << " in "
                                        << ownerId;
                    continue;
                }
                if (recordType == ESM::REC_LVLI4)
                {
                    std::string_view failure = actorLevelFailure;
                    PlannedItems plan;
                    if (!fixedActorLevel)
                    {
                        Log(Debug::Warning) << "Ignoring non-deterministic LVLI fixed FNV creature inventory item "
                                            << itemId << " in " << ownerId << " reason=" << failure;
                        continue;
                    }

                    std::set<ESM::RefId> path;
                    if (!resolveLevelledItem(store, itemId, *fixedActorLevel, 1, path, plan, failure))
                    {
                        Log(Debug::Warning) << "Ignoring non-deterministic LVLI fixed FNV creature inventory item "
                                            << itemId << " in " << ownerId << " reason=" << failure;
                        continue;
                    }

                    PlannedItems scaledPlan;
                    bool scaled = true;
                    for (const auto& [resolvedId, resolvedCount] : plan)
                    {
                        int total = 0;
                        if (!checkedMultiply(resolvedCount, static_cast<int>(item.count), total))
                        {
                            failure = "outer-inventory-count-overflow";
                            scaled = false;
                            break;
                        }
                        if (!addPlannedCount(scaledPlan, resolvedId, total, failure))
                        {
                            scaled = false;
                            break;
                        }
                    }
                    if (!scaled || !commitLevelledPlan(store, scaledPlan, failure))
                    {
                        Log(Debug::Warning) << "Ignoring non-deterministic LVLI fixed FNV creature inventory item "
                                            << itemId << " in " << ownerId << " reason=" << failure;
                    }
                    continue;
                }

                const int count = static_cast<int>(item.count);
                AddResult result = AddResult::Missing;
                switch (recordType)
                {
                    case ESM::REC_AMMO4:
                        result = addInitialRecord<ESM4::Ammunition>(store, itemId, count);
                        break;
                    case ESM::REC_ARMO4:
                        result = addInitialRecord<ESM4::Armor>(store, itemId, count);
                        break;
                    case ESM::REC_MISC4:
                        result = addInitialRecord<ESM4::MiscItem>(store, itemId, count);
                        break;
                    case ESM::REC_WEAP4:
                        result = addInitialRecord<ESM4::Weapon>(store, itemId, count);
                        break;
                    case ESM::REC_ALCH4:
                        result = addInitialRecord<ESM4::Potion>(store, itemId, count);
                        break;
                    case ESM::REC_BOOK4:
                        result = addInitialRecord<ESM4::Book>(store, itemId, count);
                        break;
                    case ESM::REC_CLOT4:
                        result = addInitialRecord<ESM4::Clothing>(store, itemId, count);
                        break;
                    case ESM::REC_INGR4:
                        result = addInitialRecord<ESM4::Ingredient>(store, itemId, count);
                        break;
                    case ESM::REC_IMOD4:
                        result = addInitialRecord<ESM4::ItemMod>(store, itemId, count);
                        break;
                    case ESM::REC_KEYM4:
                        result = addInitialRecord<ESM4::Key>(store, itemId, count);
                        break;
                    case ESM::REC_LIGH4:
                        result = addInitialRecord<ESM4::Light>(store, itemId, count);
                        break;
                    default:
                        Log(Debug::Warning) << "Ignoring unsupported fixed FNV creature inventory item " << itemId
                                            << " recordType=" << recordType << " in " << ownerId;
                        continue;
                }

                if (result != AddResult::Stored)
                    Log(Debug::Warning) << "Ignoring fixed FNV creature inventory item " << itemId << " count="
                                        << count << " in " << ownerId << " reason=" << getAddFailure(result);
            }
        }

        std::unique_ptr<MWWorld::ContainerStore> clone() override
        {
            auto result = std::make_unique<ESM4CreatureContainerStore>(*this);
            result->updateRefNums();
            return result;
        }
    };

    class ESM4CreatureCustomData : public MWWorld::TypedCustomData<ESM4CreatureCustomData>
    {
    public:
        ESM4::CreatureTemplateCategories mTemplates;
        MWMechanics::CreatureStats mCreatureStats;
        MWMechanics::Movement mMovement;
        std::unique_ptr<MWWorld::ContainerStore> mContainerStore
            = std::make_unique<ESM4CreatureContainerStore>();
        bool mContainerItemsRegistered = false;
        bool mFnvAiSequenceInitialised = false;

        ESM4CreatureCustomData() = default;
        ESM4CreatureCustomData(const ESM4CreatureCustomData& other)
            : mTemplates(other.mTemplates)
            , mCreatureStats(other.mCreatureStats)
            , mMovement(other.mMovement)
            , mContainerStore(other.mContainerStore ? other.mContainerStore->clone()
                                                    : std::make_unique<ESM4CreatureContainerStore>())
            , mFnvAiSequenceInitialised(other.mFnvAiSequenceInitialised)
        {
        }
    };

    static int positiveOrDefault(int value, int fallback)
    {
        value = value < 0 ? -value : value;
        return value > 0 ? value : fallback;
    }

    static int getLevel(const ESM4::Creature& creature)
    {
        return positiveOrDefault(creature.mBaseConfig.fo3.levelOrMult, 1);
    }

    static float getSpeedMultiplier(const ESM4::Creature& creature)
    {
        return std::max<int>(creature.mBaseConfig.fo3.speedMultiplier, 1) / 100.f;
    }

    struct CreatureTemplateLookup
    {
        const ESM4::Creature* mRecord = nullptr;
        bool mThroughLevelledList = false;
    };

    static CreatureTemplateLookup searchCreatureTemplate(
        ESM::FormId id, int depth = 0, bool throughLevelledList = false)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr || id.isZeroOrUnset() || depth > 8)
            return {};

        const ESM::RecNameInts foundType = static_cast<ESM::RecNameInts>(store->find(id));
        if (foundType == ESM::RecNameInts::REC_CREA4)
            return { store->get<ESM4::Creature>().search(id), throughLevelledList };

        if (foundType != ESM::RecNameInts::REC_LVLC4)
            return {};

        const ESM4::LevelledCreature* list = store->get<ESM4::LevelledCreature>().search(id);
        if (list == nullptr || list->mLvlObject.empty())
            return {};

        const ESM4::LVLO* selected = nullptr;
        for (const ESM4::LVLO& entry : list->mLvlObject)
        {
            if (entry.item == 0)
                continue;
            if (selected == nullptr || entry.level <= 1)
                selected = &entry;
            if (entry.level <= 1)
                break;
        }

        if (selected == nullptr)
            return {};

        // Preserve the engine's existing deterministic level-1 bridge. Retail
        // level, chance, and random selection remain a separate runtime slice.
        return searchCreatureTemplate(ESM::FormId::fromUint32(selected->item), depth + 1, true);
    }

    static const ESM4::Creature& getEffectiveCreature(const ESM4::Creature& creature)
    {
        const ESM4::Creature* current = &creature;
        for (int depth = 0; depth < 8; ++depth)
        {
            if (!current->mModel.empty() || !current->mNif.empty())
                return *current;
            if (current->mBaseTemplate.isZeroOrUnset())
                return *current;

            const ESM4::Creature* templated = searchCreatureTemplate(current->mBaseTemplate).mRecord;
            if (templated == nullptr || templated == current)
                return *current;

            current = templated;
        }

        return *current;
    }

    static ESM4::CreatureTemplateChain getCreatureTemplateChain(const ESM4::Creature& creature)
    {
        ESM4::CreatureTemplateChain records;
        const ESM4::Creature* current = &creature;
        records.push_back({ current, {}, false });
        for (std::size_t depth = 1; depth < ESM4::sMaxCreatureTemplateDepth; ++depth)
        {
            if (current->mBaseTemplate.isZeroOrUnset())
                break;

            const CreatureTemplateLookup templated = searchCreatureTemplate(current->mBaseTemplate);
            if (templated.mRecord == nullptr)
                break;

            const auto duplicate = std::find_if(records.begin(), records.end(), [&](const auto& entry) {
                return entry.mRecord != nullptr && entry.mRecord->mId == templated.mRecord->mId;
            });
            if (duplicate != records.end())
                break;

            records.push_back({ templated.mRecord, current->mBaseTemplate, templated.mThroughLevelledList });
            current = templated.mRecord;
        }
        return records;
    }

    static std::vector<const ESM4::Creature*> getLegacyCreatureTemplateRecords(const ESM4::Creature& creature)
    {
        const ESM4::CreatureTemplateChain chain = getCreatureTemplateChain(creature);
        std::vector<const ESM4::Creature*> records;
        records.reserve(chain.size());
        for (const ESM4::CreatureTemplateChainEntry& entry : chain)
            records.push_back(entry.mRecord);
        return records;
    }

    static bool fnvPackageHasExplicitTime(const ESM4::AIPackage& package)
    {
        return package.mSchedule.time != 0xff && package.mSchedule.duration != 0;
    }

    static bool fnvPackageCoversHour(const ESM4::AIPackage& package, float hour)
    {
        if (!fnvPackageHasExplicitTime(package))
            return false;

        const float start = static_cast<float>(package.mSchedule.time);
        const float duration = static_cast<float>(std::min<std::uint32_t>(package.mSchedule.duration, 24));
        const float end = std::fmod(start + duration, 24.f);
        if (duration >= 24.f)
            return true;
        if (start <= end)
            return hour >= start && hour < end;
        return hour >= start || hour < end;
    }

    static float getFnvPackageHour(bool& usedHourOverride)
    {
        usedHourOverride = false;
        float hour = 0.f;
        if (MWBase::Environment::get().getWorld() != nullptr)
            hour = MWBase::Environment::get().getWorld()->getTimeStamp().getHour();
        if (const char* env = std::getenv("OPENMW_FNV_PROCEDURE_HOUR"))
        {
            char* end = nullptr;
            const float overrideHour = std::strtof(env, &end);
            if (end != env && std::isfinite(overrideHour))
            {
                hour = std::fmod(std::max(0.f, overrideHour), 24.f);
                usedHourOverride = true;
            }
        }
        return hour;
    }

    static bool fnvPackageConditionsPass(const ESM4::AIPackage& package)
    {
        if (package.mConditions.empty())
            return true;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return false;
        return world->getESM4QuestRuntime().evaluateConditions(package.mConditions);
    }

    static const ESM4::AIPackage* selectFnvPackage(const std::vector<ESM::FormId>& packageIds, float hour)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr)
            return nullptr;

        const auto& packageStore = store->get<ESM4::AIPackage>();
        const ESM4::AIPackage* fallback = nullptr;
        for (ESM::FormId packageId : packageIds)
        {
            const ESM4::AIPackage* package = packageStore.search(packageId);
            if (package == nullptr)
                continue;
            if (!fnvPackageConditionsPass(*package))
                continue;
            if (fnvPackageCoversHour(*package, hour))
                return package;
            if (fallback == nullptr && !fnvPackageHasExplicitTime(*package))
                fallback = package;
        }
        return fallback;
    }

    static const ESM4::Reference* resolveFnvPackageReference(
        const MWWorld::ESMStore& store, const ESM4::AIPackage& package)
    {
        if (package.mLocation.type == 0 || package.mLocation.type == 4)
        {
            if (const ESM4::Reference* ref
                = store.get<ESM4::Reference>().search(ESM::FormId::fromUint32(package.mLocation.location)))
                return ref;
        }
        if (package.mTarget.type == 0)
            return store.get<ESM4::Reference>().search(ESM::FormId::fromUint32(package.mTarget.target));
        return nullptr;
    }

    static const char* getFnvPackageTypeName(int type)
    {
        switch (type)
        {
            case 3:
                return "Eat";
            case 4:
                return "Sleep";
            case 5:
                return "Wander";
            case 6:
                return "Travel";
            case 8:
                return "UseItemAt";
            case 11:
            case 12:
                return "Sandbox";
            default:
                return "Other";
        }
    }

    static bool isFnvPackageTravelLike(int type)
    {
        return type == 3 || type == 4 || type == 6 || type == 8;
    }

    static bool isFnvPackageWanderLike(int type)
    {
        return type == 5 || type == 11 || type == 12;
    }

    static void initialiseFnvAiSequence(
        ESM4CreatureCustomData& data, const MWWorld::Ptr& ptr, const ESM4::Creature& creature)
    {
        if (data.mFnvAiSequenceInitialised)
            return;
        data.mFnvAiSequenceInitialised = true;

        if (std::getenv("OPENMW_FNV_DISABLE_AI_PACKAGES") != nullptr)
        {
            Log(Debug::Verbose) << "FNV/ESM4 diag: native FNV creature AI package movement disabled by proof env for "
                             << creature.mEditorId;
            return;
        }

        if (creature.mAIPackages.empty() || ptr.getCell() == nullptr || ptr.getCell()->getCell() == nullptr)
            return;

        bool usedHourOverride = false;
        const float hour = getFnvPackageHour(usedHourOverride);
        const ESM4::AIPackage* package = selectFnvPackage(creature.mAIPackages, hour);
        if (package == nullptr)
            return;

        MWMechanics::AiSequence& sequence = data.mCreatureStats.getAiSequence();
        if (!sequence.isEmpty())
            return;

        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr)
            return;

        const ESM::RefId& currentCellId = ptr.getCell()->getCell()->getId();
        if (isFnvPackageTravelLike(package->mData.type))
        {
            const ESM4::Reference* target = resolveFnvPackageReference(*store, *package);
            if (target == nullptr || target->mParent != currentCellId)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: skipped native creature AI travel package "
                                 << package->mEditorId << " type=" << getFnvPackageTypeName(package->mData.type)
                                 << " targetResolved=" << static_cast<bool>(target)
                                 << " currentCell=" << currentCellId << " for " << creature.mEditorId;
                return;
            }

            const ESM::Position& actorPos = ptr.getRefData().getPosition();
            const float dx = actorPos.pos[0] - target->mPos.pos[0];
            const float dy = actorPos.pos[1] - target->mPos.pos[1];
            const float dz = actorPos.pos[2] - target->mPos.pos[2];
            const bool furnitureTarget = store->get<ESM4::Furniture>().search(target->mBaseObj) != nullptr;
            const float arrivalDistance = furnitureTarget ? 128.f : 8.f;
            if (dx * dx + dy * dy + dz * dz < arrivalDistance * arrivalDistance)
            {
                Log(Debug::Verbose) << "FNV/ESM4 diag: skipped native creature AI travel package "
                                 << package->mEditorId << " type=" << getFnvPackageTypeName(package->mData.type)
                                 << " because actor is already at targetRef=" << target->mEditorId
                                 << " furnitureTarget=" << furnitureTarget
                                 << " arrivalDistance=" << arrivalDistance << " for " << creature.mEditorId;
                return;
            }

            MWMechanics::AiTravel travel(target->mPos.pos[0], target->mPos.pos[1], target->mPos.pos[2], true);
            sequence.stack(travel, ptr, true);
            Log(Debug::Verbose) << "FNV/ESM4 diag: stacked native creature AI travel from FNV package "
                             << package->mEditorId << " type=" << getFnvPackageTypeName(package->mData.type)
                             << " hour=" << hour << " override=" << usedHourOverride
                             << " targetRef=" << target->mEditorId << " pos=(" << target->mPos.pos[0] << ","
                             << target->mPos.pos[1] << "," << target->mPos.pos[2] << ") for "
                             << creature.mEditorId;
            return;
        }

        if (isFnvPackageWanderLike(package->mData.type))
        {
            const int distance = std::max(64, package->mLocation.radius > 0 ? package->mLocation.radius : 256);
            const int duration = package->mSchedule.duration > 0
                ? static_cast<int>(std::min<std::uint32_t>(package->mSchedule.duration, 24))
                : 5;
            const int timeOfDay = package->mSchedule.time != 0xff ? package->mSchedule.time : 0;
            std::vector<unsigned char> idles(8, 0);
            MWMechanics::AiWander wander(distance, duration, timeOfDay, idles, true);
            sequence.stack(wander, ptr, true);
            Log(Debug::Verbose) << "FNV/ESM4 diag: stacked native creature AI wander from FNV package "
                             << package->mEditorId << " type=" << getFnvPackageTypeName(package->mData.type)
                             << " hour=" << hour << " override=" << usedHourOverride << " distance=" << distance
                             << " duration=" << duration << " for " << creature.mEditorId;
        }
    }

    static void initialiseActorStats(ESM4CreatureCustomData& data, const ESM4::Creature* statsRecord,
        const ESM4::Creature* aiRecord, const ESM4::Creature& baseCreature)
    {
        MWMechanics::CreatureStats& stats = data.mCreatureStats;
        stats.setLevel(statsRecord != nullptr ? getLevel(*statsRecord) : 1);

        float health = 100.f;
        if (statsRecord != nullptr && statsRecord->mIsFONV && statsRecord->mHasFNVData)
        {
            const ESM4::Creature::FNVData& attributes = statsRecord->mFNVData;
            stats.setAttribute(ESM::Attribute::Strength, attributes.strength);
            stats.setAttribute(ESM::Attribute::Intelligence, attributes.intelligence);
            // OpenMW's ESM3 stat shell has no Perception slot. Keep the same
            // Fallout-to-shell mapping as native ESM4 NPCs use.
            stats.setAttribute(ESM::Attribute::Willpower, attributes.perception);
            stats.setAttribute(ESM::Attribute::Agility, attributes.agility);
            // Fallout CREA DATA has no speed attribute; 50 is the neutral
            // shell value and ACBS still supplies the authored multiplier.
            stats.setAttribute(ESM::Attribute::Speed, 50);
            stats.setAttribute(ESM::Attribute::Endurance, attributes.endurance);
            stats.setAttribute(ESM::Attribute::Personality, attributes.charisma);
            stats.setAttribute(ESM::Attribute::Luck, attributes.luck);
            health = static_cast<float>(attributes.health);
        }
        else if (statsRecord != nullptr)
        {
            const ESM4::AttributeValues& attributes = statsRecord->mData.attribs;
            stats.setAttribute(ESM::Attribute::Strength, attributes.strength ? attributes.strength : 50);
            stats.setAttribute(ESM::Attribute::Intelligence, attributes.intelligence ? attributes.intelligence : 25);
            stats.setAttribute(ESM::Attribute::Willpower, attributes.willpower ? attributes.willpower : 25);
            stats.setAttribute(ESM::Attribute::Agility, attributes.agility ? attributes.agility : 50);
            stats.setAttribute(ESM::Attribute::Speed, attributes.speed ? attributes.speed : 50);
            stats.setAttribute(ESM::Attribute::Endurance, attributes.endurance ? attributes.endurance : 50);
            stats.setAttribute(ESM::Attribute::Personality, attributes.personality ? attributes.personality : 25);
            stats.setAttribute(ESM::Attribute::Luck, attributes.luck ? attributes.luck : 50);
            health = statsRecord->mData.health > 0 ? static_cast<float>(statsRecord->mData.health) : 100.f;
        }
        else
        {
            // A delegated category with no valid provider fails closed to a
            // neutral shell instead of consuming the delegating record.
            stats.setAttribute(ESM::Attribute::Strength, 50);
            stats.setAttribute(ESM::Attribute::Intelligence, 25);
            stats.setAttribute(ESM::Attribute::Willpower, 25);
            stats.setAttribute(ESM::Attribute::Agility, 50);
            stats.setAttribute(ESM::Attribute::Speed, 50);
            stats.setAttribute(ESM::Attribute::Endurance, 50);
            stats.setAttribute(ESM::Attribute::Personality, 25);
            stats.setAttribute(ESM::Attribute::Luck, 50);
        }

        const float fatigue
            = statsRecord != nullptr && statsRecord->mIsFONV && statsRecord->mBaseConfig.fo3.fatigue > 0
            ? static_cast<float>(statsRecord->mBaseConfig.fo3.fatigue)
            : 100.f;
        stats.setHealth(health);
        stats.setMagicka(0.f);
        stats.setFatigue(fatigue);
        stats.getSpells().setSpells(ESM::RefId(baseCreature.mId), ESM::REC_CREA4);

        int aggression = 0;
        int confidence = 0;
        int responsibility = 0;
        if (aiRecord != nullptr && aiRecord->mIsFONV && aiRecord->mHasFNVAIData)
        {
            aggression = aiRecord->mFNVAIData.aggression;
            confidence = aiRecord->mFNVAIData.confidence;
            responsibility = aiRecord->mFNVAIData.responsibility;
        }
        else if (aiRecord != nullptr)
        {
            aggression = aiRecord->mAIData.aggression;
            confidence = aiRecord->mAIData.confidence;
            responsibility = aiRecord->mAIData.responsibility;
        }
        stats.setAiSetting(MWMechanics::AiSetting::Hello, 30);
        stats.setAiSetting(MWMechanics::AiSetting::Fight,
            std::getenv("OPENMW_FNV_DISABLE_AI_PACKAGES") != nullptr ? 0 : aggression);
        stats.setAiSetting(MWMechanics::AiSetting::Flee, 100 - confidence);
        stats.setAiSetting(MWMechanics::AiSetting::Alarm, responsibility);

        if (stats.isDead())
            stats.setDeathAnimationFinished((baseCreature.mFlags & ESM::FLAG_Persistent) != 0);
    }

    static ESM4::CreatureTemplateCategories makeSingleCreatureCategories(const ESM4::Creature* record)
    {
        ESM4::CreatureTemplateCategories result;
        result.mTraits = record;
        result.mStats = record;
        result.mFactions = record;
        result.mActorEffects = record;
        result.mAIData = record;
        result.mAIPackages = record;
        result.mModel = record;
        result.mBaseData = record;
        result.mInventory = record;
        result.mScript = record;
        return result;
    }

    namespace
    {
        bool isFinitePosition(const ESM::Position& position)
        {
            for (int i = 0; i < 3; ++i)
            {
                if (!std::isfinite(position.pos[i]) || !std::isfinite(position.rot[i]))
                    return false;
            }
            return true;
        }

        bool isFiniteObjectState(const ESM::ObjectState& state)
        {
            if (!std::isfinite(state.mRef.mScale) || !std::isfinite(state.mRef.mChargeIntRemainder)
                || !std::isfinite(state.mRef.mEnchantmentCharge) || !isFinitePosition(state.mRef.mPos)
                || !isFinitePosition(state.mPosition) || state.mHasLocals > 1 || state.mEnabled > 1)
                return false;
            for (const ESM::AnimationState::ScriptedAnimation& animation : state.mAnimationState.mScriptedAnims)
            {
                if (!std::isfinite(animation.mTime))
                    return false;
            }
            for (const ESM::LuaScript& script : state.mLuaScripts.mScripts)
            {
                for (const ESM::LuaTimer& timer : script.mTimers)
                {
                    if (!std::isfinite(timer.mTime))
                        return false;
                }
            }
            return true;
        }

        template <class T>
        bool isFiniteStat(const ESM::StatState<T>& state)
        {
            if constexpr (std::is_floating_point_v<T>)
            {
                if (!std::isfinite(state.mBase) || !std::isfinite(state.mMod) || !std::isfinite(state.mCurrent))
                    return false;
            }
            return std::isfinite(state.mDamage) && std::isfinite(state.mProgress);
        }

        bool isSupportedFnvInventoryType(int type)
        {
            switch (type)
            {
                case ESM::REC_AMMO4:
                case ESM::REC_ARMO4:
                case ESM::REC_MISC4:
                case ESM::REC_WEAP4:
                case ESM::REC_ALCH4:
                case ESM::REC_BOOK4:
                case ESM::REC_CLOT4:
                case ESM::REC_INGR4:
                case ESM::REC_IMOD4:
                case ESM::REC_KEYM4:
                case ESM::REC_LIGH4:
                    return true;
                default:
                    return false;
            }
        }

        bool validateCreatureStats(const ESM::CreatureStats& stats, std::string& error)
        {
            for (const auto& attribute : stats.mAttributes)
            {
                if (!isFiniteStat(attribute))
                {
                    error = "non-finite attribute state";
                    return false;
                }
            }
            for (const auto& dynamic : stats.mDynamic)
            {
                if (!isFiniteStat(dynamic))
                {
                    error = "non-finite dynamic state";
                    return false;
                }
            }
            for (const auto& setting : stats.mAiSettings)
            {
                if (!isFiniteStat(setting))
                {
                    error = "non-finite AI setting state";
                    return false;
                }
            }
            if (!std::isfinite(stats.mTradeTime.mHour) || !std::isfinite(stats.mTimeOfDeath.mHour)
                || !std::isfinite(stats.mFallHeight))
            {
                error = "non-finite creature time/fall state";
                return false;
            }
            if (stats.mDrawState < 0 || stats.mDrawState > 2)
            {
                error = "invalid draw state";
                return false;
            }
            if (stats.mMissingACDT || stats.mRecalcDynamicStats)
            {
                // Current FNV CreatureState writers always persist the complete compatibility shell and never ask
                // the ESM3 loader to recalculate magicka from unrelated Morrowind GMSTs.
                error = "unsupported legacy creature-stat state";
                return false;
            }
            for (const auto& [effect, values] : stats.mMagicEffects.mEffects)
            {
                (void)effect;
                if (!std::isfinite(values.second))
                {
                    error = "non-finite magic-effect state";
                    return false;
                }
            }
            const auto validateActiveSpells = [&](const auto& spells) {
                for (const auto& spell : spells)
                {
                    if (spell.mWorsenings >= 0 && !std::isfinite(spell.mNextWorsening.mHour))
                        return false;
                    for (const auto& effect : spell.mEffects)
                    {
                        if (!std::isfinite(effect.mMagnitude) || !std::isfinite(effect.mMinMagnitude)
                            || !std::isfinite(effect.mMaxMagnitude) || !std::isfinite(effect.mDuration)
                            || !std::isfinite(effect.mTimeLeft))
                            return false;
                    }
                }
                return true;
            };
            if (!validateActiveSpells(stats.mActiveSpells.mSpells)
                || !validateActiveSpells(stats.mActiveSpells.mQueue))
            {
                error = "non-finite active-spell state";
                return false;
            }
            for (const auto& [id, timestamp] : stats.mSpells.mUsedPowers)
            {
                (void)id;
                if (!std::isfinite(timestamp.mHour))
                {
                    error = "non-finite used-power time";
                    return false;
                }
            }
            for (const auto& [id, parameters] : stats.mSpells.mSpellParams)
            {
                (void)id;
                for (const auto& [effect, magnitude] : parameters.mEffectRands)
                {
                    (void)effect;
                    if (!std::isfinite(magnitude))
                    {
                        error = "non-finite spell random state";
                        return false;
                    }
                }
            }
            for (const auto& package : stats.mAiSequence.mPackages)
            {
                if (package.mPackage == nullptr)
                {
                    error = "missing AI package payload";
                    return false;
                }
                switch (package.mType)
                {
                    case ESM::AiSequence::Ai_Wander:
                    {
                        const auto* value = dynamic_cast<const ESM::AiSequence::AiWander*>(package.mPackage.get());
                        if (value == nullptr || !std::isfinite(value->mDurationData.mRemainingDuration)
                            || (value->mStoredInitialActorPosition
                                && (!std::isfinite(value->mInitialActorPosition.mValues[0])
                                    || !std::isfinite(value->mInitialActorPosition.mValues[1])
                                    || !std::isfinite(value->mInitialActorPosition.mValues[2]))))
                        {
                            error = "invalid wander AI package state";
                            return false;
                        }
                        break;
                    }
                    case ESM::AiSequence::Ai_Travel:
                    {
                        const auto* value = dynamic_cast<const ESM::AiSequence::AiTravel*>(package.mPackage.get());
                        if (value == nullptr || !std::isfinite(value->mData.mX) || !std::isfinite(value->mData.mY)
                            || !std::isfinite(value->mData.mZ))
                        {
                            error = "invalid travel AI package state";
                            return false;
                        }
                        break;
                    }
                    case ESM::AiSequence::Ai_Escort:
                    {
                        const auto* value = dynamic_cast<const ESM::AiSequence::AiEscort*>(package.mPackage.get());
                        if (value == nullptr || !std::isfinite(value->mData.mX) || !std::isfinite(value->mData.mY)
                            || !std::isfinite(value->mData.mZ) || !std::isfinite(value->mRemainingDuration))
                        {
                            error = "invalid escort AI package state";
                            return false;
                        }
                        break;
                    }
                    case ESM::AiSequence::Ai_Follow:
                    {
                        const auto* value = dynamic_cast<const ESM::AiSequence::AiFollow*>(package.mPackage.get());
                        if (value == nullptr || !std::isfinite(value->mData.mX) || !std::isfinite(value->mData.mY)
                            || !std::isfinite(value->mData.mZ) || !std::isfinite(value->mRemainingDuration))
                        {
                            error = "invalid follow AI package state";
                            return false;
                        }
                        break;
                    }
                    case ESM::AiSequence::Ai_Activate:
                        if (dynamic_cast<const ESM::AiSequence::AiActivate*>(package.mPackage.get()) == nullptr)
                        {
                            error = "invalid activate AI package state";
                            return false;
                        }
                        break;
                    case ESM::AiSequence::Ai_Combat:
                        if (dynamic_cast<const ESM::AiSequence::AiCombat*>(package.mPackage.get()) == nullptr)
                        {
                            error = "invalid combat AI package state";
                            return false;
                        }
                        break;
                    case ESM::AiSequence::Ai_Pursue:
                        if (dynamic_cast<const ESM::AiSequence::AiPursue*>(package.mPackage.get()) == nullptr)
                        {
                            error = "invalid pursue AI package state";
                            return false;
                        }
                        break;
                    default:
                        error = "unknown AI package discriminator";
                        return false;
                }
            }
            if (stats.mAiSequence.mLastAiPackage < -1 || stats.mAiSequence.mLastAiPackage > 11)
            {
                error = "invalid last AI package type";
                return false;
            }
            return true;
        }

        std::unique_ptr<ESM4CreatureCustomData> makeCreatureCustomData(const ESM4::Creature& creature)
        {
            auto data = std::make_unique<ESM4CreatureCustomData>();
            if (creature.mIsFONV)
                data->mTemplates = ESM4::resolveCreatureTemplateCategories(getCreatureTemplateChain(creature));
            else
                data->mTemplates = makeSingleCreatureCategories(&getEffectiveCreature(creature));

            initialiseActorStats(*data, data->mTemplates.mStats, data->mTemplates.mAIData, creature);
            if (creature.mIsFONV && data->mTemplates.mInventory != nullptr)
            {
                const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
                if (store != nullptr)
                    static_cast<ESM4CreatureContainerStore&>(*data->mContainerStore)
                        .fill(*data->mTemplates.mInventory, data->mTemplates.mStats, *store);
                else
                    Log(Debug::Warning) << "Unable to initialize fixed FNV creature inventory for "
                                        << ESM::RefId(creature.mId) << ": ESM store is unavailable";
            }
            return data;
        }
    }

    bool validateFnvPlacedObjectState(
        const ESM::ObjectState& state, std::string_view objectKind, std::string& error)
    {
        if (!isFiniteObjectState(state))
        {
            error = "non-finite or invalid outer object state";
            return false;
        }
        if (state.mRef.mCount < 0)
        {
            // Zero is the normal saved deletion state. Negative counts are ESM3 restocking semantics and are not
            // valid for placed FNV references.
            error = "negative outer " + std::string(objectKind) + " count";
            return false;
        }
        if (!state.mRef.mRefNum.isSet())
        {
            error = "unset outer " + std::string(objectKind) + " reference number";
            return false;
        }
        return true;
    }

    bool validateFnvActorState(bool isFnv, std::string_view actorKind, const ESM::CreatureState& state,
        const MWWorld::ESMStore& store, std::string& error)
    {
        if (!validateFnvPlacedObjectState(state, "actor", error))
            return false;
        if (!isFnv)
        {
            error = "CreatureState is only supported for Fallout: New Vegas " + std::string(actorKind);
            return false;
        }
        if (!state.mHasCustomState)
            return true;
        if (!validateCreatureStats(state.mCreatureStats, error))
            return false;

        std::set<ESM::RefNum> refNums;
        std::map<ESM::RefId, std::int64_t> counts;
        for (const ESM::ObjectState& item : state.mInventory.mItems)
        {
            if (!isFiniteObjectState(item) || item.mRef.mCount <= 0)
            {
                error = "invalid inventory object state";
                return false;
            }
            if (item.mRef.mRefNum.isSet())
            {
                if (item.mRef.mRefNum == state.mRef.mRefNum || !refNums.insert(item.mRef.mRefNum).second)
                {
                    error = "duplicate inventory reference number";
                    return false;
                }
            }

            const int type = store.find(item.mRef.mRefID);
            if (type == 0)
                continue; // Missing/remapped-away content is consumed and dropped by ContainerStore::readState.
            if (!isSupportedFnvInventoryType(type))
            {
                error = "unsupported FNV actor inventory record type";
                return false;
            }
            std::int64_t& total = counts[item.mRef.mRefID];
            total += item.mRef.mCount;
            if (total > std::numeric_limits<int>::max())
            {
                error = "inventory count overflow";
                return false;
            }
        }
        for (const auto& [index, slot] : state.mInventory.mEquipmentSlots)
        {
            (void)slot;
            if (index >= state.mInventory.mItems.size())
            {
                error = "invalid equipment-state item index";
                return false;
            }
        }
        if (state.mInventory.mSelectedEnchantItem
            && *state.mInventory.mSelectedEnchantItem >= state.mInventory.mItems.size())
        {
            error = "invalid selected-enchantment item index";
            return false;
        }
        for (const auto& [id, effects] : state.mInventory.mPermanentMagicEffectMagnitudes)
        {
            (void)id;
            for (const auto& [magnitude, multiplier] : effects)
            {
                if (!std::isfinite(magnitude) || !std::isfinite(multiplier))
                {
                    error = "non-finite permanent inventory effect";
                    return false;
                }
            }
        }
        return true;
    }

    bool ESM4Creature::validateState(const ESM4::Creature& creature, const ESM::CreatureState& state,
        const MWWorld::ESMStore& store, std::string& error)
    {
        return validateFnvActorState(creature.mIsFONV, "CREA", state, store, error);
    }

    ESM4CreatureCustomData& ESM4Creature::getCustomData(const MWWorld::ConstPtr& ptr)
    {
        MWWorld::RefData& refData = const_cast<MWWorld::RefData&>(ptr.getRefData());
        if (MWWorld::CustomData* customData = refData.getCustomData())
            return *dynamic_cast<ESM4CreatureCustomData*>(customData);

        const ESM4::Creature* creature = ptr.get<ESM4::Creature>()->mBase;
        auto data = makeCreatureCustomData(*creature);
        ESM4CreatureCustomData& result = *data;
        refData.setCustomData(std::move(data));

        const ESM4::Creature* model = result.mTemplates.mModel;
        const ESM4::Creature* traits = result.mTemplates.mTraits;
        Log(Debug::Verbose) << "FNV/ESM4 diag: initialized creature actor shell for \"" << creature->mEditorId << "\" ("
                         << ESM::RefId(creature->mId) << ") level=" << result.mCreatureStats.getLevel()
                         << " health=" << result.mCreatureStats.getHealth().getModified()
                         << " model=" << (model != nullptr ? model->mModel : std::string{})
                         << " kfCount=" << (model != nullptr ? model->mKf.size() : 0)
                         << " modelResolved=" << static_cast<bool>(model)
                         << " statsResolved=" << static_cast<bool>(result.mTemplates.mStats)
                         << " aiResolved=" << static_cast<bool>(result.mTemplates.mAIData)
                         << " traitsResolved=" << static_cast<bool>(traits)
                         << " template=" << ESM::RefId(creature->mBaseTemplate);
        return result;
    }

    const ESM4::Creature* ESM4Creature::getFactionsRecord(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mTemplates.mFactions;
    }

    std::string_view ESM4Creature::getModel(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4::Creature* base = ptr.get<ESM4::Creature>()->mBase;
        if (!base->mIsFONV)
        {
            const ESM4::CreatureVisualTemplate visual
                = ESM4::resolveCreatureVisualTemplate(getLegacyCreatureTemplateRecords(*base));
            if (visual.mModel != nullptr)
                return visual.mModel->mModel;
            if (visual.mNif != nullptr && !visual.mNif->mNif.empty())
                return visual.mNif->mNif.front();
            return {};
        }

        const ESM4::Creature* model = getCustomData(ptr).mTemplates.mModel;
        if (model != nullptr && !model->mModel.empty())
            return model->mModel;
        if (model != nullptr && !model->mNif.empty())
            return model->mNif.front();
        return {};
    }

    std::string_view ESM4Creature::getName(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4::Creature* creature = ptr.get<ESM4::Creature>()->mBase;
        if (creature->mIsFONV)
        {
            const ESM4::Creature* baseData = getCustomData(ptr).mTemplates.mBaseData;
            return baseData != nullptr ? std::string_view(baseData->mFullName) : std::string_view{};
        }
        if (!creature->mFullName.empty())
            return creature->mFullName;
        return getEffectiveCreature(*creature).mFullName;
    }

    bool ESM4Creature::hasToolTip(const MWWorld::ConstPtr& ptr) const
    {
        return !getName(ptr).empty();
    }

    MWGui::ToolTipInfo ESM4Creature::getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const
    {
        return ESM4Impl::getToolTipInfo(getName(ptr), count);
    }

    MWMechanics::CreatureStats& ESM4Creature::getCreatureStats(const MWWorld::Ptr& ptr) const
    {
        ESM4CreatureCustomData& data = getCustomData(ptr);
        if (data.mTemplates.mAIPackages != nullptr)
            initialiseFnvAiSequence(data, ptr, *data.mTemplates.mAIPackages);
        else
            data.mFnvAiSequenceInitialised = true;
        return data.mCreatureStats;
    }

    MWWorld::ContainerStore& ESM4Creature::getContainerStore(const MWWorld::Ptr& ptr) const
    {
        ESM4CreatureCustomData& data = getCustomData(ptr);
        MWWorld::ContainerStore& store = *data.mContainerStore;
        store.setPtr(ptr);
        if (!data.mContainerItemsRegistered)
        {
            MWWorld::WorldModel* worldModel = MWBase::Environment::get().getWorldModel();
            if (worldModel != nullptr)
            {
                for (const MWWorld::Ptr item : store)
                    worldModel->registerPtr(item);
                data.mContainerItemsRegistered = true;
            }
        }
        return store;
    }

    void ESM4Creature::readAdditionalState(const MWWorld::Ptr& ptr, const ESM::ObjectState& state) const
    {
        if (!state.mHasCustomState)
            return;

        const ESM4::Creature* creature = ptr.get<ESM4::Creature>()->mBase;
        if (creature == nullptr || !creature->mIsFONV)
            return;

        // CellStore validates the complete CreatureState before LiveCellRef applies either its CellRef/RefData or this
        // payload. Build the replacement off-reference so the old CustomData remains intact until both stores load.
        const ESM::CreatureState& creatureState = state.asCreatureState();
        auto data = makeCreatureCustomData(*creature);
        // Explicit CreatureState is the complete mutable inventory. Do not clear an authored baseline through a
        // detached candidate store; replace it with an empty store and load the saved stacks directly.
        data->mContainerStore = std::make_unique<ESM4CreatureContainerStore>();
        data->mContainerStore->setPtr(ptr);
        data->mContainerStore->readState(creatureState.mInventory);
        data->mCreatureStats.readState(creatureState.mCreatureStats);
        data->mContainerItemsRegistered = true; // ContainerStore::readState registered each retained saved item.
        data->mFnvAiSequenceInitialised = true;
        ptr.getRefData().setCustomData(std::move(data));
    }

    void ESM4Creature::writeAdditionalState(const MWWorld::ConstPtr& ptr, ESM::ObjectState& state) const
    {
        const ESM4::Creature* creature = ptr.get<ESM4::Creature>()->mBase;
        const MWWorld::CustomData* customData = ptr.getRefData().getCustomData();
        if (creature == nullptr || !creature->mIsFONV || customData == nullptr)
        {
            state.mHasCustomState = false;
            return;
        }

        const auto* data = dynamic_cast<const ESM4CreatureCustomData*>(customData);
        if (data == nullptr)
        {
            state.mHasCustomState = false;
            return;
        }

        ESM::CreatureState& creatureState = state.asCreatureState();
        data->mContainerStore->writeState(creatureState.mInventory);
        data->mCreatureStats.writeState(creatureState.mCreatureStats);
        state.mHasCustomState = true;
    }

    MWMechanics::Movement& ESM4Creature::getMovementSettings(const MWWorld::Ptr& ptr) const
    {
        return getCustomData(ptr).mMovement;
    }

    float ESM4Creature::getWalkSpeed(const MWWorld::Ptr& ptr) const
    {
        ESM4CreatureCustomData& data = getCustomData(ptr);
        const ESM4::Creature* creature = data.mTemplates.mStats;
        return std::max(1.f,
                   data.mCreatureStats.getAttribute(ESM::Attribute::Speed).getModified())
            * 2.5f * (creature != nullptr ? getSpeedMultiplier(*creature) : 1.f);
    }

    float ESM4Creature::getRunSpeed(const MWWorld::Ptr& ptr) const
    {
        return getWalkSpeed(ptr) * 1.65f;
    }

    float ESM4Creature::getSwimSpeed(const MWWorld::Ptr& ptr) const
    {
        return getWalkSpeed(ptr);
    }

    float ESM4Creature::getCapacity(const MWWorld::Ptr& ptr) const
    {
        const MWMechanics::CreatureStats& stats = getCreatureStats(ptr);
        return stats.getAttribute(ESM::Attribute::Strength).getModified() * 5.f;
    }

    float ESM4Creature::getMaxSpeed(const MWWorld::Ptr& ptr) const
    {
        const MWMechanics::CreatureStats& stats = getCreatureStats(ptr);
        if (stats.isParalyzed() || stats.getKnockedDown() || stats.isDead())
            return 0.f;
        if (stats.getStance(MWMechanics::CreatureStats::Stance_Run))
            return getRunSpeed(ptr);
        return getWalkSpeed(ptr);
    }

    float ESM4Creature::getSkill(const MWWorld::Ptr& ptr, ESM::RefId id) const
    {
        (void)ptr;
        (void)id;
        return 50.f;
    }

    bool ESM4Creature::isPersistent(const MWWorld::ConstPtr& ptr) const
    {
        return (ptr.get<ESM4::Creature>()->mBase->mFlags & ESM::FLAG_Persistent) != 0;
    }

    bool ESM4Creature::canFly(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4::Creature* base = ptr.get<ESM4::Creature>()->mBase;
        const ESM4::Creature* model
            = base->mIsFONV ? getCustomData(ptr).mTemplates.mModel : &getEffectiveCreature(*base);
        return model != nullptr && (model->mBaseConfig.fo3.flags & ESM4::Creature::FO3_CanFly) != 0;
    }

    bool ESM4Creature::canSwim(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4::Creature* base = ptr.get<ESM4::Creature>()->mBase;
        const ESM4::Creature* model
            = base->mIsFONV ? getCustomData(ptr).mTemplates.mModel : &getEffectiveCreature(*base);
        return model != nullptr && (model->mBaseConfig.fo3.flags & ESM4::Creature::FO3_CanSwim) != 0;
    }

    bool ESM4Creature::canWalk(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4::Creature* base = ptr.get<ESM4::Creature>()->mBase;
        const ESM4::Creature* model
            = base->mIsFONV ? getCustomData(ptr).mTemplates.mModel : &getEffectiveCreature(*base);
        return model != nullptr && (model->mBaseConfig.fo3.flags & ESM4::Creature::FO3_CanWalk) != 0;
    }

    void ESM4Creature::adjustScale(
        const MWWorld::ConstPtr& ptr, osg::Vec3f& scale, bool /* rendering */) const
    {
        // Fallout's CREA BNAM is an actor-base scale, independent of the placed ACHR/ACRE XSCL.  Physics
        // already used it while the render root only received the placed-reference scale, which made ravens
        // 1/2.5 size, mantises 2x size, and every other non-1.0 creature disagree with retail.  Apply the same
        // multiplicative class-scale contract used by native ESM3 creatures to both render and collision.
        const ESM4::Creature* base = ptr.get<ESM4::Creature>()->mBase;
        const ESM4::Creature* stats
            = base->mIsFONV ? getCustomData(ptr).mTemplates.mStats : &getEffectiveCreature(*base);
        if (stats != nullptr)
            scale *= stats->mBaseScale;
    }
}
