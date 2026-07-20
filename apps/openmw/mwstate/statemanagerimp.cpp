#include "statemanagerimp.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <SDL_clipboard.h>

#include <components/debug/debuglog.hpp>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm4/fonvsavegame.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/loadinglistener.hpp>

#include <components/files/conversion.hpp>
#include <components/misc/algorithm.hpp>
#include <components/settings/values.hpp>

#include <osg/Image>

#include <osgDB/Registry>

#include "../mwbase/dialoguemanager.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/journal.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/scriptmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvsavepreflight.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/scene.hpp"
#include "../mwworld/worldmodel.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"

#include "../mwscript/globalscripts.hpp"

#include "../mwvr/vrgui.hpp"

#include "quicksavemanager.hpp"

namespace
{
    bool readPlayableStartHour(float& hour)
    {
        const char* value = std::getenv("OPENMW_PLAYABLE_START_HOUR");
        if (value == nullptr || *value == '\0')
            return false;

        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        if (end == value || !std::isfinite(parsed))
            return false;

        hour = std::fmod(parsed, 24.f);
        if (hour < 0.f)
            hour += 24.f;
        return true;
    }
}

void MWState::StateManager::cleanup(bool force)
{
    mNativeFalloutSaveLoaded = false;

    if (mState != State_NoGame || force)
    {
        MWBase::Environment::get().getSoundManager()->clear();
        MWBase::Environment::get().getDialogueManager()->clear();
        MWBase::Environment::get().getJournal()->clear();
        MWBase::Environment::get().getScriptManager()->clear();
        MWBase::Environment::get().getWindowManager()->clear();
        MWBase::Environment::get().getWorld()->clear();
        MWBase::Environment::get().getInputManager()->clear();
        MWBase::Environment::get().getMechanicsManager()->clear();
        if (VR::getVR())
        {
            MWVR::VRGUIManager::instance().clearLua();
        }

        mCharacterManager.setCurrentCharacter(nullptr);
        mTimePlayed = 0;
        mLastSavegame.clear();
        MWMechanics::CreatureStats::cleanup();

        mState = State_NoGame;
        MWBase::Environment::get().getLuaManager()->noGame();
    }
    else
    {
        // TODO: do we need this cleanup?
        MWBase::Environment::get().getLuaManager()->clear();
    }
}

std::map<int, int> MWState::StateManager::buildContentFileIndexMap(const ESM::ESMReader& reader) const
{
    const std::vector<std::string>& current = MWBase::Environment::get().getWorld()->getContentFiles();

    const std::vector<ESM::Header::MasterData>& prev = reader.getGameFiles();

    std::map<int, int> map;

    for (int iPrev = 0; iPrev < static_cast<int>(prev.size()); ++iPrev)
    {
        for (int iCurrent = 0; iCurrent < static_cast<int>(current.size()); ++iCurrent)
            if (Misc::StringUtils::ciEqual(prev[iPrev].name, current[iCurrent]))
            {
                map.insert(std::make_pair(iPrev, iCurrent));
                break;
            }
    }

    return map;
}

MWState::StateManager::StateManager(const std::filesystem::path& saves, const std::vector<std::string>& contentFiles)
    : mQuitRequest(false)
    , mAskLoadRecent(false)
    , mState(State_NoGame)
    , mCharacterManager(saves, contentFiles)
    , mTimePlayed(0)
{
}

void MWState::StateManager::requestQuit()
{
    mQuitRequest = true;
}

bool MWState::StateManager::hasQuitRequest() const
{
    return mQuitRequest;
}

void MWState::StateManager::askLoadRecent()
{
    if (MWBase::Environment::get().getWindowManager()->getMode() == MWGui::GM_MainMenu)
        return;

    if (!mAskLoadRecent)
    {
        if (mLastSavegame.empty()) // no saves
        {
            MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_MainMenu);
        }
        else
        {
            std::string saveName = Files::pathToUnicodeString(mLastSavegame.filename());
            // Assume the last saved game belongs to the current character's slot list.
            const Character* character = getCurrentCharacter();
            if (character)
            {
                for (const auto& slot : *character)
                {
                    if (slot.mPath == mLastSavegame)
                    {
                        saveName = slot.mProfile.mDescription;
                        break;
                    }
                }
            }

            std::vector<std::string> buttons;
            buttons.emplace_back("#{Interface:Yes}");
            buttons.emplace_back("#{Interface:No}");
            std::string message
                = MWBase::Environment::get().getL10nManager()->getMessage("OMWEngine", "AskLoadLastSave");
            std::string_view tag = "%s";
            size_t pos = message.find(tag);
            message.replace(pos, tag.length(), saveName);
            MWBase::Environment::get().getWindowManager()->interactiveMessageBox(message, buttons);
            mAskLoadRecent = true;
        }
    }
}

MWState::StateManager::State MWState::StateManager::getState() const
{
    return mState;
}

void MWState::StateManager::newGame(bool bypass)
{
    cleanup();

    if (!bypass)
        MWBase::Environment::get().getWindowManager()->setNewGame(true);

    try
    {
        Log(Debug::Info) << "Starting a new game";
        MWBase::Environment::get().getScriptManager()->getGlobalScripts().addStartup();
        MWBase::Environment::get().getWorld()->startNewGame(bypass);

        mState = State_Running;
        MWBase::Environment::get().getLuaManager()->gameLoaded();

        MWBase::Environment::get().getWindowManager()->fadeScreenOut(0);
        MWBase::Environment::get().getWindowManager()->fadeScreenIn(1);
    }
    catch (std::exception& e)
    {
        std::stringstream error;
        error << "Failed to start new game: " << e.what();

        Log(Debug::Error) << error.str();
        cleanup(true);

        MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_MainMenu);

        std::vector<std::string> buttons;
        buttons.emplace_back("#{Interface:OK}");
        MWBase::Environment::get().getWindowManager()->interactiveMessageBox(error.str(), buttons);
    }
}

