#include <components/sceneutil/visitor.hpp>

#include <gtest/gtest.h>

namespace
{
    using SceneUtil::NodeNameMatchKind;

    TEST(SceneUtilNodeNameMatcher, KeepsExactNamesHighestPriority)
    {
        const SceneUtil::NodeNameMatch match = SceneUtil::matchNodeName("Bip01 Screen10", "bip01 screen10");
        EXPECT_EQ(match.mKind, NodeNameMatchKind::Exact);
        EXPECT_EQ(match.mScore, 900);
    }

    TEST(SceneUtilNodeNameMatcher, NormalizesSeparatorsAndExporterGeometryIndex)
    {
        EXPECT_EQ(SceneUtil::matchNodeName("Bip01 L Shoulder", "Bip01_LShoulder").mKind,
            NodeNameMatchKind::Canonical);
        EXPECT_EQ(SceneUtil::matchNodeName("ScreenStatic:0", "Screen Static").mKind,
            NodeNameMatchKind::Canonical);
    }

    TEST(SceneUtilNodeNameMatcher, CanonicalizesRetailProjectileHelperDecoration)
    {
        const SceneUtil::NodeNameMatch match
            = SceneUtil::matchNodeName("ProjectileNode", "##ProjectileNode");
        EXPECT_EQ(match.mKind, NodeNameMatchKind::Canonical);
    }

    TEST(SceneUtilNodeNameMatcher, UniqueBestVisitorPrefersExactProjectileHelper)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::MatrixTransform> canonical = new osg::MatrixTransform;
        canonical->setName("##ProjectileNode");
        root->addChild(canonical);
        osg::ref_ptr<osg::MatrixTransform> exact = new osg::MatrixTransform;
        exact->setName("ProjectileNode");
        root->addChild(exact);

        SceneUtil::FindUniqueBestByNameVisitor visitor("ProjectileNode");
        root->accept(visitor);

        EXPECT_EQ(visitor.getFoundNode(), exact.get());
        EXPECT_EQ(visitor.getFoundMatch().mKind, NodeNameMatchKind::Exact);
        EXPECT_EQ(visitor.getBestMatchCount(), 1u);
    }

    TEST(SceneUtilNodeNameMatcher, UniqueBestVisitorRejectsAmbiguousCanonicalProjectileHelpers)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        for (const char* const name : { "##ProjectileNode", "Projectile_Node" })
        {
            osg::ref_ptr<osg::MatrixTransform> candidate = new osg::MatrixTransform;
            candidate->setName(name);
            root->addChild(candidate);
        }

        SceneUtil::FindUniqueBestByNameVisitor visitor("ProjectileNode");
        root->accept(visitor);

        EXPECT_EQ(visitor.getFoundNode(), nullptr);
        EXPECT_EQ(visitor.getFoundMatch().mKind, NodeNameMatchKind::Canonical);
        EXPECT_EQ(visitor.getBestMatchCount(), 2u);
    }

    TEST(SceneUtilNodeNameMatcher, MatchesSkeletonPrefixAndRootDecorationSemantically)
    {
        const SceneUtil::NodeNameMatch match
            = SceneUtil::matchNodeName("Bip01 ScreenStatic", "ScreenStaticRoot");
        EXPECT_EQ(match.mKind, NodeNameMatchKind::Semantic);
        EXPECT_GT(match.mScore, 0);
    }

    TEST(SceneUtilNodeNameMatcher, MatchesZeroPaddedNumericSuffix)
    {
        const SceneUtil::NodeNameMatch match = SceneUtil::matchNodeName("Screen1", "Screen01Root");
        EXPECT_EQ(match.mKind, NodeNameMatchKind::NumericEquivalent);
    }

    TEST(SceneUtilNodeNameMatcher, RecoversReversedTwoDigitExporterSuffix)
    {
        const SceneUtil::NodeNameMatch match
            = SceneUtil::matchNodeName("Bip01 Screen10", "Bip01 Screen01");
        EXPECT_EQ(match.mKind, NodeNameMatchKind::NumericReversed);
    }

    TEST(SceneUtilNodeNameMatcher, AllowsUniqueDefaultIndexSpelling)
    {
        const SceneUtil::NodeNameMatch match = SceneUtil::matchNodeName("Bip01 VoiceBox1", "VoiceBox_Root");
        EXPECT_EQ(match.mKind, NodeNameMatchKind::DefaultIndex);
    }

    TEST(SceneUtilNodeNameMatcher, RejectsDifferentSemanticTargets)
    {
        EXPECT_EQ(SceneUtil::matchNodeName("Bip01 LShoulder", "Bip01 RShoulder").mKind,
            NodeNameMatchKind::None);
    }
}
