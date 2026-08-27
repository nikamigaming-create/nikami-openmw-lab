#ifndef OPENMW_MWWORLD_FNVCRAFTINGRUNTIME_H
#define OPENMW_MWWORLD_FNVCRAFTINGRUNTIME_H

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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
    class ESMStore;
    enum class ESM4Game;

    struct FnvCraftingStationRule
    {
        ESM::FormId mStationBase;
        ESM::FormId mStationScript;
        ESM::FormId mCategory;
    };

    /// The skill provider is deliberately injected. Player-save state and
    /// actor-value formulas are separate runtime contracts.
    class FnvCraftingSkillProvider
    {
    public:
        virtual ~FnvCraftingSkillProvider() = default;
        virtual std::optional<double> getSkill(std::uint32_t actorValue) const noexcept = 0;
    };

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
        MissingSkillProvider,
        InvalidNoSkillGate,
        UnsupportedSkill,
        InsufficientSkill,
        MissingItem,
        UnsupportedCurrency,
        UnsupportedItemType,
        ScriptedItem,
        InvalidQuantity,
        QuantityOverflow,
        InvalidInventory,
        InsufficientIngredients,
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

    /// Mutation boundary for the headless planner. Implementations must apply
    /// all deltas or none of them; they must not clone an InventoryStore.
    class FnvCraftingInventory
    {
    public:
        virtual ~FnvCraftingInventory() = default;

        virtual bool prepareForActor(const Ptr& actor) noexcept = 0;
        virtual bool stillBelongsTo(const Ptr& actor) const noexcept = 0;
        virtual std::optional<std::int64_t> getCount(const ESM::RefId& item) const noexcept = 0;
        virtual bool apply(
            std::span<const FnvCraftingItemDelta> ingredients,
            std::span<const FnvCraftingItemDelta> outputs) noexcept = 0;
    };

    struct FnvCraftingTransactionSource
    {
        ESM4Game mGame;
        const ESMStore* mStore = nullptr;
        const ESM4::Activator* mStation = nullptr;
        const ESM4::RecipeCategory* mStationCategory = nullptr;
        const ESM4::Recipe* mRecipe = nullptr;
        std::span<const FnvCraftingStationRule> mStationRules;
        Ptr mActor;
        Ptr mPlayer;
        FnvCraftingInventory* mInventory = nullptr;
        const FnvCraftingSkillProvider* mSkills = nullptr;
    };

    class PreparedFnvCraftingPlan final
    {
        struct Impl;
        std::unique_ptr<Impl> mImpl;

        explicit PreparedFnvCraftingPlan(std::unique_ptr<Impl> impl);

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

    [[nodiscard]] std::optional<PreparedFnvCraftingPlan> prepareFnvCraftingTransaction(
        const FnvCraftingTransactionSource& source, FnvCraftingPreparationError* error = nullptr);

    [[nodiscard]] FnvCraftingCommitResult commitFnvCraftingTransaction(PreparedFnvCraftingPlan&& plan) noexcept;

    std::string_view getFnvCraftingPreparationErrorName(FnvCraftingPreparationError error);
    std::string_view getFnvCraftingCommitResultName(FnvCraftingCommitResult result);
}

#endif
