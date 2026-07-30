#include <gtest/gtest.h>

#include <components/esm3/cellref.hpp>
#include <components/esm3/objectstate.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadrefr.hpp>

#include "apps/openmw/mwworld/cellref.hpp"

namespace
{
    TEST(MWWorldCellRefTest, ChangesAndClearsEsm4ReferenceOwner)
    {
        const ESM::FormId referenceId{ .mIndex = 0x100, .mContentFile = 0 };
        const ESM::FormId ownerId{ .mIndex = 0x101, .mContentFile = 0 };
        ESM4::Reference source{};
        source.mId = referenceId;

        MWWorld::CellRef reference(source);
        EXPECT_FALSE(reference.hasChanged());

        reference.setOwner(ESM::RefId(ownerId));
        EXPECT_EQ(reference.getOwner(), ESM::RefId(ownerId));
        EXPECT_TRUE(reference.hasChanged());

        ESM::ObjectState state;
        reference.writeState(state);
        EXPECT_EQ(state.mRef.mOwner, ESM::RefId(ownerId));

        reference.setOwner({});
        EXPECT_TRUE(reference.getOwner().empty());
    }

    TEST(MWWorldCellRefTest, ChangesAndPersistsEsm4ActorOwner)
    {
        const ESM::FormId actorId{ .mIndex = 0x110, .mContentFile = 0 };
        const ESM::FormId ownerId{ .mIndex = 0x111, .mContentFile = 0 };
        ESM4::ActorCharacter source{};
        source.mId = actorId;

        MWWorld::CellRef actor(source);
        actor.setOwner(ESM::RefId(ownerId));
        ASSERT_EQ(actor.getOwner(), ESM::RefId(ownerId));
        ASSERT_TRUE(actor.hasChanged());

        ESM::ObjectState state;
        actor.writeState(state);
        ASSERT_EQ(state.mRef.mOwner, ESM::RefId(ownerId));

        ESM4::ActorCharacter restoredSource{};
        restoredSource.mId = actorId;
        MWWorld::CellRef restored(restoredSource);
        restored.loadState(state.mRef);
        EXPECT_EQ(restored.getOwner(), ESM::RefId(ownerId));
    }

    TEST(MWWorldCellRefTest, RetainsEsm3OwnerBehavior)
    {
        ESM::CellRef source;
        source.blank();
        MWWorld::CellRef reference(source);
        const ESM::RefId owner = ESM::RefId::stringRefId("house owner");

        reference.setOwner(owner);

        EXPECT_EQ(reference.getOwner(), owner);
        EXPECT_TRUE(reference.hasChanged());
    }
}
