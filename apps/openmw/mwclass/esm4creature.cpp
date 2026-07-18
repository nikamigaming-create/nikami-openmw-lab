#include "esm4creature.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <memory>

#include <components/debug/debuglog.hpp>
#include <components/esm/attr.hpp>
#include <components/esm/defs.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadlvlc.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/script.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/aitravel.hpp"
#include "../mwmechanics/aiwander.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/movement.hpp"

#include "../mwworld/containerstore.hpp"
#include "../mwworld/customdata.hpp"
#include "../mwworld/esm4questruntime.hpp"

#include "../mwgui/tooltips.hpp"

#include "esm4base.hpp"

namespace MWClass
{
    class ESM4CreatureCustomData : public MWWorld::TypedCustomData<ESM4CreatureCustomData>
    {
    public:
        MWMechanics::CreatureStats mCreatureStats;
        MWMechanics::Movement mMovement;
        std::unique_ptr<MWWorld::ContainerStore> mContainerStore = std::make_unique<MWWorld::ContainerStore>();
        bool mFnvAiSequenceInitialised = false;

        ESM4CreatureCustomData() = default;
        ESM4CreatureCustomData(const ESM4CreatureCustomData& other)
            : mCreatureStats(other.mCreatureStats)
            , mMovement(other.mMovement)
            , mContainerStore(other.mContainerStore ? other.mContainerStore->clone()
                                                    : std::make_unique<MWWorld::ContainerStore>())
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

    static const ESM4::Creature* searchCreatureTemplate(ESM::FormId id, int depth = 0)
    {
        const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
        if (store == nullptr || id.isZeroOrUnset() || depth > 8)
            return nullptr;

        const ESM::RecNameInts foundType = static_cast<ESM::RecNameInts>(store->find(id));
        if (foundType == ESM::RecNameInts::REC_CREA4)
            return store->get<ESM4::Creature>().search(id);

        if (foundType != ESM::RecNameInts::REC_LVLC4)
            return nullptr;

        const ESM4::LevelledCreature* list = store->get<ESM4::LevelledCreature>().search(id);
        if (list == nullptr || list->mLvlObject.empty())
            return nullptr;

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
            return nullptr;

        return searchCreatureTemplate(ESM::FormId::fromUint32(selected->item), depth + 1);
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

            const ESM4::Creature* templated = searchCreatureTemplate(current->mBaseTemplate);
            if (templated == nullptr || templated == current)
                return *current;

            current = templated;
        }

        return *current;
    }

