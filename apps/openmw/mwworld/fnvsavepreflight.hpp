#ifndef OPENMW_MWWORLD_FNVSAVEPREFLIGHT_H
#define OPENMW_MWWORLD_FNVSAVEPREFLIGHT_H

#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include <components/esm/formid.hpp>
#include <components/esm4/fonvsavegame.hpp>

#include "fnvplayerstate.hpp"
#include "fnvweather.hpp"

namespace ESM4
{
    struct Ammunition;
    struct Cell;
    struct Climate;
    struct FormIdList;
    struct Weather;
    struct World;
}

namespace MWWorld
{
    class ESMStore;

    template <class T>
    class Store;

    struct FalloutNativePlayerRecordIdentities
    {
        ESM::FormId mBaseNpc;
        ESM::FormId mReference;
        ESM::FormId mClass;
        ESM::FormId mRace;
    };

    // This is a read-only transaction candidate. Building it does not clear or mutate the current game. Keeping the
    // exact parsed prefix and every resolved native identity together prevents a later load phase from silently
    // substituting an ESM3 carrier, a TES3 weather, or a display-only location label.
    struct FalloutSavePreflightContext
    {
        ESM4::FONVSaveGamePrefix mSave;
        FalloutPlayerState mPlayer;
        FalloutNativePlayerRecordIdentities mNativePlayer;
        FalloutSaveLoadPlan mPlan;
        FalloutExteriorPlayerPlacement mPlacement;
        FalloutWeatherModel mWeather;
    };

    struct FalloutSavePreflightResolution
    {
        std::optional<FalloutSavePreflightContext> mContext;
        std::string mError;

        explicit operator bool() const { return mContext.has_value(); }
    };

    bool isFalloutNewVegasSavePath(const std::filesystem::path& path);

    // Production entry point. The ESMStore accessors publish a Player state only after validating the exact winning
    // FalloutNV.esm NPC_/CLAS/RACE view plus the engine-reserved Player reference identity; this function deliberately
    // has no filename-candidate fallback.
    FalloutSavePreflightResolution resolveFalloutSavePreflightContext(
        ESM4::FONVSaveGamePrefix save, const ESMStore& store, std::span<const std::string> currentContentFiles);

    // Pure core used to prove the transaction boundary without needing a running World. Production obtains these
    // inputs exclusively from the validated ESMStore entry point above.
    FalloutSavePreflightResolution resolveFalloutSavePreflightContext(ESM4::FONVSaveGamePrefix save,
        const FalloutPlayerState* player, const FalloutNativePlayerRecordsResolution& nativePlayer,
        const Store<ESM4::FormIdList>& formLists, const Store<ESM4::Ammunition>& ammunition,
        const Store<ESM4::World>& worlds, const Store<ESM4::Cell>& cells, const Store<ESM4::Climate>& climates,
        const Store<ESM4::Weather>& weather, std::span<const std::string> currentContentFiles);

    // This is the mutation gate. It returns only when every semantic domain is covered; callers must invoke it before
    // cleanup or any save/world mutation.
    void requireFalloutSavePreflightReady(const FalloutSavePreflightContext& context);

    // Narrow mutation gate for the normal-path Save330 visual slice. Full-game uncovered domains remain explicit in
    // mPlan and are not claimed as loaded by this gate.
    void requireFalloutSaveVisualApplicationReady(const FalloutSavePreflightContext& context);
}

#endif
