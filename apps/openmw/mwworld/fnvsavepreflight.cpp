#include "fnvsavepreflight.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadclmt.hpp>
#include <components/esm4/loadfact.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadglob.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadperk.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadrefr.hpp>
#include <components/esm4/loadweap.hpp>
#include <components/esm4/loadwrld.hpp>

#include "esmstore.hpp"
#include "fnvplayerruntimestate.hpp"
#include "store.hpp"

namespace
{
    MWWorld::FalloutSavePreflightResolution failure(std::string message)
    {
        return { std::nullopt, std::move(message) };
    }

    std::string validateConditionedInventory(
        const MWWorld::FalloutSavePlayerHeaderState& player, const MWWorld::ESMStore& store)
    {
        for (const MWWorld::FalloutSavePlayerHeaderState::ConditionedStack& stack : player.mConditionedStacks)
        {
            const ESM::RefId id(stack.mRecord);
            std::uint32_t maximumHealth = 0;
            switch (store.find(id))
            {
                case ESM::REC_ARMO4:
                {
                    const ESM4::Armor* armor = store.get<ESM4::Armor>().search(id);
                    if (armor != nullptr)
                        maximumHealth = armor->mData.health;
                    break;
                }
                case ESM::REC_WEAP4:
                {
                    const ESM4::Weapon* weapon = store.get<ESM4::Weapon>().search(id);
                    if (weapon != nullptr)
                        maximumHealth = weapon->mData.health;
                    break;
                }
                default:
                    break;
            }
            if (maximumHealth == 0)
                return "FNV save conditioned inventory item is not a loaded damageable ARMO/WEAP: " + id.toString();
            if (stack.mHealth > static_cast<float>(maximumHealth))
                return "FNV save inventory ExtraHealth exceeds authored maximum health for " + id.toString();
        }
        return {};
    }

    std::string validateInventorySelections(
        const MWWorld::FalloutSavePlayerHeaderState& player, const MWWorld::ESMStore& store)
    {
        const auto hasInventoryRecord = [&](ESM::FormId record) {
            return std::ranges::find(player.mInventoryItems, record, &MWWorld::FalloutInventoryItem::mRecord)
                != player.mInventoryItems.end();
        };
        for (const MWWorld::FalloutSavePlayerHeaderState::HotkeyItem& hotkey : player.mHotkeyItems)
        {
            const ESM::RefId item(hotkey.mRecord);
            if (!hasInventoryRecord(hotkey.mRecord) || store.find(item) == 0)
                return "FNV save ExtraHotkey item is not available in the loaded Player inventory: " + item.toString();
        }
        for (const MWWorld::FalloutSavePlayerHeaderState::AmmoSelection& selection : player.mAmmoSelections)
        {
            const ESM::RefId weaponId(selection.mWeapon);
            const ESM4::Weapon* weapon = store.get<ESM4::Weapon>().search(weaponId);
            if (!hasInventoryRecord(selection.mWeapon) || weapon == nullptr)
                return "FNV save ExtraAmmo owner is not a loaded Player weapon: " + weaponId.toString();
            if (store.get<ESM4::Ammunition>().search(ESM::RefId(selection.mAmmo)) == nullptr)
                return "FNV save ExtraAmmo selection is not a loaded AMMO record: " + selection.mAmmo.toString();
            if (selection.mSavedCount < 0 || selection.mSavedCount > weapon->mData.clipSize)
                return "FNV save ExtraAmmo count exceeds the authored WEAP magazine: " + weaponId.toString();

            bool compatible = weapon->mAmmo == selection.mAmmo;
            if (!compatible)
            {
                if (const ESM4::FormIdList* list = store.get<ESM4::FormIdList>().search(weapon->mAmmo))
                    compatible = std::ranges::find(list->mObjects, selection.mAmmo) != list->mObjects.end();
            }
            if (!compatible)
                return "FNV save ExtraAmmo selection is incompatible with its WEAP: " + selection.mAmmo.toString();
        }
        return {};
    }

    std::string validateFactionChanges(
        const MWWorld::FalloutSavePlayerHeaderState& player, const MWWorld::ESMStore& store)
    {
        for (const MWWorld::FalloutSavePlayerHeaderState::FactionChange& change : player.mFactionChanges)
        {
            if (store.get<ESM4::Faction>().search(ESM::RefId(change.mFaction)) == nullptr)
                return "FNV save Player faction change is not a loaded FACT record: " + change.mFaction.toString();
        }
        return {};
    }

