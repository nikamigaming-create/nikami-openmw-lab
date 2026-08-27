#include "apps/openmw/mwclass/classes.hpp"
#include "apps/openmw/mwworld/class.hpp"
#include "apps/openmw/mwworld/containerstore.hpp"
#include "apps/openmw/mwworld/livecellref.hpp"

#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadkeym.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadweap.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <set>

namespace MWWorld
{
    namespace
    {
        constexpr std::uint32_t kAmmunitionId = 0x01030001;
        constexpr std::uint32_t kArmorId = 0x01030002;
        constexpr std::uint32_t kMiscellaneousId = 0x01030003;
        constexpr std::uint32_t kWeaponId = 0x01030004;
        constexpr std::uint32_t kPotionId = 0x01030005;
        constexpr std::uint32_t kBookId = 0x01030006;
        constexpr std::uint32_t kClothingId = 0x01030007;
        constexpr std::uint32_t kIngredientId = 0x01030008;
        constexpr std::uint32_t kItemModId = 0x01030009;
        constexpr std::uint32_t kKeyId = 0x0103000a;
        constexpr std::uint32_t kLightId = 0x0103000b;

        class TestContainerStore final : public ContainerStore
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

            ammunition.mData.mWeight = 1.f;
            armor.mData.weight = 2.f;
            miscellaneous.mData.weight = 3.f;
            weapon.mData.weight = 4.f;
            potion.mData.weight = 5.f;
            book.mData.weight = 6.f;
            clothing.mData.weight = 7.f;
            ingredient.mData.weight = 8.f;
            itemMod.mData.mWeight = 9.f;
            key.mData.weight = 10.f;
            light.mData.weight = 11.f;

            TestContainerStore store;
            store.addRecord(ammunition, kAmmunitionId, 1);
            store.addRecord(armor, kArmorId, 2);
            store.addRecord(miscellaneous, kMiscellaneousId, 3);
            store.addRecord(weapon, kWeaponId, 4);
            store.addRecord(potion, kPotionId, 5);
            store.addRecord(book, kBookId, 6);
            store.addRecord(clothing, kClothingId, 7);
            store.addRecord(ingredient, kIngredientId, 8);
            store.addRecord(itemMod, kItemModId, 9);
            store.addRecord(key, kKeyId, 10);
            store.addRecord(light, kLightId, 11);

            std::set<unsigned int> recordTypes;
            int totalCount = 0;
            for (const ConstPtr item : store)
            {
                recordTypes.insert(item.getType());
                totalCount += item.getCellRef().getCount();
            }

            EXPECT_EQ(recordTypes.size(), 11u);
            EXPECT_EQ(totalCount, 66);
            EXPECT_FLOAT_EQ(store.getWeight(), 506.f);
            EXPECT_EQ(store.count(ESM::RefId::formIdRefId(ESM::FormId::fromUint32(kAmmunitionId))), 1);
            EXPECT_EQ(store.count(ESM::RefId::formIdRefId(ESM::FormId::fromUint32(kKeyId))), 10);
        }

        TEST(ESM4ContainerStoreTest, authoredWeaponAndArmorHealthUsesNativeClassContract)
        {
            MWClass::registerClasses();

            ESM4::Weapon weapon;
            weapon.mData.health = 500;
            ESM::CellRef weaponCell = ESM::makeBlankCellRef();
            LiveCellRef<ESM4::Weapon> weaponRef(weaponCell, &weapon);
            const ConstPtr weaponPtr(&weaponRef);

            EXPECT_TRUE(weaponPtr.getClass().hasItemHealth(weaponPtr));
            EXPECT_EQ(weaponPtr.getClass().getItemMaxHealth(weaponPtr), 500);

            ESM4::Armor armor;
            armor.mData.health = 275;
            ESM::CellRef armorCell = ESM::makeBlankCellRef();
            LiveCellRef<ESM4::Armor> armorRef(armorCell, &armor);
            const ConstPtr armorPtr(&armorRef);

            EXPECT_TRUE(armorPtr.getClass().hasItemHealth(armorPtr));
            EXPECT_EQ(armorPtr.getClass().getItemMaxHealth(armorPtr), 275);

            ESM4::MiscItem misc;
            ESM::CellRef miscCell = ESM::makeBlankCellRef();
            LiveCellRef<ESM4::MiscItem> miscRef(miscCell, &misc);
            const ConstPtr miscPtr(&miscRef);
            EXPECT_FALSE(miscPtr.getClass().hasItemHealth(miscPtr));
        }
    }
}
