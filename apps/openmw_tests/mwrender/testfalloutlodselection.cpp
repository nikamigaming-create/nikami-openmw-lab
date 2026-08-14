#include <gtest/gtest.h>

#include <components/esm/common.hpp>
#include <components/esm4/common.hpp>

#include "apps/openmw/mwrender/falloutlodselection.hpp"

namespace MWRender
{
    namespace
    {
        // Frozen winning FNV load-order census (2026-07-18): 10 official plugins and 21 configured archives.
        // Of 211,339 enabled supported exterior placements, all 23,810 references marked Visible When Distant
        // resolve a proxy. Another 4,676 unmarked references are proxy-capable. The authored reference flag,
        // rather than proxy existence alone, therefore controls participation in distant paging.
        TEST(FalloutLodSelectionTest, RecognizesOnlyFalloutNewVegasRecordVersions)
        {
            EXPECT_TRUE(isFalloutNewVegasVersion(ESM::VER_132));
            EXPECT_TRUE(isFalloutNewVegasVersion(ESM::VER_133));
            EXPECT_TRUE(isFalloutNewVegasVersion(ESM::VER_134));

            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_080));
            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_094));
            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_095));
            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_100));
            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_120));
            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_130));
            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_170));
            EXPECT_FALSE(isFalloutNewVegasVersion(ESM::VER_171));
        }

        TEST(FalloutLodSelectionTest, ActiveGridAlwaysKeepsTheFullModel)
        {
            EXPECT_EQ(
                selectFalloutNewVegasDistantReference(0, true, false), FalloutDistantReferenceSelection::FullModel);
            EXPECT_EQ(selectFalloutNewVegasDistantReference(ESM4::Rec_VisDistant, true, true),
                FalloutDistantReferenceSelection::FullModel);
        }

        TEST(FalloutLodSelectionTest, DisabledReferencesRemainExcludedBeforeSelection)
        {
            EXPECT_TRUE(isEsm4ReferenceEnabledForPaging(0));
            EXPECT_TRUE(isEsm4ReferenceEnabledForPaging(ESM4::Rec_VisDistant));
            EXPECT_FALSE(isEsm4ReferenceEnabledForPaging(ESM4::Rec_Disabled));
            EXPECT_FALSE(isEsm4ReferenceEnabledForPaging(ESM4::Rec_Disabled | ESM4::Rec_VisDistant));
        }

        TEST(FalloutLodSelectionTest, ProxyPresenceDoesNotPromoteAnUnflaggedPlacement)
        {
            EXPECT_EQ(selectFalloutNewVegasDistantReference(0, false, false), FalloutDistantReferenceSelection::Skip);
            EXPECT_EQ(selectFalloutNewVegasDistantReference(0, false, true), FalloutDistantReferenceSelection::Skip);
        }

        TEST(FalloutLodSelectionTest, VisibleDistantPlacementUsesAnAvailableProxy)
        {
            EXPECT_EQ(selectFalloutNewVegasDistantReference(ESM4::Rec_VisDistant, false, true),
                FalloutDistantReferenceSelection::DistantModel);
        }

        TEST(FalloutLodSelectionTest, VisibleDistantPlacementFallsBackToFullModelWithoutAProxy)
        {
            EXPECT_EQ(selectFalloutNewVegasDistantReference(ESM4::Rec_VisDistant, false, false),
                FalloutDistantReferenceSelection::FullModel);
        }
    }
}