void MWState::StateManager::endGame()
{
    mState = State_Ended;
    MWBase::Environment::get().getLuaManager()->gameEnded();
}

void MWState::StateManager::resumeGame()
{
    mState = State_Running;
    MWBase::Environment::get().getLuaManager()->gameLoaded();
}

void MWState::StateManager::saveGame(std::string_view description, const Slot* slot)
{
    MWBase::Environment::get().getLuaManager()->applyDelayedActions();

    MWState::Character* character = getCurrentCharacter();

    try
    {
        const auto start = std::chrono::steady_clock::now();

        MWBase::Environment::get().getWindowManager()->asyncPrepareSaveMap();

        if (!character)
        {
            MWWorld::ConstPtr player = MWMechanics::getPlayer();
            const std::string& name = player.get<ESM::NPC>()->mBase->mName;

            character = mCharacterManager.createCharacter(name);
            mCharacterManager.setCurrentCharacter(character);
        }

        ESM::SavedGame profile;

        MWBase::World& world = *MWBase::Environment::get().getWorld();

        MWWorld::Ptr player = world.getPlayerPtr();

        profile.mContentFiles = world.getContentFiles();

        profile.mPlayerName = player.get<ESM::NPC>()->mBase->mName;
        profile.mPlayerLevel = player.getClass().getNpcStats(player).getLevel();

        const ESM::RefId& classId = player.get<ESM::NPC>()->mBase->mClass;
        if (world.getStore().get<ESM::Class>().isDynamic(classId))
            profile.mPlayerClassName = world.getStore().get<ESM::Class>().find(classId)->mName;
        else
            profile.mPlayerClassId = classId;

        const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);

        profile.mPlayerCellName = world.getCellName();
        profile.mInGameTime = world.getTimeManager()->getEpochTimeStamp();
        profile.mTimePlayed = mTimePlayed;
        profile.mDescription = description;
        profile.mCurrentDay = world.getTimeManager()->getTimeStamp().getDay();
        profile.mCurrentHealth = stats.getHealth().getCurrent();
        profile.mMaximumHealth = stats.getHealth().getModified();

        Log(Debug::Info) << "Making a screenshot for saved game '" << description << "'";
        writeScreenshot(profile.mScreenshot);

        if (!slot)
            slot = character->createSlot(profile);
        else
            slot = character->updateSlot(slot, profile);

        // Make sure the animation state held by references is up to date before saving the game.
        MWBase::Environment::get().getMechanicsManager()->persistAnimationStates();

        Log(Debug::Info) << "Writing saved game '" << description << "' for character '" << profile.mPlayerName << "'";

        // Write to a memory stream first. If there is an exception during the save process, we don't want to trash the
        // existing save file we are overwriting.
        std::stringstream stream;

        ESM::ESMWriter writer;

        for (const std::string& contentFile : MWBase::Environment::get().getWorld()->getContentFiles())
            writer.addMaster(contentFile, 0); // not using the size information anyway -> use value of 0

        writer.setFormatVersion(ESM::CurrentSaveGameFormatVersion);

        // all unused
        writer.setVersion(0);
        writer.setType(0);
        writer.setAuthor("");
        writer.setDescription("");

        int recordCount = 1 // saved game header
            + MWBase::Environment::get().getJournal()->countSavedGameRecords()
            + MWBase::Environment::get().getLuaManager()->countSavedGameRecords()
            + MWBase::Environment::get().getWorld()->countSavedGameRecords()
            + MWBase::Environment::get().getScriptManager()->getGlobalScripts().countSavedGameRecords()
            + MWBase::Environment::get().getDialogueManager()->countSavedGameRecords()
            + MWBase::Environment::get().getMechanicsManager()->countSavedGameRecords()
            + MWBase::Environment::get().getInputManager()->countSavedGameRecords()
            + MWBase::Environment::get().getWindowManager()->countSavedGameRecords();
        writer.setRecordCount(recordCount);

        writer.save(stream);

        Loading::Listener& listener = *MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        // Using only Cells for progress information, since they typically have the largest records by far
        listener.setProgressRange(MWBase::Environment::get().getWorld()->countSavedGameCells());
        listener.setLabel("#{OMWEngine:SavingInProgress}", true);

        Loading::ScopedLoad load(&listener);

        writer.startRecord(ESM::REC_SAVE);
        slot->mProfile.save(writer);
        writer.endRecord(ESM::REC_SAVE);

        MWBase::Environment::get().getJournal()->write(writer, listener);
        MWBase::Environment::get().getDialogueManager()->write(writer, listener);
        // LuaManager::write should be called before World::write because world also saves
        // local scripts that depend on LuaManager.
        MWBase::Environment::get().getLuaManager()->write(writer, listener);
        MWBase::Environment::get().getWorld()->write(writer, listener);
        MWBase::Environment::get().getScriptManager()->getGlobalScripts().write(writer, listener);
        MWBase::Environment::get().getMechanicsManager()->write(writer, listener);
        MWBase::Environment::get().getInputManager()->write(writer, listener);
        MWBase::Environment::get().getWindowManager()->write(writer, listener);

        // Ensure we have written the number of records that was estimated
        if (writer.getRecordCount() != recordCount + 1) // 1 extra for TES3 record
            Log(Debug::Warning) << "Warning: number of written savegame records does not match. Estimated: "
                                << recordCount + 1 << ", written: " << writer.getRecordCount();

        writer.close();

        if (stream.fail())
            throw std::runtime_error(
                "Write operation failed (memory stream): " + std::generic_category().message(errno));

        // All good, write to file
        std::ofstream filestream(slot->mPath, std::ios::binary);
        filestream << stream.rdbuf();

        if (filestream.fail())
            throw std::runtime_error("Write operation failed (file stream): " + std::generic_category().message(errno));

        Settings::saves().mCharacter.set(Files::pathToUnicodeString(slot->mPath.parent_path().filename()));
        mLastSavegame = slot->mPath;

        const auto finish = std::chrono::steady_clock::now();

        Log(Debug::Info) << '\'' << description << "' is saved in "
                         << std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(finish - start).count()
                         << "ms";
    }
    catch (const std::exception& e)
    {
        std::stringstream error;
        error << "Failed to save game: " << e.what();

        Log(Debug::Error) << error.str();

        std::vector<std::string> buttons;
        buttons.emplace_back("#{Interface:OK}");
        MWBase::Environment::get().getWindowManager()->interactiveMessageBox(error.str(), buttons);

        // If no file was written, clean up the slot
        if (character && slot && !std::filesystem::exists(slot->mPath))
        {
            character->deleteSlot(slot);
            character->cleanup();
        }
    }
}

