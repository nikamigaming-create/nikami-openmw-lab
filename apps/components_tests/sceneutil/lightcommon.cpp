#include <components/esm4/loadligh.hpp>
#include <components/sceneutil/lightcommon.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(SceneUtilLightCommonTest, PreservesRetailFragExplosionLightRadiusAndHdrColor)
    {
        // FalloutNV.esm LIGH 00014BFF Explosion768: radius 768, RGB F3/E2/9C, neutral flags,
        // and FNAM 2.0. This is the light referenced by EXPL 000179DA GrenadeFragExplosion.
        ESM4::Light light;
        light.mData.time = -1;
        light.mData.radius = 768;
        light.mData.colour = 0x009ce2f3;
        light.mData.flags = 0;
        light.mFade = 2.f;

        const SceneUtil::LightCommon common(light);

        EXPECT_FLOAT_EQ(common.mRadius, 768.f);
        EXPECT_FLOAT_EQ(common.mColor.r(), 2.f * 243.f / 255.f);
        EXPECT_FLOAT_EQ(common.mColor.g(), 2.f * 226.f / 255.f);
        EXPECT_FLOAT_EQ(common.mColor.b(), 2.f * 156.f / 255.f);
        EXPECT_FLOAT_EQ(common.mColor.a(), 1.f);
        EXPECT_FALSE(common.mFlicker);
        EXPECT_FALSE(common.mPulse);
        EXPECT_FALSE(common.mNegative);
        EXPECT_FALSE(common.mOffDefault);
    }
}
