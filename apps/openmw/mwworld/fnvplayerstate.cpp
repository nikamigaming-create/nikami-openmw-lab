#include "fnvplayerstate.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <components/esm/refid.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/fonvsavegame.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/esm4/loadclas.hpp>
#include <components/esm4/loadnpc.hpp>
#include <components/esm4/loadrace.hpp>
#include <components/esm4/loadwrld.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "store.hpp"

namespace
{
    struct CategoryResolution
    {
        const ESM4::Npc* mRecord = nullptr;
        std::string mError;
    };

    CategoryResolution resolveCategory(const MWWorld::Store<ESM4::Npc>& npcs, const ESM4::Npc& player,
        std::uint16_t templateFlag, std::string_view category)
    {
        std::unordered_set<ESM::FormId> visited;
        const ESM4::Npc* current = &player;
        while (current != nullptr)
        {
            if (!visited.insert(current->mId).second)
            {
                return { nullptr,
                    "template cycle while resolving " + std::string(category) + " at " + current->mId.toString() };
            }
            if (!current->mIsFONV)
            {
                return { nullptr,
                    "non-FNV template while resolving " + std::string(category) + " at " + current->mId.toString() };
            }
            if (!current->mHasFNVBaseConfig)
            {
                return { nullptr,
                    "missing exact 24-byte ACBS while resolving " + std::string(category) + " at "
                        + current->mId.toString() };
            }
            if ((current->mBaseConfig.fo3.templateFlags & templateFlag) == 0)
                return { current, {} };
            if (current->mBaseTemplate.isZeroOrUnset())
            {
                return { nullptr,
                    "missing TPLT while resolving delegated " + std::string(category) + " at "
                        + current->mId.toString() };
            }

            current = npcs.search(ESM::RefId(current->mBaseTemplate));
            if (current == nullptr)
            {
                return { nullptr,
                    "unresolved TPLT while resolving " + std::string(category) + " from " + player.mId.toString() };
            }
        }

        return { nullptr, "unresolved " + std::string(category) };
    }

    MWWorld::FalloutPlayerStateResolution failure(std::string message)
    {
        return { std::nullopt, std::move(message) };
    }

    MWWorld::FalloutSaveLoadPlanResolution loadFailure(std::string message)
    {
        return { std::nullopt, std::move(message) };
    }

    MWWorld::FalloutNativePlayerRecordsResolution nativeRecordsFailure(std::string message)
    {
        return { std::nullopt, std::move(message) };
    }

    MWWorld::FalloutExteriorPlayerPlacementResolution placementFailure(std::string message)
    {
        return { std::nullopt, std::move(message) };
    }

    std::vector<std::size_t> findContentFileIndices(
        std::span<const std::string> contentFiles, std::string_view fileName)
    {
        std::vector<std::size_t> result;
        for (std::size_t index = 0; index < contentFiles.size(); ++index)
        {
            if (Misc::StringUtils::ciEqual(contentFiles[index], fileName))
                result.push_back(index);
        }
        return result;
    }

    std::vector<std::size_t> findSaveMasterIndices(const ESM4::FONVSaveGamePrefix& save, std::string_view fileName)
    {
        std::vector<std::size_t> result;
        for (std::size_t index = 0; index < save.mMasters.size(); ++index)
        {
            if (Misc::StringUtils::ciEqual(save.mMasters[index].mFileName.mValue, fileName))
                result.push_back(index);
        }
        return result;
    }

    std::vector<std::size_t> findCurrentFalloutPluginIndices(std::span<const std::string> contentFiles)
    {
        std::vector<std::size_t> result;
        for (std::size_t index = 0; index < contentFiles.size(); ++index)
        {
            if (Misc::StringUtils::ciEndsWith(contentFiles[index], ".esm")
                || Misc::StringUtils::ciEndsWith(contentFiles[index], ".esp"))
            {
                result.push_back(index);
            }
        }
        return result;
    }

