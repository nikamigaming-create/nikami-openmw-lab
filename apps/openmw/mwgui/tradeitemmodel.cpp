#include "tradeitemmodel.hpp"

#include <limits>

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
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/settings/values.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

namespace
{
    bool isFlatFalloutItemType(unsigned int type)
    {
        return type == ESM4::Ammunition::sRecordId || type == ESM4::Armor::sRecordId
            || type == ESM4::MiscItem::sRecordId || type == ESM4::Weapon::sRecordId
            || type == ESM4::Potion::sRecordId || type == ESM4::Book::sRecordId
            || type == ESM4::Clothing::sRecordId || type == ESM4::Ingredient::sRecordId
            || type == ESM4::ItemMod::sRecordId || type == ESM4::Key::sRecordId
            || type == ESM4::Light::sRecordId;
    }
}

namespace MWGui
{

    bool isFlatFalloutMerchant(const MWWorld::ConstPtr& merchant)
    {
        if (merchant.isEmpty() || merchant.getType() != ESM4::Npc::sRecordId)
            return false;
        const MWWorld::LiveCellRef<ESM4::Npc>* reference = merchant.get<ESM4::Npc>();
        return reference != nullptr && reference->mBase != nullptr && reference->mBase->mIsFONV;
    }

    bool isItemAcceptedForBarter(
        const MWWorld::ConstPtr& item, const MWWorld::ConstPtr& merchant, int merchantServices)
    {
        if (item.isEmpty())
            return false;
        if (isFlatFalloutMerchant(merchant))
            return isFlatFalloutItemType(item.getType());
        return item.getClass().canSell(item, merchantServices);
    }

    ESM::RefId findFlatFalloutCurrency(const MWWorld::ESMStore& store)
    {
        for (const ESM4::MiscItem& item : store.get<ESM4::MiscItem>())
        {
            if (Misc::StringUtils::ciEqual(item.mEditorId, "Caps001"))
                return ESM::RefId(item.mId);
        }
        return {};
    }

    int countBarterCurrency(const std::vector<MWWorld::Ptr>& sources, const ESM::RefId& currency)
    {
        if (currency.empty())
            return 0;

        int total = 0;
        for (const MWWorld::Ptr& source : sources)
        {
            const int count = source.getClass().getContainerStore(source).count(currency);
            if (count >= std::numeric_limits<int>::max() - total)
                return std::numeric_limits<int>::max();
            total += count;
        }
        return total;
    }

    bool transferBarterCurrency(const std::vector<MWWorld::Ptr>& from, const std::vector<MWWorld::Ptr>& to,
        const ESM::RefId& currency, int amount)
    {
        if (amount < 0 || currency.empty() || from.empty() || to.empty())
            return false;
        if (amount == 0)
            return true;
        if (countBarterCurrency(from, currency) < amount)
            return false;

        MWWorld::ContainerStore& target = to.front().getClass().getContainerStore(to.front());
        target.add(currency, amount, false);

        int remaining = amount;
        for (const MWWorld::Ptr& source : from)
        {
            MWWorld::ContainerStore& store = source.getClass().getContainerStore(source);
            remaining -= store.remove(currency, remaining);
            if (remaining == 0)
                return true;
        }

        // The count preflight and removal use the same stores, so this is only an integrity guard.
        target.remove(currency, amount);
        return false;
    }

    TradeItemModel::TradeItemModel(
        std::unique_ptr<ItemModel> sourceModel, const MWWorld::Ptr& merchant, const ESM::RefId& currency)
        : mMerchant(merchant)
        , mCurrency(currency.empty() ? MWWorld::ContainerStore::sGoldId : currency)
    {
        mSourceModel = std::move(sourceModel);
    }

    void TradeItemModel::setMerchant(const MWWorld::Ptr& merchant, const ESM::RefId& currency)
    {
        mMerchant = merchant;
        mCurrency = currency.empty() ? MWWorld::ContainerStore::sGoldId : currency;
    }

    bool TradeItemModel::allowedToUseItems() const
    {
        return true;
    }

    ItemStack TradeItemModel::getItem(ModelIndex index)
    {
        if (index < 0)
            throw std::runtime_error("Invalid index supplied");
        if (mItems.size() <= static_cast<size_t>(index))
            throw std::runtime_error("Item index out of range");
        return mItems[index];
    }

    size_t TradeItemModel::getItemCount()
    {
        return mItems.size();
    }

    void TradeItemModel::borrowImpl(const ItemStack& item, std::vector<ItemStack>& out)
    {
        bool found = false;
        for (ItemStack& itemStack : out)
        {
            if (itemStack.mBase == item.mBase)
            {
                itemStack.mCount += item.mCount;
                found = true;
                break;
            }
        }
        if (!found)
            out.push_back(item);
    }