void MWState::StateManager::quickSave(std::string name)
{
    if (!(mState == State_Running
            && MWBase::Environment::get().getWorld()->getGlobalInt(MWWorld::Globals::sCharGenState) == -1 // char gen
            && MWBase::Environment::get().getWindowManager()->isSavingAllowed()))
    {
        // You can not save your game right now
        MWBase::Environment::get().getWindowManager()->messageBox("#{OMWEngine:SaveGameDenied}");
        return;
    }

    Character* currentCharacter = getCurrentCharacter(); // Get current character
    QuickSaveManager saveFinder(name, Settings::saves().mMaxQuicksaves);

    if (currentCharacter)
    {
        for (auto& save : *currentCharacter)
        {
            // Visiting slots allows the quicksave finder to find the oldest quicksave
            saveFinder.visitSave(&save);
        }
    }

    // Once all the saves have been visited, the save finder can tell us which
    // one to replace (or create)
    saveGame(name, saveFinder.getNextQuickSaveSlot());
}

void MWState::StateManager::loadGame(const std::filesystem::path& filepath)
{
    for (const auto& character : mCharacterManager)
    {
        for (const auto& slot : character)
        {
            if (slot.mPath == filepath)
            {
                loadGame(&character, slot.mPath);
                return;
            }
        }
    }

    MWState::Character* character = getCurrentCharacter();
    loadGame(character, filepath);
}

struct SaveFormatVersionError : public std::exception
{
    using std::exception::exception;

    SaveFormatVersionError(ESM::FormatVersion savegameFormat, const std::string& message)
        : mSavegameFormat(savegameFormat)
        , mErrorMessage(message)
    {
    }

    const char* what() const noexcept override { return mErrorMessage.c_str(); }
    ESM::FormatVersion getFormatVersion() const { return mSavegameFormat; }

protected:
    ESM::FormatVersion mSavegameFormat = ESM::DefaultFormatVersion;
    std::string mErrorMessage;
};

struct SaveVersionTooOldError : SaveFormatVersionError
{
    SaveVersionTooOldError(ESM::FormatVersion savegameFormat)
        : SaveFormatVersionError(savegameFormat, "format version " + std::to_string(savegameFormat) + " is too old")
    {
    }
};

struct SaveVersionTooNewError : SaveFormatVersionError
{
    SaveVersionTooNewError(ESM::FormatVersion savegameFormat)
        : SaveFormatVersionError(savegameFormat, "format version " + std::to_string(savegameFormat) + " is too new")
    {
    }
};

