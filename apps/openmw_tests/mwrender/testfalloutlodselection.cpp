#include <gtest/gtest.h>

#include <components/esm/common.hpp>
#include <components/esm4/common.hpp>

#include "apps/openmw/mwrender/falloutlodselection.hpp"

namespace MWRender
{
    namespace
    {
        // Frozen winning FNV load-order census (2026-07-18): 10 official plugins, 21 configured archives.
        // Ordered plugin-manifest SHA-256: 415624767cedf96499ec973e92a5c78b59d28aa1f4dc37e9f13063d984cc6d5a.
        // Ordered archive-manifest SHA-256: ed488e144da50670c3dc4ab498ea10358d832b45fe29bd049bd65a1ca9023f1a.
        // Combined corpus SHA-256: 8fd2492f6236133dec238b17c5f28eade3b9600c6f673c94667e9b5adc1ec7bd.
        // Each ordered manifest hashes UTF-8 name<TAB>byte-size<TAB>file-SHA-256<LF> entries.
        // Of 211,339 enabled supported exterior REFR placements, all 23,810 marked Visible When Distant resolve
        // a _lod[_N] proxy. 4,676 unmarked placements are also proxy-capable, and 897 disabled placements are
        // excluded before selection.
        // The authored REFR flag, not proxy existence alone, therefore controls distant participation.

        TEST(FalloutLodSelectionTest, recognizesOnlyFalloutNewVegasRecordVersions)
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

        TEST(FalloutLodSelectionTest, activeGridAlwaysKeepsTheFullModel)
        {
            EXPECT_EQ(
                selectFalloutNewVegasDistantReference(0, true, false), FalloutDistantReferenceSelection::FullModel);
            EXPECT_EQ(selectFalloutNewVegasDistantReference(ESM4::Rec_VisDistant, true, true),
                FalloutDistantReferenceSelection::FullModel);
        }

        TEST(FalloutLodSelectionTest, disabledReferencesRemainExcludedBeforeFalloutSelection)
        {
            EXPECT_TRUE(isEsm4ReferenceEnabledForPaging(0));
            EXPECT_TRUE(isEsm4ReferenceEnabledForPaging(ESM4::Rec_VisDistant));
            EXPECT_FALSE(isEsm4ReferenceEnabledForPaging(ESM4::Rec_Disabled));
            EXPECT_FALSE(isEsm4ReferenceEnabledForPaging(ESM4::Rec_Disabled | ESM4::Rec_VisDistant));
        }

        TEST(FalloutLodSelectionTest, proxyPresenceDoesNotPromoteAnUnflaggedPlacement)
        {
            EXPECT_EQ(selectFalloutNewVegasDistantReference(0, false, false), FalloutDistantReferenceSelection::Skip);
            EXPECT_EQ(selectFalloutNewVegasDistantReference(0, false, true), FalloutDistantReferenceSelection::Skip);
        }

        TEST(FalloutLodSelectionTest, visibleDistantPlacementUsesAnAvailableProxy)
        {
            EXPECT_EQ(selectFalloutNewVegasDistantReference(ESM4::Rec_VisDistant, false, true),
                FalloutDistantReferenceSelection::DistantModel);
        }

        TEST(FalloutLodSelectionTest, visibleDistantPlacementFallsBackToTheFullModelWhenProxyIsMissing)
        {
            EXPECT_EQ(selectFalloutNewVegasDistantReference(ESM4::Rec_VisDistant, false, false),
                FalloutDistantReferenceSelection::FullModel);
        }
    }
}