    void TradeItemModel::unborrowImpl(const ItemStack& item, size_t count, std::vector<ItemStack>& out)
    {
        std::vector<ItemStack>::iterator it = out.begin();
        bool found = false;
        for (; it != out.end(); ++it)
        {
            if (it->mBase == item.mBase)
            {
                if (it->mCount < count)
                    throw std::runtime_error("Not enough borrowed items to return");
                it->mCount -= count;
                if (it->mCount == 0)
                    out.erase(it);
                found = true;
                break;
            }
        }
        if (!found)
            throw std::runtime_error("Can't find borrowed item to return");
    }

    void TradeItemModel::borrowItemFromUs(ModelIndex itemIndex, size_t count)
    {
        ItemStack item = getItem(itemIndex);
        item.mCount = count;
        borrowImpl(item, mBorrowedFromUs);
    }

    void TradeItemModel::borrowItemToUs(ModelIndex itemIndex, ItemModel* source, size_t count)
    {
        ItemStack item = source->getItem(itemIndex);
        item.mCount = count;
        borrowImpl(item, mBorrowedToUs);
    }

    void TradeItemModel::returnItemBorrowedToUs(ModelIndex itemIndex, size_t count)
    {
        ItemStack item = getItem(itemIndex);
        unborrowImpl(item, count, mBorrowedToUs);
    }

    void TradeItemModel::returnItemBorrowedFromUs(ModelIndex itemIndex, ItemModel* source, size_t count)
    {
        ItemStack item = source->getItem(itemIndex);
        unborrowImpl(item, count, mBorrowedFromUs);
    }

    void TradeItemModel::adjustEncumbrance(float& encumbrance)
    {
        for (ItemStack& itemStack : mBorrowedToUs)
        {
            MWWorld::Ptr& item = itemStack.mBase;
            encumbrance += item.getClass().getWeight(item) * itemStack.mCount;
        }
        for (ItemStack& itemStack : mBorrowedFromUs)
        {
            MWWorld::Ptr& item = itemStack.mBase;
            encumbrance -= item.getClass().getWeight(item) * itemStack.mCount;
        }
        encumbrance = std::max(0.f, encumbrance);
    }

    void TradeItemModel::updateBorrowed()
    {
        auto update = [](std::vector<ItemStack>& list) {
            for (auto it = list.begin(); it != list.end();)
            {
                size_t actualCount = it->mBase.getCellRef().getCount();
                if (actualCount < it->mCount)
                    it->mCount = actualCount;
                if (it->mCount == 0)
                    it = list.erase(it);
                else
                    ++it;
            }
        };

        update(mBorrowedFromUs);
        update(mBorrowedToUs);
    }

    void TradeItemModel::abort()
    {
        mBorrowedFromUs.clear();
        mBorrowedToUs.clear();
    }

    const std::vector<ItemStack> TradeItemModel::getItemsBorrowedToUs() const
    {
        return mBorrowedToUs;
    }

    void TradeItemModel::transferItems()
    {
        for (ItemStack& itemStack : mBorrowedToUs)
        {
            // get index in the source model
            ItemModel* sourceModel = itemStack.mCreator;
            size_t i = 0;
            for (; i < sourceModel->getItemCount(); ++i)
            {
                if (itemStack.mBase == sourceModel->getItem(i).mBase)
                    break;
            }
            if (i == sourceModel->getItemCount())
                throw std::runtime_error("The borrowed item disappeared");

            sourceModel->moveItem(
                sourceModel->getItem(i), itemStack.mCount, this, !Settings::game().mPreventMerchantEquipping);
        }
        mBorrowedToUs.clear();
        mBorrowedFromUs.clear();
    }

    void TradeItemModel::update()
    {
        mSourceModel->update();

        int services = 0;
        if (!mMerchant.isEmpty())
            services = mMerchant.getClass().getServices(mMerchant);

        mItems.clear();
        // add regular items
        for (size_t i = 0; i < mSourceModel->getItemCount(); ++i)
        {
            ItemStack item = mSourceModel->getItem(i);
            if (!mMerchant.isEmpty())
            {
                MWWorld::Ptr base = item.mBase;
                if (base.getCellRef().getRefId() == mCurrency)
                    continue;

                if (!base.getClass().showsInInventory(base))
                    continue;

                if (!isItemAcceptedForBarter(base, mMerchant, services))
                    continue;

                // Bound items may not be bought
                if (item.mFlags & ItemStack::Flag_Bound)
                    continue;

                // don't show equipped items
                if (mMerchant.getClass().hasInventoryStore(mMerchant))
                {
                    MWWorld::InventoryStore& store = mMerchant.getClass().getInventoryStore(mMerchant);
                    if (store.isEquipped(base))
                        continue;
                }
            }

            // don't show items that we borrowed to someone else
            for (ItemStack& itemStack : mBorrowedFromUs)
            {
                if (itemStack.mBase == item.mBase)
                {
                    if (item.mCount < itemStack.mCount)
                        throw std::runtime_error("Lent more items than present");
                    item.mCount -= itemStack.mCount;
                }
            }

            if (item.mCount > 0)
                mItems.push_back(item);
        }

        // add items borrowed to us
        for (ItemStack& itemStack : mBorrowedToUs)
        {
            itemStack.mType = ItemStack::Type_Barter;
            mItems.push_back(itemStack);
        }
    }

}
