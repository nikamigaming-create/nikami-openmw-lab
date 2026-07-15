#include <gtest/gtest.h>

#include <components/nif/particle.hpp>
#include <components/nifosg/particle.hpp>

namespace
{
    TEST(NifOsgParticleTest, EvaluatesBethesdaThreeColorAndFadeCurve)
    {
        Nif::BSPSysSimpleColorModifier modifier;
        modifier.mFadeInPercent = 0.1f;
        modifier.mFadeOutPercent = 0.9f;
        modifier.mColor1EndPercent = 0.2f;
        modifier.mColor1StartPercent = 0.4f;
        modifier.mColor2EndPercent = 0.6f;
        modifier.mColor2StartPercent = 0.8f;
        modifier.mColors = { osg::Vec4f(1.f, 0.f, 0.f, 0.25f), osg::Vec4f(0.f, 1.f, 0.f, 0.5f),
            osg::Vec4f(0.f, 0.f, 1.f, 0.75f) };

        const NifOsg::BethesdaParticleColorAffector affector(&modifier);
        EXPECT_EQ(affector.evaluate(0.f).a(), 0.f);
        EXPECT_EQ(affector.evaluate(0.2f), modifier.mColors[0]);
        EXPECT_EQ(affector.evaluate(0.5f), modifier.mColors[1]);

        const osg::Vec4f firstBlend = affector.evaluate(0.3f);
        EXPECT_NEAR(firstBlend.r(), 0.5f, 1e-6f);
        EXPECT_NEAR(firstBlend.g(), 0.5f, 1e-6f);
        EXPECT_NEAR(firstBlend.a(), 0.375f, 1e-6f);

        const osg::Vec4f secondBlend = affector.evaluate(0.7f);
        EXPECT_NEAR(secondBlend.g(), 0.5f, 1e-6f);
        EXPECT_NEAR(secondBlend.b(), 0.5f, 1e-6f);
        EXPECT_NEAR(secondBlend.a(), 0.625f, 1e-6f);
        EXPECT_EQ(affector.evaluate(1.f).a(), 0.f);
    }

    TEST(NifOsgParticleTest, ZeroColorPercentagesUseMiddleColorWithFade)
    {
        Nif::BSPSysSimpleColorModifier modifier;
        modifier.mFadeInPercent = 0.05f;
        modifier.mFadeOutPercent = 0.35f;
        modifier.mColor1EndPercent = 0.f;
        modifier.mColor1StartPercent = 0.f;
        modifier.mColor2EndPercent = 0.f;
        modifier.mColor2StartPercent = 0.f;
        modifier.mColors = { osg::Vec4f(1.f, 0.f, 0.f, 0.f), osg::Vec4f(0.4f, 0.5f, 0.6f, 0.6f),
            osg::Vec4f(0.f, 0.f, 1.f, 0.f) };

        const NifOsg::BethesdaParticleColorAffector affector(&modifier);
        EXPECT_EQ(affector.evaluate(0.2f), modifier.mColors[1]);
        EXPECT_NEAR(affector.evaluate(0.025f).a(), 0.3f, 1e-6f);
        EXPECT_NEAR(affector.evaluate(0.675f).a(), 0.3f, 1e-6f);
    }
}