    std::optional<ESM::FormId> normalizeSavedFormId(
        std::uint32_t rawFormId, std::span<const std::size_t> currentPluginIndices)
    {
        const std::size_t saveOwnerIndex = rawFormId >> 24;
        const std::uint32_t objectIndex = rawFormId & 0x00ffffffu;
        if (objectIndex == 0 || saveOwnerIndex >= currentPluginIndices.size())
            return std::nullopt;
        const std::size_t currentOwnerIndex = currentPluginIndices[saveOwnerIndex];
        if (currentOwnerIndex > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return std::nullopt;
        return ESM::FormId{ objectIndex, static_cast<std::int32_t>(currentOwnerIndex) };
    }

    std::vector<std::string> falloutSaveLoadBlockers()
    {
        return {
            "player-runtime-actor-values-modifiers-health-limbs-perks",
            "player-inventory-equipment-ammo",
            "player-factions-reputation-crime-disguise",
            "quest-stages-objectives-variables",
            "global-variables",
            "world-change-forms-doors-containers-actors-unloaded-references",
            "dynamic-forms-refid-array-visited-worldspaces",
        };
    }

    template <class T, std::size_t Size>
    std::array<std::uint8_t, Size> bytesOf(const T& value)
    {
        static_assert(sizeof(T) == Size);
        std::array<std::uint8_t, Size> result{};
        std::memcpy(result.data(), &value, Size);
        return result;
    }
}

namespace MWWorld
{
    float convertFalloutReferenceFovToOpenMwVertical(float horizontalReferenceFov)
    {
        if (!std::isfinite(horizontalReferenceFov) || horizontalReferenceFov <= 0.f || horizontalReferenceFov >= 180.f)
        {
            throw std::invalid_argument("Fallout reference FOV must be finite and in (0, 180)");
        }

        constexpr double referenceAspect = 4.0 / 3.0;
        constexpr double radiansPerDegree = std::numbers::pi_v<double> / 180.0;
        constexpr double degreesPerRadian = 180.0 / std::numbers::pi_v<double>;
        const double horizontalRadians = static_cast<double>(horizontalReferenceFov) * radiansPerDegree;
        return static_cast<float>(
            2.0 * std::atan(std::tan(horizontalRadians * 0.5) / referenceAspect) * degreesPerRadian);
    }

    std::uint8_t FalloutPlayerState::getSpecial(FalloutSpecial value) const
    {
        return mSpecial.at(static_cast<std::size_t>(value));
    }

    std::uint8_t FalloutPlayerState::getSkillValue(FalloutSkill value) const
    {
        return mSkillValues.at(static_cast<std::size_t>(value));
    }

    std::uint8_t FalloutPlayerState::getSkillOffset(FalloutSkill value) const
    {
        return mSkillOffsets.at(static_cast<std::size_t>(value));
    }

    std::optional<FalloutActorValueComponents> FalloutPlayerState::getActorValueComponents(
        std::uint32_t actorValue) const
    {
        if (actorValue <= 4)
        {
            const std::array<std::uint8_t, 5> aiValues{ mAIData.aggression, mAIData.confidence, mAIData.energyLevel,
                mAIData.responsibility, mAIData.mood };
            return FalloutActorValueComponents{ static_cast<double>(aiValues[actorValue]), std::nullopt };
        }
        if (actorValue >= 5 && actorValue <= 11)
        {
            return FalloutActorValueComponents{ static_cast<double>(mSpecial[static_cast<std::size_t>(actorValue - 5)]),
                std::nullopt };
        }
        if (actorValue == 16)
            return FalloutActorValueComponents{ static_cast<double>(mHealth), std::nullopt };
        if (actorValue >= 32 && actorValue <= 45)
        {
            const std::size_t index = static_cast<std::size_t>(actorValue - 32);
            return FalloutActorValueComponents{ static_cast<double>(mSkillValues[index]), mSkillOffsets[index] };
        }
        return std::nullopt;
    }

