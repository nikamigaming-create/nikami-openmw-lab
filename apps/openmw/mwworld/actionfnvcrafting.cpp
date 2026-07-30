#include "actionfnvcrafting.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/defs.hpp>
#include <components/esm/refid.hpp>
#include <components/esm4/common.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadrcct.hpp>
#include <components/esm4/loadrcpe.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "class.hpp"
#include "containerstore.hpp"
#include "esmstore.hpp"

namespace
{
    constexpr std::size_t MaxCraftingRedraws = 256;

    class WindowCraftingSessionPresenter final : public MWWorld::FnvCraftingSessionPresenter
    {
    public:
        int show(std::string_view message, const std::vector<std::string>& buttons) override
        {
            MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
            windowManager->interactiveMessageBox(message, buttons, true);
            return windowManager->readPressedButton();
        }
    };

    std::string describeEntry(const MWWorld::PreparedFnvCraftingCatalogEntry& entry)
    {
        std::ostringstream stream;
        stream << entry.getName();
        if (!entry.getSubCategoryName().empty())
            stream << "\nCategory: " << entry.getSubCategoryName();
        if (entry.getRequiredSkill() == -1)
            stream << "\nSkill: none";
        else
            stream << "\nSkill actor value " << entry.getRequiredSkill() << ": " << entry.getRequiredSkillLevel();

        stream << "\n\nIngredients:";
        for (const MWWorld::PreparedFnvCraftingCatalogItem& item : entry.getIngredients())
            stream << "\n  " << item.getDelta().mQuantity << " x " << item.getName();
        stream << "\n\nOutputs:";
        for (const MWWorld::PreparedFnvCraftingCatalogItem& item : entry.getOutputs())
            stream << "\n  " << item.getDelta().mQuantity << " x " << item.getName();
        if (!entry.isStaticallySupported())
        {
            stream << "\n\nBlocked: " << MWWorld::getFnvCraftingPreparationErrorName(entry.getStaticBlocker());
        }
        return stream.str();
    }

    class LiveFnvCraftingBackend final : public MWWorld::FnvCraftingSessionBackend
    {
        const MWWorld::PreparedFnvCraftingCatalog& mCatalog;
        const MWWorld::ESMStore& mStore;
        const ESM4::Activator& mStation;
        const ESM4::RecipeCategory& mCategory;
        const MWWorld::Ptr mActor;
        const MWWorld::Ptr mPlayer;
        MWWorld::ContainerStoreCraftingInventory mInventory;
        const MWWorld::FalloutPlayerRuntimeState& mPlayerState;

    public:
        LiveFnvCraftingBackend(const MWWorld::PreparedFnvCraftingCatalog& catalog, const MWWorld::ESMStore& store,
            const ESM4::Activator& station, const ESM4::RecipeCategory& category, const MWWorld::Ptr& actor,
            const MWWorld::Ptr& player, MWWorld::ContainerStore& inventory,
            const MWWorld::FalloutPlayerRuntimeState& playerState)
            : mCatalog(catalog)
            , mStore(store)
            , mStation(station)
            , mCategory(category)
            , mActor(actor)
            , mPlayer(player)
            , mInventory(inventory)
            , mPlayerState(playerState)
        {
        }

        MWWorld::FnvCraftingAttemptResult craft(ESM::FormId recipeId) override
        {
            const auto entry = std::ranges::find_if(mCatalog.getEntries(), [&](const auto& candidate) {
                return candidate.getRecipe() == recipeId && candidate.isStaticallySupported();
            });
            if (entry == mCatalog.getEntries().end())
            {
                return { false, MWWorld::FnvCraftingPreparationError::RecipeNotInStore,
                    MWWorld::FnvCraftingCommitResult::InvalidPlan };
            }

            const ESM4::Recipe* recipe = mStore.get<ESM4::Recipe>().search(ESM::RefId(recipeId));
            MWWorld::FnvCraftingPreparationError preparationError = MWWorld::FnvCraftingPreparationError::None;
            std::optional<MWWorld::PreparedFnvCraftingPlan> plan
                = MWWorld::prepareFnvCraftingTransaction({ mStore.getESM4Game(), &mStore, &mStation, &mCategory, recipe,
                                                             mActor, mPlayer, &mInventory, &mPlayerState },
                    &preparationError);
            if (!plan)
            {
                return { false, preparationError, MWWorld::FnvCraftingCommitResult::InvalidPlan };
            }
            return { true, MWWorld::FnvCraftingPreparationError::None,
                MWWorld::commitFnvCraftingTransaction(std::move(*plan)) };
        }
    };
}