void MWState::StateManager::loadGame(const Character* character, const std::filesystem::path& filepath)
{
    // A normal retail .fos is not an ESM3 save. Resolve every input before cleanup: the legacy catch path and cleanup
    // both mutate the session, while a failed native preflight must leave it untouched.
    if (MWWorld::isFalloutNewVegasSavePath(filepath))
    {
        Log(Debug::Info) << "Preflighting native FNV save file " << filepath.filename();
        ESM4::FONVSaveGamePrefix save = ESM4::readFONVSaveGamePrefix(filepath);
        const MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWWorld::FalloutSavePreflightResolution preflight = MWWorld::resolveFalloutSavePreflightContext(
            std::move(save), world.getStore(), world.getContentFiles());
        if (!preflight)
            throw std::runtime_error("native FNV save preflight failed: " + preflight.mError);

        const MWWorld::FalloutSavePreflightContext& context = *preflight.mContext;
        MWWorld::requireFalloutSaveVisualApplicationReady(context);

        if (const char* trace = std::getenv("OPENMW_FNV_VATS_TRACE"); trace != nullptr && *trace != '\0'
            && context.mSave.mPlayerActorValueData)
        {
            constexpr std::size_t actionPointsActorValue = 12;
            const ESM4::FONVSavePlayerActorValueData& values = *context.mSave.mPlayerActorValueData;
            Log(Debug::Info) << "FNV VATS retail state: actorValue=" << actionPointsActorValue
                             << " values244=" << values.mActorValues244[actionPointsActorValue].mValue
                             << " values244Offset=" << values.mActorValues244[actionPointsActorValue].mRange.mOffset
                             << " values378=" << values.mActorValues378[actionPointsActorValue].mValue
                             << " values378Offset=" << values.mActorValues378[actionPointsActorValue].mRange.mOffset
                             << " values4B0=" << values.mActorValues4B0[actionPointsActorValue].mValue
                             << " values4B0Offset=" << values.mActorValues4B0[actionPointsActorValue].mRange.mOffset
                             << " exact=1";
        }

        const ESM::RefId playerId = ESM::RefId::stringRefId("Player");
        const ESM::NPC* playerCarrier = world.getStore().get<ESM::NPC>().searchStatic(playerId);
        if (playerCarrier == nullptr)
            throw std::runtime_error("native FNV visual application has no validated Player compatibility carrier");
        ESM::NPC savedPlayer = *playerCarrier;
        MWWorld::applyFalloutSavePlayerHeader(savedPlayer, context.mPlan.mPlayer);
        Log(Debug::Info) << "Native FNV save Player inventory: stacks="
                         << context.mPlan.mPlayer.mInventoryItems.size() << " worn="
                         << context.mPlan.mPlayer.mWornVisualItems.size();
        for (const MWWorld::FalloutInventoryItem& item : context.mPlan.mPlayer.mInventoryItems)
        {
            const ESM::RefId record(item.mRecord);
            Log(Debug::Verbose) << "Native FNV save Player inventory item: form=" << record
                                << " count=" << item.mCount << " type=" << world.getStore().find(record);
        }

        ESM::Position savedPosition;
        for (std::size_t index = 0; index < 3; ++index)
        {
            savedPosition.pos[index] = context.mPlan.mTransform.mPosition[index];
            savedPosition.rot[index] = context.mPlan.mTransform.mRotationRadians[index];
        }
        const ESM::RefId savedCell(context.mPlacement.mCellRecord);
        const ESM::RefId savedWeather(context.mWeather.mCurrent.mWeather);
        if (world.getWeather(savedWeather) == nullptr)
        {
            throw std::runtime_error(
                "native FNV visual application current WTHR was not imported by WeatherManager");
        }
        const float savedFirstPersonFov
            = MWWorld::convertFalloutReferenceFovToOpenMwVertical(context.mPlan.mCamera.mFirstPersonModelFov);
        const float savedWorldFov
            = MWWorld::convertFalloutReferenceFovToOpenMwVertical(context.mPlan.mCamera.mWorldFov);

        std::ostringstream uncovered;
        for (const std::string& domain : context.mPlan.mUncoveredState)
        {
            if (uncovered.tellp() > 0)
                uncovered << ", ";
            uncovered << domain;
        }
        Log(Debug::Warning) << "Loading native FNV visual slice through the normal .fos path; full gameplay state "
                            << "remains uncovered: " << uncovered.str();

        cleanup();

        MWBase::World& mutableWorld = *MWBase::Environment::get().getWorld();
        mutableWorld.getStore().overrideRecord(savedPlayer);
        mCharacterManager.setCurrentCharacter(character);
        mState = State_Running;
        if (character)
            Settings::saves().mCharacter.set(Files::pathToUnicodeString(character->getPath().filename()));
        mLastSavegame = filepath;

        MWBase::Environment::get().getWindowManager()->setNewGame(false);
        mutableWorld.saveLoaded();
        if (context.mPlan.mQuestProgress)
        {
            std::string error;
            if (!mutableWorld.getESM4QuestRuntime().loadSavedProgress(*context.mPlan.mQuestProgress, &error))
                throw std::runtime_error("native FNV quest-progress application failed after preflight: " + error);
        }
        mutableWorld.setupPlayer();

        MWWorld::Ptr player = mutableWorld.getPlayerPtr();
        // setupPlayer replaces the Player base record but intentionally retains the live reference data.  Native
        // Fallout saves replace the authored compatibility-carrier inventory, so retaining that custom data would
        // leave the pre-load inventory store (and its two placeholder entries) alive.  Rebuild it from savedPlayer
        // before rendering or opening any inventory UI.
        player.getRefData().setCustomData(nullptr);
        MWWorld::InventoryStore& savedInventory = player.getClass().getInventoryStore(player);
        savedInventory.unequipAll();
        for (const MWWorld::FalloutSavePlayerHeaderState::ConditionedStack& stack
            : context.mPlan.mPlayer.mConditionedStacks)
        {
            const ESM::RefId record(stack.mRecord);
            MWWorld::ContainerStoreIterator found = savedInventory.end();
            for (MWWorld::ContainerStoreIterator item = savedInventory.begin(); item != savedInventory.end(); ++item)
            {
                if (item->getCellRef().getRefId() == record && item->getCellRef().getCharge() == -1
                    && item->getCellRef().getCount(false) >= stack.mCount)
                {
                    found = item;
                    break;
                }
            }
            if (found == savedInventory.end())
                throw std::runtime_error("native FNV conditioned item is absent from rebuilt Player inventory");

            if (found->getCellRef().getCount(false) > stack.mCount)
                savedInventory.unstack(*found, stack.mCount);
            const int maximumHealth = found->getClass().getItemMaxHealth(*found);
            const int health = static_cast<int>(std::lround(stack.mHealth));
            if (maximumHealth <= 0 || health < 0 || health > maximumHealth)
                throw std::runtime_error("native FNV conditioned item escaped preflight health validation");
            found->getCellRef().setCharge(health);
            Log(Debug::Info) << "Native FNV save Player restored ExtraHealth stack: form=" << record
                             << " count=" << stack.mCount << " health=" << health
                             << " sourceOffset=" << stack.mSourceOffset;
        }
        for (const MWWorld::FalloutSavePlayerHeaderState::AmmoSelection& selection
            : context.mPlan.mPlayer.mAmmoSelections)
        {
            const ESM::RefId weapon(selection.mWeapon);
            const ESM::RefId ammo(selection.mAmmo);
            savedInventory.setFalloutAmmoSelection(weapon, ammo);
            Log(Debug::Info) << "Native FNV save Player restored selected ammo: weapon=" << weapon
                             << " ammo=" << ammo << " savedCount=" << selection.mSavedCount
                             << " sourceOffset=" << selection.mSourceOffset;
        }
        std::size_t runtimeStacks = 0;
        std::size_t visibleStacks = 0;
        for (MWWorld::ContainerStoreIterator item = savedInventory.begin(); item != savedInventory.end(); ++item)
        {
            ++runtimeStacks;
            if (item->getClass().showsInInventory(*item))
                ++visibleStacks;
        }
        Log(Debug::Info) << "Native FNV save Player runtime inventory rebuilt: stacks=" << runtimeStacks
                         << " visible=" << visibleStacks;
        for (const MWWorld::FalloutSavePlayerHeaderState::WornVisualItem& worn
            : context.mPlan.mPlayer.mWornVisualItems)
        {
            const ESM::RefId record(worn.mRecord);
            MWWorld::ContainerStoreIterator found = savedInventory.end();
            for (MWWorld::ContainerStoreIterator item = savedInventory.begin(); item != savedInventory.end(); ++item)
            {
                if (item->getCellRef().getRefId() == record
                    && (!worn.mHealth
                        || item->getClass().getItemHealth(*item) == static_cast<int>(std::lround(*worn.mHealth))))
                {
                    found = item;
                    break;
                }
            }
            if (found == savedInventory.end())
                throw std::runtime_error("native FNV ExtraWorn item is absent from rebuilt Player inventory");

            const std::vector<int>& slots = found->getClass().getEquipmentSlots(*found).first;
            if (slots.empty())
                throw std::runtime_error("native FNV ExtraWorn item has no compatible runtime equipment slot");
            savedInventory.equip(slots.front(), found);
            Log(Debug::Info) << "Native FNV save Player equipped ExtraWorn: form=" << record
                             << " slot=" << slots.front() << " name=" << found->getClass().getName(*found);
        }
        player.getClass().getCreatureStats(player).setDrawState(context.mPlan.mPlayer.mWeaponDrawn
                ? MWMechanics::DrawState::Weapon
                : MWMechanics::DrawState::Nothing);
        Log(Debug::Info) << "Native FNV save Player restored weapon stance: drawn="
                         << context.mPlan.mPlayer.mWeaponDrawn;
        for (const MWWorld::FalloutSavePlayerHeaderState::HotkeyItem& hotkey
            : context.mPlan.mPlayer.mHotkeyItems)
        {
            const ESM::RefId item(hotkey.mRecord);
            if (!MWBase::Environment::get().getWindowManager()->setFalloutSaveQuickKey(hotkey.mIndex, item))
                throw std::runtime_error("native FNV Player hotkey escaped preflight inventory validation");
            Log(Debug::Info) << "Native FNV save Player restored hotkey: index=" << static_cast<int>(hotkey.mIndex)
                             << " form=" << item << " sourceOffset=" << hotkey.mSourceOffset;
        }
        player.getRefData().setPosition(savedPosition);
        player.getCellRef().setPosition(savedPosition);
        std::vector<ESM::FormId> wornVisualItems;
        wornVisualItems.reserve(context.mPlan.mPlayer.mWornVisualItems.size());
        for (std::size_t ordinal = 0; ordinal < context.mPlan.mPlayer.mWornVisualItems.size(); ++ordinal)
        {
            const MWWorld::FalloutSavePlayerHeaderState::WornVisualItem& item
                = context.mPlan.mPlayer.mWornVisualItems[ordinal];
            wornVisualItems.push_back(item.mRecord);
            Log(Debug::Info) << "Native FNV save ExtraWorn visual: ordinal=" << ordinal + 1
                             << " form=" << ESM::RefId(item.mRecord)
                             << " sourceOffset=" << item.mSourceOffset;
        }
        mutableWorld.getRenderingManager()->setFalloutSaveWornVisualItems(std::move(wornVisualItems));
        mutableWorld.getRenderingManager()->setFirstPersonFieldOfView(savedFirstPersonFov);
        mutableWorld.renderPlayer();
        MWBase::Environment::get().getWindowManager()->updatePlayer();
        MWBase::Environment::get().getMechanicsManager()->playerLoaded();
        mutableWorld.toggleVanityMode(false);

        mutableWorld.getRenderingManager()->setFieldOfView(savedWorldFov);
        mutableWorld.setGlobalFloat(MWWorld::Globals::sGameHour, context.mPlan.mScene.mGameHour);
        if (!mutableWorld.forceWeather(savedWeather))
            throw std::runtime_error("native FNV visual application lost its preflighted current WTHR");

        // Preserve the authored Z and then update the persistent player render node and physics actor explicitly.
        mutableWorld.changeToCell(savedCell, savedPosition, false, false);
        player = mutableWorld.moveObject(mutableWorld.getPlayerPtr(), savedPosition.asVec3());
        mutableWorld.rotateObject(player, savedPosition.asRotationVec3());

        // Camera tracking uses inverse player Euler angles. Apply the save-owned view only after the final cell,
        // render-node and physics transforms exist, so no transitional/default camera state can win a frame.
        MWRender::Camera* camera = mutableWorld.getCamera();
        if (camera == nullptr)
            throw std::runtime_error("native FNV visual application has no player camera");
        camera->attachTo(player);
        camera->setMode(context.mPlan.mCamera.mFirstPerson ? MWRender::Camera::Mode::FirstPerson
                                                          : MWRender::Camera::Mode::ThirdPerson,
            true);
        if (context.mPlan.mCamera.mFirstPerson)
            camera->setPreferredCameraDistance(0.f);
        camera->processViewChange();
        camera->instantTransition();
        camera->setPitch(-savedPosition.rot[0], true);
        camera->setYaw(-savedPosition.rot[2], true);
        camera->setRoll(-savedPosition.rot[1]);
        camera->update(0.f, false);
        camera->updateCamera();
        mutableWorld.updateProjectilesCasters();
        MWBase::Environment::get().getWorldScene()->markCellAsUnchanged();
        MWBase::Environment::get().getLuaManager()->gameLoaded();
        mNativeFalloutSaveLoaded = true;
        Log(Debug::Info) << "Native FNV save owns camera mode=" << static_cast<int>(camera->getMode())
                         << " pitch=" << camera->getPitch() << " yaw=" << camera->getYaw()
                         << " roll=" << camera->getRoll() << " worldFov=" << savedWorldFov
                         << " firstPersonModelFov=" << savedFirstPersonFov;
        return;
    }

    try
    {
        cleanup();

        Log(Debug::Info) << "Reading save file " << filepath.filename();

        ESM::ESMReader reader;
        reader.open(filepath);

        ESM::FormatVersion version = reader.getFormatVersion();
        if (version > ESM::CurrentSaveGameFormatVersion)
            throw SaveVersionTooNewError(version);
        else if (version < ESM::MinSupportedSaveGameFormatVersion)
            throw SaveVersionTooOldError(version);

        std::map<int, int> contentFileMap = buildContentFileIndexMap(reader);
        reader.setContentFileMapping(&contentFileMap);
        MWBase::Environment::get().getLuaManager()->setContentFileMapping(contentFileMap);

        Loading::Listener& listener = *MWBase::Environment::get().getWindowManager()->getLoadingScreen();

        listener.setProgressRange(100);
        listener.setLabel("#{OMWEngine:LoadingInProgress}");

        Loading::ScopedLoad load(&listener);

        bool firstPersonCam = false;

        size_t total = reader.getFileSize();
        int currentPercent = 0;
        while (reader.hasMoreRecs())
        {
            ESM::NAME n = reader.getRecName();
            reader.getRecHeader();

            switch (n.toInt())
            {
                case ESM::REC_SAVE:
                {
                    ESM::SavedGame profile;
                    profile.load(reader);
                    const auto& selectedContentFiles = MWBase::Environment::get().getWorld()->getContentFiles();
                    auto missingFiles = profile.getMissingContentFiles(selectedContentFiles);
                    if (!missingFiles.empty() && !confirmLoading(missingFiles))
                    {
                        cleanup(true);
                        MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_MainMenu);
                        return;
                    }
                    mTimePlayed = profile.mTimePlayed;
                    Log(Debug::Info) << "Loading saved game '" << profile.mDescription << "' for character '"
                                     << profile.mPlayerName << "'";
                }
                break;

                case ESM::REC_JOUR:
                case ESM::REC_QUES:

                    MWBase::Environment::get().getJournal()->readRecord(reader, n.toInt());
                    break;

                case ESM::REC_DIAS:

                    MWBase::Environment::get().getDialogueManager()->readRecord(reader, n.toInt());
                    break;

                case ESM::REC_ALCH:
                case ESM::REC_MISC:
                case ESM::REC_ACTI:
                case ESM::REC_ARMO:
                case ESM::REC_BOOK:
                case ESM::REC_CLAS:
                case ESM::REC_CLOT:
                case ESM::REC_ENCH:
                case ESM::REC_NPC_:
                case ESM::REC_SPEL:
                case ESM::REC_WEAP:
                case ESM::REC_GLOB:
                case ESM::REC_PLAY:
                case ESM::REC_CSTA:
                case ESM::REC_WTHR:
                case ESM::REC_DYNA:
                case ESM::REC_ACTC:
                case ESM::REC_PROJ:
                case ESM::REC_MPRJ:
                case ESM::REC_ENAB:
                case ESM::REC_LEVC:
                case ESM::REC_LEVI:
                case ESM::REC_LIGH:
                case ESM::REC_CREA:
                case ESM::REC_CONT:
                case ESM::REC_RAND:
                case ESM::REC_FQST:
                case ESM::REC_FPLR:
                    MWBase::Environment::get().getWorld()->readRecord(reader, n.toInt());
                    break;

                case ESM::REC_CAM_:
                    reader.getHNT(firstPersonCam, "FIRS");
                    break;

                case ESM::REC_GSCR:

                    MWBase::Environment::get().getScriptManager()->getGlobalScripts().readRecord(reader, n.toInt());
                    break;

                case ESM::REC_GMAP:
                case ESM::REC_KEYS:
                case ESM::REC_ASPL:
                case ESM::REC_MARK:

                    MWBase::Environment::get().getWindowManager()->readRecord(reader, n.toInt());
                    break;

                case ESM::REC_DCOU:
                case ESM::REC_STLN:

                    MWBase::Environment::get().getMechanicsManager()->readRecord(reader, n.toInt());
                    break;

                case ESM::REC_INPU:
                    MWBase::Environment::get().getInputManager()->readRecord(reader, n.toInt());
                    break;

                case ESM::REC_LUAM:
                    MWBase::Environment::get().getLuaManager()->readRecord(reader, n.toInt());
                    break;

                default:

                    // ignore invalid records
                    Log(Debug::Warning) << "Warning: Ignoring unknown record: " << n.toStringView();
                    reader.skipRecord();
            }
            int progressPercent = static_cast<int>(float(reader.getFileOffset()) / total * 100);
            if (progressPercent > currentPercent)
            {
                listener.increaseProgress(progressPercent - currentPercent);
                currentPercent = progressPercent;
            }
        }

        mCharacterManager.setCurrentCharacter(character);

        mState = State_Running;

        if (character)
            Settings::saves().mCharacter.set(Files::pathToUnicodeString(character->getPath().filename()));
        mLastSavegame = filepath;

        MWBase::Environment::get().getWindowManager()->setNewGame(false);
        MWBase::Environment::get().getWorld()->saveLoaded();
        MWBase::Environment::get().getWorld()->setupPlayer();
        MWBase::Environment::get().getWorld()->renderPlayer();
        MWBase::Environment::get().getWindowManager()->updatePlayer();
        MWBase::Environment::get().getMechanicsManager()->playerLoaded();
        MWBase::Environment::get().getWorld()->toggleVanityMode(false);

        if (firstPersonCam != MWBase::Environment::get().getWorld()->isFirstPerson())
            MWBase::Environment::get().getWorld()->togglePOV();

        float playableStartHour = 0.f;
        if (readPlayableStartHour(playableStartHour))
        {
            MWBase::Environment::get().getWorld()->setGlobalFloat(MWWorld::Globals::sGameHour, playableStartHour);
            MWBase::Environment::get().getWorld()->advanceTime(0.0, false);
            Log(Debug::Info) << "OpenMW playable start hour=" << playableStartHour << " applied before cell insertion";
        }

        MWWorld::ConstPtr ptr = MWMechanics::getPlayer();

        if (ptr.isInCell())
        {
            const ESM::RefId cellId = ptr.getCell()->getCell()->getId();

            // Use detectWorldSpaceChange=false, otherwise some of the data we just loaded would be cleared again
            MWBase::Environment::get().getWorld()->changeToCell(cellId, ptr.getRefData().getPosition(), false, false);
        }
        else
        {
            // Cell no longer exists (i.e. changed game files), choose a default cell
            Log(Debug::Warning) << "Player character's cell no longer exists, changing to the default cell";
            ESM::ExteriorCellLocation cellIndex(0, 0, ESM::Cell::sDefaultWorldspaceId);
            MWWorld::CellStore& cell = MWBase::Environment::get().getWorldModel()->getExterior(cellIndex);
            const osg::Vec2f posFromIndex = ESM::indexToPosition(cellIndex, false);
            ESM::Position pos;
            pos.pos[0] = posFromIndex.x();
            pos.pos[1] = posFromIndex.y();
            pos.pos[2] = 0; // should be adjusted automatically (adjustPlayerPos=true)
            pos.rot[0] = 0;
            pos.rot[1] = 0;
            pos.rot[2] = 0;
            MWBase::Environment::get().getWorld()->changeToCell(cell.getCell()->getId(), pos, true, false);
        }

        MWBase::Environment::get().getWorld()->updateProjectilesCasters();

        // Vanilla MW will restart startup scripts when a save game is loaded. This is unintuitive,
        // but some mods may be using it as a reload detector.
        MWBase::Environment::get().getScriptManager()->getGlobalScripts().addStartup();

        // Since we passed "changeEvent=false" to changeCell, we shouldn't have triggered the cell change flag.
        // But make sure the flag is cleared anyway in case it was set from an earlier game.
        MWBase::Environment::get().getWorldScene()->markCellAsUnchanged();

        MWBase::Environment::get().getLuaManager()->gameLoaded();
    }
    catch (const SaveVersionTooNewError& e)
    {
        std::string error = "#{OMWEngine:LoadingRequiresNewVersionError}";
        printSavegameFormatError(e.what(), error);
    }
    catch (const SaveVersionTooOldError& e)
    {
        const char* release;
        // Report the last version still capable of reading this save
        if (e.getFormatVersion() <= ESM::OpenMW0_48SaveGameFormatVersion)
            release = "OpenMW 0.48.0";
        else if (e.getFormatVersion() <= ESM::OpenMW0_49SaveGameFormatVersion)
            release = "OpenMW 0.49.0";
        else
        {
            // Insert additional else if statements above to cover future releases
            static_assert(ESM::MinSupportedSaveGameFormatVersion <= ESM::OpenMW0_50SaveGameFormatVersion);
            release = "OpenMW 0.50.0";
        }
        auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
        std::string error = l10n->formatMessage("LoadingRequiresOldVersionError", { "version" }, { release });
        printSavegameFormatError(e.what(), error);
    }
    catch (const std::exception& e)
    {
        std::string error = "#{OMWEngine:LoadingFailed}: " + std::string(e.what());
        printSavegameFormatError(e.what(), error);
    }
}