    std::string validateFactionStates(const MWWorld::FalloutSaveLoadPlan& plan, const MWWorld::ESMStore& store)
    {
        for (const MWWorld::FalloutSaveLoadPlan::FactionState& saved : plan.mFactions)
        {
            if (store.get<ESM4::Faction>().search(ESM::RefId(saved.mFaction)) == nullptr)
                return "FNV save changed faction is not a loaded FACT record: " + saved.mFaction.toString();
            std::set<ESM::FormId> targets;
            for (const MWWorld::FalloutSaveLoadPlan::FactionRelation& relation : saved.mRelations)
            {
                if (store.get<ESM4::Faction>().search(ESM::RefId(relation.mFaction)) == nullptr)
                    return "FNV save faction reaction target is not a loaded FACT record: "
                        + relation.mFaction.toString();
                if (!targets.insert(relation.mFaction).second)
                    return "FNV save faction reaction list contains a duplicate target FACT";
                if (relation.mGroupCombatReaction
                    > static_cast<std::uint32_t>(ESM4::Faction::GroupCombatReaction::Friend))
                {
                    return "FNV save faction reaction has an invalid group-combat reaction";
                }
            }
        }
        return {};
    }

    std::string validateWorldReferenceTransforms(
        const MWWorld::FalloutSaveLoadPlan& plan, const MWWorld::ESMStore& store)
    {
        std::set<ESM::FormId> references;
        for (const MWWorld::FalloutSaveLoadPlan::WorldReferenceTransform& transform
            : plan.mWorldReferenceTransforms)
        {
            if (!references.insert(transform.mReference).second)
                return "FNV save contains duplicate movement state for one REFR/ACHR/ACRE";
            bool found = false;
            switch (transform.mChangeType)
            {
                case 0:
                    found = store.get<ESM4::Reference>().search(transform.mReference) != nullptr;
                    break;
                case 1:
                    found = store.get<ESM4::ActorCharacter>().search(transform.mReference) != nullptr;
                    break;
                case 2:
                    found = store.get<ESM4::ActorCreature>().search(transform.mReference) != nullptr;
                    break;
                default:
                    return "FNV save movement state has an unsupported changed-reference type";
            }
            if (!found)
                return "FNV save moved reference is not a loaded record of its exact REFR/ACHR/ACRE type: "
                    + transform.mReference.toString();
            if (store.get<ESM4::World>().search(ESM::RefId(transform.mCellOrWorldspace)) == nullptr
                && store.get<ESM4::Cell>().search(ESM::RefId(transform.mCellOrWorldspace)) == nullptr)
            {
                return "FNV save moved reference target is not a loaded CELL or WRLD: "
                    + transform.mCellOrWorldspace.toString();
            }
        }
        return {};
    }

    std::string validatePlayerActorValuesAndPerks(
        const MWWorld::FalloutSavePlayerHeaderState& player, const MWWorld::ESMStore& store)
    {
        for (const MWWorld::FalloutSavePlayerHeaderState::ActorValueModifier& modifier
            : player.mActorValueModifiers)
        {
            if (modifier.mActorValue >= MWWorld::FalloutPlayerRuntimeState::ActorValueCount
                || !std::isfinite(modifier.mModifier))
            {
                return "FNV save Player actor-value modifier is outside the supported native range";
            }
        }
        for (const MWWorld::FalloutSavePlayerHeaderState::PerkRank& perk : player.mPerks)
        {
            if (store.get<ESM4::Perk>().search(ESM::RefId(perk.mPerk)) == nullptr)
                return "FNV save Player perk is not a loaded PERK record: " + perk.mPerk.toString();
        }
        return {};
    }

