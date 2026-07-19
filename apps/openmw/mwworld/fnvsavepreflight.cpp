#include "fnvsavepreflight.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

#include <components/files/conversion.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/esm4/loadachr.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadclmt.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadwrld.hpp>

#include "esmstore.hpp"
#include "store.hpp"

namespace
{
    MWWorld::FalloutSavePreflightResolution failure(std::string message)
    {
        return { std::nullopt, std::move(message) };
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
        return resolveFalloutSavePreflightContext(std::move(save), store.getFalloutPlayerState(),
            store.getFalloutNativePlayerRecords(), store.get<ESM4::World>(), store.get<ESM4::Cell>(),
            store.get<ESM4::Climate>(), store.get<ESM4::Weather>(), currentContentFiles);
    }

    FalloutSavePreflightResolution resolveFalloutSavePreflightContext(ESM4::FONVSaveGamePrefix save,
        const FalloutPlayerState* player, const FalloutNativePlayerRecordsResolution& nativePlayer,
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
        if (records.mBaseNpc == nullptr || records.mReference == nullptr || records.mClass == nullptr
            || records.mRace == nullptr)
        {
            return failure("ESMStore native FNV Player view is incomplete");
        }
        if (records.mBaseNpc->mId != player->mBaseRecord || records.mReference->mId != player->mReferenceRecord
            || records.mClass->mId != player->mClass || records.mRace->mId != player->mRace)
        {
            return failure("ESMStore native FNV Player record identities do not match its validated Player state");
        }

        FalloutSaveLoadPlanResolution plan
            = resolveFalloutSaveLoadPlan(save, player, currentContentFiles);
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
        identities.mReference = records.mReference->mId;
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
}