void MWState::StateManager::printSavegameFormatError(
    const std::string& exceptionText, const std::string& messageBoxText)
{
    Log(Debug::Error) << "Failed to load saved game: " << exceptionText;

    cleanup(true);

    MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_MainMenu);

    std::vector<std::string> buttons;
    buttons.emplace_back("#{Interface:OK}");

    MWBase::Environment::get().getWindowManager()->interactiveMessageBox(messageBoxText, buttons);
}

void MWState::StateManager::quickLoad()
{
    if (Character* currentCharacter = getCurrentCharacter())
    {
        if (currentCharacter->begin() == currentCharacter->end())
            return;
        // use requestLoad, otherwise we can crash by loading during the wrong part of the frame
        requestLoad(currentCharacter->begin()->mPath);
    }
}

void MWState::StateManager::deleteGame(const MWState::Character* character, const MWState::Slot* slot)
{
    const std::filesystem::path savePath = slot->mPath;
    mCharacterManager.deleteSlot(slot, character);
    if (mLastSavegame == savePath)
    {
        if (character != nullptr)
            mLastSavegame = character->begin()->mPath;
        else
            mLastSavegame.clear();
    }
}

MWState::Character* MWState::StateManager::getCurrentCharacter()
{
    return mCharacterManager.getCurrentCharacter();
}