    static std::vector<const ESM4::Creature*> getCreatureTemplateChain(const ESM4::Creature& creature)
    {
        std::vector<const ESM4::Creature*> records;
        const ESM4::Creature* current = &creature;
        for (int depth = 0; current != nullptr && depth < 16; ++depth)
        {
            if (std::find(records.begin(), records.end(), current) != records.end())
                break;
            records.push_back(current);
            if (current->mBaseTemplate.isZeroOrUnset())
                break;

            const ESM4::Creature* templated = searchCreatureTemplate(current->mBaseTemplate);
            if (templated == nullptr || templated == current)
                break;
            current = templated;
        }
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

        std::vector<ESM4::TargetCondition> conditions;
        conditions.reserve(package.mConditions.size());
        for (const ESM4::AIPackage::CTDA& source : package.mConditions)
        {
            ESM4::TargetCondition target;
            target.condition = static_cast<std::uint32_t>(source.condition)
                | (static_cast<std::uint32_t>(source.unknown1) << 8)
                | (static_cast<std::uint32_t>(source.unknown2) << 16)
                | (static_cast<std::uint32_t>(source.unknown3) << 24);
            target.comparison = source.compValue;
            target.functionIndex = static_cast<std::uint32_t>(source.fnIndex);
            target.param1 = source.param1;
            target.param2 = source.param2;
            conditions.push_back(target);
        }
        return world->getESM4QuestRuntime().evaluateConditions(conditions);
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

    static void initialiseActorStats(ESM4CreatureCustomData& data, const ESM4::Creature& creature)
    {
        MWMechanics::CreatureStats& stats = data.mCreatureStats;
        stats.setLevel(getLevel(creature));

        float health = 100.f;
        if (creature.mIsFONV && creature.mHasFNVData)
        {
            const ESM4::Creature::FNVData& attributes = creature.mFNVData;
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
        else
        {
            const ESM4::AttributeValues& attributes = creature.mData.attribs;
            stats.setAttribute(ESM::Attribute::Strength, attributes.strength ? attributes.strength : 50);
            stats.setAttribute(ESM::Attribute::Intelligence, attributes.intelligence ? attributes.intelligence : 25);
            stats.setAttribute(ESM::Attribute::Willpower, attributes.willpower ? attributes.willpower : 25);
            stats.setAttribute(ESM::Attribute::Agility, attributes.agility ? attributes.agility : 50);
            stats.setAttribute(ESM::Attribute::Speed, attributes.speed ? attributes.speed : 50);
            stats.setAttribute(ESM::Attribute::Endurance, attributes.endurance ? attributes.endurance : 50);
            stats.setAttribute(ESM::Attribute::Personality, attributes.personality ? attributes.personality : 25);
            stats.setAttribute(ESM::Attribute::Luck, attributes.luck ? attributes.luck : 50);
            health = creature.mData.health > 0 ? static_cast<float>(creature.mData.health) : 100.f;
        }

        stats.setHealth(health);
        stats.setMagicka(0.f);
        stats.setFatigue(100.f);

        const int aggression
            = creature.mIsFONV && creature.mHasFNVAIData ? creature.mFNVAIData.aggression : creature.mAIData.aggression;
        const int confidence
            = creature.mIsFONV && creature.mHasFNVAIData ? creature.mFNVAIData.confidence : creature.mAIData.confidence;
        const int responsibility = creature.mIsFONV && creature.mHasFNVAIData
            ? creature.mFNVAIData.responsibility
            : creature.mAIData.responsibility;
        stats.setAiSetting(MWMechanics::AiSetting::Hello, 30);
        stats.setAiSetting(MWMechanics::AiSetting::Fight,
            std::getenv("OPENMW_FNV_DISABLE_AI_PACKAGES") != nullptr ? 0 : aggression);
        stats.setAiSetting(MWMechanics::AiSetting::Flee, 100 - confidence);
        stats.setAiSetting(MWMechanics::AiSetting::Alarm, responsibility);

        if (stats.isDead())
            stats.setDeathAnimationFinished((creature.mFlags & ESM::FLAG_Persistent) != 0);
    }

    ESM4CreatureCustomData& ESM4Creature::getCustomData(const MWWorld::ConstPtr& ptr)
    {
        MWWorld::RefData& refData = const_cast<MWWorld::RefData&>(ptr.getRefData());
        if (MWWorld::CustomData* customData = refData.getCustomData())
            return *dynamic_cast<ESM4CreatureCustomData*>(customData);

        auto data = std::make_unique<ESM4CreatureCustomData>();
        ESM4CreatureCustomData& result = *data;
        const ESM4::Creature* creature = ptr.get<ESM4::Creature>()->mBase;
        const ESM4::Creature& effective = getEffectiveCreature(*creature);
        initialiseActorStats(result, effective);
        refData.setCustomData(std::move(data));

        Log(Debug::Verbose) << "FNV/ESM4 diag: initialized creature actor shell for \"" << creature->mEditorId << "\" ("
                         << ESM::RefId(creature->mId) << ") level=" << result.mCreatureStats.getLevel()
                         << " health=" << result.mCreatureStats.getHealth().getModified()
                         << " model=" << effective.mModel << " kfCount=" << effective.mKf.size()
                         << " baseFlags=0x" << std::hex << effective.mBaseConfig.fo3.flags << std::dec
                         << " canWalk=" << ((effective.mBaseConfig.fo3.flags & ESM4::Creature::FO3_CanWalk) != 0)
                         << " canSwim=" << ((effective.mBaseConfig.fo3.flags & ESM4::Creature::FO3_CanSwim) != 0)
                         << " canFly=" << ((effective.mBaseConfig.fo3.flags & ESM4::Creature::FO3_CanFly) != 0)
                         << " effective=" << effective.mEditorId << " effectiveId=" << ESM::RefId(effective.mId)
                         << " template=" << ESM::RefId(creature->mBaseTemplate);
        return result;
    }

    std::string_view ESM4Creature::getModel(const MWWorld::ConstPtr& ptr) const
    {
        const std::vector<const ESM4::Creature*> records
            = getCreatureTemplateChain(*ptr.get<ESM4::Creature>()->mBase);
        const ESM4::CreatureVisualTemplate visual = ESM4::resolveCreatureVisualTemplate(records);
        if (visual.mModel != nullptr)
            return visual.mModel->mModel;
        if (visual.mNif != nullptr && !visual.mNif->mNif.empty())
            return visual.mNif->mNif.front();
        return {};
    }

    std::string_view ESM4Creature::getName(const MWWorld::ConstPtr& ptr) const
    {
        const ESM4::Creature* creature = ptr.get<ESM4::Creature>()->mBase;
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
        initialiseFnvAiSequence(data, ptr, getEffectiveCreature(*ptr.get<ESM4::Creature>()->mBase));
        return data.mCreatureStats;
    }

    MWWorld::ContainerStore& ESM4Creature::getContainerStore(const MWWorld::Ptr& ptr) const
    {
        MWWorld::ContainerStore& store = *getCustomData(ptr).mContainerStore;
        store.setPtr(ptr);
        return store;
    }

    MWMechanics::Movement& ESM4Creature::getMovementSettings(const MWWorld::Ptr& ptr) const
    {
        return getCustomData(ptr).mMovement;
    }

    float ESM4Creature::getWalkSpeed(const MWWorld::Ptr& ptr) const
    {
        const ESM4::Creature* creature = &getEffectiveCreature(*ptr.get<ESM4::Creature>()->mBase);
        return std::max(1.f,
                   getCustomData(ptr).mCreatureStats.getAttribute(ESM::Attribute::Speed).getModified())
            * 2.5f * getSpeedMultiplier(*creature);
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
        return (getEffectiveCreature(*ptr.get<ESM4::Creature>()->mBase).mBaseConfig.fo3.flags
                   & ESM4::Creature::FO3_CanFly)
            != 0;
    }

    bool ESM4Creature::canSwim(const MWWorld::ConstPtr& ptr) const
    {
        return (getEffectiveCreature(*ptr.get<ESM4::Creature>()->mBase).mBaseConfig.fo3.flags
                   & ESM4::Creature::FO3_CanSwim)
            != 0;
    }

    bool ESM4Creature::canWalk(const MWWorld::ConstPtr& ptr) const
    {
        return (getEffectiveCreature(*ptr.get<ESM4::Creature>()->mBase).mBaseConfig.fo3.flags
                   & ESM4::Creature::FO3_CanWalk)
            != 0;
    }

    void ESM4Creature::adjustScale(
        const MWWorld::ConstPtr& ptr, osg::Vec3f& scale, bool /* rendering */) const
    {
        // Fallout's CREA BNAM is an actor-base scale, independent of the placed ACHR/ACRE XSCL.  Physics
        // already used it while the render root only received the placed-reference scale, which made ravens
        // 1/2.5 size, mantises 2x size, and every other non-1.0 creature disagree with retail.  Apply the same
        // multiplicative class-scale contract used by native ESM3 creatures to both render and collision.
        const ESM4::Creature& creature = getEffectiveCreature(*ptr.get<ESM4::Creature>()->mBase);
        scale *= creature.mBaseScale;
    }
}
