#include <apps/openmw/mwmechanics/ownership.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(OwnershipTest, AllowsPersonalOwnerByBaseOrPlacedReference)
    {
        const ESM::FormId actorBase{ .mIndex = 0x100, .mContentFile = 0 };
        const ESM::FormId actorReference{ .mIndex = 0x101, .mContentFile = 0 };
        MWMechanics::Ownership ownership;

        ownership.mOwner = ESM::RefId(actorBase);
        EXPECT_TRUE(MWMechanics::isOwnershipAllowed(
            ownership, ESM::RefId(actorBase), actorReference, nullptr));

        ownership.mOwner = ESM::RefId(actorReference);
        EXPECT_TRUE(MWMechanics::isOwnershipAllowed(
            ownership, ESM::RefId(actorBase), actorReference, nullptr));

        ownership.mOwner = ESM::RefId(ESM::FormId{ .mIndex = 0x102, .mContentFile = 0 });
        EXPECT_FALSE(MWMechanics::isOwnershipAllowed(
            ownership, ESM::RefId(actorBase), actorReference, nullptr));
    }

    TEST(OwnershipTest, AppliesFactionMembershipAndRequiredRank)
    {
        const ESM::RefId faction(ESM::FormId{ .mIndex = 0x110, .mContentFile = 0 });
        MWMechanics::Ownership ownership;
        ownership.mOwner = faction;
        ownership.mOwnerIsFaction = true;
        ownership.mFaction = faction;
        ownership.mRequiredFactionRank = 2;

        std::map<ESM::RefId, int> factions{ { faction, 1 } };
        EXPECT_FALSE(MWMechanics::isOwnershipAllowed(ownership, {}, {}, &factions));

        factions[faction] = 2;
        EXPECT_TRUE(MWMechanics::isOwnershipAllowed(ownership, {}, {}, &factions));
        EXPECT_FALSE(MWMechanics::isOwnershipAllowed(ownership, {}, {}, nullptr));
    }
}
