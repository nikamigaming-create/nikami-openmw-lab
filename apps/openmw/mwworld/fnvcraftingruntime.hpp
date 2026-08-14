#ifndef OPENMW_MWWORLD_FNVCRAFTINGRUNTIME_H
#define OPENMW_MWWORLD_FNVCRAFTINGRUNTIME_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/esm/refid.hpp>

#include "ptr.hpp"

namespace ESM4
{
    struct Activator;
    struct Recipe;
    struct RecipeCategory;
}

namespace MWWorld
{
    class ContainerStore;
    class ESMStore;
    enum class ESM4Game;
    class FalloutPlayerRuntimeState;

    enum class FnvCraftingPreparationError
    {
        None,
        NotFalloutNewVegas,
        MissingStore,
        MissingStation,
        StationNotInStore,
        UnsupportedStation,
        MissingStationCategory,
        CategoryNotInStore,
        StationCategoryMismatch,
        MissingRecipe,
        RecipeNotInStore,
        DeletedRecord,
        ConditionalRecipe,
        RecipeCategoryMismatch,
        MissingSubCategory,
        SubCategoryNotInStore,
        MissingActor,
        ActorIsNotPlayer,
        MissingInventory,
        InventoryMismatch,
        PlayerStateUninitialized,
        PlayerStateMismatch,
        InvalidNoSkillGate,
        UnsupportedSkill,
        UnresolvedSkillOffset,
        InsufficientSkill,
        MissingItem,
        UnsupportedCurrency,
        UnsupportedItemType,
        ScriptedItem,
        InvalidQuantity,
        QuantityOverflow,
        InvalidInventory,
        InsufficientIngredients,
        OutputConstructionFailed,
    };

    enum class FnvCraftingCommitResult
    {
        Applied,
        InvalidPlan,
        ActorOrInventoryChanged,
        InventoryChanged,
        MutationFailed,
    };

    struct FnvCraftingItemDelta
    {
        ESM::RefId mItem;
        int mQuantity = 0;

        bool operator==(const FnvCraftingItemDelta&) const = default;
    };

    struct FnvCraftingCatalogSource
    {
        ESM4Game mGame;
        const ESMStore* mStore = nullptr;
        const ESM4::Activator* mStation = nullptr;
    };

    class FnvCraftingCatalogBuilder;

    class PreparedFnvCraftingCatalogItem final
    {
        const FnvCraftingItemDelta mDelta;
        const std::string mName;

        PreparedFnvCraftingCatalogItem(FnvCraftingItemDelta delta, std::string name);

        friend class FnvCraftingCatalogBuilder;

    public:
        const FnvCraftingItemDelta& getDelta() const { return mDelta; }
        std::string_view getName() const { return mName; }
    };

    class PreparedFnvCraftingCatalogEntry final
    {
        const ESM::FormId mRecipe;
        const std::string mName;
        const ESM::FormId mSubCategory;
        const std::string mSubCategoryName;
        const std::int32_t mRequiredSkill;
        const std::uint32_t mRequiredSkillLevel;
        const std::vector<PreparedFnvCraftingCatalogItem> mIngredients;
        const std::vector<PreparedFnvCraftingCatalogItem> mOutputs;
        const FnvCraftingPreparationError mStaticBlocker;

        PreparedFnvCraftingCatalogEntry(ESM::FormId recipe, std::string name, ESM::FormId subCategory,
            std::string subCategoryName, std::int32_t requiredSkill, std::uint32_t requiredSkillLevel,
            std::vector<PreparedFnvCraftingCatalogItem> ingredients,
            std::vector<PreparedFnvCraftingCatalogItem> outputs, FnvCraftingPreparationError staticBlocker);

        friend class FnvCraftingCatalogBuilder;

    public:
        ESM::FormId getRecipe() const { return mRecipe; }
        std::string_view getName() const { return mName; }
        ESM::FormId getSubCategory() const { return mSubCategory; }
        std::string_view getSubCategoryName() const { return mSubCategoryName; }
        std::int32_t getRequiredSkill() const { return mRequiredSkill; }
        std::uint32_t getRequiredSkillLevel() const { return mRequiredSkillLevel; }
        const std::vector<PreparedFnvCraftingCatalogItem>& getIngredients() const { return mIngredients; }
        const std::vector<PreparedFnvCraftingCatalogItem>& getOutputs() const { return mOutputs; }
        FnvCraftingPreparationError getStaticBlocker() const { return mStaticBlocker; }
        bool isStaticallySupported() const { return mStaticBlocker == FnvCraftingPreparationError::None; }
    };

    /// Immutable station menu snapshot. Every winning, live recipe in the
    /// station category is retained; unsupported records carry their exact
    /// fail-closed blocker instead of disappearing from the menu.
    class PreparedFnvCraftingCatalog final
    {
        const ESM::FormId mStation;
        const ESM::FormId mCategory;
        const std::string mCategoryName;
        const std::vector<PreparedFnvCraftingCatalogEntry> mEntries;

        PreparedFnvCraftingCatalog(ESM::FormId station, ESM::FormId category, std::string categoryName,
            std::vector<PreparedFnvCraftingCatalogEntry> entries);

        friend class FnvCraftingCatalogBuilder;

    public:
        ESM::FormId getStation() const { return mStation; }
        ESM::FormId getCategory() const { return mCategory; }
        std::string_view getCategoryName() const { return mCategoryName; }
        const std::vector<PreparedFnvCraftingCatalogEntry>& getEntries() const { return mEntries; }
    };

