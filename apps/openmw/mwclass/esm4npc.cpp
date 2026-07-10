#include "esm4npc.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <string>

#include <components/esm/attr.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadlvli.hpp>
#include <components/esm4/loadlvln.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadotft.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadweap.hpp>

#include <components/misc/resourcehelpers.hpp>

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/aitravel.hpp"
#include "../mwmechanics/aiwander.hpp"
#include "../mwmechanics/movement.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/containerstore.hpp"
#include "../mwworld/customdata.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/actiontalk.hpp"
#include "../mwworld/failedaction.hpp"

#include "esm4base.hpp"

namespace MWClass
{
    static bool worldViewerActorTelemetryEnabled()
    {
        const char* value = std::getenv("OPENMW_WORLD_VIEWER_ACTOR_TELEMETRY");
        return value != nullptr && *value != '\0' && value[0] != '0';
    }

    static const char* getWorldViewerNpcGameTag(const ESM4::Npc& npc)
    {
        if (npc.mIsTES4)
            return "TES4";
        if (npc.mIsFO3)
            return "FO3";
        if (npc.mIsFONV)
            return "FONV";
        if (npc.mIsFO4)
            return "FO4";
        return "TES5_OR_UNKNOWN";
    }

    static bool isWorldViewerMarkerActorModel(std::string_view model)
    {
        std::string lowered;
        lowered.reserve(model.size());
        for (char ch : model)
        {
            const char normalized = ch == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            lowered.push_back(normalized);
        }

        return lowered.ends_with("marker_creature.nif") || lowered.ends_with("marker_npc.nif");
    }

    template <class LevelledRecord, class TargetRecord>
    static std::vector<const TargetRecord*> withBaseTemplates(
        const TargetRecord* rec, int level = MWClass::ESM4Impl::sDefaultLevel)
    {
        std::vector<const TargetRecord*> res{ rec };
        while (true)
        {
            const TargetRecord* newRec
                = MWClass::ESM4Impl::resolveLevelled<ESM4::LevelledNpc, ESM4::Npc>(rec->mBaseTemplate, level);
            if (!newRec || newRec == rec)
                return res;
            res.push_back(rec = newRec);
        }
    }

    static const ESM4::Npc* chooseTemplate(const std::vector<const ESM4::Npc*>& recs, uint16_t flag)
    {
        for (const auto* rec : recs)
        {
            if (rec->mIsTES4)
                return rec;
            else if (rec->mIsFONV)
            {
                if (!(rec->mBaseConfig.fo3.templateFlags & flag))
                    return rec;
            }
            else if (rec->mIsFO4)
            {
                if (!(rec->mBaseConfig.fo4.templateFlags & flag))
                    return rec;
            }
            else if (!(rec->mBaseConfig.tes5.templateFlags & flag))
                return rec;
        }
        return nullptr;
    }

    class ESM4NpcCustomData : public MWWorld::TypedCustomData<ESM4NpcCustomData>
    {
    public:
        const ESM4::Npc* mTraits = nullptr;
        const ESM4::Npc* mModel = nullptr;
        const ESM4::Npc* mAIPackage = nullptr;
        const ESM4::Npc* mStats = nullptr;
        const ESM4::Npc* mAIData = nullptr;
        const ESM4::Npc* mBaseData = nullptr;
        const ESM4::Race* mRace = nullptr;
        bool mIsFemale = false;
        MWMechanics::CreatureStats mCreatureStats;
        MWMechanics::Movement mMovement;
        std::unique_ptr<MWWorld::ContainerStore> mContainerStore;

        // TODO: Use InventoryStore instead (currently doesn't support ESM4 objects)
        std::vector<const ESM4::Armor*> mEquippedArmor;
        std::vector<const ESM4::Clothing*> mEquippedClothing;
        const ESM4::Weapon* mEquippedWeapon = nullptr;
        bool mFnvAiSequenceInitialised = false;

        ESM4NpcCustomData();
        ESM4NpcCustomData(const ESM4NpcCustomData& other);

        ESM4NpcCustomData& asESM4NpcCustomData() override { return *this; }
        const ESM4NpcCustomData& asESM4NpcCustomData() const override { return *this; }
    };

    ESM4NpcCustomData::ESM4NpcCustomData()
        : mContainerStore(std::make_unique<MWWorld::ContainerStore>())
    {
    }

