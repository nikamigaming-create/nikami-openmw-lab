#include <gtest/gtest.h>

#include <memory>

#include <components/esm4/common.hpp>
#include <components/esm4/loadanio.hpp>
#include <components/esm4/loadidle.hpp>
#include <components/esm4/loadidlm.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm3/aisequence.hpp>

#include "apps/openmw/mwclass/fnvsandbox.hpp"
#include "apps/openmw/mwmechanics/aisequence.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    ESM::FormId form(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    ESM::RefId cell(std::uint32_t value)
    {
        return ESM::RefId(form(value));
    }

    MWClass::FalloutSandboxMarker marker(std::uint32_t id, float x)
    {
        MWClass::FalloutSandboxMarker result;
        result.mReference = form(id);
        result.mPosition = osg::Vec3f(x, 0.f, 0.f);
        result.mIdles.emplace_back();
        return result;
    }

    ESM4::Reference reference(
        std::uint32_t id, ESM::FormId base, const ESM::RefId& parent, float x, std::uint32_t flags = 0)
    {
        ESM4::Reference result{};
        result.mId = form(id);
        result.mBaseObj = base;
        result.mParent = parent;
        result.mFlags = flags;
        result.mPos.pos[0] = x;
        result.mPos.pos[1] = 2.f;
        result.mPos.pos[2] = 3.f;
        result.mPos.rot[2] = 0.75f;
        return result;
    }

    template <class T>
    void insertStatic(MWWorld::ESMStore& store, const T& record)
    {
        const_cast<MWWorld::Store<T>&>(store.get<T>()).insertStatic(record);
    }

    TEST(FnvSandboxTest, SelectsNearestUnclaimedMarker)
    {
        const std::vector<MWClass::FalloutSandboxMarker> markers{ marker(0x101, 20.f), marker(0x102, 5.f),
            marker(0x103, 100.f) };

        std::optional<std::size_t> selected = MWClass::selectNearestFalloutSandboxMarker(
            markers, osg::Vec3f(), [](ESM::FormId id) { return id == form(0x102); });
        ASSERT_TRUE(selected.has_value());
        EXPECT_EQ(*selected, 0u);

        selected = MWClass::selectNearestFalloutSandboxMarker(markers, osg::Vec3f());
        ASSERT_TRUE(selected.has_value());
        EXPECT_EQ(*selected, 1u);
        EXPECT_FALSE(MWClass::selectNearestFalloutSandboxMarker(
            markers, osg::Vec3f(), [](ESM::FormId) { return true; })
                         .has_value());
    }

    TEST(FnvSandboxTest, ClaimsAreExclusiveAndExplicitlyReleased)
    {
        const ESM::FormId markerId = form(0x201);
        MWClass::releaseFalloutSandboxMarker(markerId);
        EXPECT_TRUE(MWClass::tryClaimFalloutSandboxMarker(markerId));
        EXPECT_TRUE(MWClass::isFalloutSandboxMarkerClaimed(markerId));
        EXPECT_FALSE(MWClass::tryClaimFalloutSandboxMarker(markerId));
        MWClass::releaseFalloutSandboxMarker(markerId);
        EXPECT_FALSE(MWClass::isFalloutSandboxMarkerClaimed(markerId));
    }

    TEST(FnvSandboxTest, GivesEveryAuthoredIdleAStableSourceSpecificGroup)
    {
        MWClass::FalloutSandboxIdle raking;
        raking.mId = form(0x0108957a);
        raking.mModel = "meshes/characters/_male/idleanims/raking.kf";

        MWClass::FalloutSandboxIdle sweeping = raking;
        sweeping.mId = form(0x0108957b);

        const std::string rakingGroup = MWClass::getFalloutSandboxAnimationGroup(raking);
        const std::string sweepingGroup = MWClass::getFalloutSandboxAnimationGroup(sweeping);
        EXPECT_TRUE(rakingGroup.starts_with("specialidle_"));
        EXPECT_TRUE(sweepingGroup.starts_with("specialidle_"));
        EXPECT_NE(rakingGroup, sweepingGroup);
        EXPECT_EQ(rakingGroup, "specialidle_0x108957a");
    }

    TEST(FnvSandboxTest, SaveFallbackRoundTripsAuthoredPackageParameters)
    {
        ESM::AiSequence::AiSequence sequence;
        const osg::Vec3f origin(10.f, 20.f, 30.f);
        MWClass::writeFalloutSandboxFallback(sequence, 512.f, 16, 6, origin);

        ASSERT_EQ(sequence.mPackages.size(), 1u);
        EXPECT_EQ(sequence.mPackages[0].mType, ESM::AiSequence::Ai_Wander);
        const auto* wander = dynamic_cast<const ESM::AiSequence::AiWander*>(sequence.mPackages[0].mPackage.get());
        ASSERT_NE(wander, nullptr);
        EXPECT_EQ(wander->mData.mDistance, 512);
        EXPECT_EQ(wander->mData.mDuration, 16);
        EXPECT_EQ(wander->mData.mTimeOfDay, 6);
        EXPECT_EQ(wander->mData.mShouldRepeat, 1);
        EXPECT_TRUE(wander->mReevaluateFnvSandbox);

        const std::optional<MWClass::FalloutSandboxSaveFallback> fallback
            = MWClass::getFalloutSandboxSaveFallback(sequence);
        ASSERT_TRUE(fallback.has_value());
        EXPECT_FLOAT_EQ(fallback->mRadius, 512.f);
        EXPECT_EQ(fallback->mDuration, 16);
        EXPECT_EQ(fallback->mTimeOfDay, 6);
        EXPECT_EQ(fallback->mOrigin, origin);

        ESM::AiSequence::AiSequence secondSave;
        MWClass::writeFalloutSandboxFallback(
            secondSave, fallback->mRadius, fallback->mDuration, fallback->mTimeOfDay, fallback->mOrigin);
        EXPECT_TRUE(MWClass::getFalloutSandboxSaveFallback(secondSave).has_value());
    }

    TEST(FnvSandboxTest, TaggedFallbackIsOmittedForAuthoredPackageReevaluation)
    {
        ESM::AiSequence::AiSequence sequence;
        MWClass::writeFalloutSandboxFallback(sequence, 512.f, 16, 6, osg::Vec3f(10.f, 20.f, 30.f));

        MWMechanics::AiSequence runtime;
        EXPECT_FALSE(MWMechanics::shouldRestoreSavedAiWander(
            static_cast<const ESM::AiSequence::AiWander&>(*sequence.mPackages[0].mPackage)));
        runtime.readState(sequence);
        EXPECT_TRUE(runtime.isEmpty());
    }

    TEST(FnvSandboxTest, OrdinarySavedWanderUsesUnchangedRuntimeRestorePath)
    {
        ESM::AiSequence::AiSequence sequence;
        auto wander = std::make_unique<ESM::AiSequence::AiWander>();
        wander->mData.mDistance = 256;
        wander->mData.mDuration = 5;
        wander->mDurationData.mRemainingDuration = 5.f;
        ESM::AiSequence::AiPackageContainer package;
        package.mType = ESM::AiSequence::Ai_Wander;
        package.mPackage = std::move(wander);
        sequence.mPackages.push_back(std::move(package));

        const auto& saved = static_cast<const ESM::AiSequence::AiWander&>(*sequence.mPackages[0].mPackage);
        EXPECT_TRUE(MWMechanics::shouldRestoreSavedAiWander(saved));
    }

    TEST(FnvSandboxTest, ResolvesAuthoredMarkerIdleAndAnimatedObjectByCell)
    {
        MWWorld::ESMStore store;
        const ESM::RefId targetCell = cell(0x301);
        const ESM::RefId otherCell = cell(0x302);
        const ESM::FormId markerBase = form(0x303);
        const ESM::FormId idleId = form(0x304);

        ESM4::IdleAnimation idle{};
        idle.mId = idleId;
        idle.mEditorId = "AuthoredRakingIdle";
        idle.mModel = "Characters\\_Male\\IdleAnims\\Raking.kf";
        insertStatic(store, idle);

        ESM4::IdleMarker idleMarker{};
        idleMarker.mId = markerBase;
        idleMarker.mEditorId = "DefaultRakingMarker";
        idleMarker.mIdleTimer = 10.f;
        idleMarker.mIdleAnim.push_back(idleId);
        insertStatic(store, idleMarker);

        ESM4::AnimObject animatedObject{};
        animatedObject.mId = form(0x305);
        animatedObject.mEditorId = "aoRaking";
        animatedObject.mModel = "AnimObjects\\aoRake.NIF";
        animatedObject.mIdleAnim = idleId;
        insertStatic(store, animatedObject);

        insertStatic(store, reference(0x306, markerBase, targetCell, 12.f));
        insertStatic(store, reference(0x307, markerBase, otherCell, 1.f));
        insertStatic(store, reference(0x308, markerBase, targetCell, 2.f, ESM4::Rec_Disabled));
        insertStatic(store, reference(0x30a, markerBase, targetCell, 35.f));

        std::vector<MWClass::FalloutSandboxMarker> markers = MWClass::collectFalloutSandboxMarkers(
            store, targetCell, osg::Vec3f(10.f, 2.f, 3.f), 20.f);
        ASSERT_EQ(markers.size(), 1u);
        EXPECT_EQ(markers[0].mReference, form(0x306));
        EXPECT_EQ(markers[0].mCell, targetCell);
        EXPECT_FLOAT_EQ(markers[0].mPosition.x(), 12.f);
        EXPECT_FLOAT_EQ(markers[0].mYaw, 0.75f);
        EXPECT_FLOAT_EQ(markers[0].mTimer, 10.f);
        ASSERT_EQ(markers[0].mIdles.size(), 1u);
        EXPECT_EQ(markers[0].mIdles[0].mId, idleId);
        EXPECT_EQ(markers[0].mIdles[0].mModel, "meshes/characters/_male/idleanims/raking.kf");
        EXPECT_EQ(markers[0].mIdles[0].mAnimatedObjectModel, "meshes/animobjects/aorake.nif");
        EXPECT_EQ(store.get<ESM4::AnimObject>().getSize(), 1u);

        // Candidate membership stays pinned to the package origin (x=10), even after the actor walks near the
        // authored radius edge. The out-of-bound x=35 marker cannot cause marker-to-marker drift.
        const std::optional<std::size_t> movedActorSelection
            = MWClass::selectNearestFalloutSandboxMarker(markers, osg::Vec3f(34.f, 2.f, 3.f));
        ASSERT_TRUE(movedActorSelection.has_value());
        EXPECT_EQ(markers[*movedActorSelection].mReference, form(0x306));

        // The cache is generation-aware: adding a record rebuilds the cell index rather than returning stale data.
        insertStatic(store, reference(0x309, markerBase, targetCell, 11.f));
        markers = MWClass::collectFalloutSandboxMarkers(
            store, targetCell, osg::Vec3f(10.f, 2.f, 3.f), 20.f);
        EXPECT_EQ(markers.size(), 2u);
    }
}