    std::string validateGlobalVariables(MWWorld::FalloutSaveLoadPlan& plan, const MWWorld::ESMStore& store)
    {
        bool skippedMissing = false;
        const auto remove = std::remove_if(plan.mGlobals.begin(), plan.mGlobals.end(),
            [&](const MWWorld::FalloutSaveLoadPlan::GlobalValue& saved) {
                const ESM4::GlobalVariable* global
                    = store.get<ESM4::GlobalVariable>().search(ESM::RefId(saved.mVariable));
                if (global != nullptr)
                    return false;
                if (store.find(ESM::RefId(saved.mVariable)) != 0)
                    return false;
                skippedMissing = true;
                Log(Debug::Warning) << "Native FNV save skipped stale global-variable identity absent from loaded "
                                       "content: "
                                    << saved.mVariable.toString();
                return true;
            });
        plan.mGlobals.erase(remove, plan.mGlobals.end());
        if (skippedMissing)
            plan.mUncoveredState.push_back("global-variables-unresolved-content-forms");

        for (const MWWorld::FalloutSaveLoadPlan::GlobalValue& saved : plan.mGlobals)
        {
            const ESM4::GlobalVariable* global
                = store.get<ESM4::GlobalVariable>().search(ESM::RefId(saved.mVariable));
            if (global == nullptr)
                return "FNV save global variable resolves to a loaded non-GLOB record: "
                    + saved.mVariable.toString();
            if (global->mId != saved.mVariable)
                return "FNV save global variable resolved with a different identity: " + saved.mVariable.toString();
            if (global->mEditorId.empty())
                return "FNV save global variable is an unnamed GLOB record: " + saved.mVariable.toString();
            if (!std::isfinite(saved.mValue))
                return "FNV save global variable has a non-finite value: " + saved.mVariable.toString();
        }
        return {};
    }
}

namespace MWWorld
{
    bool isFalloutNewVegasSavePath(const std::filesystem::path& path)
    {
        return Misc::StringUtils::ciEqual(Files::pathToUnicodeString(path.extension()), ".fos");
    }

    FalloutSavePreflightResolution resolveFalloutSavePreflightContext(
        ESM4::FONVSaveGamePrefix save, const ESMStore& store, std::span<const std::string> currentContentFiles)
    {
        FalloutSavePreflightResolution result = resolveFalloutSavePreflightContext(std::move(save), store.getFalloutPlayerState(),
            store.getFalloutNativePlayerRecords(), store.get<ESM4::FormIdList>(), store.get<ESM4::Ammunition>(),
            store.get<ESM4::World>(), store.get<ESM4::Cell>(), store.get<ESM4::Climate>(),
            store.get<ESM4::Weather>(), currentContentFiles);
        if (!result)
            return result;

        const std::string inventoryError = validateConditionedInventory(result.mContext->mPlan.mPlayer, store);
        if (!inventoryError.empty())
            return failure(inventoryError);
        const std::string selectionError = validateInventorySelections(result.mContext->mPlan.mPlayer, store);
        if (!selectionError.empty())
            return failure(selectionError);
        const std::string factionError = validateFactionChanges(result.mContext->mPlan.mPlayer, store);
        if (!factionError.empty())
            return failure(factionError);
        const std::string factionStateError = validateFactionStates(result.mContext->mPlan, store);
        if (!factionStateError.empty())
            return failure(factionStateError);
        const std::string transformError = validateWorldReferenceTransforms(result.mContext->mPlan, store);
        if (!transformError.empty())
            return failure(transformError);
        const std::string actorValueError
            = validatePlayerActorValuesAndPerks(result.mContext->mPlan.mPlayer, store);
        if (!actorValueError.empty())
            return failure(actorValueError);
        const std::string globalError = validateGlobalVariables(result.mContext->mPlan, store);
        if (!globalError.empty())
            return failure(globalError);
        for (const FalloutInventoryItem& item : result.mContext->mPlan.mPlayer.mInventoryItems)
        {
            if (store.find(ESM::RefId(item.mRecord)) == 0)
            {
                result.mContext->mPlan.mUncoveredState.push_back("player-inventory-unresolved-content-forms");
                break;
            }
        }
        if (!result.mContext->mPlan.mQuestProgress)
            return result;

        ESM4QuestRuntime validationRuntime;
        validationRuntime.initialize(store);
        std::string error;
        if (!validationRuntime.loadSavedProgress(*result.mContext->mPlan.mQuestProgress, &error))
            return failure("FNV save quest-progress preflight failed: " + error);
        return result;
    }

