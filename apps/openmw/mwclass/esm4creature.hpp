#ifndef GAME_MWCLASS_ESM4CREATURE_H
#define GAME_MWCLASS_ESM4CREATURE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <components/esm4/loadcrea.hpp>
#include <components/vfs/pathutil.hpp>

#include <osg/Vec3f>

#include "../mwgui/tooltips.hpp"

#include "../mwphysics/physicssystem.hpp"
#include "../mwrender/objects.hpp"
#include "../mwrender/renderinginterface.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/registeredclass.hpp"

#include "actor.hpp"
#include "esm4base.hpp"

namespace ESM
{
    struct CreatureState;
}

namespace MWWorld
{
    class ESMStore;
}

namespace MWClass
{
    [[nodiscard]] constexpr bool fnvCreatureAiPackageProcedureSupported(std::int32_t packageType) noexcept
    {
        return packageType == 1 || packageType == 3 || packageType == 4 || packageType == 5 || packageType == 6
            || packageType == 7 || packageType == 8 || packageType == 11 || packageType == 12 || packageType == 13;
    }

    // The Goodsprings CheyenneAccompany package and adult bighorner herd packages target an exact ACHR/ACRE.
    // Calf packages instead use PTDT type 3 (linked reference), which ActorCharacter does not currently retain.
    // Keep that form out of the supported set so package selection can continue to an authored fallback.
    [[nodiscard]] constexpr bool fnvCreatureFollowTargetSupported(
        std::int32_t packageType, std::int32_t targetType, std::uint32_t target) noexcept
    {
        return (packageType == 1 || packageType == 7) && targetType == 0 && target != 0;
    }

    [[nodiscard]] constexpr bool fnvCreaturePackageConditionComparisonPasses(
        std::uint32_t condition, float actual, float expected) noexcept
    {
        switch (condition & 0xe0)
        {
            case 0x00:
                return actual == expected;
            case 0x20:
                return actual != expected;
            case 0x40:
                return actual > expected;
            case 0x60:
                return actual >= expected;
            case 0x80:
                return actual < expected;
            case 0xa0:
                return actual <= expected;
            default:
                return false;
        }
    }

    [[nodiscard]] constexpr int fnvCreatureWanderDistance(std::int32_t authoredRadius) noexcept
    {
        return authoredRadius > 0 ? authoredRadius : 256;
    }

    [[nodiscard]] constexpr unsigned fnvCreatureWanderDestinationTolerance(unsigned distance) noexcept
    {
        constexpr unsigned sLegacyTolerance = 64;
        const unsigned scaledTolerance = distance / 8;
        return std::max(1u, std::min(sLegacyTolerance, scaledTolerance));
    }

    // Fallout package procedure types 11 and 12 are the two sandbox variants. A creature that can both fly and
    // walk cannot be treated as OpenMW's "pure flying" kind, so mapping its ambient sandbox to AiWander makes it
    // choose a land path and leave an authored perch. Preserve that placement until combat, script, or a directed
    // package supplies an actual destination.
    [[nodiscard]] constexpr bool fnvAmbientFlyerRetainsAuthoredPosition(
        std::uint32_t creatureFlags, std::int32_t packageType) noexcept
    {
        constexpr std::int32_t sSandbox = 11;
        constexpr std::int32_t sSandboxEditorLocation = 12;
        constexpr std::uint32_t sFlyAndWalk
            = ESM4::Creature::FO3_CanFly | ESM4::Creature::FO3_CanWalk;
        return (creatureFlags & sFlyAndWalk) == sFlyAndWalk
            && (packageType == sSandbox || packageType == sSandboxEditorLocation);
    }

    struct FnvCreaturePatrolPoint
    {
        ESM::FormId mReference;
        ESM::RefId mCell;
        ESM::RefId mWorldspace;
        osg::Vec3f mPosition;
        float mYaw = 0.f;
        float mWaitSeconds = 0.f;
        bool mUsesAuthoredHeading = false;
        bool mIsPatrolIdleScriptMarker = false;
    };