    /// Return a category only for the two frozen FNV base/script pairs.
    [[nodiscard]] std::optional<ESM::FormId> getFnvCraftingStationCategory(const ESM4::Activator& station);

    /// Additionally resolve the strict canonical player.showrecipemenu wrapper from the station's authored SCPT.
    /// Scripts with extra gameplay gates or ambiguous/missing RCCT editor IDs remain unsupported.
    [[nodiscard]] std::optional<ESM::FormId> getFnvCraftingStationCategory(
        const ESMStore& store, const ESM4::Activator& station);

    /// Freeze the complete live recipe catalog before opening UI. This does
    /// not inspect inventory, construct outputs, or create a mutation plan.
    [[nodiscard]] std::optional<PreparedFnvCraftingCatalog> prepareFnvCraftingCatalog(
        const FnvCraftingCatalogSource& source, FnvCraftingPreparationError* error = nullptr);

    /// Narrow mutation boundary used by the headless crafting planner.
    ///
    /// Implementations must not clone an InventoryStore. prepareForActor may
    /// resolve the live store, but it must not consume or create recipe items.
    class FnvCraftingInventory
    {
    public:
        virtual ~FnvCraftingInventory() = default;

        virtual bool prepareForActor(const Ptr& actor) noexcept = 0;
        virtual bool stillBelongsTo(const Ptr& actor) const noexcept = 0;
        virtual std::optional<std::int64_t> getCount(const ESM::RefId& item) const noexcept = 0;
        virtual bool remove(const ESM::RefId& item, int count) noexcept = 0;
        virtual bool add(const Ptr& item, int count, bool allowAutoEquip) noexcept = 0;
    };

    /// Production adapter over the one live ContainerStore. In particular,
    /// this never uses InventoryStore::clone(), whose copied stacks would be
    /// registered as additional global pointers.
    class ContainerStoreCraftingInventory final : public FnvCraftingInventory
    {
        ContainerStore* mStore;

    public:
        explicit ContainerStoreCraftingInventory(ContainerStore& store);

        bool prepareForActor(const Ptr& actor) noexcept override;
        bool stillBelongsTo(const Ptr& actor) const noexcept override;
        std::optional<std::int64_t> getCount(const ESM::RefId& item) const noexcept override;
        bool remove(const ESM::RefId& item, int count) noexcept override;
        bool add(const Ptr& item, int count, bool allowAutoEquip) noexcept override;
    };

    struct FnvCraftingTransactionSource
    {
        ESM4Game mGame;
        const ESMStore* mStore = nullptr;
        const ESM4::Activator* mStation = nullptr;
        const ESM4::RecipeCategory* mStationCategory = nullptr;
        const ESM4::Recipe* mRecipe = nullptr;
        Ptr mActor;
        Ptr mPlayer;
        FnvCraftingInventory* mInventory = nullptr;
        const FalloutPlayerRuntimeState* mPlayerState = nullptr;
    };

    class FnvCraftingPlanBuilder;

    /// Complete fail-closed preflight for one conditionless recipe. The plan
    /// owns every constructed output until commit and is consumed by the
    /// commit function, so the same plan cannot apply twice.
    class PreparedFnvCraftingPlan final
    {
        struct Impl;
        std::unique_ptr<Impl> mImpl;

        explicit PreparedFnvCraftingPlan(std::unique_ptr<Impl> impl);

        friend class FnvCraftingPlanBuilder;
        friend std::optional<PreparedFnvCraftingPlan> prepareFnvCraftingTransaction(
            const FnvCraftingTransactionSource& source, FnvCraftingPreparationError* error);
        friend FnvCraftingCommitResult commitFnvCraftingTransaction(PreparedFnvCraftingPlan&& plan) noexcept;

    public:
        ~PreparedFnvCraftingPlan();
        PreparedFnvCraftingPlan(PreparedFnvCraftingPlan&&) noexcept;
        PreparedFnvCraftingPlan& operator=(PreparedFnvCraftingPlan&&) noexcept;
        PreparedFnvCraftingPlan(const PreparedFnvCraftingPlan&) = delete;
        PreparedFnvCraftingPlan& operator=(const PreparedFnvCraftingPlan&) = delete;

        ESM::FormId getStation() const;
        ESM::FormId getCategory() const;
        ESM::FormId getRecipe() const;
        const std::vector<FnvCraftingItemDelta>& getIngredients() const;
        const std::vector<FnvCraftingItemDelta>& getOutputs() const;
    };

    /// Preflight station mapping, category, player/skill state, every record
    /// and signed count, inventory sufficiency/final-count overflow, and all
    /// output construction before returning a plan.
    [[nodiscard]] std::optional<PreparedFnvCraftingPlan> prepareFnvCraftingTransaction(
        const FnvCraftingTransactionSource& source, FnvCraftingPreparationError* error = nullptr);

    /// Consume a prepared plan. The live snapshot is checked again before
    /// all ingredients are removed; only then are authored outputs added,
    /// once each, with auto-equip disabled. This preserves remove-then-add
    /// semantics when an item occurs on both sides of a recipe.
    [[nodiscard]] FnvCraftingCommitResult commitFnvCraftingTransaction(PreparedFnvCraftingPlan&& plan) noexcept;

    std::string_view getFnvCraftingPreparationErrorName(FnvCraftingPreparationError error);
    std::string_view getFnvCraftingCommitResultName(FnvCraftingCommitResult result);
}

#endif
