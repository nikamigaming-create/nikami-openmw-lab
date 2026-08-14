#ifndef GAME_MWMECHANICS_AITRAVEL_H
#define GAME_MWMECHANICS_AITRAVEL_H

#include <optional>

#include "typedaipackage.hpp"

namespace ESM
{
    namespace AiSequence
    {
        struct AiTravel;
    }
}

namespace MWMechanics
{
    struct AiInternalTravel;

    /// \brief Causes the AI to travel to the specified point
    class AiTravel : public TypedAiPackage<AiTravel>
    {
    public:
        AiTravel(float x, float y, float z, bool repeat, AiTravel* derived);

        AiTravel(float x, float y, float z, AiInternalTravel* derived);

        AiTravel(float x, float y, float z, bool repeat);

        /// Use the shared travel/pathfinding implementation but require the
        /// actor to enter the supplied authored radius before completion.
        /// This is opt-in so legacy Morrowind travel keeps its generous
        /// "close enough for two seconds" behavior.
        AiTravel(float x, float y, float z, bool repeat, float destinationTolerance);

        /// Use the authored final heading after reaching a data-driven travel
        /// marker. A negative tolerance preserves legacy completion behavior.
        AiTravel(float x, float y, float z, bool repeat, float destinationTolerance,
            std::optional<float> destinationYaw);

        explicit AiTravel(const ESM::AiSequence::AiTravel* travel);

        /// Simulates the passing of time
        void fastForward(const MWWorld::Ptr& actor, AiState& state) override;

        void writeState(ESM::AiSequence::AiSequence& sequence) const override;

        bool execute(const MWWorld::Ptr& actor, CharacterController& characterController, AiState& state,
            float duration) override;

        static constexpr AiPackageTypeId getTypeId() { return AiPackageTypeId::Travel; }

        static constexpr Options makeDefaultOptions()
        {
            AiPackage::Options options;
            options.mUseVariableSpeed = true;
            options.mAlwaysActive = true;
            return options;
        }

        osg::Vec3f getDestination() const override { return osg::Vec3f(mX, mY, mZ); }

    private:
        const float mX;
        const float mY;
        const float mZ;

        const bool mHidden;

        float mDestinationTimer;
        float mDestinationTolerance;
        std::optional<float> mDestinationYaw;

        // Route diagnostics only.  Keep per-package rather than global state so
        // an unattended compatibility trace can establish whether the actual
        // one-shot package is being executed without flooding the log.
        unsigned int mRouteTraceExecutions = 0;
        bool mRouteTraceCompletionLogged = false;
    };

    struct AiInternalTravel final : public AiTravel
    {
        AiInternalTravel(float x, float y, float z);

        explicit AiInternalTravel(const ESM::AiSequence::AiTravel* travel);

        static constexpr AiPackageTypeId getTypeId() { return AiPackageTypeId::InternalTravel; }

        std::unique_ptr<AiPackage> clone() const override;
    };
}

#endif
