#include "apps/openmw/mwworld/containerstore.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"
#include "apps/openmw/mwclass/classes.hpp"

#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <set>

namespace MWWorld
{
    namespace
    {
        class TestContainerStore : public ContainerStore
        {
        public:
            template <class Record>
            void addRecord(Record& record, std::uint32_t formId, int count)
            {
                record.mId = ESM::FormId::fromUint32(formId);
                ESM::CellRef cellRef = ESM::makeBlankCellRef();
                cellRef.mRefID = ESM::RefId::formIdRefId(record.mId);
                LiveCellRef<Record> liveRef(cellRef, &record);
                const ContainerStoreIterator inserted = addNewStack(Ptr(&liveRef), count);
                ASSERT_NE(inserted, end());
                EXPECT_EQ(inserted->getCellRef().getCount(), count);
                EXPECT_EQ(inserted->getType(), Record::sRecordId);
            }
        };

        TEST(ESM4ContainerStoreTest, allRegisteredInventoryRecordsAreStorable)
        {
            static_assert(ContainerStore::isStorableType<ESM4::Ammunition>());
            static_assert(ContainerStore::isStorableType<ESM4::Armor>());
            static_assert(ContainerStore::isStorableType<ESM4::MiscItem>());
            static_assert(ContainerStore::isStorableType<ESM4::Weapon>());
            static_assert(ContainerStore::isStorableType<ESM4::Potion>());
            static_assert(ContainerStore::isStorableType<ESM4::Book>());
            static_assert(ContainerStore::isStorableType<ESM4::Clothing>());
            static_assert(ContainerStore::isStorableType<ESM4::Ingredient>());
            static_assert(ContainerStore::isStorableType<ESM4::ItemMod>());
            static_assert(ContainerStore::isStorableType<ESM4::Key>());
            static_assert(ContainerStore::isStorableType<ESM4::Light>());
        }

        TEST(ESM4ContainerStoreTest, iteratorAndCountsCoverEverySupportedRecord)
        {
            MWClass::registerClasses();

            ESM4::Ammunition ammunition;
            ESM4::Armor armor;
            ESM4::MiscItem miscellaneous;
            ESM4::Weapon weapon;
            ESM4::Potion potion;
            ESM4::Book book;
            ESM4::Clothing clothing;
            ESM4::Ingredient ingredient;
            ESM4::ItemMod itemMod;
            ESM4::Key key;
            ESM4::Light light;

            TestContainerStore store;
            store.addRecord(ammunition, 0x01000001, 1);
            store.addRecord(armor, 0x01000002, 2);
            store.addRecord(miscellaneous, 0x01000003, 3);
            store.addRecord(weapon, 0x01000004, 4);
            store.addRecord(potion, 0x01000005, 5);
            store.addRecord(book, 0x01000006, 6);
            store.addRecord(clothing, 0x01000007, 7);
            store.addRecord(ingredient, 0x01000008, 8);
            store.addRecord(itemMod, 0x01000009, 9);
            store.addRecord(key, 0x0100000a, 10);
            store.addRecord(light, 0x0100000b, 11);

            std::set<unsigned int> recordTypes;
            int totalCount = 0;
            for (const ConstPtr item : store)
            {
                recordTypes.insert(item.getType());
                totalCount += item.getCellRef().getCount();
            }

            EXPECT_EQ(recordTypes.size(), 11u);
            EXPECT_EQ(totalCount, 66);
        }
    }
}
