#include <gtest/gtest.h>

#include <components/esm4/loadligh.hpp>

#include <apps/openmw/mwclass/light4.hpp>

namespace MWClass
{
    TEST(Esm4LightTest, PlaysAuthoredLoopWhenEnabledByDefault)
    {
        ESM4::Light light;
        light.mSound = ESM::FormId(0x1234);

        EXPECT_TRUE(shouldPlayEsm4LightLoop(light));
    }

    TEST(Esm4LightTest, DoesNotPlayWithoutAuthoredSound)
    {
        ESM4::Light light;

        EXPECT_FALSE(shouldPlayEsm4LightLoop(light));
    }

    TEST(Esm4LightTest, DoesNotPlayWhileOffByDefault)
    {
        ESM4::Light light;
        light.mSound = ESM::FormId(0x1234);
        light.mData.flags = ESM4::Light::OffDefault;

        EXPECT_FALSE(shouldPlayEsm4LightLoop(light));
    }
}
