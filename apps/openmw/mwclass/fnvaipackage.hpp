#ifndef OPENMW_MWCLASS_FNVAI_PACKAGE_H
#define OPENMW_MWCLASS_FNVAI_PACKAGE_H

namespace MWWorld
{
    class Ptr;
}

namespace MWClass
{
    // Procedure types currently represented by the native Fallout NPC
    // package bridge. Unsupported procedures must not shadow a later package
    // which the runtime can actually execute.
    [[nodiscard]] constexpr bool fnvNpcAiPackageProcedureSupported(int type) noexcept
    {
        switch (type)
        {
            case 3: // Eat
            case 4: // Sleep
            case 5: // Wander
            case 6: // Travel
            case 8: // Use item at
            case 11: // Sandbox
            case 12: // Sandbox
                return true;
            default:
                return false;
        }
    }

    // Package targets are goals for runtime pathing. Load-time relocation is
    // retained only for focused compatibility captures which opt in.
    [[nodiscard]] constexpr bool fnvPackagePrePlacementEnabled(
        bool sameCell, bool sameCellOptIn, bool crossCellOptIn) noexcept
    {
        return sameCell ? sameCellOptIn : crossCellOptIn;
    }

    // Re-evaluate the authored Fallout package list for a live actor. This is
    // the runtime contract behind the GECK EvaluatePackage/evp command.
    bool requestFnvAiPackageEvaluation(const MWWorld::Ptr& ptr);

    // Creature-side implementation used by the actor-generic entry point.
    bool requestFnvCreatureAiPackageEvaluation(const MWWorld::Ptr& ptr);
}

#endif
