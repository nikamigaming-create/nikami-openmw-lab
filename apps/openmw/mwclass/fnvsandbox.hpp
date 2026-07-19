#ifndef OPENMW_MWCLASS_FNVSANDBOX_H
#define OPENMW_MWCLASS_FNVSANDBOX_H

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <components/esm/formid.hpp>
#include <components/esm/refid.hpp>

#include <osg/Vec3f>

namespace ESM4
{
    struct AIPackage;
}

namespace ESM::AiSequence
{
    struct AiSequence;
}

namespace MWWorld
{
    class ESMStore;
}

namespace MWClass
{
    struct FalloutSandboxIdle
    {
        ESM::FormId mId;
        std::string mEditorId;
        std::string mModel;
        std::string mAnimatedObjectModel;
    };

    struct FalloutSandboxMarker
    {
        ESM::FormId mReference;
        ESM::FormId mBase;
        ESM::RefId mCell;
        osg::Vec3f mPosition;
        float mYaw = 0.f;
        float mTimer = 0.f;
        std::vector<FalloutSandboxIdle> mIdles;
    };

    struct FalloutSandboxSaveFallback
    {
        float mRadius = 0.f;
        int mDuration = 0;
        int mTimeOfDay = 0;
        osg::Vec3f mOrigin;
    };

    /// Give every authored IDLE record a source-specific animation group. Fallout chore KFs commonly expose only
    /// event groups such as "sound"; sharing one synthetic "specialidle" group would also let the last loaded KF
    /// override the marker's selected source.
    std::string getFalloutSandboxAnimationGroup(const FalloutSandboxIdle& idle);

    float getFalloutSandboxRadius(const ESM4::AIPackage& package);

    std::vector<FalloutSandboxMarker> collectFalloutSandboxMarkers(const MWWorld::ESMStore& store,
        const ESM::RefId& cell, const osg::Vec3f& actorPosition, float radius);

    std::optional<std::size_t> selectNearestFalloutSandboxMarker(std::span<const FalloutSandboxMarker> markers,
        const osg::Vec3f& actorPosition,
        const std::function<bool(ESM::FormId)>& isClaimed = {});

    bool tryClaimFalloutSandboxMarker(ESM::FormId marker);
    void releaseFalloutSandboxMarker(ESM::FormId marker);
    bool isFalloutSandboxMarkerClaimed(ESM::FormId marker);

    /// Persist an engine-compatible repeating wander when the live record-driven package cannot be represented by
    /// OpenMW's legacy save schema. Reloaded actors remain active instead of inheriting an explicitly empty sequence.
    void writeFalloutSandboxFallback(ESM::AiSequence::AiSequence& sequence, float radius, int duration,
        int timeOfDay, const osg::Vec3f& origin);
    std::optional<FalloutSandboxSaveFallback> getFalloutSandboxSaveFallback(
        const ESM::AiSequence::AiSequence& sequence);
}

#endif