MWState::StateManager::CharacterIterator MWState::StateManager::characterBegin()
{
    return mCharacterManager.begin();
}

MWState::StateManager::CharacterIterator MWState::StateManager::characterEnd()
{
    return mCharacterManager.end();
}

void MWState::StateManager::update(float duration)
{
    mTimePlayed += duration;

    // Note: It would be nicer to trigger this from InputManager, i.e. the very beginning of the frame update.
    if (mAskLoadRecent)
    {
        int iButton = MWBase::Environment::get().getWindowManager()->readPressedButton();
        MWState::Character* curCharacter = getCurrentCharacter();
        if (iButton == 0 && curCharacter)
        {
            mAskLoadRecent = false;
            // Load last saved game for current character
            // loadGame resets the game state along with mLastSavegame so we want to preserve it
            const std::filesystem::path filePath = std::move(mLastSavegame);
            loadGame(curCharacter, filePath);
        }
        else if (iButton == 1)
        {
            mAskLoadRecent = false;
            MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_MainMenu);
        }
    }

    if (mNewGameRequest)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_MainMenu);
        newGame();
        mNewGameRequest = false;
    }

    if (mLoadRequest)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_MainMenu);
        loadGame(*mLoadRequest);
        mLoadRequest = std::nullopt;
    }
}

