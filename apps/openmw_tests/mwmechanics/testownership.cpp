#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include <components/esm/formid.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadrefr.hpp>

#include "apps/openmw/mwbase/environment.hpp"

#include "apps/openmw/mwmechanics/ownership.hpp"

#include "apps/openmw/mwworld/cell.hpp"
#include "apps/openmw/mwworld/cellref.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    constexpr std::uint32_t sGoodspringsFaction = 0x00104c6e;
    constexpr std::uint32_t sChet = 0x00104c78;
    constexpr std::uint32_t sTrudy = 0x00104c6c;
    constexpr std::uint32_t sSettler = 0x00104f02;

    ESM::FormId formId(std::uint32_t value)
    {
        return ESM::FormId::fromUint32(value);
    }

    ESM::RefId refId(std::uint32_t value)
    {
        return ESM::RefId(formId(value));
    }

    ESM4::ActorFaction membership(std::uint32_t faction, std::int8_t rank)
    {
        return ESM4::ActorFaction{ faction, rank, 0, 0, 0 };
    }

    void populateStore(MWWorld::ESMStore& store)
    {
        ESM4::Faction faction{};
        faction.mId = formId(sGoodspringsFaction);
        faction.mEditorId = "GoodspringsFaction";
        store.overrideRecord(faction);
        store.setUp();
    }

    class ObjectOwnershipTest : public testing::Test
    {
    protected:
        MWBase::Environment mEnvironment;
        MWWorld::ESMStore mStore;

        void SetUp() override
        {
            mEnvironment.setESMStore(mStore);
            populateStore(mStore);
        }
    };

    TEST_F(ObjectOwnershipTest, InheritsExactGoodspringsCellOwnersAndClassifiesFacts)
    {
        ESM4::Reference reference{};
        MWWorld::CellRef cellRef(reference);

        struct Case
        {
            std::uint32_t mCell;
            std::uint32_t mOwner;
            bool mFaction;
        };
        constexpr std::array cases{
            Case{ 0x00105659, sGoodspringsFaction, true }, // GSHouseInterior01
            Case{ 0x001055e1, sGoodspringsFaction, true }, // GSHouseInterior02
            Case{ 0x00105cf7, sGoodspringsFaction, true }, // GSHouseInterior04
            Case{ 0x001057f1, sChet, false }, // GSGeneralStore
            Case{ 0x00106185, sTrudy, false }, // GSProspectorSaloonInterior
        };

        for (const Case& value : cases)
        {
            SCOPED_TRACE(testing::Message() << "cell=" << std::hex << value.mCell);
            ESM4::Cell record{};
            record.mId = refId(value.mCell);
            record.mOwner = formId(value.mOwner);
            record.mCellFlags = ESM4::CELL_Interior;
            MWWorld::Cell cell(record);

            const MWMechanics::ObjectOwnership ownership
                = MWMechanics::resolveObjectOwnership(cellRef, &cell, mStore);
            EXPECT_EQ(ownership.mOwner, value.mFaction ? ESM::RefId{} : refId(value.mOwner));
            EXPECT_EQ(ownership.mFaction, value.mFaction ? refId(value.mOwner) : ESM::RefId{});
            EXPECT_EQ(ownership.mFactionRank, -1);
        }
    }

    TEST_F(ObjectOwnershipTest, DirectXownPrecedesCellXownAndRetainsFactionRank)
    {
        ESM4::Cell record{};
        record.mId = refId(0x001055e1);
        record.mOwner = formId(sChet);
        record.mCellFlags = ESM4::CELL_Interior;
        MWWorld::Cell cell(record);

        ESM4::Reference factionReference{};
        factionReference.mOwner = formId(sGoodspringsFaction);
        factionReference.mFactionRank = 2;
        const MWMechanics::ObjectOwnership factionOwnership
            = MWMechanics::resolveObjectOwnership(MWWorld::CellRef(factionReference), &cell, mStore);
        EXPECT_TRUE(factionOwnership.mOwner.empty());
        EXPECT_EQ(factionOwnership.mFaction, refId(sGoodspringsFaction));
        EXPECT_EQ(factionOwnership.mFactionRank, 2);

        record.mOwner = formId(sGoodspringsFaction);
        MWWorld::Cell factionCell(record);
        ESM4::Reference actorReference{};
        actorReference.mOwner = formId(sSettler);
        actorReference.mFactionRank = 3;
        const MWMechanics::ObjectOwnership actorOwnership
            = MWMechanics::resolveObjectOwnership(MWWorld::CellRef(actorReference), &factionCell, mStore);
        EXPECT_EQ(actorOwnership.mOwner, refId(sSettler));
        EXPECT_TRUE(actorOwnership.mFaction.empty());
        EXPECT_EQ(actorOwnership.mFactionRank, 3);
    }

    TEST_F(ObjectOwnershipTest, AppliesAuthoredFalloutFactionRankRequirement)
    {
        MWMechanics::ObjectOwnership ownership;
        ownership.mFaction = refId(sGoodspringsFaction);
        ownership.mFactionRank = 2;

        const std::array exact{ membership(sGoodspringsFaction, 2) };
        const std::array higher{ membership(sGoodspringsFaction, 3) };
        const std::array lower{ membership(sGoodspringsFaction, 1) };
        const std::array different{ membership(0x00104c70, 4) };
        EXPECT_TRUE(MWMechanics::isFactionOwnershipAllowed(ownership, exact));
        EXPECT_TRUE(MWMechanics::isFactionOwnershipAllowed(ownership, higher));
        EXPECT_FALSE(MWMechanics::isFactionOwnershipAllowed(ownership, lower));
        EXPECT_FALSE(MWMechanics::isFactionOwnershipAllowed(ownership, different));

        ownership.mFactionRank = -1;
        EXPECT_TRUE(MWMechanics::isFactionOwnershipAllowed(ownership, lower));
    }
}