    ESM4NpcCustomData::ESM4NpcCustomData(const ESM4NpcCustomData& other)
        : mTraits(other.mTraits)
        , mModel(other.mModel)
        , mAIPackage(other.mAIPackage)
        , mStats(other.mStats)
        , mAIData(other.mAIData)
        , mBaseData(other.mBaseData)
        , mRace(other.mRace)
        , mIsFemale(other.mIsFemale)
        , mCreatureStats(other.mCreatureStats)
        , mMovement(other.mMovement)
        , mContainerStore(other.mContainerStore ? other.mContainerStore->clone()
                                                : std::make_unique<MWWorld::ContainerStore>())
        , mEquippedArmor(other.mEquippedArmor)
        , mEquippedClothing(other.mEquippedClothing)
        , mEquippedWeapon(other.mEquippedWeapon)
        , mFnvAiSequenceInitialised(other.mFnvAiSequenceInitialised)
    {
    }

    static std::string_view getFalloutNpcFallbackSkeleton(const ESM4NpcCustomData& data)
    {
        if (data.mTraits != nullptr && data.mTraits->mIsFONV)
            return "characters/_male/skeleton.nif";
        return {};
    }

    static void logWorldViewerNpcModelFallback(
        const MWWorld::ConstPtr& ptr, const ESM4NpcCustomData& data, std::string_view model, std::string_view reason)
    {
        if (!worldViewerActorTelemetryEnabled() || data.mTraits == nullptr)
            return;

        Log(Debug::Info) << "World viewer actor ledger: phase=npc-model-fallback"
                         << " ref=" << ptr.getCellRef().getRefId()
                         << " base=" << ESM::FormId(data.mTraits->mId)
                         << " game=" << getWorldViewerNpcGameTag(*data.mTraits)
                         << " npc=\"" << data.mTraits->mEditorId << "\""
                         << " model=\"" << model << "\""
                         << " fallback=\"characters/_male/skeleton.nif\""
                         << " reason=\"" << reason << "\"";
    }

    static const ESM4::Npc* chooseStatsRecord(const ESM4NpcCustomData& data)
    {
        if (data.mStats != nullptr)
            return data.mStats;
        if (data.mBaseData != nullptr)
            return data.mBaseData;
        return data.mTraits;
    }

    static int positiveOrDefault(int value, int fallback)
    {
        value = value < 0 ? -value : value;
        return value > 0 ? value : fallback;
    }

    static int getLevel(const ESM4::Npc& npc)
    {
        if (npc.mIsFONV)
            return positiveOrDefault(npc.mBaseConfig.fo3.levelOrMult, 1);
        if (npc.mIsFO4)
            return positiveOrDefault(npc.mBaseConfig.fo4.levelOrMult, 1);
        if (npc.mIsTES4)
            return positiveOrDefault(npc.mBaseConfig.tes4.levelOrOffset, 1);
        return positiveOrDefault(npc.mBaseConfig.tes5.levelOrMult, 1);
    }

    static float getSpeedMultiplier(const ESM4::Npc& npc)
    {
        int multiplier = 100;
        if (npc.mIsFONV)
            multiplier = npc.mBaseConfig.fo3.speedMultiplier;
        else if (!npc.mIsFO4 && !npc.mIsTES4)
            multiplier = npc.mBaseConfig.tes5.speedMultiplier;

        return std::max(multiplier, 1) / 100.f;
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
        ESM4NpcCustomData& data, const MWWorld::Ptr& ptr, const std::vector<ESM::FormId>& packageIds)
    {
        if (data.mFnvAiSequenceInitialised)
            return;
        data.mFnvAiSequenceInitialised = true;

        const ESM4::Npc* traits = data.mTraits;
        if (std::getenv("OPENMW_FNV_DISABLE_AI_PACKAGES") != nullptr)
        {
            if (traits != nullptr && traits->mIsFONV)
                Log(Debug::Info) << "FNV/ESM4 diag: native FNV NPC AI package movement disabled by proof env for "
                                 << traits->mEditorId;
            return;
        }

        if (traits == nullptr || !traits->mIsFONV || packageIds.empty() || ptr.getCell() == nullptr
            || ptr.getCell()->getCell() == nullptr)
            return;

        bool usedHourOverride = false;
        const float hour = getFnvPackageHour(usedHourOverride);
        const ESM4::AIPackage* package = selectFnvPackage(packageIds, hour);
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
                Log(Debug::Info) << "FNV/ESM4 diag: skipped native AI travel package " << package->mEditorId
                                 << " type=" << getFnvPackageTypeName(package->mData.type)
                                 << " targetResolved=" << static_cast<bool>(target)
                                 << " currentCell=" << currentCellId << " for " << traits->mEditorId;
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
                Log(Debug::Info) << "FNV/ESM4 diag: skipped native AI travel package " << package->mEditorId
                                 << " type=" << getFnvPackageTypeName(package->mData.type)
                                 << " because actor is already at targetRef=" << target->mEditorId
                                 << " furnitureTarget=" << furnitureTarget
                                 << " arrivalDistance=" << arrivalDistance << " for " << traits->mEditorId;
                return;
            }