    FalloutPlayerStateResolution resolveFalloutPlayerState(
        const Store<ESM4::Npc>& npcs, ESM::FormId normalizedPlayerFormId)
    {
        const ESM4::Npc* player = npcs.search(ESM::RefId(normalizedPlayerFormId));
        if (player == nullptr)
        {
            return failure("missing winning FalloutNV.esm Player NPC_ FormID 0x00000007 at normalized "
                + normalizedPlayerFormId.toString());
        }
        if (!player->mIsFONV)
            return failure("winning NPC_ FormID 0x00000007 is not an FNV record");
        if (player->mEditorId != "Player")
            return failure("winning NPC_ FormID 0x00000007 does not have EDID Player");

        const CategoryResolution traits = resolveCategory(npcs, *player, ESM4::Npc::Template_UseTraits, "traits");
        if (traits.mRecord == nullptr)
            return failure(traits.mError);
        const CategoryResolution stats = resolveCategory(npcs, *player, ESM4::Npc::Template_UseStats, "stats");
        if (stats.mRecord == nullptr)
            return failure(stats.mError);
        const CategoryResolution factions = resolveCategory(npcs, *player, ESM4::Npc::Template_UseFactions, "factions");
        if (factions.mRecord == nullptr)
            return failure(factions.mError);
        const CategoryResolution ai = resolveCategory(npcs, *player, ESM4::Npc::Template_UseAIData, "AI data");
        if (ai.mRecord == nullptr)
            return failure(ai.mError);
        const CategoryResolution model = resolveCategory(npcs, *player, ESM4::Npc::Template_UseModel, "model");
        if (model.mRecord == nullptr)
            return failure(model.mError);
        const CategoryResolution baseData
            = resolveCategory(npcs, *player, ESM4::Npc::Template_UseBaseData, "base data");
        if (baseData.mRecord == nullptr)
            return failure(baseData.mError);

        if (!stats.mRecord->mHasFNVData)
            return failure("resolved Player stats record lacks exact 11-byte DATA");
        if (!stats.mRecord->mHasFNVSkills)
            return failure("resolved Player stats record lacks exact 28-byte DNAM");
        if (!ai.mRecord->mHasFNVAIData)
            return failure("resolved Player AI record lacks exact 20-byte AIDT");
        if (traits.mRecord->mRace.isZeroOrUnset())
            return failure("resolved Player traits record lacks RNAM race identity");
        if (stats.mRecord->mClass.isZeroOrUnset())
            return failure("resolved Player stats record lacks CNAM class identity");
        if (model.mRecord->mModel.empty())
            return failure("resolved Player model record lacks MODL identity");
        if (stats.mRecord->mFNVData.health < 0
            || stats.mRecord->mFNVData.health > std::numeric_limits<std::uint16_t>::max())
            return failure("resolved Player health cannot be represented by the temporary ESM3 proxy");

        FalloutPlayerState result;
        result.mBaseRecord = player->mId;
        result.mTraitsRecord = traits.mRecord->mId;
        result.mStatsRecord = stats.mRecord->mId;
        result.mFactionsRecord = factions.mRecord->mId;
        result.mAIDataRecord = ai.mRecord->mId;
        result.mModelRecord = model.mRecord->mId;
        result.mBaseDataRecord = baseData.mRecord->mId;
        result.mEditorId = player->mEditorId;
        result.mFullName = baseData.mRecord->mFullName;
        result.mModel = model.mRecord->mModel;
        result.mRace = traits.mRecord->mRace;
        result.mClass = stats.mRecord->mClass;
        result.mHair = traits.mRecord->mHair;
        result.mEyes = traits.mRecord->mEyes;
        result.mVoiceType = traits.mRecord->mVoiceType;
        result.mRecordFlags = baseData.mRecord->mFlags;
        result.mFactions = factions.mRecord->mFactions;
        result.mStatsConfig = stats.mRecord->mBaseConfig.fo3;
        result.mAIData = ai.mRecord->mFNVAIData;
        result.mHealth = stats.mRecord->mFNVData.health;
        result.mSpecial = { stats.mRecord->mFNVData.strength, stats.mRecord->mFNVData.perception,
            stats.mRecord->mFNVData.endurance, stats.mRecord->mFNVData.charisma, stats.mRecord->mFNVData.intelligence,
            stats.mRecord->mFNVData.agility, stats.mRecord->mFNVData.luck };
        result.mSkillValues
            = bytesOf<ESM4::Npc::FNVSkillValues, FalloutPlayerState::SkillCount>(stats.mRecord->mFNVSkills.values);
        result.mSkillOffsets
            = bytesOf<ESM4::Npc::FNVSkillValues, FalloutPlayerState::SkillCount>(stats.mRecord->mFNVSkills.offsets);
        return { std::move(result), {} };
    }

