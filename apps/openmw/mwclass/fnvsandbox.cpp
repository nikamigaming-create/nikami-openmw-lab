#include "fnvsandbox.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>

#include <components/esm4/common.hpp>
#include <components/esm4/loadanio.hpp>
#include <components/esm4/loadidle.hpp>
#include <components/esm4/loadidlm.hpp>
#include <components/esm4/loadpack.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm3/aisequence.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwworld/esmstore.hpp"

namespace
{
    std::set<ESM::FormId>& getMarkerClaims()
    {
        static std::set<ESM::FormId> claims;
        return claims;
    }

    std::string normalizeMeshModel(std::string model)
    {
        VFS::Path::normalizeFilenameInPlace(model);
        if (!model.empty() && model.rfind("meshes/", 0) != 0)
            model = "meshes/" + model;
        return model;
    }

    struct SandboxMarkerIndex
    {
        const MWWorld::ESMStore* mStore = nullptr;
        std::size_t mReferenceCount = 0;
        std::size_t mMarkerCount = 0;
        std::size_t mIdleCount = 0;
        std::size_t mAnimatedObjectCount = 0;
        std::map<ESM::RefId, std::vector<MWClass::FalloutSandboxMarker>> mMarkersByCell;

        bool matches(const MWWorld::ESMStore& store) const
        {
            return mStore == &store && mReferenceCount == store.get<ESM4::Reference>().getSize()
                && mMarkerCount == store.get<ESM4::IdleMarker>().getSize()
                && mIdleCount == store.get<ESM4::IdleAnimation>().getSize()
                && mAnimatedObjectCount == store.get<ESM4::AnimObject>().getSize();
        }

        void rebuild(const MWWorld::ESMStore& store)
        {
            mStore = &store;
            mMarkersByCell.clear();

            const auto& referenceStore = store.get<ESM4::Reference>();
            const auto& markerStore = store.get<ESM4::IdleMarker>();
            const auto& idleStore = store.get<ESM4::IdleAnimation>();
            const auto& animatedObjectStore = store.get<ESM4::AnimObject>();
            mReferenceCount = referenceStore.getSize();
            mMarkerCount = markerStore.getSize();
            mIdleCount = idleStore.getSize();
            mAnimatedObjectCount = animatedObjectStore.getSize();

            std::map<ESM::FormId, std::string> animatedObjects;
            for (std::size_t i = 0; i < animatedObjectStore.getSize(); ++i)
            {
                const ESM4::AnimObject* animatedObject = animatedObjectStore.at(i);
                if (animatedObject != nullptr && !animatedObject->mIdleAnim.isZeroOrUnset()
                    && !animatedObject->mModel.empty())
                {
                    animatedObjects.try_emplace(
                        animatedObject->mIdleAnim, normalizeMeshModel(animatedObject->mModel));
                }
            }

            for (std::size_t i = 0; i < referenceStore.getSize(); ++i)
            {
                const ESM4::Reference* reference = referenceStore.at(i);
                if (reference == nullptr
                    || (reference->mFlags & (ESM4::Rec_Deleted | ESM4::Rec_Disabled | ESM4::Rec_Ignored)) != 0)
                    continue;

                const ESM4::IdleMarker* marker = markerStore.search(reference->mBaseObj);
                if (marker == nullptr || marker->mIdleAnim.empty())
                    continue;

                MWClass::FalloutSandboxMarker candidate;
                candidate.mReference = reference->mId;
                candidate.mBase = marker->mId;
                candidate.mCell = reference->mParent;
                candidate.mPosition = osg::Vec3f(
                    reference->mPos.pos[0], reference->mPos.pos[1], reference->mPos.pos[2]);
                candidate.mYaw = reference->mPos.rot[2];
                candidate.mTimer = std::isfinite(marker->mIdleTimer) && marker->mIdleTimer > 0.f
                    ? marker->mIdleTimer
                    : 1.f;

                for (ESM::FormId idleId : marker->mIdleAnim)
                {
                    const ESM4::IdleAnimation* idle = idleStore.search(idleId);
                    if (idle == nullptr || idle->mModel.empty())
                        continue;

                    MWClass::FalloutSandboxIdle idleCandidate;
                    idleCandidate.mId = idle->mId;
                    idleCandidate.mEditorId = idle->mEditorId;
                    idleCandidate.mModel = normalizeMeshModel(idle->mModel);
                    const auto animatedObject = animatedObjects.find(idle->mId);
                    if (animatedObject != animatedObjects.end())
                        idleCandidate.mAnimatedObjectModel = animatedObject->second;
                    candidate.mIdles.push_back(std::move(idleCandidate));
                }

                if (!candidate.mIdles.empty())
                    mMarkersByCell[reference->mParent].push_back(std::move(candidate));
            }
        }
    };

    SandboxMarkerIndex& getSandboxMarkerIndex(const MWWorld::ESMStore& store)
    {
        static SandboxMarkerIndex index;
        if (!index.matches(store))
            index.rebuild(store);
        return index;
    }
}

namespace MWClass
{
    std::string getFalloutSandboxAnimationGroup(const FalloutSandboxIdle& idle)
    {
        return "specialidle_" + idle.mId.toString();
    }

