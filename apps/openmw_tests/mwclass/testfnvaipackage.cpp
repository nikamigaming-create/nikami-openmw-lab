#include <gtest/gtest.h>

#include <components/esm4/script.hpp>

#include "apps/openmw/mwclass/esm4creature.hpp"

namespace
{
    TEST(FnvAiPackageTest, SupportsExactGoodspringsCreatureFollowProcedures)
    {
        EXPECT_TRUE(MWClass::fnvCreatureAiPackageProcedureSupported(1));
        EXPECT_TRUE(MWClass::fnvCreatureAiPackageProcedureSupported(7));

        // FalloutNV.esm 00152AFA CheyenneAccompany -> Sunny ACHR 00104E85.
        EXPECT_TRUE(MWClass::fnvCreatureFollowTargetSupported(7, 0, 0x00104e85));
        // FalloutNV.esm 00174BAA VSpecialBonnieHornerFollowLeader -> leader ACRE 00174BAC.
        EXPECT_TRUE(MWClass::fnvCreatureFollowTargetSupported(7, 0, 0x00174bac));
    }

    TEST(FnvAiPackageTest, RejectsLinkedReferenceFollowUntilActorPlacementsRetainIt)
    {
        // FalloutNV.esm 00174BAB VSpecialBonnieHornerCalfFollow uses PTDT type 3 with no direct target.
        EXPECT_FALSE(MWClass::fnvCreatureFollowTargetSupported(1, 3, 0));
        EXPECT_FALSE(MWClass::fnvCreatureFollowTargetSupported(7, 0, 0));
    }

    TEST(FnvAiPackageTest, MatchesCheyenneAliveCondition)
    {
        // CheyenneAccompany requires GetDeadCount GSSunnySmiles == 0.
        EXPECT_TRUE(MWClass::fnvCreaturePackageConditionComparisonPasses(
            ESM4::CTF_EqualTo, 0.f, 0.f));
        EXPECT_FALSE(MWClass::fnvCreaturePackageConditionComparisonPasses(
            ESM4::CTF_EqualTo, 1.f, 0.f));
    }
}