    FalloutPlayerStateResolution resolveFalloutPlayerIdentity(
        const Store<ESM4::Npc>& npcs, ESM::FormId normalizedPlayerFormId, ESM::FormId normalizedPlayerReferenceFormId)
    {
        FalloutPlayerStateResolution resolution = resolveFalloutPlayerState(npcs, normalizedPlayerFormId);
        if (!resolution)
            return resolution;

        // FormID 0x14 is the engine-reserved Player reference. It is serialized by FNV saves but is not an authored
        // ACHR record in FalloutNV.esm, so it must never be resolved through the content ActorCharacter store.
        if (!normalizedPlayerReferenceFormId.hasContentFile() || normalizedPlayerReferenceFormId.mIndex != 0x14)
            return failure("FNV engine-reserved Player reference is not canonical FormID 0x00000014");
        if (normalizedPlayerReferenceFormId.mContentFile != normalizedPlayerFormId.mContentFile)
        {
            return failure(
                "FNV engine-reserved Player reference FormID 0x00000014 is not in the Player NPC_ content "
                "namespace");
        }

        resolution.mState->mReferenceRecord = normalizedPlayerReferenceFormId;
        resolution.mState->mReferenceBaseRecord = normalizedPlayerFormId;
        return resolution;
    }

    FalloutNativePlayerRecordsResolution resolveFalloutNativePlayerRecords(const Store<ESM4::Npc>& npcs,
        const Store<ESM4::Class>& classes, const Store<ESM4::Race>& races, const FalloutPlayerState& playerState)
    {
        if (!playerState.mBaseRecord.hasContentFile() || playerState.mBaseRecord.mIndex != 7)
        {
            return nativeRecordsFailure(
                "native FNV Player state does not identify exact normalized NPC_ FormID 0x00000007");
        }
        if (!playerState.mReferenceRecord.hasContentFile() || playerState.mReferenceRecord.mIndex != 0x14
            || playerState.mReferenceRecord.mContentFile != playerState.mBaseRecord.mContentFile)
        {
            return nativeRecordsFailure(
                "native FNV Player state does not identify exact normalized ACHR FormID 0x00000014 in the Player "
                "content namespace");
        }
        if (playerState.mEditorId != "Player")
            return nativeRecordsFailure("native FNV Player state does not preserve EDID Player");
        if (playerState.mReferenceBaseRecord != playerState.mBaseRecord)
            return nativeRecordsFailure("native FNV Player state does not preserve the ACHR-to-NPC_ base relation");
        if (!playerState.mClass.hasContentFile() || playerState.mClass.mIndex == 0)
            return nativeRecordsFailure("native FNV Player CLAS identity has no normalized content provenance");
        if (!playerState.mRace.hasContentFile() || playerState.mRace.mIndex == 0)
            return nativeRecordsFailure("native FNV Player RACE identity has no normalized content provenance");

        const std::array<std::pair<std::string_view, ESM::FormId>, 4> identities{ {
            { "NPC_", playerState.mBaseRecord },
            { "ACHR", playerState.mReferenceRecord },
            { "CLAS", playerState.mClass },
            { "RACE", playerState.mRace },
        } };
        for (std::size_t left = 0; left < identities.size(); ++left)
        {
            for (std::size_t right = left + 1; right < identities.size(); ++right)
            {
                if (identities[left].second == identities[right].second)
                {
                    return nativeRecordsFailure("native FNV Player record identities collide: "
                        + std::string(identities[left].first) + " and " + std::string(identities[right].first));
                }
            }
        }

        const ESM4::Npc* baseNpc = npcs.search(ESM::RefId(playerState.mBaseRecord));
        if (baseNpc == nullptr)
            return nativeRecordsFailure("missing exact native FNV Player NPC_ " + playerState.mBaseRecord.toString());
        if (baseNpc->mId != playerState.mBaseRecord)
            return nativeRecordsFailure("native FNV Player NPC_ resolved with the wrong typed FormID");
        if (!baseNpc->mIsFONV)
            return nativeRecordsFailure("native FNV Player NPC_ is not marked as an FNV record");
        if (baseNpc->mEditorId != "Player")
            return nativeRecordsFailure("native FNV Player NPC_ does not have EDID Player");
        if (baseNpc->mClass != playerState.mClass)
            return nativeRecordsFailure("native FNV Player NPC_ does not preserve the resolved CLAS identity");
        if (baseNpc->mRace != playerState.mRace)
            return nativeRecordsFailure("native FNV Player NPC_ does not preserve the resolved RACE identity");

        const ESM4::Class* playerClass = classes.search(ESM::RefId(playerState.mClass));
        if (playerClass == nullptr)
            return nativeRecordsFailure("missing exact native FNV Player CLAS " + playerState.mClass.toString());
        if (playerClass->mId != playerState.mClass)
            return nativeRecordsFailure("native FNV Player CLAS resolved with the wrong typed FormID");
        if (!playerClass->mHasFalloutData)
            return nativeRecordsFailure("native FNV Player CLAS lacks exact 28-byte DATA");
        if (!playerClass->mHasFalloutAttributes)
            return nativeRecordsFailure("native FNV Player CLAS lacks exact 7-byte ATTR");

        const ESM4::Race* playerRace = races.search(ESM::RefId(playerState.mRace));
        if (playerRace == nullptr)
            return nativeRecordsFailure("missing exact native FNV Player RACE " + playerState.mRace.toString());
        if (playerRace->mId != playerState.mRace)
            return nativeRecordsFailure("native FNV Player RACE resolved with the wrong typed FormID");
        if (!playerRace->mHasFalloutData)
            return nativeRecordsFailure("native FNV Player RACE lacks exact 36-byte DATA");

        return { FalloutNativePlayerRecords{ baseNpc, playerClass, playerRace }, {} };
    }