namespace MWWorld
{
    FnvCraftingSessionRunResult runPreparedFnvCraftingSession(const PreparedFnvCraftingCatalog& catalog,
        FnvCraftingSessionPresenter& presenter, FnvCraftingSessionBackend& backend)
    {
        if (catalog.getEntries().empty())
            return FnvCraftingSessionRunResult::InvalidSelection;

        const std::vector<std::string> acknowledgeButton{ "#{Interface:OK}" };
        std::size_t page = 0;
        std::size_t redraws = 0;
        const std::size_t pageCount
            = (catalog.getEntries().size() + FnvCraftingCatalogPageSize - 1) / FnvCraftingCatalogPageSize;
        while (true)
        {
            if (redraws == MaxCraftingRedraws)
                return FnvCraftingSessionRunResult::RedrawLimitExceeded;
            ++redraws;

            const std::size_t first = page * FnvCraftingCatalogPageSize;
            const std::size_t count = std::min(FnvCraftingCatalogPageSize, catalog.getEntries().size() - first);
            std::vector<std::string> buttons;
            buttons.reserve(count + 3);
            for (std::size_t index = 0; index < count; ++index)
            {
                const PreparedFnvCraftingCatalogEntry& entry = catalog.getEntries()[first + index];
                std::string label(entry.getName());
                if (!entry.isStaticallySupported())
                    label += " [blocked]";
                buttons.push_back(std::move(label));
            }
            const bool hasPrevious = page != 0;
            const bool hasNext = page + 1 < pageCount;
            if (hasPrevious)
                buttons.emplace_back("Previous");
            if (hasNext)
                buttons.emplace_back("Next");
            buttons.emplace_back("Exit");

            std::ostringstream title;
            title << catalog.getCategoryName() << "\nPage " << (page + 1) << " of " << pageCount;
            const int selection = presenter.show(title.str(), buttons);
            if (selection < 0)
                return FnvCraftingSessionRunResult::Cancelled;
            if (static_cast<std::size_t>(selection) < count)
            {
                const PreparedFnvCraftingCatalogEntry& entry
                    = catalog.getEntries()[first + static_cast<std::size_t>(selection)];
                if (!entry.isStaticallySupported())
                {
                    const int acknowledged = presenter.show(describeEntry(entry), acknowledgeButton);
                    if (acknowledged < 0)
                        return FnvCraftingSessionRunResult::Cancelled;
                    if (acknowledged != 0)
                        return FnvCraftingSessionRunResult::InvalidSelection;
                    continue;
                }

                const std::vector<std::string> confirmationButtons{ "Craft", "Back" };
                const int confirmation = presenter.show(describeEntry(entry), confirmationButtons);
                if (confirmation < 0)
                    return FnvCraftingSessionRunResult::Cancelled;
                if (confirmation == 1)
                    continue;
                if (confirmation != 0)
                    return FnvCraftingSessionRunResult::InvalidSelection;

                const FnvCraftingAttemptResult attempt = backend.craft(entry.getRecipe());
                std::string result;
                if (!attempt.mPrepared)
                {
                    result = "Crafting blocked: ";
                    result += getFnvCraftingPreparationErrorName(attempt.mPreparationError);
                }
                else if (attempt.mCommitResult == FnvCraftingCommitResult::Applied)
                {
                    result = "Crafted ";
                    result += entry.getName();
                }
                else
                {
                    result = "Crafting failed: ";
                    result += getFnvCraftingCommitResultName(attempt.mCommitResult);
                }
                const int acknowledged = presenter.show(result, acknowledgeButton);
                if (acknowledged < 0)
                    return FnvCraftingSessionRunResult::Cancelled;
                if (acknowledged != 0)
                    return FnvCraftingSessionRunResult::InvalidSelection;
                continue;
            }

            std::size_t navigation = count;
            if (hasPrevious && static_cast<std::size_t>(selection) == navigation++)
            {
                --page;
                continue;
            }
            if (hasNext && static_cast<std::size_t>(selection) == navigation++)
            {
                ++page;
                continue;
            }
            if (static_cast<std::size_t>(selection) == navigation)
                return FnvCraftingSessionRunResult::Closed;
            return FnvCraftingSessionRunResult::InvalidSelection;
        }
    }

    ActionFnvCraftingStation::ActionFnvCraftingStation(const Ptr& target, PreparedFnvCraftingCatalog catalog)
        : Action(false, target)
        , mCatalog(std::move(catalog))
    {
    }

    void ActionFnvCraftingStation::executeImp(const Ptr& actor)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        const Ptr& target = getTarget();
        if (world == nullptr || target.isEmpty() || target.getType() != ESM::REC_ACTI4 || target.mRef->isDeleted()
            || !target.getRefData().isEnabled())
        {
            Log(Debug::Warning) << "FNV crafting: target unavailable before session";
            return;
        }

        const Ptr player = world->getPlayerPtr();
        if (actor.isEmpty() || player.isEmpty() || actor != player)
        {
            Log(Debug::Warning) << "FNV crafting: rejected non-player activation actor=" << actor.toString();
            return;
        }

        const ESMStore& store = world->getStore();
        const auto* liveStation = target.get<ESM4::Activator>();
        const ESM4::Activator* station = liveStation != nullptr ? liveStation->mBase : nullptr;
        const ESM4::Activator* storedStation
            = station != nullptr ? store.get<ESM4::Activator>().search(ESM::RefId(station->mId)) : nullptr;
        const std::optional<ESM::FormId> mappedCategory
            = station != nullptr ? getFnvCraftingStationCategory(*station) : std::nullopt;
        const ESM4::RecipeCategory* category
            = store.get<ESM4::RecipeCategory>().search(ESM::RefId(mCatalog.getCategory()));
        if (store.getESM4Game() != ESM4Game::FalloutNewVegas || station == nullptr || storedStation != station
            || (station->mFlags & ESM4::Rec_Deleted) != 0 || !mappedCategory
            || *mappedCategory != mCatalog.getCategory() || mCatalog.getStation() != station->mId || category == nullptr
            || category->mId != mCatalog.getCategory() || (category->mFlags & ESM4::Rec_Deleted) != 0)
        {
            Log(Debug::Warning) << "FNV crafting: exact store/station/category guard failed target="
                                << target.toString();
            return;
        }

        ContainerStore& inventory = actor.getClass().getContainerStore(actor);
        LiveFnvCraftingBackend backend(
            mCatalog, store, *station, *category, actor, player, inventory, world->getFalloutPlayerRuntimeState());
        WindowCraftingSessionPresenter presenter;
        const FnvCraftingSessionRunResult result = runPreparedFnvCraftingSession(mCatalog, presenter, backend);
        Log(Debug::Info) << "FNV crafting: session closed station=" << ESM::RefId(mCatalog.getStation()).toDebugString()
                         << " result=" << static_cast<int>(result);
    }
}
