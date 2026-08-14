#ifndef MWGUI_TRADE_ITEM_MODEL_H
#define MWGUI_TRADE_ITEM_MODEL_H

#include "itemmodel.hpp"

namespace MWWorld
{
    class ESMStore;
}

namespace MWGui
{

    class ItemModel;

    bool isFlatFalloutMerchant(const MWWorld::ConstPtr& merchant);
    bool isItemAcceptedForBarter(
        const MWWorld::ConstPtr& item, const MWWorld::ConstPtr& merchant, int merchantServices);
    ESM::RefId findFlatFalloutCurrency(const MWWorld::ESMStore& store);
    int countBarterCurrency(const std::vector<MWWorld::Ptr>& sources, const ESM::RefId& currency);
    bool transferBarterCurrency(const std::vector<MWWorld::Ptr>& from, const std::vector<MWWorld::Ptr>& to,
        const ESM::RefId& currency, int amount);

    /// @brief An item model that allows 'borrowing' items from another item model. Used for previewing barter offers.
    /// Also filters items that the merchant does not sell.
    class TradeItemModel : public ProxyItemModel
    {
    public:
        TradeItemModel(std::unique_ptr<ItemModel> sourceModel, const MWWorld::Ptr& merchant,
            const ESM::RefId& currency = {});

        void setMerchant(const MWWorld::Ptr& merchant, const ESM::RefId& currency = {});

        bool allowedToUseItems() const override;

        ItemStack getItem(ModelIndex index) override;
        size_t getItemCount() override;

        void update() override;

        void borrowItemFromUs(ModelIndex itemIndex, size_t count);

        void borrowItemToUs(ModelIndex itemIndex, ItemModel* source, size_t count);
        ///< @note itemIndex points to an item in \a source

        void returnItemBorrowedToUs(ModelIndex itemIndex, size_t count);

        void returnItemBorrowedFromUs(ModelIndex itemIndex, ItemModel* source, size_t count);

        /// Update borrowed items in this model
        void updateBorrowed();

        /// Permanently transfers items that were borrowed to us from another model to this model
        void transferItems();
        /// Aborts trade
        void abort();

        /// Adjusts the given encumbrance by adding weight for items that have been lent to us,
        /// and removing weight for items we've lent to someone else.
        void adjustEncumbrance(float& encumbrance);

        const std::vector<ItemStack> getItemsBorrowedToUs() const;

    private:
        void borrowImpl(const ItemStack& item, std::vector<ItemStack>& out);
        void unborrowImpl(const ItemStack& item, size_t count, std::vector<ItemStack>& out);

        std::vector<ItemStack> mItems;

        std::vector<ItemStack> mBorrowedToUs;
        std::vector<ItemStack> mBorrowedFromUs;

        MWWorld::Ptr mMerchant;
        ESM::RefId mCurrency;
    };

}

#endif