    /// Resolve a Fallout XLKR patrol chain, rejecting missing records, cycles, overlong chains, and transitions
    /// outside the first marker's worldspace (or interior cell).
    [[nodiscard]] std::optional<std::vector<FnvCreaturePatrolPoint>> collectFnvCreaturePatrolRoute(
        const MWWorld::ESMStore& store, ESM::FormId firstMarker, std::size_t maxPoints = 256);

    class ESM4Creature final : public MWWorld::RegisteredClass<ESM4Creature, Actor>
    {
    public:
        ESM4Creature()
            : MWWorld::RegisteredClass<ESM4Creature, Actor>(ESM4::Creature::sRecordId)
        {
        }

        MWWorld::Ptr copyToCellImpl(const MWWorld::ConstPtr& ptr, MWWorld::CellStore& cell) const override
        {
            const MWWorld::LiveCellRef<ESM4::Creature>* ref = ptr.get<ESM4::Creature>();
            return MWWorld::Ptr(cell.insert(ref), &cell);
        }

        void insertObjectRendering(const MWWorld::Ptr& ptr, const std::string& model,
            MWRender::RenderingInterface& renderingInterface) const override
        {
            if (ESM4Impl::worldViewerDisableEsm4Actors() && !ESM4Impl::worldViewerUseEsm4ActorProxies())
            {
                ESM4Impl::logWorldViewerSkippedActor(ptr, "creature");
                return;
            }
            renderingInterface.getObjects().insertCreature(ptr, model, false);
        }

        void insertObject(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override
        {
            insertObjectPhysics(ptr, model, rotation, physics);
        }

        void insertObjectPhysics(const MWWorld::Ptr& ptr, const std::string& model, const osg::Quat& rotation,
            MWPhysics::PhysicsSystem& physics) const override
        {
            if (ESM4Impl::worldViewerDisableEsm4Actors())
                return;
            (void)rotation;
            physics.addActor(ptr, VFS::Path::toNormalized(model.empty() ? std::string(getModel(ptr)) : model));
        }

        std::string_view getModel(const MWWorld::ConstPtr& ptr) const override;
        std::string_view getName(const MWWorld::ConstPtr& ptr) const override;
        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override;
        MWGui::ToolTipInfo getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const override;
        MWMechanics::CreatureStats& getCreatureStats(const MWWorld::Ptr& ptr) const override;
        MWWorld::ContainerStore& getContainerStore(const MWWorld::Ptr& ptr) const override;
        MWMechanics::Movement& getMovementSettings(const MWWorld::Ptr& ptr) const override;
        float getCapacity(const MWWorld::Ptr& ptr) const override;
        float getMaxSpeed(const MWWorld::Ptr& ptr) const override;
        float getWalkSpeed(const MWWorld::Ptr& ptr) const override;
        float getRunSpeed(const MWWorld::Ptr& ptr) const override;
        float getSwimSpeed(const MWWorld::Ptr& ptr) const override;
        float getSkill(const MWWorld::Ptr& ptr, ESM::RefId id) const override;
        bool isPersistent(const MWWorld::ConstPtr& ptr) const override;
        bool canFly(const MWWorld::ConstPtr& ptr) const override;
        bool canSwim(const MWWorld::ConstPtr& ptr) const override;
        bool canWalk(const MWWorld::ConstPtr& ptr) const override;
        void adjustScale(const MWWorld::ConstPtr& ptr, osg::Vec3f& scale, bool rendering) const override;

        void readAdditionalState(const MWWorld::Ptr& ptr, const ESM::ObjectState& state) const override;
        void writeAdditionalState(const MWWorld::ConstPtr& ptr, ESM::ObjectState& state) const override;

        static const ESM4::Creature* getFactionsRecord(const MWWorld::Ptr& ptr);

        /// Validate all fallible FNV creature payload data before LiveCellRef applies the enclosing CellRef/RefData.
        static bool validateState(const ESM4::Creature& creature, const ESM::CreatureState& state,
            const MWWorld::ESMStore& store, std::string& error);

    private:
        static class ESM4CreatureCustomData& getCustomData(const MWWorld::ConstPtr& ptr);
    };
}

#endif
