#include <gtest/gtest.h>

#include <limits>
#include <string>

#include <osg/Group>
#include <osg/Image>
#include <osg/NodeVisitor>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>

#include <osgUtil/UpdateVisitor>

#include "apps/openmw/mwrender/skyutil.hpp"

namespace MWRender
{
    namespace
    {
        class InspectableCloudUpdater : public CloudUpdater
        {
        public:
            using CloudUpdater::apply;
            using CloudUpdater::setDefaults;
        };

        osg::ref_ptr<osg::Texture2D> makeTexture(const std::string& name, unsigned char value)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            image->data()[0] = value;
            image->data()[1] = value;
            image->data()[2] = value;
            image->data()[3] = 255;
            image->setFileName(name);
            return new osg::Texture2D(image);
        }

        osg::Texture2D* textureAt(osg::StateSet& stateSet, unsigned int unit)
        {
            return dynamic_cast<osg::Texture2D*>(
                stateSet.getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
        }

        TEST(FalloutCloudTransitionTest, bindsDistinctEndpointSamplersAndClearsEmptySides)
        {
            osg::ref_ptr<InspectableCloudUpdater> updater = new InspectableCloudUpdater;
            osg::ref_ptr<osg::Texture2D> current = makeTexture("current.dds", 64);
            osg::ref_ptr<osg::Texture2D> next = makeTexture("next.dds", 192);
            updater->setFalloutCloudShader(true);
            updater->setTexture(current);
            updater->setBlendTexture(next);
            updater->setBlendFactor(0.25f);

            osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
            updater->setDefaults(stateSet);
            updater->apply(stateSet, nullptr);

            EXPECT_EQ(textureAt(*stateSet, 0), current.get());
            EXPECT_EQ(textureAt(*stateSet, 1), next.get());
            ASSERT_NE(stateSet->getUniform("diffuseMap"), nullptr);
            ASSERT_NE(stateSet->getUniform("falloutCloudBlendMap"), nullptr);
            ASSERT_NE(stateSet->getUniform("falloutCloudBlendFactor"), nullptr);
            int currentUnit = -1;
            int nextUnit = -1;
            float blend = -1.f;
            stateSet->getUniform("diffuseMap")->get(currentUnit);
            stateSet->getUniform("falloutCloudBlendMap")->get(nextUnit);
            stateSet->getUniform("falloutCloudBlendFactor")->get(blend);
            EXPECT_EQ(currentUnit, 0);
            EXPECT_EQ(nextUnit, 1);
            EXPECT_FLOAT_EQ(blend, 0.25f);

            updater->setBlendFactor(std::numeric_limits<float>::quiet_NaN());
            updater->apply(stateSet, nullptr);
            stateSet->getUniform("falloutCloudBlendFactor")->get(blend);
            EXPECT_FLOAT_EQ(blend, 0.f);

            updater->setTexture(nullptr);
            updater->apply(stateSet, nullptr);
            osg::Texture2D* emptyCurrent = textureAt(*stateSet, 0);
            ASSERT_NE(emptyCurrent, nullptr);
            EXPECT_NE(emptyCurrent, current.get());
            ASSERT_NE(emptyCurrent->getImage(), nullptr);
            EXPECT_EQ(emptyCurrent->getImage()->getFileName(), "__openmw_empty_fallout_cloud__");
            EXPECT_EQ(textureAt(*stateSet, 1), next.get());

            updater->setBlendTexture(nullptr);
            updater->apply(stateSet, nullptr);
            EXPECT_EQ(textureAt(*stateSet, 0), emptyCurrent);
            EXPECT_EQ(textureAt(*stateSet, 1), emptyCurrent);
            EXPECT_NE(textureAt(*stateSet, 1), next.get());
        }

        TEST(FalloutCloudTransitionTest, appliesLatestEndpointsAfterDisabledNodeIsReenabled)
        {
            osg::ref_ptr<CloudUpdater> updater = new CloudUpdater;
            osg::ref_ptr<osg::Texture2D> firstCurrent = makeTexture("first-current.dds", 32);
            osg::ref_ptr<osg::Texture2D> firstNext = makeTexture("first-next.dds", 96);
            osg::ref_ptr<osg::Texture2D> resumedCurrent = makeTexture("resumed-current.dds", 128);
            osg::ref_ptr<osg::Texture2D> resumedNext = makeTexture("resumed-next.dds", 224);
            updater->setFalloutCloudShader(true);
            updater->setTexture(firstCurrent);
            updater->setBlendTexture(firstNext);
            updater->setBlendFactor(0.2f);

            osg::ref_ptr<osg::Group> node = new osg::Group;
            node->setUpdateCallback(updater);
            osgUtil::UpdateVisitor visitor;
            visitor.setTraversalNumber(0);
            node->accept(visitor);
            ASSERT_NE(node->getStateSet(), nullptr);
            EXPECT_EQ(textureAt(*node->getStateSet(), 0), firstCurrent.get());
            EXPECT_EQ(textureAt(*node->getStateSet(), 1), firstNext.get());

            node->setNodeMask(0);
            updater->setTexture(resumedCurrent);
            updater->setBlendTexture(resumedNext);
            updater->setBlendFactor(0.8f);
            visitor.setTraversalNumber(1);
            node->accept(visitor);

            node->setNodeMask(~osg::Node::NodeMask(0));
            visitor.setTraversalNumber(2);
            node->accept(visitor);
            ASSERT_NE(node->getStateSet(), nullptr);
            EXPECT_EQ(textureAt(*node->getStateSet(), 0), resumedCurrent.get());
            EXPECT_EQ(textureAt(*node->getStateSet(), 1), resumedNext.get());
            float blend = -1.f;
            ASSERT_NE(node->getStateSet()->getUniform("falloutCloudBlendFactor"), nullptr);
            node->getStateSet()->getUniform("falloutCloudBlendFactor")->get(blend);
            EXPECT_FLOAT_EQ(blend, 0.8f);
        }

        TEST(FalloutCloudTransitionTest, visibilityTracksOnlyContributingEndpointTextures)
        {
            EXPECT_FALSE(hasVisibleFalloutCloudContribution("", "next.dds", 0.f));
            EXPECT_TRUE(hasVisibleFalloutCloudContribution("", "next.dds", 0.01f));
            EXPECT_TRUE(hasVisibleFalloutCloudContribution("current.dds", "", 0.99f));
            EXPECT_FALSE(hasVisibleFalloutCloudContribution("current.dds", "", 1.f));
            EXPECT_TRUE(hasVisibleFalloutCloudContribution("current.dds", "next.dds", 0.5f));
            EXPECT_FALSE(hasVisibleFalloutCloudContribution("", "", 0.5f));
            EXPECT_FALSE(hasVisibleFalloutCloudContribution(
                "current.dds", "next.dds", std::numeric_limits<float>::quiet_NaN()));
        }
    }
}
