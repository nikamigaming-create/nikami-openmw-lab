#include <apps/openmw/mwclass/esm4npc.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(FnvIdleOnlyPackageTest, RecognisesAuthoredIdleWithoutMovementTargets)
    {
        ESM4::AIPackage package;
        package.mData.type = 6;
        package.mIdleAnim.push_back(ESM::FormId::fromUint32(0x07084439));

        EXPECT_TRUE(MWClass::isFalloutIdleOnlyPackage(package));
    }

    TEST(FnvIdleOnlyPackageTest, RecognisesNearReferenceLocationAsIdleAnchor)
    {
        ESM4::AIPackage package;
        package.mData.type = 6;
        package.mIdleAnim.push_back(ESM::FormId::fromUint32(0x07084439));
        package.mLocation.type = 0;
        package.mLocation.location = ESM::FormId32(0x070290a0);

        EXPECT_TRUE(MWClass::isFalloutIdleOnlyPackage(package));
    }

    TEST(FnvIdleOnlyPackageTest, KeepsExplicitTargetInNativeMovementPipeline)
    {
        ESM4::AIPackage package;
        package.mData.type = 6;
        package.mIdleAnim.push_back(ESM::FormId::fromUint32(0x07084439));
        package.mTarget.type = 0;
        package.mTarget.target = ESM::FormId32(0x070290a0);

        EXPECT_FALSE(MWClass::isFalloutIdleOnlyPackage(package));
    }

    TEST(FnvIdleOnlyPackageTest, DoesNotFabricateBehaviorWithoutAnIdle)
    {
        ESM4::AIPackage package;
        package.mData.type = 6;

        EXPECT_FALSE(MWClass::isFalloutIdleOnlyPackage(package));
    }
}