bool MWState::StateManager::confirmLoading(const std::vector<std::string_view>& missingFiles) const
{
    std::ostringstream stream;
    for (auto& contentFile : missingFiles)
    {
        Log(Debug::Warning) << "Warning: Saved game dependency " << contentFile << " is missing.";
        stream << contentFile << "\n";
    }

    auto fullList = stream.str();
    if (!fullList.empty())
        fullList.pop_back();

    constexpr size_t missingPluginsDisplayLimit = 12;

    std::vector<std::string> buttons;
    buttons.emplace_back("#{Interface:Yes}");
    buttons.emplace_back("#{Interface:Copy}");
    buttons.emplace_back("#{Interface:No}");
    std::string message = "#{OMWEngine:MissingContentFilesConfirmation}";

    auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
    message += l10n->formatMessage("MissingContentFilesList", { "files" }, { static_cast<int>(missingFiles.size()) });
    auto cappedSize = std::min(missingFiles.size(), missingPluginsDisplayLimit);
    if (cappedSize == missingFiles.size())
    {
        message += fullList;
    }
    else
    {
        for (size_t i = 0; i < cappedSize - 1; ++i)
        {
            message += missingFiles[i];
            message += "\n";
        }

        message += "...";
    }

    message
        += l10n->formatMessage("MissingContentFilesListCopy", { "files" }, { static_cast<int>(missingFiles.size()) });

    int selectedButton = -1;
    while (true)
    {
        auto windowManager = MWBase::Environment::get().getWindowManager();
        windowManager->interactiveMessageBox(message, buttons, true, selectedButton);
        selectedButton = windowManager->readPressedButton();
        if (selectedButton == 0)
            break;

        if (selectedButton == 1)
        {
            SDL_SetClipboardText(fullList.c_str());
            continue;
        }

        return false;
    }

    return true;
}

void MWState::StateManager::writeScreenshot(std::vector<char>& imageData) const
{
    int screenshotW = 259 * 2, screenshotH = 133 * 2; // *2 to get some nice antialiasing

    osg::ref_ptr<osg::Image> screenshot(new osg::Image);

    MWBase::Environment::get().getWorld()->screenshot(screenshot.get(), screenshotW, screenshotH);

    osgDB::ReaderWriter* readerwriter = osgDB::Registry::instance()->getReaderWriterForExtension("jpg");
    if (!readerwriter)
    {
        Log(Debug::Error) << "Error: Unable to write screenshot, can't find a jpg ReaderWriter";
        return;
    }

    std::ostringstream ostream;
    osgDB::ReaderWriter::WriteResult result = readerwriter->writeImage(*screenshot, ostream);
    if (!result.success())
    {
        Log(Debug::Error) << "Error: Unable to write screenshot: " << result.message() << " code " << result.status();
        return;
    }

    std::string data = ostream.str();
    imageData = std::vector<char>(data.begin(), data.end());
}