    float getFalloutSandboxRadius(const ESM4::AIPackage& package)
    {
        return static_cast<float>(std::max(64, package.mLocation.radius > 0 ? package.mLocation.radius : 256));
    }

    std::vector<FalloutSandboxMarker> collectFalloutSandboxMarkers(const MWWorld::ESMStore& store,
        const ESM::RefId& cell, const osg::Vec3f& actorPosition, float radius)
    {
        std::vector<FalloutSandboxMarker> result;
        if (radius <= 0.f)
            return result;

        const float radiusSquared = radius * radius;
        const SandboxMarkerIndex& index = getSandboxMarkerIndex(store);
        const auto cellMarkers = index.mMarkersByCell.find(cell);
        if (cellMarkers == index.mMarkersByCell.end())
            return result;

        for (const FalloutSandboxMarker& marker : cellMarkers->second)
        {
            if ((marker.mPosition - actorPosition).length2() > radiusSquared)
                continue;
            result.push_back(marker);
        }

        return result;
    }

    std::optional<std::size_t> selectNearestFalloutSandboxMarker(std::span<const FalloutSandboxMarker> markers,
        const osg::Vec3f& actorPosition, const std::function<bool(ESM::FormId)>& isClaimed)
    {
        float bestDistanceSquared = std::numeric_limits<float>::max();
        std::optional<std::size_t> result;
        for (std::size_t i = 0; i < markers.size(); ++i)
        {
            const FalloutSandboxMarker& marker = markers[i];
            if (marker.mIdles.empty() || (isClaimed && isClaimed(marker.mReference)))
                continue;

            const float distanceSquared = (marker.mPosition - actorPosition).length2();
            if (!std::isfinite(distanceSquared))
                continue;

            if (!result || distanceSquared < bestDistanceSquared)
            {
                result = i;
                bestDistanceSquared = distanceSquared;
            }
        }
        return result;
    }

    bool tryClaimFalloutSandboxMarker(ESM::FormId marker)
    {
        return !marker.isZeroOrUnset() && getMarkerClaims().insert(marker).second;
    }

    void releaseFalloutSandboxMarker(ESM::FormId marker)
    {
        getMarkerClaims().erase(marker);
    }

    bool isFalloutSandboxMarkerClaimed(ESM::FormId marker)
    {
        return getMarkerClaims().contains(marker);
    }

    void writeFalloutSandboxFallback(ESM::AiSequence::AiSequence& sequence, float radius, int duration,
        int timeOfDay, const osg::Vec3f& origin)
    {
        auto wander = std::make_unique<ESM::AiSequence::AiWander>();
        const long roundedRadius = std::isfinite(radius) ? std::lround(radius) : 0;
        wander->mData.mDistance = static_cast<std::int16_t>(
            std::clamp(roundedRadius, 0l, static_cast<long>(std::numeric_limits<std::int16_t>::max())));
        wander->mData.mDuration = static_cast<std::int16_t>(
            std::clamp(duration, 0, static_cast<int>(std::numeric_limits<std::int16_t>::max())));
        wander->mData.mTimeOfDay = static_cast<std::uint8_t>(std::clamp(timeOfDay, 0, 23));
        std::fill_n(wander->mData.mIdle, 8, 0);
        wander->mData.mShouldRepeat = 1;
        wander->mDurationData.mRemainingDuration = static_cast<float>(wander->mData.mDuration);
        wander->mDurationData.mDestinationTolerance = 0;
        wander->mStoredInitialActorPosition = true;
        wander->mInitialActorPosition.mValues[0] = origin.x();
        wander->mInitialActorPosition.mValues[1] = origin.y();
        wander->mInitialActorPosition.mValues[2] = origin.z();
        wander->mReevaluateFnvSandbox = true;

        ESM::AiSequence::AiPackageContainer package;
        package.mType = ESM::AiSequence::Ai_Wander;
        package.mPackage = std::move(wander);
        sequence.mPackages.push_back(std::move(package));
    }

    std::optional<FalloutSandboxSaveFallback> getFalloutSandboxSaveFallback(
        const ESM::AiSequence::AiSequence& sequence)
    {
        for (const ESM::AiSequence::AiPackageContainer& package : sequence.mPackages)
        {
            if (package.mType != ESM::AiSequence::Ai_Wander || package.mPackage == nullptr)
                continue;
            const auto* wander = dynamic_cast<const ESM::AiSequence::AiWander*>(package.mPackage.get());
            if (wander == nullptr || !wander->mReevaluateFnvSandbox)
                continue;
            osg::Vec3f origin;
            if (wander->mStoredInitialActorPosition)
            {
                origin.set(wander->mInitialActorPosition.mValues[0], wander->mInitialActorPosition.mValues[1],
                    wander->mInitialActorPosition.mValues[2]);
            }
            return FalloutSandboxSaveFallback{ static_cast<float>(std::max<std::int16_t>(0, wander->mData.mDistance)),
                std::max<int>(0, wander->mData.mDuration), wander->mData.mTimeOfDay, origin };
        }
        return std::nullopt;
    }
}