    FalloutSaveLoadPlanResolution resolveFalloutSaveLoadPlan(const ESM4::FONVSaveGamePrefix& save,
        const FalloutPlayerState* nativePlayerState, std::span<const std::string> currentContentFiles)
    {
        constexpr std::string_view falloutMaster = "FalloutNV.esm";
        if (nativePlayerState == nullptr)
            return loadFailure("missing resolved native FNV Player state");

        const std::vector<std::size_t> currentMasterIndices
            = findContentFileIndices(currentContentFiles, falloutMaster);
        if (currentMasterIndices.empty())
            return loadFailure("current content has no FalloutNV.esm master for native Player FormID 0x00000007");
        if (currentMasterIndices.size() != 1)
            return loadFailure("current content has ambiguous duplicate FalloutNV.esm masters");

        const std::vector<std::size_t> saveMasterIndices = findSaveMasterIndices(save, falloutMaster);
        if (saveMasterIndices.empty())
            return loadFailure("FNV save master table has no FalloutNV.esm Player provenance");
        if (saveMasterIndices.size() != 1)
            return loadFailure("FNV save master table has ambiguous duplicate FalloutNV.esm Player provenance");

        const std::vector<std::size_t> currentPluginIndices = findCurrentFalloutPluginIndices(currentContentFiles);
        if (currentPluginIndices.size() != save.mMasters.size())
            return loadFailure("current ESM/ESP content count does not exactly match the FNV save master table");
        for (std::size_t position = 0; position < save.mMasters.size(); ++position)
        {
            const std::string& current = currentContentFiles[currentPluginIndices[position]];
            const std::string& expected = save.mMasters[position].mFileName.mValue;
            if (!Misc::StringUtils::ciEqual(current, expected))
            {
                return loadFailure("current ESM/ESP content order does not exactly match the FNV save master table at "
                    + std::to_string(position) + ": expected " + expected + ", got " + current);
            }
        }

        const std::size_t currentMasterIndex = currentMasterIndices.front();
        if (currentMasterIndex > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return loadFailure("current FalloutNV.esm master index cannot be represented by a normalized FormID");
        const ESM::FormId expectedPlayerId{ 7, static_cast<std::int32_t>(currentMasterIndex) };
        if (nativePlayerState->mBaseRecord != expectedPlayerId)
        {
            return loadFailure("resolved native FNV Player state is not exact winning FalloutNV.esm FormID "
                + expectedPlayerId.toString());
        }
        if (nativePlayerState->mEditorId != "Player")
            return loadFailure("resolved native FNV Player state does not have EDID Player");

        const ESM::FormId expectedPlayerReferenceId{ 0x14, static_cast<std::int32_t>(currentMasterIndex) };
        if (nativePlayerState->mReferenceRecord != expectedPlayerReferenceId
            || nativePlayerState->mReferenceBaseRecord != expectedPlayerId)
        {
            return loadFailure(
                "resolved native FNV Player identity does not preserve the exact FalloutNV.esm "
                "ACHR 0x00000014 -> NPC_ 0x00000007 relation");
        }

        const ESM4::FONVSaveChangedFormEnvelope* playerChange = nullptr;
        try
        {
            playerChange = &save.requirePlayerReferenceChangeForm();
        }
        catch (const ESM4::FONVSaveError& error)
        {
            return loadFailure(
                std::string("FNV save has no unique canonical Player ACHR change form: ") + error.what());
        }

        const std::uint32_t level = save.mHeader.mPlayerLevel.mValue;
        if (level == 0)
            return loadFailure("FNV save header player level must be nonzero");

        FalloutSaveLoadPlan plan;
        plan.mPlayer.mBaseRecord = expectedPlayerId;
        plan.mPlayer.mReferenceRecord = expectedPlayerReferenceId;
        plan.mPlayer.mSaveFalloutNewVegasMasterIndex = saveMasterIndices.front();
        plan.mPlayer.mCurrentFalloutNewVegasMasterIndex = currentMasterIndex;
        plan.mPlayer.mReferenceChangeFlags = playerChange->mChangeFlags.mValue;
        plan.mPlayer.mReferencePayloadOffset = playerChange->mUnparsedPayload.mRange.mOffset;
        plan.mPlayer.mReferencePayloadBytes = playerChange->mUnparsedPayload.mRange.mSize;
        plan.mPlayer.mSaveNumber = save.mHeader.mSaveNumber.mValue;
        plan.mPlayer.mName = save.mHeader.mPlayerName.mValue;
        plan.mPlayer.mKarmaTitle = save.mHeader.mPlayerKarmaTitle.mValue;
        plan.mPlayer.mLevel = level;
        plan.mPlayer.mLocationLabel = save.mHeader.mPlayerLocation.mValue;
        plan.mPlayer.mPlayTimeLabel = save.mHeader.mPlayTime.mValue;

        if (!save.mPlayerReferenceMovement)
            return loadFailure("FNV save does not expose a proven canonical Player reference-movement prefix");
        const ESM4::FONVSavePlayerReferenceMovement& movement = *save.mPlayerReferenceMovement;
        if (!movement.mCellOrWorldspace.mResolvedFormId)
            return loadFailure("FNV save Player movement CELL/WRLD RefID did not resolve");
        const std::optional<ESM::FormId> normalizedContainer
            = normalizeSavedFormId(*movement.mCellOrWorldspace.mResolvedFormId, currentPluginIndices);
        if (!normalizedContainer)
            return loadFailure("FNV save Player movement CELL/WRLD FormID has unsupported provenance");
        plan.mTransform.mCellOrWorldspaceRecord = *normalizedContainer;
        for (std::size_t index = 0; index < plan.mTransform.mPosition.size(); ++index)
        {
            plan.mTransform.mPosition[index] = movement.mPosition[index].mValue;
            plan.mTransform.mRotationRadians[index] = movement.mRotationRadians[index].mValue;
        }

        if (!save.mPlayerCharacterScalarReferenceState)
            return loadFailure("FNV save does not expose the canonical Player camera/FOV state");
        const ESM4::FONVSavePlayerCharacterScalarReferenceState& camera = *save.mPlayerCharacterScalarReferenceState;
        if (camera.mFirstPersonMode.mValue > 1)
            return loadFailure("FNV save Player camera mode is not a canonical boolean");
        if (!std::isfinite(camera.mFirstPersonModelFov.mValue) || camera.mFirstPersonModelFov.mValue <= 0.f
            || camera.mFirstPersonModelFov.mValue >= 180.f || !std::isfinite(camera.mWorldFov.mValue)
            || camera.mWorldFov.mValue <= 0.f || camera.mWorldFov.mValue >= 180.f)
        {
            return loadFailure("FNV save Player camera FOV values must be finite and in (0, 180)");
        }
        plan.mCamera.mThirdPersonMode = camera.mFirstPersonMode.mValue;
        plan.mCamera.mFirstPerson = camera.mFirstPersonMode.mValue == 0;
        plan.mCamera.mFirstPersonModelFov = camera.mFirstPersonModelFov.mValue;
        plan.mCamera.mWorldFov = camera.mWorldFov.mValue;
        plan.mCamera.mModeOffset = camera.mFirstPersonMode.mRange.mOffset;
        plan.mCamera.mFirstPersonModelFovOffset = camera.mFirstPersonModelFov.mRange.mOffset;
        plan.mCamera.mWorldFovOffset = camera.mWorldFov.mRange.mOffset;

        if (!save.mSky)
            return loadFailure("FNV save does not expose a proven Sky global-data payload");
        const ESM4::FONVSaveSkyState& sky = *save.mSky;
        if (!sky.mCurrentWeather.mResolvedFormId || !sky.mDefaultWeather.mResolvedFormId)
            return loadFailure("FNV save Sky state has no current/default weather identity");
        const std::optional<ESM::FormId> currentWeather
            = normalizeSavedFormId(*sky.mCurrentWeather.mResolvedFormId, currentPluginIndices);
        const std::optional<ESM::FormId> defaultWeather
            = normalizeSavedFormId(*sky.mDefaultWeather.mResolvedFormId, currentPluginIndices);
        if (!currentWeather || !defaultWeather)
            return loadFailure("FNV save Sky weather FormID has unsupported provenance");
        plan.mScene.mCurrentWeather = *currentWeather;
        plan.mScene.mDefaultWeather = *defaultWeather;
        auto normalizeOptionalWeather
            = [&](const ESM4::FONVSaveResolvedReferenceId& source, std::optional<ESM::FormId>& destination) {
                  if (!source.mResolvedFormId)
                      return true;
                  destination = normalizeSavedFormId(*source.mResolvedFormId, currentPluginIndices);
                  return destination.has_value();
              };
        if (!normalizeOptionalWeather(sky.mTransitionWeather, plan.mScene.mTransitionWeather)
            || !normalizeOptionalWeather(sky.mOverrideWeather, plan.mScene.mOverrideWeather))
        {
            return loadFailure("FNV save optional Sky weather FormID has unsupported provenance");
        }
        plan.mScene.mGameHour = sky.mGameHour.mValue;
        plan.mScene.mLastUpdateHour = sky.mLastUpdateHour.mValue;
        plan.mScene.mWeatherPercent = sky.mWeatherPercent.mValue;
        plan.mScene.mFogPower = sky.mFogPower.mValue;
        plan.mScene.mFlags = sky.mFlags.mValue;
        plan.mScene.mSkyMode = sky.mSkyMode.mValue;
        plan.mScene.mPayloadOffset = sky.mRange.mOffset;
        plan.mScene.mPayloadBytes = sky.mRange.mSize;
        plan.mUncoveredState = falloutSaveLoadBlockers();
        return { std::move(plan), {} };
    }

    FalloutExteriorPlayerPlacementResolution resolveFalloutExteriorPlayerPlacement(const Store<ESM4::World>& worlds,
        const Store<ESM4::Cell>& cells, const FalloutSaveLoadPlan::PlayerTransform& transform)
    {
        if (transform.mCellOrWorldspaceRecord.isZeroOrUnset())
            return placementFailure("Player movement has no normalized CELL/WRLD identity");

        const ESM::RefId worldspaceId(transform.mCellOrWorldspaceRecord);
        const ESM4::World* worldspace = worlds.search(worldspaceId);
        if (worldspace == nullptr)
        {
            return placementFailure(
                "Player movement container is not a loaded WRLD; interior CELL placement remains "
                "unsupported");
        }
        if (worldspace->mId != transform.mCellOrWorldspaceRecord)
            return placementFailure("Player movement WRLD resolved with a different normalized FormID");

        const ESM::ExteriorCellLocation location
            = ESM::positionToExteriorCellLocation(transform.mPosition[0], transform.mPosition[1], worldspaceId);
        const ESM4::Cell* cell = cells.searchExterior(location);
        if (cell == nullptr)
            return placementFailure("Player movement position has no authored exterior CELL in the resolved WRLD");
        if (!cell->isExterior() || cell->mParent != worldspaceId || cell->mX != location.mX || cell->mY != location.mY)
        {
            return placementFailure("Player movement exterior CELL identity does not match its WRLD/grid index");
        }
        const ESM::FormId* cellFormId = cell->mId.getIf<ESM::FormId>();
        if (cellFormId == nullptr)
            return placementFailure("Player movement exterior CELL does not preserve a native FormID identity");

        FalloutExteriorPlayerPlacement placement;
        placement.mWorldspaceRecord = transform.mCellOrWorldspaceRecord;
        placement.mCellRecord = *cellFormId;
        placement.mCellX = location.mX;
        placement.mCellY = location.mY;
        return { std::move(placement), {} };
    }

    void applyFalloutSavePlayerHeader(ESM::NPC& proxy, const FalloutSavePlayerHeaderState& state)
    {
        if (proxy.mId != ESM::RefId::stringRefId("Player"))
            throw std::runtime_error("FNV save header target is not the Player compatibility carrier");
        if (state.mBaseRecord.mIndex != 7 || state.mReferenceRecord.mIndex != 0x14 || state.mLevel == 0
            || state.mLevel > static_cast<std::uint32_t>(std::numeric_limits<std::int16_t>::max()))
        {
            throw std::runtime_error("invalid semantically available FNV save Player header state");
        }

        if (!state.mName.empty())
            proxy.mName = state.mName;
        proxy.mNpdt.mLevel = static_cast<std::int16_t>(state.mLevel);
    }

    void seedFalloutPlayerProxy(ESM::NPC& proxy, const FalloutPlayerState& state)
    {
        if (!state.mFullName.empty())
            proxy.mName = state.mFullName;
        proxy.mModel = state.mModel;
        proxy.mNpdt.mLevel = state.mStatsConfig.levelOrMult;
        proxy.mNpdt.mHealth = static_cast<std::uint16_t>(state.mHealth);
        proxy.mNpdt.mFatigue = state.mStatsConfig.fatigue;
        if ((state.mStatsConfig.flags & ESM4::Npc::FO3_Female) != 0)
            proxy.mFlags |= ESM::NPC::Female;
        else
            proxy.mFlags &= ~ESM::NPC::Female;
    }
}