            MWMechanics::AiTravel travel(target->mPos.pos[0], target->mPos.pos[1], target->mPos.pos[2], true);
            sequence.stack(travel, ptr, true);
            Log(Debug::Info) << "FNV/ESM4 diag: stacked native AI travel from FNV package " << package->mEditorId
                             << " type=" << getFnvPackageTypeName(package->mData.type) << " hour=" << hour
                             << " override=" << usedHourOverride << " targetRef=" << target->mEditorId << " pos=("
                             << target->mPos.pos[0] << "," << target->mPos.pos[1] << "," << target->mPos.pos[2]
                             << ") for " << traits->mEditorId;
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
            Log(Debug::Info) << "FNV/ESM4 diag: stacked native AI wander from FNV package " << package->mEditorId
                             << " type=" << getFnvPackageTypeName(package->mData.type) << " hour=" << hour
                             << " override=" << usedHourOverride << " distance=" << distance << " duration="
                             << duration << " for " << traits->mEditorId;
        }
    }

    static void considerEquippedWeapon(ESM4NpcCustomData& data, const ESM4::Weapon* weapon)
    {
        if (weapon == nullptr || weapon->mModel.empty())
            return;

        if (data.mEquippedWeapon == nullptr || weapon->mData.damage > data.mEquippedWeapon->mData.damage)
            data.mEquippedWeapon = weapon;
    }

    static bool isPersistentRecord(const ESM4::Npc& npc)
    {
        return (npc.mFlags & ESM::FLAG_Persistent) != 0;
    }

    static void initialiseActorStats(ESM4NpcCustomData& data)
    {
        const ESM4::Npc* statsRecord = chooseStatsRecord(data);
        if (statsRecord == nullptr)
            return;

        MWMechanics::CreatureStats& stats = data.mCreatureStats;
        stats.setLevel(getLevel(*statsRecord));

        const ESM4::AttributeValues& attributes = statsRecord->mData.attribs;
        stats.setAttribute(ESM::Attribute::Strength, attributes.strength ? attributes.strength : 50);
        stats.setAttribute(ESM::Attribute::Intelligence, attributes.intelligence ? attributes.intelligence : 50);
        stats.setAttribute(ESM::Attribute::Willpower, attributes.willpower ? attributes.willpower : 50);
        stats.setAttribute(ESM::Attribute::Agility, attributes.agility ? attributes.agility : 50);
        stats.setAttribute(ESM::Attribute::Speed, attributes.speed ? attributes.speed : 50);
        stats.setAttribute(ESM::Attribute::Endurance, attributes.endurance ? attributes.endurance : 50);
        stats.setAttribute(ESM::Attribute::Personality, attributes.personality ? attributes.personality : 50);
        stats.setAttribute(ESM::Attribute::Luck, attributes.luck ? attributes.luck : 50);

        const float health = statsRecord->mData.health > 0 ? static_cast<float>(statsRecord->mData.health) : 100.f;
        const float fatigue = statsRecord->mIsFONV && statsRecord->mBaseConfig.fo3.fatigue > 0
            ? static_cast<float>(statsRecord->mBaseConfig.fo3.fatigue)
            : 100.f;
        stats.setHealth(health);
        stats.setMagicka(0.f);
        stats.setFatigue(fatigue);

        const ESM4::Npc* aiRecord = data.mAIData != nullptr ? data.mAIData : statsRecord;
        stats.setAiSetting(MWMechanics::AiSetting::Hello, 30);
        stats.setAiSetting(MWMechanics::AiSetting::Fight, aiRecord->mAIData.aggression);
        stats.setAiSetting(MWMechanics::AiSetting::Flee, 100 - aiRecord->mAIData.confidence);
        stats.setAiSetting(MWMechanics::AiSetting::Alarm, aiRecord->mAIData.responsibility);

        if (stats.isDead())
            stats.setDeathAnimationFinished(data.mBaseData != nullptr && isPersistentRecord(*data.mBaseData));
    }

    ESM4NpcCustomData& ESM4Npc::getCustomData(const MWWorld::ConstPtr& ptr)
    {
        // Note: the argument is ConstPtr because this function is used in `getModel` and `getName`
        // which are virtual and work with ConstPtr. `getModel` and `getName` use custom data
        // because they require a lot of work including levelled records resolving and it would be
        // stupid to not to cache the results. Maybe we should stop using ConstPtr at all
        // to avoid such workarounds.
        MWWorld::RefData& refData = const_cast<MWWorld::RefData&>(ptr.getRefData());

        if (auto* data = refData.getCustomData())
            return data->asESM4NpcCustomData();

        auto data = std::make_unique<ESM4NpcCustomData>();

        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        const ESM4::Npc* const base = ptr.get<ESM4::Npc>()->mBase;
        auto npcRecs = withBaseTemplates<ESM4::LevelledNpc, ESM4::Npc>(base);

        data->mTraits = chooseTemplate(npcRecs, ESM4::Npc::Template_UseTraits);

        if (data->mTraits == nullptr)
            Log(Debug::Warning) << "Traits are not found for ESM4 NPC base record: \"" << base->mEditorId << "\" ("
                                << ESM::RefId(base->mId) << ")";

        data->mModel = chooseTemplate(npcRecs, ESM4::Npc::Template_UseModel);

        if (data->mModel == nullptr)
            Log(Debug::Warning) << "Model data is not found for ESM4 NPC base record: \"" << base->mEditorId << "\" ("
                                << ESM::RefId(base->mId) << ")";

        data->mAIPackage = chooseTemplate(npcRecs, ESM4::Npc::Template_UseAIPackage);

        if (data->mAIPackage == nullptr)
            Log(Debug::Warning) << "AI package data is not found for ESM4 NPC base record: \"" << base->mEditorId
                                << "\" (" << ESM::RefId(base->mId) << ")";

        data->mBaseData = chooseTemplate(npcRecs, ESM4::Npc::Template_UseBaseData);

        if (data->mBaseData == nullptr)
            Log(Debug::Warning) << "Base data is not found for ESM4 NPC base record: \"" << base->mEditorId << "\" ("
                                << ESM::RefId(base->mId) << ")";

        data->mStats = chooseTemplate(npcRecs, ESM4::Npc::Template_UseStats);

        if (data->mStats == nullptr)
            Log(Debug::Warning) << "Stats are not found for ESM4 NPC base record: \"" << base->mEditorId << "\" ("
                                << ESM::RefId(base->mId) << ")";

        data->mAIData = chooseTemplate(npcRecs, ESM4::Npc::Template_UseAIData);

        if (data->mAIData == nullptr)
            Log(Debug::Warning) << "AI data is not found for ESM4 NPC base record: \"" << base->mEditorId << "\" ("
                                << ESM::RefId(base->mId) << ")";

        if (data->mTraits != nullptr)
        {
            data->mRace = store->get<ESM4::Race>().find(data->mTraits->mRace);
            if (data->mRace == nullptr)
                Log(Debug::Warning) << "FNV/ESM4 diag: race " << ESM::RefId(data->mTraits->mRace)
                                    << " not found for NPC \"" << base->mEditorId << "\" (" << ESM::RefId(base->mId)
                                    << ")";
            if (data->mTraits->mIsTES4)
                data->mIsFemale = data->mTraits->mBaseConfig.tes4.flags & ESM4::Npc::TES4_Female;
            else if (data->mTraits->mIsFONV)
                data->mIsFemale = data->mTraits->mBaseConfig.fo3.flags & ESM4::Npc::FO3_Female;
            else if (data->mTraits->mIsFO4)
                data->mIsFemale
                    = data->mTraits->mBaseConfig.fo4.flags & ESM4::Npc::TES5_Female; // FO4 flags are the same as TES5
            else
                data->mIsFemale = data->mTraits->mBaseConfig.tes5.flags & ESM4::Npc::TES5_Female;
        }

        const auto addArmor = [&](const ESM4::Armor* armor) {
            if (armor == nullptr)
                return false;
            if (std::find(data->mEquippedArmor.begin(), data->mEquippedArmor.end(), armor)
                != data->mEquippedArmor.end())
                return false;
            data->mEquippedArmor.push_back(armor);
            return true;
        };
        const auto addClothing = [&](const ESM4::Clothing* clothing) {
            if (clothing == nullptr)
                return false;
            if (std::find(data->mEquippedClothing.begin(), data->mEquippedClothing.end(), clothing)
                != data->mEquippedClothing.end())
                return false;
            data->mEquippedClothing.push_back(clothing);
            return true;
        };
        const auto logInventoryItem = [&](std::string_view source, const ESM4::Npc* owner, ESM::FormId itemId,
                                      std::string_view result, std::string_view editor) {
            if (!worldViewerActorTelemetryEnabled())
                return;

            Log(Debug::Info) << "World viewer actor ledger: phase=npc-inventory-item"
                             << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                             << " base=" << ptr.getCellRef().getRefId().toDebugString()
                             << " source=\"" << source << "\""
                             << " owner=\"" << (owner != nullptr ? owner->mEditorId : std::string()) << "\""
                             << " item=" << ESM::RefId(itemId)
                             << " result=\"" << result << "\""
                             << " editor=\"" << editor << "\"";
        };
        const auto equipInventoryItem = [&](ESM::FormId itemId, std::string_view source, const ESM4::Npc* owner) {
            std::vector<const ESM4::Armor*> armors;
            ESM4Impl::resolveLevelledAll<ESM4::LevelledItem, ESM4::Armor>(itemId, armors);
            if (!armors.empty())
            {
                bool usedAny = false;
                for (const ESM4::Armor* armor : armors)
                {
                    const bool added = addArmor(armor);
                    logInventoryItem(source, owner, itemId, added ? "armor" : "armor-duplicate", armor->mEditorId);
                    usedAny = added || usedAny;
                }
                return usedAny;
            }

            std::vector<const ESM4::Clothing*> clothes;
            ESM4Impl::resolveLevelledAll<ESM4::LevelledItem, ESM4::Clothing>(itemId, clothes);
            if (!clothes.empty())
            {
                bool usedAny = false;
                for (const ESM4::Clothing* clothing : clothes)
                {
                    const bool added = addClothing(clothing);
                    logInventoryItem(
                        source, owner, itemId, added ? "clothing" : "clothing-duplicate", clothing->mEditorId);
                    usedAny = added || usedAny;
                }
                return usedAny;
            }

            std::vector<const ESM4::Weapon*> weapons;
            ESM4Impl::resolveLevelledAll<ESM4::LevelledItem, ESM4::Weapon>(itemId, weapons);
            if (!weapons.empty())
            {
                for (const ESM4::Weapon* weapon : weapons)
                {
                    considerEquippedWeapon(*data, weapon);
                    logInventoryItem(source, owner, itemId, "weapon", weapon->mEditorId);
                }
                return true;
            }

            logInventoryItem(source, owner, itemId, "unresolved", {});
            return false;
        };
        const auto equipOutfit = [&](ESM::FormId outfitId, std::string_view source, const ESM4::Npc* owner) {
            if (outfitId.isZeroOrUnset())
                return false;

            const ESM4::Outfit* outfit = store->get<ESM4::Outfit>().search(outfitId);
            if (worldViewerActorTelemetryEnabled())
            {
                Log(Debug::Info) << "World viewer actor ledger: phase=npc-outfit"
                                 << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                 << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                 << " source=\"" << source << "\""
                                 << " owner=\"" << (owner != nullptr ? owner->mEditorId : std::string()) << "\""
                                 << " outfit=" << ESM::RefId(outfitId)
                                 << " resolved=" << static_cast<bool>(outfit)
                                 << " editor=\"" << (outfit != nullptr ? outfit->mEditorId : std::string()) << "\""
                                 << " items=" << (outfit != nullptr ? outfit->mInventory.size() : 0);
            }
            if (outfit == nullptr)
            {
                Log(Debug::Error) << "Outfit not found: " << ESM::RefId(outfitId);
                return false;
            }

            bool usedAny = false;
            for (ESM::FormId itemId : outfit->mInventory)
                usedAny = equipInventoryItem(itemId, source, owner) || usedAny;
            return usedAny;
        };
        const auto equipNpcInventory = [&](const ESM4::Npc* inv, std::string_view source) {
            if (inv == nullptr)
                return false;

            if (worldViewerActorTelemetryEnabled())
            {
                Log(Debug::Info) << "World viewer actor ledger: phase=npc-inventory-source"
                                 << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                                 << " base=" << ptr.getCellRef().getRefId().toDebugString()
                                 << " source=\"" << source << "\""
                                 << " owner=\"" << inv->mEditorId << "\""
                                 << " inventoryItems=" << inv->mInventory.size()
                                 << " defaultOutfit=" << ESM::RefId(inv->mDefaultOutfit)
                                 << " sleepOutfit=" << ESM::RefId(inv->mSleepOutfit);
            }

            bool usedAny = false;
            for (const ESM4::InventoryItem& item : inv->mInventory)
                usedAny = equipInventoryItem(ESM::FormId::fromUint32(item.item), source, inv) || usedAny;
            usedAny = equipOutfit(inv->mDefaultOutfit, source, inv) || usedAny;
            return usedAny;
        };

        const ESM4::Npc* chosenInventory = chooseTemplate(npcRecs, ESM4::Npc::Template_UseInventory);
        equipNpcInventory(chosenInventory, "chosen-template");
        if (data->mEquippedArmor.empty() && data->mEquippedClothing.empty())
        {
            for (const ESM4::Npc* candidate : npcRecs)
            {
                if (candidate == nullptr || candidate == chosenInventory)
                    continue;
                if (candidate->mInventory.empty() && candidate->mDefaultOutfit.isZeroOrUnset())
                    continue;
                if (equipNpcInventory(candidate, "fallback-template"))
                    break;
            }
        }

        initialiseActorStats(*data);

        Log(Debug::Info) << "FNV/ESM4 diag: initialized actor shell for NPC \"" << base->mEditorId << "\" ("
                         << ESM::RefId(base->mId) << ") level=" << data->mCreatureStats.getLevel()
                         << " health=" << data->mCreatureStats.getHealth().getModified()
                         << " race=" << (data->mTraits != nullptr ? ESM::RefId(data->mTraits->mRace) : ESM::RefId())
                         << " traits="
                         << (data->mTraits != nullptr ? data->mTraits->mEditorId : std::string_view{})
                         << " modelRecord="
                         << (data->mModel != nullptr ? data->mModel->mEditorId : std::string_view{})
                         << " modelKfCount=" << (data->mModel != nullptr ? data->mModel->mKf.size() : 0)
                         << " aiPackageRecord="
                         << (data->mAIPackage != nullptr ? data->mAIPackage->mEditorId : std::string_view{})
                         << " packageCount=" << (data->mAIPackage != nullptr ? data->mAIPackage->mAIPackages.size() : 0)
                         << " weapon="
                         << (data->mEquippedWeapon != nullptr ? data->mEquippedWeapon->mEditorId : std::string_view{});
        if (worldViewerActorTelemetryEnabled())
        {
            const ESM::Position& pos = ptr.getRefData().getPosition();
            Log(Debug::Info) << "World viewer actor ledger: phase=npc-custom-data"
                             << " ref=" << ptr.getCellRef().getRefNum().toString("FormId:")
                             << " base=" << ptr.getCellRef().getRefId().toDebugString()
                             << " game=" << (data->mTraits != nullptr ? getWorldViewerNpcGameTag(*data->mTraits) : "UNKNOWN")
                             << " npc=\"" << base->mEditorId << "\""
                             << " traits=\"" << (data->mTraits != nullptr ? data->mTraits->mEditorId : std::string()) << "\""
                             << " modelRecord=\""
                             << (data->mModel != nullptr ? data->mModel->mEditorId : std::string()) << "\""
                             << " aiPackageRecord=\""
                             << (data->mAIPackage != nullptr ? data->mAIPackage->mEditorId : std::string()) << "\""
                             << " race=" << (data->mTraits != nullptr ? ESM::RefId(data->mTraits->mRace) : ESM::RefId())
                             << " raceResolved=" << (data->mRace != nullptr)
                             << " female=" << data->mIsFemale
                             << " armor=" << data->mEquippedArmor.size()
                             << " clothing=" << data->mEquippedClothing.size()
                             << " weapon=\""
                             << (data->mEquippedWeapon != nullptr ? data->mEquippedWeapon->mEditorId : std::string())
                             << "\""
                             << " pos=(" << pos.pos[0] << "," << pos.pos[1] << "," << pos.pos[2] << ")";
        }

        ESM4NpcCustomData& res = *data;
        refData.setCustomData(std::move(data));
        return res;
    }

    const std::vector<const ESM4::Armor*>& ESM4Npc::getEquippedArmor(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mEquippedArmor;
    }

    const std::vector<const ESM4::Clothing*>& ESM4Npc::getEquippedClothing(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mEquippedClothing;
    }

    const ESM4::Weapon* ESM4Npc::getEquippedWeapon(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mEquippedWeapon;
    }

    bool ESM4Npc::addEquippedArmor(const MWWorld::Ptr& ptr, const ESM4::Armor* armor)
    {
        if (armor == nullptr)
            return false;

        std::vector<const ESM4::Armor*>& equippedArmor = getCustomData(ptr).mEquippedArmor;
        if (std::find(equippedArmor.begin(), equippedArmor.end(), armor) != equippedArmor.end())
            return false;

        equippedArmor.push_back(armor);
        return true;
    }

    const ESM4::Npc* ESM4Npc::getTraitsRecord(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mTraits;
    }

    const ESM4::Npc* ESM4Npc::getModelRecord(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mModel;
    }

    const ESM4::Npc* ESM4Npc::getAIPackageRecord(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mAIPackage;
    }

    const ESM4::Npc* ESM4Npc::getStatsRecord(const MWWorld::Ptr& ptr)
    {
        return chooseStatsRecord(getCustomData(ptr));
    }

    const ESM4::Npc* ESM4Npc::getBaseDataRecord(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mBaseData;
    }

    const ESM4::Race* ESM4Npc::getRace(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mRace;
    }

    bool ESM4Npc::isFemale(const MWWorld::Ptr& ptr)
    {
        return getCustomData(ptr).mIsFemale;
    }

    std::string_view ESM4Npc::chooseEquipmentModel(const ESM4::Armor* rec, bool isFemale)
    {
        if (rec == nullptr)
            return {};
        if (isFemale && !rec->mModelFemale.empty())
            return rec->mModelFemale;
        if (!isFemale && !rec->mModelMale.empty())
            return rec->mModelMale;
        return rec->mModel;
    }

    std::string_view ESM4Npc::chooseEquipmentModel(const ESM4::Clothing* rec, bool isFemale)
    {
        if (rec == nullptr)
            return {};
        if (isFemale && !rec->mModelFemale.empty())
            return rec->mModelFemale;
        if (!isFemale && !rec->mModelMale.empty())
            return rec->mModelMale;
        return rec->mModel;
    }

    std::string_view ESM4Npc::getModel(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4NpcCustomData& data = getCustomData(ptr);
        if (data.mTraits == nullptr)
            return {};
        if (data.mTraits->mIsTES4)
            return data.mTraits->mModel;
        const std::string_view falloutFallback = getFalloutNpcFallbackSkeleton(data);
        if (data.mRace != nullptr)
        {
            const std::string_view raceModel = data.mIsFemale ? data.mRace->mModelFemale : data.mRace->mModelMale;
            if (!raceModel.empty() && (falloutFallback.empty() || !isWorldViewerMarkerActorModel(raceModel)))
                return raceModel;
            if (!raceModel.empty() && !falloutFallback.empty())
                logWorldViewerNpcModelFallback(ptr, data, raceModel, "race marker model");
        }
        const ESM4::Npc* modelRecord = data.mModel != nullptr ? data.mModel : data.mTraits;
        if (!falloutFallback.empty()
            && (modelRecord->mModel.empty() || isWorldViewerMarkerActorModel(modelRecord->mModel)))
        {
            logWorldViewerNpcModelFallback(
                ptr, data, modelRecord->mModel, modelRecord->mModel.empty() ? "empty model" : "record marker model");
            return falloutFallback;
        }
        if (modelRecord->mModel.empty())
            Log(Debug::Warning) << "FNV/ESM4 diag: no skeleton model for NPC \"" << data.mTraits->mEditorId << "\" ("
                                << ESM::RefId(data.mTraits->mId) << ")";
        return modelRecord->mModel;
    }

    std::string_view ESM4Npc::getName(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4::Npc* const baseData = getCustomData(ptr).mBaseData;
        if (baseData == nullptr)
            return {};
        return baseData->mFullName;
    }

    MWMechanics::CreatureStats& ESM4Npc::getCreatureStats(const MWWorld::Ptr& ptr) const
    {
        ESM4NpcCustomData& data = getCustomData(ptr);
        const ESM4::Npc* packageRecord = data.mAIPackage != nullptr ? data.mAIPackage : data.mTraits;
        if (packageRecord != nullptr)
            initialiseFnvAiSequence(data, ptr, packageRecord->mAIPackages);
        return data.mCreatureStats;
    }

    MWMechanics::Movement& ESM4Npc::getMovementSettings(const MWWorld::Ptr& ptr) const
    {
        return getCustomData(ptr).mMovement;
    }

    MWWorld::ContainerStore& ESM4Npc::getContainerStore(const MWWorld::Ptr& ptr) const
    {
        MWWorld::ContainerStore& store = *getCustomData(ptr).mContainerStore;
        store.setPtr(ptr);
        return store;
    }

    float ESM4Npc::getCapacity(const MWWorld::Ptr& ptr) const
    {
        const MWMechanics::CreatureStats& stats = getCreatureStats(ptr);
        return stats.getAttribute(ESM::Attribute::Strength).getModified() * 5.f;
    }

    float ESM4Npc::getMaxSpeed(const MWWorld::Ptr& ptr) const
    {
        const MWMechanics::CreatureStats& stats = getCreatureStats(ptr);
        if (stats.isParalyzed() || stats.getKnockedDown() || stats.isDead())
            return 0.f;

        if (stats.getStance(MWMechanics::CreatureStats::Stance_Run))
            return getRunSpeed(ptr);
        return getWalkSpeed(ptr);
    }

    float ESM4Npc::getWalkSpeed(const MWWorld::Ptr& ptr) const
    {
        const ESM4NpcCustomData& data = getCustomData(ptr);
        const ESM4::Npc* statsRecord = chooseStatsRecord(data);
        const float multiplier = statsRecord != nullptr ? getSpeedMultiplier(*statsRecord) : 1.f;

        return std::max(1.f, data.mCreatureStats.getAttribute(ESM::Attribute::Speed).getModified()) * 2.5f
            * multiplier;
    }

    float ESM4Npc::getRunSpeed(const MWWorld::Ptr& ptr) const
    {
        return getWalkSpeed(ptr) * 1.65f;
    }

    float ESM4Npc::getSwimSpeed(const MWWorld::Ptr& ptr) const
    {
        return getWalkSpeed(ptr);
    }

    float ESM4Npc::getSkill(const MWWorld::Ptr& ptr, ESM::RefId id) const
    {
        (void)ptr;
        (void)id;
        return 50.f;
    }

    int ESM4Npc::getServices(const MWWorld::ConstPtr& ptr) const
    {
        // Fallout merchant/service data is expressed through dialogue and
        // package records rather than Morrowind's NPC service bitmask.  Zero
        // keeps the shared dialogue window on authored ESM4 topics until the
        // Fallout service-menu bridge is implemented.
        (void)ptr;
        return 0;
    }

    int ESM4Npc::getBaseGold(const MWWorld::ConstPtr& ptr) const
    {
        // Fallout caps are inventory items; they are not stored in the
        // Morrowind NPC base-gold field used by the shared dialogue widget.
        (void)ptr;
        return 0;
    }

    std::unique_ptr<MWWorld::Action> ESM4Npc::activate(
        const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const
    {
        (void)actor;
        if (getCreatureStats(ptr).isDead())
            return std::make_unique<MWWorld::FailedAction>();
        return std::make_unique<MWWorld::ActionTalk>(ptr);
    }

    bool ESM4Npc::isPersistent(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4NpcCustomData& data = getCustomData(ptr);
        return data.mBaseData != nullptr && isPersistentRecord(*data.mBaseData);
    }

    bool ESM4Npc::isBipedal(const MWWorld::ConstPtr& ptr) const
    {
        (void)ptr;
        return true;
    }

    bool ESM4Npc::canSwim(const MWWorld::ConstPtr& ptr) const
    {
        (void)ptr;
        return true;
    }

    bool ESM4Npc::canWalk(const MWWorld::ConstPtr& ptr) const
    {
        (void)ptr;
        return true;
    }
}
