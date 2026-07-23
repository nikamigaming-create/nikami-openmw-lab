#include <components/sceneutil/riggeometry.hpp>

#include <gtest/gtest.h>

#include <osg/PolygonMode>

namespace
{
    osg::ref_ptr<osg::Geometry> makeSourceGeometry()
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({ 0.f, 0.f, 0.f });
        vertices->push_back({ 1.f, 0.f, 0.f });
        vertices->push_back({ 0.f, 1.f, 0.f });
        vertices->push_back({ 0.f, 0.f, 1.f });
        geometry->setVertexArray(vertices);

        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        colors->push_back({ 0.25f, 0.5f, 0.75f, 1.f });
        geometry->setColorArray(colors, osg::Array::BIND_OVERALL);
        return geometry;
    }

    osg::ref_ptr<SceneUtil::RigGeometry> makeBodyRig()
    {
        osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
        std::vector<SceneUtil::RigGeometry::BoneInfo> bones(3);
        bones[0].mName = "Bip01 Spine2";
        bones[1].mName = "Bip01 L Calf";
        bones[2].mName = "Bip01 R Hand";
        rig->setBoneInfo(std::move(bones));
        rig->setInfluences(std::vector<SceneUtil::RigGeometry::BoneWeights>{
            { { 0, 1.f } }, { { 1, 0.8f }, { 0, 0.2f } }, { { 1, 0.1f }, { 0, 0.9f } }, { { 2, 1.f } } });
        rig->setSourceGeometry(makeSourceGeometry());
        return rig;
    }

    TEST(SceneUtilRigGeometry, FalloutVatsHighlightsWholeBodyAndSelectedLimbFromSkinWeights)
    {
        osg::ref_ptr<SceneUtil::RigGeometry> rig = makeBodyRig();
        const std::array<std::string_view, 2> targetBones{ "bip01 spine2", "BIP01 L CALF" };

        ASSERT_TRUE(rig->setFalloutVatsHighlight(targetBones, "Bip01 L Calf", true));
        for (unsigned int buffer = 0; buffer < 2; ++buffer)
        {
            const auto* colors = dynamic_cast<const osg::Vec4Array*>(rig->getRenderGeometry(buffer)->getColorArray());
            ASSERT_NE(colors, nullptr);
            ASSERT_EQ(colors->size(), 4u);
            EXPECT_EQ((*colors)[0], osg::Vec4f(1.f, 0.58f, 0.12f, 1.f));
            EXPECT_EQ((*colors)[1], osg::Vec4f(0.18f, 1.f, 0.28f, 1.f));
            EXPECT_EQ((*colors)[2], osg::Vec4f(1.f, 0.58f, 0.12f, 1.f));
            EXPECT_EQ((*colors)[3], osg::Vec4f(1.f, 0.58f, 0.12f, 1.f));
            EXPECT_EQ(rig->getRenderGeometry(buffer)->getColorBinding(), osg::Array::BIND_PER_VERTEX);
        }

        const auto* polygonMode = dynamic_cast<const osg::PolygonMode*>(
            rig->getStateSet()->getAttribute(osg::StateAttribute::POLYGONMODE));
        ASSERT_NE(polygonMode, nullptr);
        EXPECT_EQ(polygonMode->getMode(osg::PolygonMode::FRONT), osg::PolygonMode::LINE);
        EXPECT_EQ(polygonMode->getMode(osg::PolygonMode::BACK), osg::PolygonMode::LINE);
    }

    TEST(SceneUtilRigGeometry, FalloutVatsDisableRestoresOriginalColorsAndStateExactly)
    {
        osg::ref_ptr<SceneUtil::RigGeometry> rig = makeBodyRig();
        osg::ref_ptr<osg::StateSet> originalState = new osg::StateSet;
        rig->setStateSet(originalState);
        std::array<osg::ref_ptr<osg::Array>, 2> originalColors;
        for (unsigned int buffer = 0; buffer < 2; ++buffer)
            originalColors[buffer] = rig->getRenderGeometry(buffer)->getColorArray();
        const std::array<std::string_view, 2> targetBones{ "Bip01 Spine2", "Bip01 L Calf" };

        ASSERT_TRUE(rig->setFalloutVatsHighlight(targetBones, "Bip01 L Calf", true));
        EXPECT_FALSE(rig->setFalloutVatsHighlight({}, {}, false));

        EXPECT_EQ(rig->getStateSet(), originalState.get());
        for (unsigned int buffer = 0; buffer < 2; ++buffer)
            EXPECT_EQ(rig->getRenderGeometry(buffer)->getColorArray(), originalColors[buffer].get());
    }

    TEST(SceneUtilRigGeometry, FalloutVatsRejectsRigidAttachmentOnlyBoneSet)
    {
        osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
        std::vector<SceneUtil::RigGeometry::BoneInfo> bones(1);
        bones[0].mName = "Bip01 Weapon";
        rig->setBoneInfo(std::move(bones));
        rig->setInfluences(std::vector<SceneUtil::RigGeometry::BoneWeights>(4, { { 0, 1.f } }));
        rig->setSourceGeometry(makeSourceGeometry());
        const std::array<std::string_view, 1> targetBones{ "Bip01 L Calf" };

        EXPECT_FALSE(rig->setFalloutVatsHighlight(targetBones, "Bip01 Weapon", true));
        EXPECT_EQ(rig->getStateSet(), nullptr);
    }

    TEST(SceneUtilRigGeometry, FalloutVatsMakesOtherBodyDrawableAmberWhenSelectedBoneIsElsewhere)
    {
        osg::ref_ptr<SceneUtil::RigGeometry> rig = makeBodyRig();
        const std::array<std::string_view, 1> targetBones{ "Bip01 Spine2" };

        ASSERT_TRUE(rig->setFalloutVatsHighlight(targetBones, "Bip01 Head", true));
        const auto* colors = dynamic_cast<const osg::Vec4Array*>(rig->getRenderGeometry(0)->getColorArray());
        ASSERT_NE(colors, nullptr);
        for (const osg::Vec4f& color : *colors)
            EXPECT_EQ(color, osg::Vec4f(1.f, 0.58f, 0.12f, 1.f));
    }
}