    FalloutSavePreflightResolution resolveFalloutSavePreflightContext(ESM4::FONVSaveGamePrefix save,
        const FalloutPlayerState* player, const FalloutNativePlayerRecordsResolution& nativePlayer,
        const Store<ESM4::FormIdList>& formLists, const Store<ESM4::Ammunition>& ammunition,
        const Store<ESM4::World>& worlds, const Store<ESM4::Cell>& cells, const Store<ESM4::Climate>& climates,
        const Store<ESM4::Weather>& weather, std::span<const std::string> currentContentFiles)
    {
        if (player == nullptr)
            return failure("ESMStore has no fully validated native FNV Player state");
        if (!nativePlayer)
        {
            return failure(nativePlayer.mError.empty() ? "ESMStore has no fully validated native FNV Player records"
                                                       : nativePlayer.mError);
        }

        const FalloutNativePlayerRecords& records = *nativePlayer.mRecords;
        if (records.mBaseNpc == nullptr || records.mClass == nullptr || records.mRace == nullptr)
        {
            return failure("ESMStore native FNV Player view is incomplete");
        }
        if (records.mBaseNpc->mId != player->mBaseRecord || records.mClass->mId != player->mClass
            || records.mRace->mId != player->mRace)
        {
            return failure("ESMStore native FNV Player record identities do not match its validated Player state");
        }

        FalloutSaveLoadPlanResolution plan
            = resolveFalloutSaveLoadPlan(save, player, formLists, ammunition, currentContentFiles);
        if (!plan)
            return failure("FNV save load-plan resolution failed: " + plan.mError);

        FalloutExteriorPlayerPlacementResolution placement
            = resolveFalloutExteriorPlayerPlacement(worlds, cells, plan.mPlan->mTransform);
        if (!placement)
            return failure("FNV save Player-placement resolution failed: " + placement.mError);

        FalloutWeatherModelResolution weatherModel
            = resolveFalloutWeatherModel(worlds, climates, weather, *placement.mPlacement, plan.mPlan->mScene);
        if (!weatherModel)
            return failure("FNV save weather resolution failed: " + weatherModel.mError);

        FalloutNativePlayerRecordIdentities identities;
        identities.mBaseNpc = records.mBaseNpc->mId;
        identities.mReference = player->mReferenceRecord;
        identities.mClass = records.mClass->mId;
        identities.mRace = records.mRace->mId;

        FalloutSavePreflightContext context{ std::move(save), *player, std::move(identities),
            std::move(*plan.mPlan), std::move(*placement.mPlacement), std::move(*weatherModel.mModel) };
        return { std::move(context), {} };
    }

    void requireFalloutSavePreflightReady(const FalloutSavePreflightContext& context)
    {
        if (context.mPlan.mUncoveredState.empty())
            return;

        std::ostringstream message;
        message << "native FNV save load remains fail-closed; semantically uncovered state:";
        for (const std::string& blocker : context.mPlan.mUncoveredState)
            message << " " << blocker << ";";
        throw std::runtime_error(message.str());
    }

    void requireFalloutSaveVisualApplicationReady(const FalloutSavePreflightContext& context)
    {
        const FalloutSaveLoadPlan& plan = context.mPlan;
        if (plan.mPlayer.mBaseRecord != context.mNativePlayer.mBaseNpc
            || plan.mPlayer.mReferenceRecord != context.mNativePlayer.mReference)
        {
            throw std::runtime_error("native FNV visual application Player identities do not match preflight");
        }
        if (plan.mTransform.mCellOrWorldspaceRecord != context.mPlacement.mWorldspaceRecord
            || context.mWeather.mWorldspace != context.mPlacement.mWorldspaceRecord
            || context.mPlacement.mCellRecord.isZeroOrUnset())
        {
            throw std::runtime_error("native FNV visual application placement identities do not match preflight");
        }
        for (float value : plan.mTransform.mPosition)
        {
            if (!std::isfinite(value))
                throw std::runtime_error("native FNV visual application position is not finite");
        }
        for (float value : plan.mTransform.mRotationRadians)
        {
            if (!std::isfinite(value))
                throw std::runtime_error("native FNV visual application rotation is not finite");
        }
        if (!std::isfinite(plan.mCamera.mWorldFov) || plan.mCamera.mWorldFov <= 0.f
            || plan.mCamera.mWorldFov >= 180.f || !std::isfinite(plan.mCamera.mFirstPersonModelFov)
            || plan.mCamera.mFirstPersonModelFov <= 0.f || plan.mCamera.mFirstPersonModelFov >= 180.f)
        {
            throw std::runtime_error("native FNV visual application camera FOV is invalid");
        }
        if (!std::isfinite(plan.mScene.mGameHour) || plan.mScene.mGameHour < 0.f || plan.mScene.mGameHour >= 24.f
            || plan.mScene.mCurrentWeather != context.mWeather.mCurrent.mWeather)
        {
            throw std::runtime_error("native FNV visual application time/weather does not match preflight");
        }
    }
}
