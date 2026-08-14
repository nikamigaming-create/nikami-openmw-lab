Warning: truncated output (original token count: 57136)
Total output lines: 5199

#include "windowmanagerimp.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <osgViewer/Viewer>

#include <MyGUI_ClipboardManager.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_FactoryManager.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_LayerManager.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_PointerManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_UString.h>
#include <MyGUI_Window.h>

// For BT_NO_PROFILE
#include <LinearMath/btQuickprof.h>

#include <SDL_clipboard.h>
#include <SDL_keyboard.h>

#include <components/debug/debuglog.hpp>

#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadflst.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadnote.hpp>
#include <components/esm4/loadweap.hpp>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>

#include <components/fontloader/fontloader.hpp>

#include <components/resource/imagemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>

#include <components/sceneutil/workqueue.hpp>

#include <components/translation/translation.hpp>

#include <components/myguiplatform/additivelayer.hpp>
#include <components/myguiplatform/myguiplatform.hpp>
#include <components/myguiplatform/myguirendermanager.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/myguiplatform/scalinglayer.hpp>

#include <components/vfs/manager.hpp>

#include <components/widgets/tags.hpp>
#include <components/widgets/widgets.hpp>

#include <components/misc/frameratelimiter.hpp>
#include <components/misc/resourcehelpers.hpp>

#include <components/l10n/manager.hpp>

#include <components/lua_ui/util.hpp>
#include <components/lua_ui/widget.hpp>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwinput/actions.hpp"

#include "../mwphysics/raycasting.hpp"

#include "../mwrender/camera.hpp"
#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/vismask.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/player.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "../mwrender/postprocessor.hpp"

#include "../mwworld/inventorystore.hpp"

#include "alchemywindow.hpp"
#include "backgroundimage.hpp"
#include "bookpage.hpp"
#include "bookwindow.hpp"
#include "companionwindow.hpp"
#include "confirmationdialog.hpp"
#include "console.hpp"
#include "container.hpp"
#include "controllerbuttonsoverlay.hpp"
#include "controllers.hpp"
#include "countdialog.hpp"
#include "cursor.hpp"
#include "debugwindow.hpp"
#include "dialogue.hpp"
#include "enchantingdialog.hpp"
#include "exposedwindow.hpp"
#include "hud.hpp"
#include "inventorytabsoverlay.hpp"
#include "inventorywindow.hpp"
#include "fnvmenuxml.hpp"
#include "itemmodel.hpp"
#include "itemchargeview.hpp"
#include "itemtransfer.hpp"
#include "itemview.hpp"
#include "itemwidget.hpp"
#include "jailscreen.hpp"
#include "journalviewmodel.hpp"
#include "journalwindow.hpp"
#include "keyboardnavigation.hpp"
#include "levelupdialog.hpp"
#include "loadingscreen.hpp"
#include "mainmenu.hpp"
#include "merchantrepair.hpp"
#include "postprocessorhud.hpp"
#include "quickkeysmenu.hpp"
#include "recharge.hpp"
#include "repair.hpp"
#include "resourceskin.hpp"
#include "screenfader.hpp"
#include "scrollwindow.hpp"
#include "settingswindow.hpp"
#include "sortfilteritemmodel.hpp"
#include "spellbuyingwindow.hpp"
#include "spellview.hpp"
#include "spellwindow.hpp"
#include "statswindow.hpp"
#include "tradewindow.hpp"
#include "trainingwindow.hpp"
#include "travelwindow.hpp"
#include "videowidget.hpp"
#include "waitdialog.hpp"

//## VR_PATCH BEGIN
#include "../mwvr/radialmenu.hpp"
#include "../mwvr/vrgui.hpp"
#include "../mwvr/vrmetamenu.hpp"
#include "../mwvr/vrvirtualkeyboard.hpp"
#include <components/vr/viewer.hpp>
#include <components/vr/vr.hpp>
//## VR_PATCH END

namespace MWGui
{
    struct FalloutDialogueCameraState
    {
        MWRender::Camera::Mode mMode = MWRender::Camera::Mode::FirstPerson;
        float mPitch = 0.f;
        float mYaw = 0.f;
        float mRoll = 0.f;
        float mFieldOfView = 0.f;
        bool mFieldOfViewWasOverridden = false;
        bool mChangedFieldOfView = false;
        MWWorld::Ptr mTarget;
        osg::Vec3f mCameraPosition;
    };

    namespace
    {
        Settings::SettingValue<bool>* findHiddenSetting(GuiWindow window)
        {
            switch (window)
            {
                case GW_Inventory:
                    return &Settings::windows().mInventoryHidden;
                case GW_Map:
                    return &Settings::windows().mMapHidden;
                case GW_Magic:
                    return &Settings::windows().mSpellsHidden;
                case GW_Stats:
                    return &Settings::windows().mStatsHidden;
                default:
                    return nullptr;
            }
        }

        bool isFalloutContentLoaded()
        {
            const MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return false;

            for (std::string file : world->getContentFiles())
            {
                std::transform(file.begin(), file.end(), file.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                constexpr std::string_view falloutNv = "falloutnv.esm";
                const bool suffixMatch = file.size() >= falloutNv.size()
                    && file.compare(file.size() - falloutNv.size(), falloutNv.size(), falloutNv) == 0;
                if (suffixMatch)
                    return true;
            }

            return false;
        }

        // ESM4-format Fallout records are addressed by FormID at runtime, not
        // by their editor-id strings. Resolve the editor-id against the loaded
        // store before asking the real inventory for a count.
        template <class T>
        ESM::RefId findFalloutEditorId(const MWWorld::ESMStore& store, std::string_view editorId)
        {
            const auto& typedStore = store.get<T>();
            for (auto it = typedStore.begin(); it != typedStore.end(); ++it)
            {
                if (it->mEditorId == editorId)
                    return ESM::RefId::formIdRefId(it->mId);
            }
            return ESM::RefId();
        }

        MWWorld::ContainerStoreIterator findFalloutInventoryItem(
            MWWorld::InventoryStore& inventory, const ESM::RefId& id)
        {
            for (MWWorld::ContainerStoreIterator it = inventory.begin(); it != inventory.end(); ++it)
            {
                if (it->getCellRef().getRefId() == id)
                    return it;
            }
            return inventory.end();
        }

        bool falloutPipBoyItemMatchesCategory(const MWWorld::Ptr& item, int category)
        {
            switch (std::clamp(category, 0, 4))
            {
                case 0:
                    return item.getType() == ESM4::Weapon::sRecordId;
                case 1:
                    return item.getType() == ESM4::Armor::sRecordId
                        || item.getType() == ESM4::Clothing::sRecordId;
                case 2:
                    return item.getType() == ESM4::Potion::sRecordId
                        || item.getType() == ESM4::Ingredient::sRecordId;
                case 3:
                    return item.getType() != ESM4::Weapon::sRecordId
                        && item.getType() != ESM4::Armor::sRecordId
                        && item.getType() != ESM4::Clothing::sRecordId
                        && item.getType() != ESM4::Potion::sRecordId
                        && item.getType() != ESM4::Ingredient::sRecordId
                        && item.getType() != ESM4::Ammunition::sRecordId;
                case 4:
                default:
                    return item.getType() == ESM4::Ammunition::sRecordId;
            }
        }

        std::vector<MWWorld::Ptr> getFalloutPipBoyInventoryRows(
            MWWorld::InventoryStore& inventory, int category)
        {
            std::vector<MWWorld::Ptr> rows;
            for (MWWorld::ContainerStoreIterator it = inventory.begin(); it != inventory.end(); ++it)
            {
                if (it->getCellRef().getCount() > 0 && it->getClass().showsInInventory(*it)
                    && falloutPipBoyItemMatchesCategory(*it, category))
                    rows.push_back(*it);
            }
            std::stable_sort(rows.begin(), rows.end(), [](const MWWorld::Ptr& left, const MWWorld::Ptr& right) {
                return Misc::StringUtils::ciLess(
                    left.getClass().getName(left), right.getClass().getName(right));
            });
            return rows;
        }

        std::string executeFalloutPipBoySelection(int pane, int submenu, int selectedRow)
        {
            MWBase::World* const world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return "NO WORLD";

            MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty())
                return "NO PLAYER";

            MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
            const MWWorld::ESMStore& store = world->getStore();
            const auto selectionState = [&]() {
                std::ostringstream state;
                const MWWorld::ConstContainerStoreIterator right
                    = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                const MWWorld::ConstContainerStoreIterator ammunition
                    = inventory.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                const MWWorld::ConstContainerStoreIterator helmet
                    = inventory.getSlot(MWWorld::InventoryStore::Slot_Helmet);
                const MWWorld::ConstContainerStoreIterator body
                    = inventory.getSlot(MWWorld::InventoryStore::Slot_Robe);
                const ESM::RefId weaponId
                    = right == inventory.end() ? ESM::RefId() : right->getCellRef().getRefId();
                state << "right="
                      << (weaponId.empty() ? std::string("none") : weaponId.toDebugString())
                      << ",ammo="
                      << (ammunition == inventory.end() ? std::string("none")
                                                        : ammunition->getCellRef().getRefId().toDebugString())
                      << ",helmet="
                      << (helmet == inventory.end() ? std::string("none")
                                                    : helmet->getCellRef().getRefId().toDebugString())
                      << ",body="
                      << (body == inventory.end() ? std::string("none")
                                                  : body->getCellRef().getRefId().toDebugString())
                      << ",loaded=" << inventory.getFalloutLoadedAmmo(weaponId).value_or(0)
                      << ",stimpak="
                      << inventory.count(findFalloutEditorId<ESM4::Potion>(store, "Stimpak"))
                      << ",health=" << player.getClass().getCreatureStats(player).getHealth().getCurrent();
                return state.str();
            };
            const std::string beforeState = selectionState();
            const auto completeSelection = [&](std::string result) {
                Log(Debug::Info) << "FNV Pip-Boy selection: pane=" << pane << " submenu=" << submenu
                                 << " row=" << selectedRow << " result=\"" << result << "\" before={"
                                 << beforeState << "} after={" << selectionState() << "}";
                return result;
            };
            const auto equip = [&](const ESM::RefId& id, int slot, std::string_view label) {
                MWWorld::ContainerStoreIterator item = findFalloutInventoryItem(inventory, id);
                if (item == inventory.end())
                    return std::string("MISSING ") + std::string(label);
                inventory.equip(slot, item);
                MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);
                return std::string("EQUIPPED ") + std::string(label);
            };
            const auto use = [&](const ESM::RefId& id, std::string_view label) {
                MWWorld::ContainerStoreIterator item = findFalloutInventoryItem(inventory, id);
                if (item == inventory.end())
                    return std::string("MISSING ") + std::string(label);

                if (item->getType() == ESM4::Potion::sRecordId)
                {
                    const ESM4::Potion& potion = *item->get<ESM4::Potion>()->mBase;
                    // FNV's Stimpak is an ALCH record whose retail effect is
                    // restorative. This production boundary consumes the real
                    // loaded inventory record and synchronizes both live health
                    // representations; broader ALCH effect execution remains
                    // outside the Pip-Boy item-selection slice.
                    if (potion.mEditorId != "Stimpak")
                        return std::string("CANNOT APPLY ") + std::string(label);

                    MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
                    MWMechanics::DynamicStat<float> health = stats.getHealth();
                    const float applied = std::min(25.f, health.getModified() - health.getCurrent());
                    if (applied <= 0.f)
                        return std::string("HEALTH FULL ") + std::string(label);
                    health.setCurrent(health.getCurrent() + applied);
                    stats.setHealth(health);
                    world->getFalloutPlayerRuntimeState().modCurrentActorValue(
                        MWWorld::FalloutPlayerRuntimeState::HealthActorValue, applied);
                    if (inventory.remove(*item, 1) != 1)
                        return std::string("FAILED TO CONSUME ") + std::string(label);
                    MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);
                    return std::string("USED ") + std::string(label);
                }

                std::unique_ptr<MWWorld::Action> action = (*item).getClass().use(*item, false);
                if (action == nullptr)
                    return std::string("CANNOT USE ") + std::string(label);
                action->execute(player);
                MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);
                return std::string("USED ") + std::string(label);
            };
            const auto toggleWearable = [&](const MWWorld::Ptr& item, std::string_view label) {
                const std::vector<int>& slots = item.getClass().getEquipmentSlots(item).first;
                if (slots.empty())
                    return std::string("CANNOT EQUIP ") + std::string(label);

                bool equipped = false;
                for (const int slot : slots)
                {
                    const MWWorld::ContainerStoreIterator current = inventory.getSlot(slot);
                    if (current != inventory.end()
                        && current->getCellRef().getRefId() == item.getCellRef().getRefId())
                    {
                        equipped = true;
                        inventory.unequipSlot(slot);
                    }
                }
                if (equipped)
                {
                    MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);
                    return std::string("UNEQUIPPED ") + std::string(label);
                }

                std::unique_ptr<MWWorld::Action> action = item.getClass().use(item, false);
                if (action == nullptr)
                    return std::string("CANNOT EQUIP ") + std::string(label);
                action->execute(player);
                MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);
                for (const int slot : slots)
                {
                    const MWWorld::ContainerStoreIterator current = inventory.getSlot(slot);
                    if (current != inventory.end()
                        && current->getCellRef().getRefId() == item.getCellRef().getRefId())
                        return std::string("EQUIPPED ") + std::string(label);
                }
                return std::string("FAILED TO EQUIP ") + std::string(label);
            };

            if (pane == 1)
            {
                std::vector<MWWorld::Ptr> rows = getFalloutPipBoyInventoryRows(inventory, submenu);
                if (rows.empty())
                    return completeSelection("EMPTY CATEGORY");
                const int rowIndex = std::clamp(selectedRow, 0, static_cast<int>(rows.size()) - 1);
                const MWWorld::Ptr selected = rows[static_cast<std::size_t>(rowIndex)];
                const std::string selectedName(selected.getClass().getName(selected));
                switch (submenu)
                {
                    case 0:
                    {
                        const ESM::RefId id = selected.getCellRef().getRefId();
                        const std::string result
                            = equip(id, MWWorld::InventoryStore::Slot_CarriedRight, selectedName);
                        if (result.starts_with("EQUIPPED"))
                        {
                            player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Weapon);
                            const ESM4::Weapon& weapon = *selected.get<ESM4::Weapon>()->mBase;
                            std::vector<ESM::FormId> ammoCandidates;
                            if (store.get<ESM4::Ammunition>().search(weapon.mAmmo) != nullptr)
                                ammoCandidates.push_back(weapon.mAmmo);
                            else if (const ESM4::FormIdList* list
                                = store.get<ESM4::FormIdList>().search(weapon.mAmmo))
                                ammoCandidates = list->mObjects;

                            const auto isOwnedAmmo = [&](ESM::FormId candidate) {
                                return !candidate.isZeroOrUnset()
                                    && store.get<ESM4::Ammunition>().search(candidate) != nullptr
                                    && inventory.count(ESM::RefId::formIdRefId(candidate)) > 0;
                            };
                            const auto ownedAmmo = std::ranges::find_if(ammoCandidates, isOwnedAmmo);
                            if (ownedAmmo != ammoCandidates.end())
                            {
                                const ESM::RefId ammunitionId = ESM::RefId::formIdRefId(*ownedAmmo);
                                equip(ammunitionId, MWWorld::InventoryStore::Slot_Ammunition, "AMMUNITION");
                                inventory.setFalloutAmmoSelection(id, ammunitionId);
                            }
                            else if (!ammoCandidates.empty())
                                Log(Debug::Error) << "FNV Pip-Boy weapon equip: status=fail reason=no-owned-compatible-ammo"
                                                  << " weapon=" << id << " authoredCandidates="
                                                  << ammoCandidates.size();
                            if (!inventory.getFalloutLoadedAmmo(id).has_value())
                                inventory.setFalloutLoadedAmmo(id, 0);
                        }
                        return completeSelection(result);
                    }
                    case 1:
                        return completeSelection(toggleWearable(selected, selectedName));
                    case 2:
                        return completeSelection(use(selected.getCellRef().getRefId(), selectedName));
                    case 3:
                        return completeSelection("SELECTED " + selectedName);
                    case 4:
                    default:
                    {
                        const MWWorld::ContainerStoreIterator right
                            = inventory.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
                        if (right == inventory.end() || right->getType() != ESM4::Weapon::sRecordId)
                            return completeSelection("NO EQUIPPED WEAPON FOR " + selectedName);

                        const ESM4::Weapon& weapon = *right->get<ESM4::Weapon>()->mBase;
                        const ESM::RefId weaponId = right->getCellRef().getRefId();
                        const ESM::RefId ammunitionId = selected.getCellRef().getRefId();
                        const ESM::FormId ammunitionForm = selected.get<ESM4::Ammunition>()->mBase->mId;
                        bool compatible = weapon.mAmmo == ammunitionForm;
                        if (!compatible)
                        {
                            if (const ESM4::FormIdList* list
                                = store.get<ESM4::FormIdList>().search(weapon.mAmmo))
                                compatible = std::ranges::find(list->mObjects, ammunitionForm)
                                    != list->mObjects.end();
                        }
                        if (!compatible)
                            return completeSelection("INCOMPATIBLE " + selectedName + " FOR "
                                + std::string(right->getClass().getName(*right)));

                        const std::string equipped = equip(
                            ammunitionId, MWWorld::InventoryStore::Slot_Ammunition, selectedName);
                        if (!equipped.starts_with("EQUIPPED"))
                            return completeSelection(equipped);
                        inventory.setFalloutAmmoSelection(weaponId, ammunitionId);
                        return completeSelection("SELECTED " + selectedName + " FOR "
                            + std::string(right->getClass().getName(*right)));
                    }
                }
            }

            if (pane == 2)
                return completeSelection("SELECTED DATA ENTRY");
            return completeSelection("SELECTED");
        }

        int getFalloutPipBoySubmenuCount(int pane)
        {
            switch (std::clamp(pane, 0, 3))
            {
                case 0:
                    return 2; // LOCAL / WORLD
                case 1:
                    return 5; // WEAPONS / APPAREL / AID / MISC / AMMO
                case 2:
                    return 3; // QUESTS / NOTES / RADIO
                case 3:
                default:
                    return 7; // CND / RAD / EFF / SPECIAL / SKILLS / PERKS / GENERAL
            }
        }

        std::string makeFalloutPipBoyTerminalHeader(int pane)
        {
            // The device shell already carries the PIP-BOY 3000 branding.  The
            // retail screen uses the active physical-button family as its title.
            switch (std::clamp(pane, 0, 3))
            {
                case 0:
                case 2:
                    return "DATA";
                case 1:
                    return "ITEMS";
                case 3:
                default:
                    return "STATS";
            }
        }

        void setWindowCoord(WindowBase* window, const MyGUI::IntCoord& coord)
        {
            if (window == nullptr || window->mMainWidget == nullptr)
                return;

            MyGUI::Window* guiWindow = window->mMainWidget->castType<MyGUI::Window>(false);
            if (guiWindow == nullptr)
                return;

            guiWindow->setCoord(coord);
            window->onWindowResize(guiWindow);
        }
    }

    WindowManager::WindowManager(SDL_Window* window, osgViewer::Viewer* viewer, osg::Group* guiRoot,
        Resource::ResourceSystem* resourceSystem, SceneUtil::WorkQueue* workQueue, const std::filesystem::path& logpath,
        bool consoleOnlyScripts, Translation::Storage& translationDataStorage, ToUTF8::FromType encoding,
        bool exportFonts, const std::string& versionDescription, bool useShaders, Files::ConfigurationManager& cfgMgr)
        : mOldUpdateMask(0)
        , mOldCullMask(0)
        , mStore(nullptr)
        , mResourceSystem(resourceSystem)
        , mWorkQueue(workQueue)
        , mViewer(viewer)
        , mConsoleOnlyScripts(consoleOnlyScripts)
        , mCurrentModals()
        , mHud(nullptr)
        , mMap(nullptr)
        , mStatsWindow(nullptr)
        , mConsole(nullptr)
        , mDialogueWindow(nullptr)
        , mInventoryWindow(nullptr)
        , mScrollWindow(nullptr)
        , mBookWindow(nullptr)
        , mCountDialog(nullptr)
        , mTradeWindow(nullptr)
        , mSettingsWindow(nullptr)
        , mConfirmationDialog(nullptr)
        , mSpellWindow(nullptr)
        , mQuickKeysMenu(nullptr)
        , mLoadingScreen(nullptr)
        , mWaitDialog(nullptr)
        , mVideoBackground(nullptr)
        , mVideoWidget(nullptr)
        , mWerewolfFader(nullptr)
        , mBlindnessFader(nullptr)
        , mHitFader(nullptr)
        , mScreenFader(nullptr)
        , mDebugWindow(nullptr)
        , mPostProcessorHud(nullptr)
        , mJailScreen(nullptr)
        , mContainerWindow(nullptr)
        , mControllerButtonsOverlay(nullptr)
        , mInventoryTabsOverlay(nullptr)
        , mTranslationDataStorage(translationDataStorage)
        , mInputBlocker(nullptr)
        , mHudEnabled(true)
        , mLegacyHudSuppressed(false)
        , mCursorVisible(true)
        , mCursorActive(true)
        , mPlayerBounty(-1)
        , mGuiModes()
        , mGarbageDialogs()
        , mShown(GW_ALL)
        , mForceHidden(GW_None)
        , mAllowed(GW_ALL)
        , mRestAllowed(true)
        , mEncoding(encoding)
        , mVersionDescription(versionDescription)
        , mWindowVisible(true)
        , mCfgMgr(cfgMgr)
//## VR_PATCH BEGIN
        , mVrMetaMenu(nullptr)
        , mVirtualKeyboardManager(nullptr)
        , mVideoEnabled(false)
//## VR_PATCH END
    {
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        int dw, dh;
        SDL_GL_GetDrawableSize(window, &dw, &dh);

        mScalingFactor = Settings::gui().mScalingFactor * (dw / w);
        mGuiPlatform = std::make_unique<MyGUIPlatform::Platform>(viewer, guiRoot, resourceSystem->getImageManager(),
            resourceSystem->getVFS(), mScalingFactor, "mygui", logpath / "MyGUI.log");
//## VR_PATCH BEGIN
// Force GUI window to be 1024x1024
        if(VR::getVR())
            mGuiPlatform->getRenderManagerPtr()->setViewSize(1024, 1024);
//## VR_PATCH END

        const VFS::Manager* vfs = resourceSystem->getVFS();
        const bool useFnvMissingGuiFallback = !VR::getVR()
            && vfs->exists(VFS::Path::Normalized("falloutnv.esm"))
            && !vfs->exists(VFS::Path::Normalized("textures/menu_thin_border_top.dds"));
        mGuiPlatform->getRenderManagerPtr()->setUseMissingTextureFallback(useFnvMissingGuiFallback);
        if (useFnvMissingGuiFallback)
            Log(Debug::Info) << "FNV UI: enabled generated fallbacks for absent MyGUI textures";

        mGui = std::make_unique<MyGUI::Gui>();
        mGui->initialise({});

        createTextures();

        MyGUI::LanguageManager::getInstance().eventRequestTag = MyGUI::newDelegate(this, &WindowManager::onRetrieveTag);

        // Load fonts
        mFontLoader
            = std::make_unique<Gui::FontLoader>(encoding, resourceSystem->getVFS(), mScalingFactor, exportFonts);

        // Register own widgets with MyGUI
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Widgets::MWSkill>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Widgets::MWAttribute>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Widgets::MWSpell>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Widgets::MWEffectList>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Widgets::MWSpellEffect>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Widgets::MWDynamicStat>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Window>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<VideoWidget>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<BackgroundImage>("Widget");
        MyGUI::FactoryManager::getInstance().registerFactory<MyGUIPlatform::AdditiveLayer>("Layer");
        MyGUI::FactoryManager::getInstance().registerFactory<MyGUIPlatform::ScalingLayer>("Layer");
        BookPage::registerMyGUIComponents();
        PostProcessorHud::registerMyGUIComponents();
        ItemView::registerComponents();
        ItemChargeView::registerComponents();
        ItemWidget::registerComponents();
        SpellView::registerComponents();
        Gui::registerAllWidgets();
        LuaUi::registerAllWidgets();

        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::Controllers::ControllerFollowMouse>("Controller");

        MyGUI::FactoryManager::getInstance().registerFactory<ResourceImageSetPointerFix>(
            "Resource", "ResourceImageSetPointer");
        MyGUI::FactoryManager::getInstance().registerFactory<AutoSizedResourceSkin>(
            "Resource", "AutoSizedResourceSkin");
//## VR_PATCH BEGIN
        if (VR::getVR())
        {
            MWVR::VRGUIManager::registerMyGUIFactories();
            MyGUI::ResourceManager::getInstance().load("core_vr.xml");
        }
        else
        {
            MyGUI::ResourceManager::getInstance().load("core.xml");
        }
//## VR_PATCH END

        const bool keyboardNav = Settings::gui().mKeyboardNavigation;
        mKeyboardNavigation = std::make_unique<KeyboardNavigation>();
        mKeyboardNavigation->setEnabled(keyboardNav);
        Gui::ImageButton::setDefaultNeedKeyFocus(keyboardNav);

        auto loadingScreen = std::make_unique<LoadingScreen>(mResourceSystem, mViewer);
        mLoadingScreen = loadingScreen.get();
        mWindows.push_back(std::move(loadingScreen));

        // set up the hardware cursor manager
        mCursorManager = std::make_unique<SDLUtil::SDLCursorManager>();

        MyGUI::PointerManager::getInstance().eventChangeMousePointer
            += MyGUI::newDelegate(this, &WindowManager::onCursorChange);

        MyGUI::InputManager::getInstance().eventChangeKeyFocus
            += MyGUI::newDelegate(this, &WindowManager::onKeyFocusChanged);

        // Create all cursors in advance
        createCursors();
        onCursorChange(MyGUI::PointerManager::getInstance().getDefaultPointer());
        mCursorManager->setEnabled(true);

        // hide mygui's pointer
        MyGUI::PointerManager::getInstance().setVisible(false);

        mVideoBackground = MyGUI::Gui::getInstance().createWidgetReal<MyGUI::ImageBox>(
            "ImageBox", 0, 0, 1, 1, MyGUI::Align::Default, "Video");
        mVideoBackground->setImageTexture("black");
        mVideoBackground->setVisible(false);
        mVideoBackground->setNeedMouseFocus(true);
        mVideoBackground->setNeedKeyFocus(true);

//## VR_PATCH BEGIN
// Assign video widget to the InputBlocker layer
        mVideoWidget = mVideoBackground->createWidgetReal<VideoWidget>(
            "ImageBox", 0, 0, 1, 1, MyGUI::Align::Default, "InputBlocker");
//## VR_PATCH END
        mVideoWidget->setNeedMouseFocus(true);
        mVideoWidget->setNeedKeyFocus(true);
        mVideoWidget->setVFS(resourceSystem->getVFS());

        // Removes default MyGUI system clipboard implementation, which supports windows only
        MyGUI::ClipboardManager::getInstance().eventClipboardChanged.clear();
        MyGUI::ClipboardManager::getInstance().eventClipboardRequested.clear();

        MyGUI::ClipboardManager::getInstance().eventClipboardChanged
            += MyGUI::newDelegate(this, &WindowManager::onClipboardChanged);
        MyGUI::ClipboardManager::getInstance().eventClipboardRequested
            += MyGUI::newDelegate(this, &WindowManager::onClipboardRequested);

//## VR_PATCH BEGIN
// SDL should not manage gamma in VR. This must be done via shaders.
        mVideoWrapper = std::make_unique<SDLUtil::VideoWrapper>(window, viewer);
//## VR_PATCH END
        mVideoWrapper->setGammaContrast(Settings::video().mGamma, Settings::video().mContrast);

        if (useShaders)
            mGuiPlatform->getRenderManagerPtr()->enableShaders(mResourceSystem->getSceneManager()->getShaderManager());

        mStatsWatcher = std::make_unique<StatsWatcher>();
    }

    void WindowManager::initUI()
    {
        // Get size info from the Gui object
        int w = MyGUI::RenderManager::getInstance().getViewSize().width;
        int h = MyGUI::RenderManager::getInstance().getViewSize().height;

        mTextColours.loadColours();

        mDragAndDrop = std::make_unique<DragAndDrop>();
        mItemTransfer = std::make_unique<ItemTransfer>(*this);

        auto recharge = std::make_unique<Recharge>();
        mGuiModeStates[GM_Recharge] = GuiModeState(recharge.get());
        mWindows.push_back(std::move(recharge));
//## VR_PATCH BEGIN
        if(VR::getVR())
            mVirtualKeyboardManager = new MWVR::VirtualKeyboardManager;

//## VR_PATCH END

        auto menu = std::make_unique<MainMenu>(w, h, mResourceSystem->getVFS(), mVersionDescription);
        mGuiModeStates[GM_MainMenu] = GuiModeState(menu.get());
        mWindows.push_back(std::move(menu));

        mLocalMapRender = std::make_unique<MWRender::LocalMap>(mViewer->getSceneData()->asGroup());
        auto map = std::make_unique<MapWindow>(mCustomMarkers, mDragAndDrop.get(), mLocalMapRender.get(), mWorkQueue);
        Log(Debug::Info) << "FNV/ESM4 UI init: map constructed";
        mMap = map.get();
        mWindows.push_back(std::move(map));
        mMap->renderGlobalMap();
        Log(Debug::Info) << "FNV/ESM4 UI init: global map rendered";
        trackWindow(mMap, makeMapWindowSettingValues());

        auto statsWindow = std::make_unique<StatsWindow>(mDragAndDrop.get());
        Log(Debug::Info) << "FNV/ESM4 UI init: stats constructed";
        mStatsWindow = statsWindow.get();
        mWindows.push_back(std::move(statsWindow));
        trackWindow(mStatsWindow, makeStatsWindowSettingValues());

        auto inventoryWindow = std::make_unique<InventoryWindow>(
            *mDragAndDrop, *mItemTransfer, mViewer->getSceneData()->asGroup(), mResourceSystem);
        Log(Debug::Info) << "FNV/ESM4 UI init: inventory constructed";
        mInventoryWindow = inventoryWindow.get();
        mWindows.push_back(std::move(inventoryWindow));

        auto spellWindow = std::make_unique<SpellWindow>(mDragAndDrop.get());
        Log(Debug::Info) << "FNV/ESM4 UI init: spell window constructed";
        mSpellWindow = spellWindow.get();
        mWindows.push_back(std::move(spellWindow));
        trackWindow(mSpellWindow, makeSpellsWindowSettingValues());

        mGuiModeStates[GM_Inventory] = GuiModeState({ mMap, mInventoryWindow, mSpellWindow, mStatsWindow });
        mGuiModeStates[GM_None] = GuiModeState({ mMap, mInventoryWindow, mSpellWindow, mStatsWindow });

        auto tradeWindow = std::make_unique<TradeWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: trade constructed";
        mTradeWindow = tradeWindow.get();
        mWindows.push_back(std::move(tradeWindow));
        trackWindow(mTradeWindow, makeBarterWindowSettingValues());
        mGuiModeStates[GM_Barter] = GuiModeState({ mInventoryWindow, mTradeWindow });

        auto console = std::make_unique<Console>(w, h, mConsoleOnlyScripts, mCfgMgr);
        Log(Debug::Info) << "FNV/ESM4 UI init: console constructed";
        mConsole = console.get();
        mWindows.push_back(std::move(console));
        trackWindow(mConsole, makeConsoleWindowSettingValues());

        constexpr VFS::Path::NormalizedView menubookOptionsOverTexture("textures/tx_menubook_options_over.dds");
        const bool questList = mResourceSystem->getVFS()->exists(menubookOptionsOverTexture);
        auto journal = JournalWindow::create(JournalViewModel::create(), questList, mEncoding);
        Log(Debug::Info) << "FNV/ESM4 UI init: journal constructed";
        mGuiModeStates[GM_Journal] = GuiModeState(journal.get());
        mWindows.push_back(std::move(journal));

        mMessageBoxManager = std::make_unique<MessageBoxManager>(
            mStore->get<ESM::GameSetting>().find("fMessageTimePerChar")->mValue.getFloat());
        Log(Debug::Info) << "FNV/ESM4 UI init: message boxes constructed";

        auto spellBuyingWindow = std::make_unique<SpellBuyingWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: spell buying constructed";
        mGuiModeStates[GM_SpellBuying] = GuiModeState(spellBuyingWindow.get());
        mWindows.push_back(std::move(spellBuyingWindow));

        auto travelWindow = std::make_unique<TravelWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: travel constructed";
        mGuiModeStates[GM_Travel] = GuiModeState(travelWindow.get());
        mWindows.push_back(std::move(travelWindow));

        auto dialogueWindow = std::make_unique<DialogueWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: dialogue constructed";
        mDialogueWindow = dialogueWindow.get();
        mWindows.push_back(std::move(dialogueWindow));
        trackWindow(mDialogueWindow, makeDialogueWindowSettingValues());
        mGuiModeStates[GM_Dialogue] = GuiModeState(mDialogueWindow);
        mTradeWindow->eventTradeDone += MyGUI::newDelegate(mDialogueWindow, &DialogueWindow::onTradeComplete);

        auto containerWindow = std::make_unique<ContainerWindow>(*mDragAndDrop, *mItemTransfer);
        Log(Debug::Info) << "FNV/ESM4 UI init: container constructed";
        mContainerWindow = containerWindow.get();
        mWindows.push_back(std::move(containerWindow));
        trackWindow(mContainerWindow, makeContainerWindowSettingValues());
        mGuiModeStates[GM_Container] = GuiModeState({ mContainerWindow, mInventoryWindow });

        auto hud = std::make_unique<HUD>(mCustomMarkers, mDragAndDrop.get(), mLocalMapRender.get());
        Log(Debug::Info) << "FNV/ESM4 UI init: HUD constructed";
        mHud = hud.get();
        mWindows.push_back(std::move(hud));

        mToolTips = std::make_unique<ToolTips>();
        Log(Debug::Info) << "FNV/ESM4 UI init: tooltips constructed";

        auto scrollWindow = std::make_unique<ScrollWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: scroll constructed";
        mScrollWindow = scrollWindow.get();
        mWindows.push_back(std::move(scrollWindow));
        mGuiModeStates[GM_Scroll] = GuiModeState(mScrollWindow);

        auto bookWindow = std::make_unique<BookWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: book constructed";
        mBookWindow = bookWindow.get();
        mWindows.push_back(std::move(bookWindow));
        mGuiModeStates[GM_Book] = GuiModeState(mBookWindow);

        auto countDialog = std::make_unique<CountDialog>();
        Log(Debug::Info) << "FNV/ESM4 UI init: count dialog constructed";
        mCountDialog = countDialog.get();
        mWindows.push_back(std::move(countDialog));

        auto settingsWindow = std::make_unique<SettingsWindow>(mCfgMgr);
        Log(Debug::Info) << "FNV/ESM4 UI init: settings constructed";
        mSettingsWindow = settingsWindow.get();
        mWindows.push_back(std::move(settingsWindow));
        trackWindow(mSettingsWindow, makeSettingsWindowSettingValues());

        auto confirmationDialog = std::make_unique<ConfirmationDialog>();
        Log(Debug::Info) << "FNV/ESM4 UI init: confirmation constructed";
        mConfirmationDialog = confirmationDialog.get();
        mWindows.push_back(std::move(confirmationDialog));

        auto alchemyWindow = std::make_unique<AlchemyWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: alchemy constructed";
        trackWindow(alchemyWindow.get(), makeAlchemyWindowSettingValues());
        mGuiModeStates[GM_Alchemy] = GuiModeState(alchemyWindow.get());
        mWindows.push_back(std::move(alchemyWindow));

        auto quickKeysMenu = std::make_unique<QuickKeysMenu>();
        Log(Debug::Info) << "FNV/ESM4 UI init: quick keys constructed";
        mQuickKeysMenu = quickKeysMenu.get();
        mWindows.push_back(std::move(quickKeysMenu));
        mGuiModeStates[GM_QuickKeysMenu] = GuiModeState(mQuickKeysMenu);

        auto levelupDialog = std::make_unique<LevelupDialog>();
        Log(Debug::Info) << "FNV/ESM4 UI init: levelup constructed";
        mGuiModeStates[GM_Levelup] = GuiModeState(levelupDialog.get());
        mWindows.push_back(std::move(levelupDialog));

        auto waitDialog = std::make_unique<WaitDialog>();
        Log(Debug::Info) << "FNV/ESM4 UI init: wait constructed";
        mWaitDialog = waitDialog.get();
        mWindows.push_back(std::move(waitDialog));
        mGuiModeStates[GM_Rest] = GuiModeState({ mWaitDialog->getProgressBar(), mWaitDialog });

        auto spellCreationDialog = std::make_unique<SpellCreationDialog>();
        Log(Debug::Info) << "FNV/ESM4 UI init: spell creation constructed";
        mGuiModeStates[GM_SpellCreation] = GuiModeState(spellCreationDialog.get());
        mWindows.push_back(std::move(spellCreationDialog));

        auto enchantingDialog = std::make_unique<EnchantingDialog>();
        Log(Debug::Info) << "FNV/ESM4 UI init: enchanting constructed";
        mGuiModeStates[GM_Enchanting] = GuiModeState(enchantingDialog.get());
        mWindows.push_back(std::move(enchantingDialog));

        auto trainingWindow = std::make_unique<TrainingWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: training constructed";
        mGuiModeStates[GM_Training] = GuiModeState({ trainingWindow->getProgressBar(), trainingWindow.get() });
        mWindows.push_back(std::move(trainingWindow));

        auto merchantRepair = std::make_unique<MerchantRepair>();
        Log(Debug::Info) << "FNV/ESM4 UI init: merchant repair constructed";
        mGuiModeStates[GM_MerchantRepair] = GuiModeState(merchantRepair.get());
        mWindows.push_back(std::move(merchantRepair));

        auto repair = std::make_unique<Repair>();
        Log(Debug::Info) << "FNV/ESM4 UI init: repair constructed";
        mGuiModeStates[GM_Repair] = GuiModeState(repair.get());
        mWindows.push_back(std::move(repair));

        mSoulgemDialog = std::make_unique<SoulgemDialog>(mMessageBoxManager.get());
        Log(Debug::Info) << "FNV/ESM4 UI init: soulgem constructed";

        auto companionWindow
            = std::make_unique<CompanionWindow>(*mDragAndDrop, *mItemTransfer, mMessageBoxManager.get());
        Log(Debug::Info) << "FNV/ESM4 UI init: companion constructed";
        trackWindow(companionWindow.get(), makeCompanionWindowSettingValues());
        mGuiModeStates[GM_Companion] = GuiModeState({ mInventoryWindow, companionWindow.get() });
        mWindows.push_back(std::move(companionWindow));

        auto jailScreen = std::make_unique<JailScreen>();
        Log(Debug::Info) << "FNV/ESM4 UI init: jail constructed";
        mJailScreen = jailScreen.get();
        mWindows.push_back(std::move(jailScreen));
        mGuiModeStates[GM_Jail] = GuiModeState(mJailScreen);

        std::string werewolfFaderTex = "textures\\werewolfoverlay.dds";
        if (mResourceSystem->getVFS()->exists(werewolfFaderTex))
        {
            auto werewolfFader = std::make_unique<ScreenFader>(werewolfFaderTex);
            mWerewolfFader = werewolfFader.get();
            mWindows.push_back(std::move(werewolfFader));
        }
        auto blindnessFader = std::make_unique<ScreenFader>("black");
        Log(Debug::Info) << "FNV/ESM4 UI init: blindness fader constructed";
        mBlindnessFader = blindnessFader.get();
        mWindows.push_back(std::move(blindnessFader));

        // fall back to player_hit_01.dds if bm_player_hit_01.dds is not available
        std::string hitFaderTexture = "textures\\bm_player_hit_01.dds";
        const std::string hitFaderLayout = "openmw_screen_fader_hit.layout";
        MyGUI::FloatCoord hitFaderCoord(0, 0, 1, 1);
        if (!mResourceSystem->getVFS()->exists(hitFaderTexture))
        {
            hitFaderTexture = "textures\\player_hit_01.dds";
            hitFaderCoord = MyGUI::FloatCoord(0.2, 0.25, 0.6, 0.5);
        }
        auto hitFader = std::make_unique<ScreenFader>(hitFaderTexture, hitFaderLayout, hitFaderCoord);
        Log(Debug::Info) << "FNV/ESM4 UI init: hit fader constructed";
        mHitFader = hitFader.get();
        mWindows.push_back(std::move(hitFader));

        auto screenFader = std::make_unique<ScreenFader>("black");
        Log(Debug::Info) << "FNV/ESM4 UI init: screen fader constructed";
        mScreenFader = screenFader.get();
        mWindows.push_back(std::move(screenFader));

        auto debugWindow = std::make_unique<DebugWindow>();
        Log(Debug::Info) << "FNV/ESM4 UI init: debug constructed";
        mDebugWindow = debugWindow.get();
        mWindows.push_back(std::move(debugWindow));
        trackWindow(mDebugWindow, makeDebugWindowSettingValues());

        auto postProcessorHud = std::make_unique<PostProcessorHud>(mCfgMgr);
        Log(Debug::Info) << "FNV/ESM4 UI init: postprocessor constructed";
        mPostProcessorHud = postProcessorHud.get();
        mWindows.push_back(std::move(postProcessorHud));
        trackWindow(mPostProcessorHud, makePostprocessorWindowSettingValues());

        auto controllerButtonsOverlay = std::make_unique<ControllerButtonsOverlay>();
        Log(Debug::Info) << "FNV/ESM4 UI init: controller overlay constructed";
        mControllerButtonsOverlay = controllerButtonsOverlay.get();
        mWindows.push_back(std::move(controllerButtonsOverlay));

        auto inventoryTabsOverlay = std::make_unique<InventoryTabsOverlay>();
        Log(Debug::Info) << "FNV/ESM4 UI init: inventory tabs overlay constructed";
        mInventoryTabsOverlay = inventoryTabsOverlay.get();
        mWindows.push_back(std::move(inventoryTabsOverlay));

        mControllerTooltipEnabled = Settings::gui().mControllerTooltips;
        mActiveControllerWindows[GM_Inventory] = 1; // Start on Inventory page

        mInputBlocker = MyGUI::Gui::getInstance().createWidget<MyGUI::Widget>(
            {}, 0, 0, w, h, MyGUI::Align::Stretch, "InputBlocker");

        initializeFalloutPipBoyRetailInventory(w, h);
        initializeFalloutPipBoyRetailStats(w, h);
        initializeFalloutPipBoyRetailMap(w, h);

        mHud->setVisible(true);

        mCharGen = std::make_unique<CharacterCreation>(mViewer->getSceneData()->asGroup(), mResourceSystem);
        Log(Debug::Info) << "FNV/ESM4 UI init: character creation constructed";

//## VR_PATCH BEGIN
        auto vrMetaMenu = std::make_unique<MWVR::VrMetaMenu>(w, h);
        mVrMetaMenu = vrMetaMenu.get();
        mWindows.emplace_back(std::move(vrMetaMenu));
        mGuiModeStates[GM_VrMetaMenu] = GuiModeState(mVrMetaMenu);

        auto radialMenu = std::make_unique<MWVR::RadialMenu>(w, h, mQuickKeysMenu);
        mRadialMenu = radialMenu.get();
        mWindows.emplace_back(std::move(radialMenu));
        mGuiModeStates[GM_RadialMenu] = GuiModeState(mRadialMenu);
//## VR_PATCH END
        Log(Debug::Info) << "FNV/ESM4 UI init: update pinned begin";
        updatePinnedWindows();
        Log(Debug::Info) << "FNV/ESM4 UI init: update pinned complete";

        // Set up visibility
        Log(Debug::Info) << "FNV/ESM4 UI init: update visible begin";
        updateVisible();
        Log(Debug::Info) << "FNV/ESM4 UI init: update visible complete";

        Log(Debug::Info) << "FNV/ESM4 UI init: stats listeners begin";
        mStatsWatcher->addListener(mHud);
        mStatsWatcher->addListener(mStatsWindow);
        mStatsWatcher->addListener(mCharGen.get());
        Log(Debug::Info) << "FNV/ESM4 UI init: stats listeners complete";

        for (auto& window : mWindows)
        {
            std::string_view id = window->getWindowIdForLua();
            if (!id.empty())
                mLuaIdToWindow.emplace(id, window.get());
        }
    }

    void WindowManager::setNewGame(bool newgame)
    {
        if (newgame)
        {
            disallowAll();

            mStatsWatcher->removeListener(mCharGen.get());
            mCharGen = std::make_unique<CharacterCreation>(mViewer->getSceneData()->asGroup(), mResourceSystem);
            mStatsWatcher->addListener(mCharGen.get());
        }
        else
            allow(GW_ALL);

        mStatsWatcher->forceUpdate();
    }

    void WindowManager::initializeFalloutPipBoyRetailInventory(int width, int height)
    {
        FnvMenuXmlParseError parseError = FnvMenuXmlParseError::None;
        const std::optional<FnvMenuXmlDocument> menu = loadFnvMenuXml(
            *mResourceSystem->getVFS(), "menus/main/inventory_menu.xml", &parseError, true);
        if (!menu)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail XML: status=fail path=menus/main/inventory_menu.xml error="
                              << getFnvMenuXmlParseErrorName(parseError);
            return;
        }

        constexpr float authoredWidth = 960.f;
        constexpr float authoredHeight = 720.f;
        const float scale = std::min(static_cast<float>(width) / authoredWidth,
            static_cast<float>(height) / authoredHeight);
        const int canvasWidth = std::max(1, static_cast<int>(std::lround(authoredWidth * scale)));
        const int canvasHeight = std::max(1, static_cast<int>(std::lround(authoredHeight * scale)));
        const int canvasX = (width - canvasWidth) / 2;
        const int canvasY = (height - canvasHeight) / 2;
        mFalloutPipBoyRetailCanvas = MyGUI::IntCoord(canvasX, canvasY, canvasWidth, canvasHeight);

        const FnvMenuLayoutEvaluationContext layoutContext{
            authoredWidth,
            authoredHeight,
            [](std::string_view symbol) -> std::optional<float> {
                if (symbol == "true" || symbol == "highdef" || symbol == "scale" || symbol == "pipboy")
                    return 1.f;
                if (symbol == "false" || symbol == "xbox")
                    return 0.f;
                return std::nullopt;
            },
            [](std::string_view node, std::string_view trait) -> std::optional<float> {
                if (node == "globals()" && trait == "_pipboy_width")
                    return 960.f;
                if (node == "globals()" && trait == "_pipboy_height")
                    return 720.f;
                return std::nullopt;
            },
        };
        const auto required = [&](std::string_view node, std::string_view trait) -> std::optional<float> {
            const std::optional<float> value
                = evaluateFnvMenuNamedScalarTrait(menu->mRoot, node, trait, layoutContext);
            if (!value)
                Log(Debug::Error) << "FNV Pip-Boy retail XML: unresolved node=" << node << " trait=" << trait;
            return value;
        };

        const std::optional<float> mainX = required("IM_MainRect", "x");
        const std::optional<float> mainY = required("IM_MainRect", "y");
        const std::optional<float> listX = required("IM_InventoryList", "x");
        const std::optional<float> listY = required("IM_InventoryList", "y");
        const std::optional<float> listWidth = required("IM_InventoryList", "width");
        const std::optional<float> listHeight = required("IM_InventoryList", "height");
        const std::optional<float> tabX = required("IM_Tabline", "x");
        const std::optional<float> tabY = required("IM_Tabline", "y");
        const std::optional<float> tabWidth = required("IM_Tabline", "width");
        if (!mainX || !mainY || !listX || !listY || !listWidth || !listHeight || !tabX || !tabY || !tabWidth)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail XML: status=fail reason=required-authored-geometry";
            return;
        }

        mFalloutPipBoyRetailLayout = std::make_unique<Layout>("openmw_fnv_menu.layout");
        mFalloutPipBoyRetailRoot = mFalloutPipBoyRetailLayout->mMainWidget;
        mFalloutPipBoyRetailRoot->setCoord(canvasX, canvasY, canvasWidth, canvasHeight);
        MyGUI::LayerManager::getInstance().attachToLayerNode("PipBoyScreen", mFalloutPipBoyRetailRoot);
        mFalloutPipBoyRetailRoot->setVisible(false);
        MyGUI::ImageBox* background = nullptr;
        MyGUI::Widget* textContainer = nullptr;
        MyGUI::Widget* unusedMessage = nullptr;
        mFalloutPipBoyRetailLayout->getWidget(background, "background");
        mFalloutPipBoyRetailLayout->getWidget(unusedMessage, "message");
        mFalloutPipBoyRetailLayout->getWidget(textContainer, "buttons");
        unusedMessage->setVisible(false);
        background->setVisible(false);

        const auto scaled = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
        const auto configureText = [&](MyGUI::TextBox* text, const MyGUI::IntCoord& coord, int fontHeight) {
            text->setCoord(coord);
            text->setFontName("MonoFont");
            text->setFontHeight(std::max(1, scaled(static_cast<float>(fontHeight))));
            text->setTextColour(MyGUI::Colour(0.18f, 1.f, 0.22f));
            text->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
            text->setNeedMouseFocus(false);
            return text;
        };
        const auto setWholeImageTexture = [](MyGUI::ImageBox* image, const std::string& texture) {
            image->setImageTexture(texture);
            if (MyGUI::ITexture* source = MyGUI::RenderManager::getInstance().getTexture(texture))
            {
                const MyGUI::IntSize sourceSize(source->getWidth(), source->getHeight());
                // ImageBox retains a one-pixel default source rectangle until
                // an explicit image coordinate is supplied. Retail menu image
                // tiles describe their destination size in XML and expect the
                // complete DDS to be scaled into it.
                image->setImageTile(sourceSize);
                image->setImageCoord(MyGUI::IntCoord(0, 0, sourceSize.width, sourceSize.height));
            }
        };
        std::unordered_map<std::string, MyGUI::Widget*> authoredWidgets;
        struct AtlasEntry
        {
            std::string mTexture;
            MyGUI::IntCoord mCoord;
        };
        std::unordered_map<std::string, AtlasEntry> sharedAtlas;
        if (const Files::IStreamPtr stream = mResourceSystem->getVFS()->find(
                VFS::Path::Normalized("textures/interface/interfaceshared.tai")))
        {
            const std::string source{ std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>() };
            std::istringstream lines(source);
            std::string line;
            while (std::getline(lines, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                std::replace(line.begin(), line.end(), ',', ' ');
                std::istringstream fields(line);
                std::string name;
                std::string atlas;
                int index = 0;
                std::string type;
                float x = 0.f;
                float y = 0.f;
                float z = 0.f;
                float w = 0.f;
                float h = 0.f;
                if (!(fields >> name >> atlas >> index >> type >> x >> y >> z >> w >> h) || type != "2D")
                    continue;
                Misc::StringUtils::lowerCaseInPlace(name);
                const std::string texture = normalizeFnvMenuTexturePath("interface/" + atlas);
                sharedAtlas.emplace(name, AtlasEntry{ texture,
                    MyGUI::IntCoord(static_cast<int>(std::lround(x * 1024.f)),
                        static_cast<int>(std::lround(y * 1024.f)),
                        std::max(1, static_cast<int>(std::lround(w * 1024.f))),
                        std::max(1, static_cast<int>(std::lround(h * 1024.f)))) });
            }
        }
        std::size_t renderedTiles = 0;
        std::size_t unresolvedImages = 0;
        const auto renderTiles = [&](const auto& self, const FnvMenuXmlNode& node, MyGUI::Widget* parent) -> void {
            if (node.mType == "template")
                return; // Retail instantiates templates from live list/tab data below.

            const bool isTile = node.mType == "rect" || node.mType == "hotrect" || node.mType == "image"
                || node.mType == "text";
            MyGUI::Widget* widget = parent;
            if (isTile)
            {
                const auto trait = [&](std::string_view name, float fallback) {
                    if (std::none_of(node.mChildren.begin(), node.mChildren.end(),
                            [name](const FnvMenuXmlNode& child) { return child.mType == name; }))
                        return fallback;
                    return evaluateFnvMenuNodeScalarTrait(menu->mRoot, node, name, layoutContext).value_or(fallback);
                };
                const int x = scaled(trait("x", 0.f));
                const int y = scaled(trait("y", 0.f));
                const bool structural = node.mType == "rect" || node.mType == "hotrect";
                const float fallbackWidth = structural
                    ? static_cast<float>(std::max(1, parent->getWidth())) / scale
                    : (node.mType == "text" ? 400.f : 1.f);
                const float fallbackHeight = structural
                    ? static_cast<float>(std::max(1, parent->getHeight())) / scale
                    : (node.mType == "text" ? 55.f : 1.f);
                const int tileWidth = std::max(1, scaled(trait("width", fallbackWidth)));
                consâ€¦27136 tokens truncatedâ€¦      previous->mMode = camera->getMode();
        previous->mPitch = camera->getPitch();
        previous->mYaw = camera->getYaw();
        previous->mRoll = camera->getRoll();
        previous->mFieldOfView = rendering->getFieldOfView();
        previous->mFieldOfViewWasOverridden = rendering->isFieldOfViewOverridden();
        previous->mTarget = target;
        previous->mCameraPosition = cameraPosition;

        camera->setMode(MWRender::Camera::Mode::Static, true);
        camera->setStaticPosition(cameraPosition);
        const osg::Vec3f aimDelta = focus - cameraPosition;
        const float aimHorizontal = std::hypot(aimDelta.x(), aimDelta.y());
        camera->setPitch(std::atan2(aimDelta.z(), aimHorizontal), true);
        camera->setYaw(-std::atan2(aimDelta.x(), aimDelta.y()), true);
        camera->setRoll(0.f);

        // The save-derived first-person projection is also the closest available retail dialogue zoom contract.
        // Explicit projection-oracle captures retain ownership of their matrix and are never overridden here.
        if (!rendering->isProjectionMatrixOverridden())
        {
            const float firstPersonFov = Settings::camera().mFirstPersonFieldOfView;
            constexpr float nativeSaveDialogueFov = 42.653862f;
            const float dialogueFov = std::clamp(firstPersonFov, 38.f, nativeSaveDialogueFov);
            rendering->overrideFieldOfView(dialogueFov);
            previous->mChangedFieldOfView = true;
        }

        mFalloutDialogueCamera = std::move(previous);
        Log(Debug::Info) << "FNV dialogue camera: target=" << target.toString() << " focus=(" << focus.x() << ","
                         << focus.y() << "," << focus.z() << ") camera=(" << cameraPosition.x() << ","
                         << cameraPosition.y() << "," << cameraPosition.z() << ") distance="
                         << (cameraPosition - focus).length() << " rayClamped=" << (rayClamped ? 1 : 0)
                         << " obstructionDistance=" << obstructionDistance
                         << " fov=" << rendering->getFieldOfView();
    }

    void WindowManager::updateFalloutDialogueCamera()
    {
        if (!mFalloutDialogueCamera)
            return;

        MWBase::World* const world = MWBase::Environment::get().getWorld();
        const MWWorld::Ptr& target = mFalloutDialogueCamera->mTarget;
        if (world == nullptr || VR::getVR() || !containsMode(GM_Dialogue) || target.isEmpty() || !target.isInCell()
            || target.getCellRef().getCount() <= 0 || target.mRef->isDeleted())
        {
            endFalloutDialogueCamera();
            return;
        }

        MWRender::Camera* const camera = world->getCamera();
        if (camera == nullptr || camera->getMode() != MWRender::Camera::Mode::Static)
        {
            endFalloutDialogueCamera();
            return;
        }

        osg::Vec3f focus = world->getActorHeadTransform(target).getTrans();
        const osg::Vec3f actorPosition = target.getRefData().getPosition().asVec3();
        if ((focus - actorPosition).length2() <= 16.f * 16.f)
        {
            const osg::Vec3f halfExtents = world->getHalfExtents(target, true);
            focus.z() += std::max(64.f, halfExtents.z() * 2.f * 0.85f);
        }
        if (!std::isfinite(focus.x()) || !std::isfinite(focus.y()) || !std::isfinite(focus.z()))
        {
            endFalloutDialogueCamera();
            return;
        }

        camera->setStaticPosition(mFalloutDialogueCamera->mCameraPosition);
        const osg::Vec3f aimDelta = focus - mFalloutDialogueCamera->mCameraPosition;
        const float horizontal = std::hypot(aimDelta.x(), aimDelta.y());
        if (horizontal < 1.f)
        {
            endFalloutDialogueCamera();
            return;
        }
        camera->setPitch(std::atan2(aimDelta.z(), horizontal), true);
        camera->setYaw(-std::atan2(aimDelta.x(), aimDelta.y()), true);
    }

    void WindowManager::endFalloutDialogueCamera()
    {
        if (!mFalloutDialogueCamera)
            return;

        std::unique_ptr<FalloutDialogueCameraState> previous = std::move(mFalloutDialogueCamera);
        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return;

        if (MWRender::Camera* const camera = world->getCamera())
        {
            camera->setMode(previous->mMode, true);
            camera->setPitch(previous->mPitch, true);
            camera->setYaw(previous->mYaw, true);
            camera->setRoll(previous->mRoll);
        }
        if (previous->mChangedFieldOfView)
        {
            if (MWRender::RenderingManager* const rendering = world->getRenderingManager())
            {
                if (previous->mFieldOfViewWasOverridden)
                    rendering->overrideFieldOfView(previous->mFieldOfView);
                else
                    rendering->resetFieldOfView();
            }
        }
        Log(Debug::Info) << "FNV dialogue camera: restored player view";
    }

    void WindowManager::beginFalloutTerminalSession(const MWWorld::Ptr& target)
    {
        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (world == nullptr || VR::getVR() || target.isEmpty() || !target.isInCell()
            || world->getStore().getESM4Game() != MWWorld::ESM4Game::FalloutNewVegas)
            return;

        const MWWorld::Ptr player = world->getPlayerPtr();
        MWRender::Camera* const camera = world->getCamera();
        MWRender::RenderingManager* const rendering = world->getRenderingManager();
        if (player.isEmpty() || !player.isInCell() || player.getCell() != target.getCell() || camera == nullptr
            || rendering == nullptr || camera->getMode() == MWRender::Camera::Mode::VR)
            return;

        endFalloutTerminalSession();
        osg::Vec3f focus = target.getRefData().getPosition().asVec3();
        const osg::Vec3f halfExtents = world->getHalfExtents(target, true);
        focus.z() += std::max(24.f, halfExtents.z() * 0.65f);
        osg::Vec3f playerFocus = player.getRefData().getPosition().asVec3();
        playerFocus.z() += std::max(48.f, world->getHalfExtents(player, true).z());
        osg::Vec3f outward = playerFocus - focus;
        const float horizontal = std::hypot(outward.x(), outward.y());
        if (horizontal < 1.f)
            return;
        outward.z() = std::clamp(outward.z(), -horizontal * 0.25f, horizontal * 0.25f);
        outward.normalize();

        const float desiredDistance = std::clamp(halfExtents.length() * 1.5f, 88.f, 160.f);
        osg::Vec3f cameraPosition = focus + outward * desiredDistance;
        MWPhysics::RayCastingResult hit{};
        const std::array<MWWorld::Ptr, 1> ignored{ target };
        if (world->castRenderingRay(hit, focus, cameraPosition, true, true, ignored))
        {
            const float available = (hit.mHitPos - focus).length() - 16.f;
            if (available < 40.f)
                return;
            cameraPosition = focus + outward * std::min(desiredDistance, available);
        }

        auto previous = std::make_unique<FalloutDialogueCameraState>();
        previous->mMode = camera->getMode();
        previous->mPitch = camera->getPitch();
        previous->mYaw = camera->getYaw();
        previous->mRoll = camera->getRoll();
        previous->mFieldOfView = rendering->getFieldOfView();
        previous->mFieldOfViewWasOverridden = rendering->isFieldOfViewOverridden();
        previous->mTarget = target;
        previous->mCameraPosition = cameraPosition;

        camera->setMode(MWRender::Camera::Mode::Static, true);
        camera->setStaticPosition(cameraPosition);
        const osg::Vec3f aimDelta = focus - cameraPosition;
        camera->setPitch(std::atan2(aimDelta.z(), std::hypot(aimDelta.x(), aimDelta.y())), true);
        camera->setYaw(-std::atan2(aimDelta.x(), aimDelta.y()), true);
        camera->setRoll(0.f);
        if (!rendering->isProjectionMatrixOverridden())
        {
            rendering->overrideFieldOfView(50.f);
            previous->mChangedFieldOfView = true;
        }
        // Retail terminal interaction replaces the gameplay HUD while retaining the physical world and shell.
        // Suppress only the HUD widget; the modal terminal presenter remains visible on its own layer.
        if (mHud)
            mHud->setVisible(false);
        mFalloutTerminalCamera = std::move(previous);
        Log(Debug::Info) << "FNV terminal camera: physical shell target=" << target.toString() << " focus=("
                         << focus.x() << "," << focus.y() << "," << focus.z() << ") distance="
                         << (cameraPosition - focus).length();
    }

    void WindowManager::endFalloutTerminalSession()
    {
        if (!mFalloutTerminalCamera)
            return;
        std::unique_ptr<FalloutDialogueCameraState> previous = std::move(mFalloutTerminalCamera);
        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return;
        if (MWRender::Camera* const camera = world->getCamera())
        {
            camera->setMode(previous->mMode, true);
            camera->setPitch(previous->mPitch, true);
            camera->setYaw(previous->mYaw, true);
            camera->setRoll(previous->mRoll);
        }
        if (previous->mChangedFieldOfView)
        {
            if (MWRender::RenderingManager* const rendering = world->getRenderingManager())
            {
                if (previous->mFieldOfViewWasOverridden)
                    rendering->overrideFieldOfView(previous->mFieldOfView);
                else
                    rendering->resetFieldOfView();
            }
        }
        updateVisible();
        Log(Debug::Info) << "FNV terminal camera: restored player view";
    }

    void WindowManager::pushGuiMode(GuiMode mode)
    {
        pushGuiMode(mode, MWWorld::Ptr());
    }

    void WindowManager::pushGuiMode(GuiMode mode, const MWWorld::Ptr& arg)
    {
        pushGuiMode(mode, arg, false);
    }

    void WindowManager::forceLootMode(const MWWorld::Ptr& ptr)
    {
        pushGuiMode(MWGui::GM_Container, ptr, true);
    }

    void WindowManager::pushGuiMode(GuiMode mode, const MWWorld::Ptr& arg, bool force)
    {
        if (mode == GM_Inventory && mAllowed == GW_None)
            return;

        if (mGuiModes.empty() || mGuiModes.back() != mode)
        {
            // If this mode already exists somewhere in the stack, just bring it to the front.
            if (std::find(mGuiModes.begin(), mGuiModes.end(), mode) != mGuiModes.end())
            {
                mGuiModes.erase(std::find(mGuiModes.begin(), mGuiModes.end(), mode));
            }

            if (!mGuiModes.empty())
            {
                mKeyboardNavigation->saveFocus(mGuiModes.back());
                mGuiModeStates[mGuiModes.back()].update(false);
            }
            mGuiModes.push_back(mode);

            mGuiModeStates[mode].update(true);
        }
        if (force)
            mContainerWindow->treatNextOpenAsLoot();

        try
        {
            for (WindowBase* window : mGuiModeStates[mode].mWindows)
                window->setPtr(arg);
        }
        catch (...)
        {
            popGuiMode();
            throw;
        }

        mKeyboardNavigation->restoreFocus(mode);

        updateVisible();
        MWBase::Environment::get().getLuaManager()->uiModeChanged(arg);

        if (mode == GM_Dialogue)
            beginFalloutDialogueCamera(arg);

        if (Settings::gui().mControllerMenus)
        {
            if (mode == GM_Container)
                mActiveControllerWindows[mode] = 0; // Ensure controller focus is on container
            // Activate first visible window. This needs to be called after updateVisible.
            mActiveControllerWindows[mode] = std::max(mActiveControllerWindows[mode] - 1, -1);
            cycleActiveControllerWindow(true);
        }
    }

    void WindowManager::setCullMask(uint32_t mask)
    {
        mViewer->getCamera()->setCullMask(mask);

        // We could check whether stereo is enabled here, but these methods are
        // trivial and have no effect in mono or multiview so just call them regardless.
        mViewer->getCamera()->setCullMaskLeft(mask);
        mViewer->getCamera()->setCullMaskRight(mask);
    }

    uint32_t WindowManager::getCullMask()
    {
        return mViewer->getCamera()->getCullMask();
    }

//## VR_PATCH BEGIN
    void WindowManager::enterVoid()
    {
        if (!mTheVoid)
        {
            mTheVoid = true;
            updateVisible();
        }
    }

    bool WindowManager::isInVoid()
    {
        return mTheVoid;
    }

    void WindowManager::exitVoid()
    {
        if (mTheVoid)
        {
            mTheVoid = false;
            updateVisible();
        }
    }

//## VR_PATCH END
    void WindowManager::popGuiMode(bool forceExit)
    {
        bool removedDialogue = false;
        if (mDragAndDrop && mDragAndDrop->mIsOnDragAndDrop)
        {
            mDragAndDrop->finish();
        }

        if (!mGuiModes.empty())
        {
            const GuiMode mode = mGuiModes.back();
            if (forceExit)
            {
                GuiModeState& state = mGuiModeStates[mode];
                for (const auto& window : state.mWindows)
                    window->exit();
            }
            mKeyboardNavigation->saveFocus(mode);
            if (containsMode(mode))
            {
                mGuiModes.pop_back();
                mGuiModeStates[mode].update(false);
                MWBase::Environment::get().getLuaManager()->uiModeChanged(MWWorld::Ptr());
                removedDialogue = mode == GM_Dialogue;
            }
        }

        if (!mGuiModes.empty())
        {
            const GuiMode mode = mGuiModes.back();
            mGuiModeStates[mode].update(true);
            mKeyboardNavigation->restoreFocus(mode);
        }

        if (!containsMode(GM_Inventory))
            mFalloutPipBoyPhysical = false;
        updateVisible();

        if (removedDialogue && !containsMode(GM_Dialogue))
            endFalloutDialogueCamera();

        // To make sure that console window get focus again
        if (mConsole && mConsole->isVisible())
            mConsole->onOpen();

        if (Settings::gui().mControllerMenus)
        {
            if (mGuiModes.empty())
                setControllerTooltipVisible(false);
            else
                reapplyActiveControllerWindow();
        }
    }

    void WindowManager::removeGuiMode(GuiMode mode)
    {
        if (!mGuiModes.empty() && mGuiModes.back() == mode)
        {
            popGuiMode();
            return;
        }

        bool removedDialogue = false;
        std::vector<GuiMode>::iterator it = mGuiModes.begin();
        while (it != mGuiModes.end())
        {
            if (*it == mode)
            {
                removedDialogue = removedDialogue || mode == GM_Dialogue;
                it = mGuiModes.erase(it);
            }
            else
                ++it;
        }

        if (!containsMode(GM_Inventory))
            mFalloutPipBoyPhysical = false;
        updateVisible();
        MWBase::Environment::get().getLuaManager()->uiModeChanged(MWWorld::Ptr());
        if (removedDialogue && !containsMode(GM_Dialogue))
            endFalloutDialogueCamera();
    }

    void WindowManager::goToJail(int days)
    {
        pushGuiMode(MWGui::GM_Jail);
        mJailScreen->goToJail(days);
    }

    void WindowManager::setSelectedSpell(const ESM::RefId& spellId, int successChancePercent)
    {
        mSelectedSpell = spellId;
        mSelectedEnchantItem = MWWorld::Ptr();
        mHud->setSelectedSpell(spellId, successChancePercent);

        const ESM::Spell* spell = mStore->get<ESM::Spell>().find(spellId);

        mSpellWindow->setTitle(spell->mName);
    }

    void WindowManager::setSelectedEnchantItem(const MWWorld::Ptr& item)
    {
        mSelectedEnchantItem = item;
        mSelectedSpell = ESM::RefId();
        const ESM::Enchantment* ench = mStore->get<ESM::Enchantment>().find(item.getClass().getEnchantment(item));

        int chargePercent = static_cast<int>(item.getCellRef().getNormalizedEnchantmentCharge(*ench) * 100);
        mHud->setSelectedEnchantItem(item, chargePercent);
        mSpellWindow->setTitle(item.getClass().getName(item));
    }

    const MWWorld::Ptr& WindowManager::getSelectedEnchantItem() const
    {
        return mSelectedEnchantItem;
    }

    void WindowManager::setSelectedWeapon(const MWWorld::Ptr& item)
    {
        mSelectedWeapon = item;
        int durabilityPercent = 100;
        if (item.getClass().hasItemHealth(item))
        {
            durabilityPercent = static_cast<int>(item.getClass().getItemNormalizedHealth(item) * 100);
        }
        mHud->setSelectedWeapon(item, durabilityPercent);
        mInventoryWindow->setTitle(item.getClass().getName(item));
    }

    const MWWorld::Ptr& WindowManager::getSelectedWeapon() const
    {
        return mSelectedWeapon;
    }

    void WindowManager::unsetSelectedSpell()
    {
        mSelectedSpell = ESM::RefId();
        mSelectedEnchantItem = MWWorld::Ptr();
        mHud->unsetSelectedSpell();

        MWWorld::Player* player = &MWBase::Environment::get().getWorld()->getPlayer();
        if (player->getDrawState() == MWMechanics::DrawState::Spell)
            player->setDrawState(MWMechanics::DrawState::Nothing);

        mSpellWindow->setTitle(isFalloutContentLoaded() ? "DATA / QUESTS" : "#{Interface:None}");
    }

    void WindowManager::unsetSelectedWeapon()
    {
        mSelectedWeapon = MWWorld::Ptr();
        mHud->unsetSelectedWeapon();
        mInventoryWindow->setTitle(isFalloutContentLoaded() ? "Unarmed" : "#{sSkillHandtohand}");
    }

    void WindowManager::getMousePosition(int& x, int& y)
    {
        const MyGUI::IntPoint& pos = MyGUI::InputManager::getInstance().getMousePosition();
        x = pos.left;
        y = pos.top;
    }

    void WindowManager::getMousePosition(float& x, float& y)
    {
        const MyGUI::IntPoint& pos = MyGUI::InputManager::getInstance().getMousePosition();
        x = static_cast<float>(pos.left);
        y = static_cast<float>(pos.top);
        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        x /= viewSize.width;
        y /= viewSize.height;
    }

    bool WindowManager::getWorldMouseOver()
    {
        return mHud->getWorldMouseOver();
    }

    float WindowManager::getScalingFactor() const
    {
        return mScalingFactor;
    }

    void WindowManager::executeInConsole(const std::filesystem::path& path)
    {
        mConsole->executeFile(path);
    }

    std::vector<MWGui::WindowBase*> WindowManager::getGuiModeWindows(GuiMode mode)
    {
        return mGuiModeStates[mode].mWindows;
    }
    MWGui::InventoryWindow* WindowManager::getInventoryWindow()
    {
        return mInventoryWindow;
    }
    MWGui::CountDialog* WindowManager::getCountDialog()
    {
        return mCountDialog;
    }
    MWGui::ConfirmationDialog* WindowManager::getConfirmationDialog()
    {
        return mConfirmationDialog;
    }
    MWGui::HUD* WindowManager::getHud()
    {
        return mHud;
    }
    MWGui::TradeWindow* WindowManager::getTradeWindow()
    {
        return mTradeWindow;
    }
    MWGui::PostProcessorHud* WindowManager::getPostProcessorHud()
    {
        return mPostProcessorHud;
    }

    void WindowManager::useItem(const MWWorld::Ptr& item, bool bypassBeastRestrictions)
    {
        if (mInventoryWindow)
            mInventoryWindow->useItem(item, bypassBeastRestrictions);
    }

    bool WindowManager::isAllowed(GuiWindow wnd) const
    {
        return (mAllowed & wnd) != 0;
    }

    void WindowManager::allow(GuiWindow wnd)
    {
        mAllowed = (GuiWindow)(mAllowed | wnd);

        if (wnd & GW_Inventory)
        {
            mBookWindow->setInventoryAllowed(true);
            mScrollWindow->setInventoryAllowed(true);
        }

        updateVisible();
    }

    void WindowManager::disallowAll()
    {
        mAllowed = GW_None;
        mRestAllowed = false;

        mBookWindow->setInventoryAllowed(false);
        mScrollWindow->setInventoryAllowed(false);

        updateVisible();
    }

    void WindowManager::toggleVisible(GuiWindow wnd)
    {
        if (getMode() != GM_Inventory)
            return;

        if (Settings::SettingValue<bool>* const hidden = findHiddenSetting(wnd))
            hidden->set(!hidden->get());

        mShown = (GuiWindow)(mShown ^ wnd);
        updateVisible();
    }

//## VR_PATCH BEGIN
    DragAndDrop& WindowManager::getDragAndDrop(void)
    {
        return *mDragAndDrop;
    }

//## VR_PATCH END
    void WindowManager::forceHide(GuiWindow wnd)
    {
        mForceHidden = (GuiWindow)(mForceHidden | wnd);
        updateVisible();
    }

    void WindowManager::unsetForceHide(GuiWindow wnd)
    {
        mForceHidden = (GuiWindow)(mForceHidden & ~wnd);
        updateVisible();
    }

    bool WindowManager::isGuiMode() const
    {
        return !mGuiModes.empty() || isConsoleMode() || isPostProcessorHudVisible() || isInteractiveMessageBoxActive();
    }

    bool WindowManager::isConsoleMode() const
    {
        return mConsole && mConsole->isVisible();
    }

    bool WindowManager::isPostProcessorHudVisible() const
    {
        return mPostProcessorHud && mPostProcessorHud->isVisible();
    }

    bool WindowManager::isSettingsWindowVisible() const
    {
        return mSettingsWindow && mSettingsWindow->isVisible();
    }

    bool WindowManager::isInteractiveMessageBoxActive() const
    {
        return mMessageBoxManager && mMessageBoxManager->isInteractiveMessageBox();
    }

    void WindowManager::closeInteractiveMessageBoxWithDefaultButton() 
    {
        if (mMessageBoxManager && mMessageBoxManager->isInteractiveMessageBox()
            && mCurrentModals.back() == mMessageBoxManager->getInteractiveMessageBox())
        {
            static_cast<MWGui::InteractiveMessageBox*>(mCurrentModals.back())->closeDefault();
        }
    }

    MWGui::GuiMode WindowManager::getMode() const
    {
        if (mGuiModes.empty())
            return GM_None;
        return mGuiModes.back();
    }

    void WindowManager::disallowMouse()
    {
        mInputBlocker->setVisible(true);
    }

    void WindowManager::allowMouse()
    {
        mInputBlocker->setVisible(!isGuiMode());
    }

    void WindowManager::notifyInputActionBound()
    {
        mSettingsWindow->updateControlsBox();
        allowMouse();
    }

    bool WindowManager::containsMode(GuiMode mode) const
    {
        if (mGuiModes.empty())
            return false;

        return std::find(mGuiModes.begin(), mGuiModes.end(), mode) != mGuiModes.end();
    }

    void WindowManager::showCrosshair(bool show)
    {
        if (mHud)
            mHud->setCrosshairVisible(show && Settings::hud().mCrosshair);
    }

    void WindowManager::updateActivatedQuickKey()
    {
        mQuickKeysMenu->updateActivatedQuickKey();
    }

    void WindowManager::activateQuickKey(int index)
    {
        mQuickKeysMenu->activateQuickKey(index);
    }

    bool WindowManager::setFalloutSaveQuickKey(std::uint8_t index, const ESM::RefId& item)
    {
        return mQuickKeysMenu->setFalloutSaveQuickKey(index, item);
    }

    bool WindowManager::setHudVisibility(bool show)
    {
        mHudEnabled = show;
        updateVisible();
        mMessageBoxManager->setVisible(mHudEnabled);
        return mHudEnabled;
    }

    void WindowManager::setLegacyHudSuppressed(bool suppress)
    {
        if (mLegacyHudSuppressed == suppress)
            return;

        mLegacyHudSuppressed = suppress;
        if (suppress)
        {
            Log(Debug::Warning) << "Suppressing legacy ESM3 HUD for a validated native FNV save; native FNV HUD "
                                   "remains uncovered";
        }
        updateVisible();
    }

    bool WindowManager::getRestEnabled()
    {
        // Enable rest dialogue if character creation finished
        if (mRestAllowed == false
            && MWBase::Environment::get().getWorld()->getGlobalFloat(MWWorld::Globals::sCharGenState) == -1)
            mRestAllowed = true;
        const MWWorld::FalloutPlayerRuntimeState& falloutState
            = MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState();
        return mRestAllowed && (!falloutState.isInitialized() || falloutState.isWaitEnabled());
    }

    bool WindowManager::getPlayerSleeping()
    {
        return mWaitDialog->getSleeping();
    }

    void WindowManager::wakeUpPlayer()
    {
        mWaitDialog->wakeUp();
    }

    void WindowManager::addVisitedLocation(const std::string& name, int x, int y)
    {
        mMap->addVisitedLocation(name, x, y);
    }

    void WindowManager::refreshFalloutMapMarkers()
    {
        mMap->refreshFalloutMapMarkers();
    }

    bool WindowManager::focusFalloutMapMarker(ESM::FormId marker, float zoom)
    {
        return mMap != nullptr && mMap->focusFalloutMapMarker(marker, zoom);
    }

    bool WindowManager::requestFalloutFastTravel(ESM::FormId marker)
    {
        return mMap != nullptr && mMap->requestFalloutFastTravel(marker);
    }

    void WindowManager::confirmFalloutFastTravel()
    {
        if (mConfirmationDialog != nullptr)
            mConfirmationDialog->confirm();
    }

    const Translation::Storage& WindowManager::getTranslationDataStorage() const
    {
        return mTranslationDataStorage;
    }

    void WindowManager::changePointer(const std::string& name)
    {
        MyGUI::PointerManager::getInstance().setPointer(name);
        onCursorChange(name);
    }

    void WindowManager::showSoulgemDialog(MWWorld::Ptr item)
    {
        mSoulgemDialog->show(item);
        updateVisible();
    }

    void WindowManager::updatePlayer()
    {
        mInventoryWindow->updatePlayer();

        const MWWorld::Ptr player = MWMechanics::getPlayer();
        if (player.getClass().getNpcStats(player).isWerewolf())
        {
            setWerewolfOverlay(true);
            forceHide((GuiWindow)(MWGui::GW_Inventory | MWGui::GW_Magic));
        }
    }

    // Remove this wrapper once onKeyFocusChanged call is rendered unnecessary
    void WindowManager::setKeyFocusWidget(MyGUI::Widget* widget)
    {
        MyGUI::InputManager::getInstance().setKeyFocusWidget(widget);
        onKeyFocusChanged(widget);
    }

    void WindowManager::onKeyFocusChanged(MyGUI::Widget* widget)
    {
        bool isEditBox = widget && widget->castType<MyGUI::EditBox>(false);
        LuaUi::WidgetExtension* luaWidget = dynamic_cast<LuaUi::WidgetExtension*>(widget);
        bool capturesInput = luaWidget ? luaWidget->isTextInput() : isEditBox;
        if (widget && capturesInput)
            SDL_StartTextInput();
        else
            SDL_StopTextInput();
    }

    void WindowManager::setEnemy(const MWWorld::Ptr& enemy)
    {
        mHud->setEnemy(enemy);
    }

    std::size_t WindowManager::getMessagesCount() const
    {
        std::size_t count = 0;
        if (mMessageBoxManager)
            count = mMessageBoxManager->getMessagesCount();

        return count;
    }

    Loading::Listener* WindowManager::getLoadingScreen()
    {
        return mLoadingScreen;
    }

    bool WindowManager::getCursorVisible()
    {
        return mCursorVisible && mCursorActive;
    }

    void WindowManager::trackWindow(Layout* layout, const WindowSettingValues& settings)
    {
        MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();

//## VR_PATCH BEGIN
        // All windows need to be maximized in VR.
        MyGUI::Window* window = layout->mMainWidget->castType<MyGUI::Window>();
        const WindowRectSettingValues& rect = settings.mIsMaximized || VR::getVR() ? settings.mMaximized : settings.mRegular;
//## VR_PATCH END

        window->setPosition(
            MyGUI::IntPoint(static_cast<int>(rect.mX * viewSize.width), static_cast<int>(rect.mY * viewSize.height)));
        window->setSize(
            MyGUI::IntSize(static_cast<int>(rect.mW * viewSize.width), static_cast<int>(rect.mH * viewSize.height)));

        window->eventWindowChangeCoord += MyGUI::newDelegate(this, &WindowManager::onWindowChangeCoord);
        WindowBase::clampWindowCoordinates(window);

        mTrackedWindows.emplace(window, settings);
    }

    void WindowManager::toggleMaximized(Layout* layout)
    {
        MyGUI::Window* window = layout->mMainWidget->castType<MyGUI::Window>();
        const auto it = mTrackedWindows.find(window);
        if (it == mTrackedWindows.end())
            return;

        const WindowSettingValues& settings = it->second;
        const WindowRectSettingValues& rect = settings.mIsMaximized ? settings.mRegular : settings.mMaximized;

//## VR_PATCH BEGIN
// Windows are always maximized in VR
        if (VR::getVR() && settings.mIsMaximized)
            return;
//## VR_PATCH END

        MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        const float x = rect.mX * viewSize.width;
        const float y = rect.mY * viewSize.height;
        const float w = rect.mW * viewSize.width;
        const float h = rect.mH * viewSize.height;
        window->setCoord(x, y, w, h);

        settings.mIsMaximized.set(!settings.mIsMaximized.get());
    }

    void WindowManager::onWindowChangeCoord(MyGUI::Window* window)
    {
//## VR_PATCH BEGIN
// Windows never move in VR
        if (VR::getVR())
            return;
//## VR_PATCH END

        // If using controller menus, don't persist changes to size of the stats or magic
        // windows.
        if (Settings::gui().mControllerMenus
            && (window == mStatsWindow->mMainWidget->castType<MyGUI::Window>()
                || window == mSpellWindow->mMainWidget->castType<MyGUI::Window>()))
            return;

        const auto it = mTrackedWindows.find(window);
        if (it == mTrackedWindows.end())
            return;

        WindowBase::clampWindowCoordinates(window);

        const WindowSettingValues& settings = it->second;

        MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        settings.mRegular.mX.set(window->getPosition().left / static_cast<float>(viewSize.width));
        settings.mRegular.mY.set(window->getPosition().top / static_cast<float>(viewSize.height));
        settings.mRegular.mW.set(window->getSize().width / static_cast<float>(viewSize.width));
        settings.mRegular.mH.set(window->getSize().height / static_cast<float>(viewSize.height));

        settings.mIsMaximized.set(false);
    }

    void WindowManager::clear()
    {
        mPlayerBounty = -1;
        // Session-local compatibility policy. Normal ESM3 loads must never inherit FNV HUD suppression.
        mLegacyHudSuppressed = false;

        for (const auto& window : mWindows)
        {
            window->clear();
            window->setDisabledByLua(false);
        }

        if (mLocalMapRender)
            mLocalMapRender->clear();

        mMessageBoxManager->clear();

        mToolTips->clear();

        mSelectedSpell = ESM::RefId();
        mCustomMarkers.clear();

        mForceHidden = GW_None;
        mRestAllowed = true;

        while (!mGuiModes.empty())
            popGuiMode();

        updateVisible();
    }

    void WindowManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        mMap->write(writer, progress);

        mQuickKeysMenu->write(writer);

        if (!mSelectedSpell.empty())
        {
            writer.startRecord(ESM::REC_ASPL);
            writer.writeHNRefId("ID__", mSelectedSpell);
            writer.endRecord(ESM::REC_ASPL);
        }

        for (CustomMarkerCollection::ContainerType::const_iterator it = mCustomMarkers.begin();
             it != mCustomMarkers.end(); ++it)
        {
            writer.startRecord(ESM::REC_MARK);
            it->second.save(writer);
            writer.endRecord(ESM::REC_MARK);
        }
    }

    void WindowManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type == ESM::REC_GMAP)
            mMap->readRecord(reader, type);
        else if (type == ESM::REC_KEYS)
            mQuickKeysMenu->readRecord(reader, type);
        else if (type == ESM::REC_ASPL)
        {
            reader.getSubNameIs("ID__");
            ESM::RefId spell = reader.getRefId();
            if (mStore->get<ESM::Spell>().search(spell))
                mSelectedSpell = spell;
        }
        else if (type == ESM::REC_MARK)
        {
            ESM::CustomMarker marker;
            marker.load(reader);
            mCustomMarkers.addMarker(marker, false);
        }
    }

    int WindowManager::countSavedGameRecords() const
    {
        return 1 // Global map
            + 1 // QuickKeysMenu
            + mCustomMarkers.size() + (!mSelectedSpell.empty() ? 1 : 0);
    }

    bool WindowManager::isSavingAllowed() const
    {
        return !MyGUI::InputManager::getInstance().isModalAny()
            && !isConsoleMode()
            // TODO: remove this, once we have properly serialized the state of open windows
            && (!isGuiMode() || (mGuiModes.size() == 1 && (getMode() == GM_MainMenu || getMode() == GM_Rest)));
    }

    void WindowManager::playVideo(std::string_view name, bool allowSkipping, bool overrideSounds)
    {
//## VR_PATCH BEGIN
        mVideoEnabled = true;
//## VR_PATCH END
        mVideoWidget->playVideo("video\\" + std::string{ name });

        mVideoWidget->eventKeyButtonPressed.clear();
        mVideoBackground->eventKeyButtonPressed.clear();
        if (allowSkipping)
        {
            mVideoWidget->eventKeyButtonPressed += MyGUI::newDelegate(this, &WindowManager::onVideoKeyPressed);
            mVideoBackground->eventKeyButtonPressed += MyGUI::newDelegate(this, &WindowManager::onVideoKeyPressed);
        }

        enableScene(false);

        MyGUI::IntSize screenSize = MyGUI::RenderManager::getInstance().getViewSize();
        sizeVideo(screenSize.width, screenSize.height);

        MyGUI::Widget* oldKeyFocus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        setKeyFocusWidget(mVideoWidget);

        mVideoBackground->setVisible(true);

//## VR_PATCH BEGIN
        if(VR::getVR())
            MWVR::VRGUIManager::instance().setForceLayerVisible(mVideoBackground->getLayer()->getName(), true);
//## VR_PATCH END

        bool cursorWasVisible = mCursorVisible;
        setCursorVisible(false);

        if (overrideSounds && mVideoWidget->hasAudioStream())
            MWBase::Environment::get().getSoundManager()->pauseSounds(
                MWSound::VideoPlayback, ~MWSound::Type::Movie & MWSound::Type::Mask);

        Misc::FrameRateLimiter frameRateLimiter
            = Misc::makeFrameRateLimiter(MWBase::Environment::get().getFrameRateLimit());
//## VR_PATCH BEGIN
        while (
            mVideoEnabled && mVideoWidget->update() && !MWBase::Environment::get().getStateManager()->hasQuitRequest())
//## VR_PATCH END
        {
            const double dt
                = std::chrono::duration_cast<std::chrono::duration<double>>(frameRateLimiter.getLastFrameDuration())
                      .count();

            MWBase::Environment::get().getInputManager()->update(dt, true, false);

            if (!mWindowVisible)
            {
                mVideoWidget->pause();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            else
            {
                if (mVideoWidget->isPaused())
                    mVideoWidget->resume();

//## VR_PATCH BEGIN
                viewerTraversals();
//## VR_PATCH END
            }
            // at the time this function is called we are in the middle of a frame,
            // so out of order calls are necessary to get a correct frameNumber for the next frame.
            // refer to the advance() and frame() order in Engine::go()
            mViewer->advance(mViewer->getFrameStamp()->getSimulationTime());

            frameRateLimiter.limit();
        }
        mVideoWidget->stop();

        MWBase::Environment::get().getSoundManager()->resumeSounds(MWSound::VideoPlayback);

        setKeyFocusWidget(oldKeyFocus);

        setCursorVisible(cursorWasVisible);

        // Restore normal rendering
        updateVisible();

//## VR_PATCH BEGIN
        if(VR::getVR())
            MWVR::VRGUIManager::instance().setForceLayerVisible(mVideoBackground->getLayer()->getName(), false);
        mVideoBackground->setVisible(false);
        mVideoEnabled = false;
//## VR_PATCH END
    }

//## VR_PATCH BEGIN
    bool WindowManager::isPlayingVideo(void) const
    {
        return mVideoEnabled;
    }
//## VR_PATCH END

    void WindowManager::sizeVideo(int screenWidth, int screenHeight)
    {
        // Use black bars to correct aspect ratio
        mVideoBackground->setSize(screenWidth, screenHeight);
        mVideoWidget->autoResize(Settings::gui().mStretchMenuBackground);
    }

    void WindowManager::exitCurrentModal()
    {
        if (!mCurrentModals.empty())
        {
            WindowModal* window = mCurrentModals.back();
            if (!window->exit())
                return;
            window->setVisible(false);
            updateControllerButtonsOverlay();
        }
    }

    void WindowManager::addCurrentModal(WindowModal* input)
    {
        if (mCurrentModals.empty())
            mKeyboardNavigation->saveFocus(getMode());

        mCurrentModals.push_back(input);
        mKeyboardNavigation->restoreFocus(-1);

        mKeyboardNavigation->setModalWindow(input->mMainWidget);
        mKeyboardNavigation->setDefaultFocus(input->mMainWidget, input->getDefaultKeyFocus());

        updateControllerButtonsOverlay();
    }

    void WindowManager::removeCurrentModal(WindowModal* input)
    {
        if (!mCurrentModals.empty())
        {
            if (input == mCurrentModals.back())
            {
                mCurrentModals.pop_back();
                mKeyboardNavigation->saveFocus(-1);
            }
            else
            {
                auto found = std::find(mCurrentModals.begin(), mCurrentModals.end(), input);
                if (found != mCurrentModals.end())
                    mCurrentModals.erase(found);
                else
                    Log(Debug::Warning) << "Warning: can't find modal window " << input;
            }
        }
        if (mCurrentModals.empty())
        {
            mKeyboardNavigation->setModalWindow(nullptr);
            mKeyboardNavigation->restoreFocus(getMode());
        }
        else
            mKeyboardNavigation->setModalWindow(mCurrentModals.back()->mMainWidget);
    }

    void WindowManager::onVideoKeyPressed(MyGUI::Widget* /*sender*/, MyGUI::KeyCode key, MyGUI::Char value)
    {
        if (key == MyGUI::KeyCode::Escape)
            mVideoWidget->stop();
    }

    void WindowManager::updatePinnedWindows()
    {
        if (Settings::gui().mControllerMenus)
        {
            // In controller mode, don't hide any menus and only allow pinning the map.
            mInventoryWindow->setPinned(false);
            mMap->setPinned(Settings::windows().mMapPin);
            mSpellWindow->setPinned(false);
            mStatsWindow->setPinned(false);
            return;
        }

        mInventoryWindow->setPinned(Settings::windows().mInventoryPin);
        if (Settings::windows().mInventoryHidden)
            mShown = (GuiWindow)(mShown ^ GW_Inventory);

        mMap->setPinned(Settings::windows().mMapPin);
        if (Settings::windows().mMapHidden)
            mShown = (GuiWindow)(mShown ^ GW_Map);

        mSpellWindow->setPinned(Settings::windows().mSpellsPin);
        if (Settings::windows().mSpellsHidden)
            mShown = (GuiWindow)(mShown ^ GW_Magic);

        mStatsWindow->setPinned(Settings::windows().mStatsPin);
        if (Settings::windows().mStatsHidden)
            mShown = (GuiWindow)(mShown ^ GW_Stats);
    }

    void WindowManager::pinWindow(GuiWindow window)
    {
        if (VR::getVR())
            // Pinning in VR will need some implementation work
            return;

        switch (window)
        {
            case GW_Inventory:
                mInventoryWindow->setPinned(true);
                break;
            case GW_Map:
                mMap->setPinned(true);
                break;
            case GW_Magic:
                mSpellWindow->setPinned(true);
                break;
            case GW_Stats:
                mStatsWindow->setPinned(true);
                break;
            default:
                break;
        }

        updateVisible();
    }

    void WindowManager::fadeScreenIn(const float time, bool clearQueue, float delay)
    {
        if (clearQueue)
            mScreenFader->clearQueue();
        mScreenFader->fadeOut(time, delay);
    }

    void WindowManager::fadeScreenOut(const float time, bool clearQueue, float delay)
    {
        if (clearQueue)
            mScreenFader->clearQueue();
        mScreenFader->fadeIn(time, delay);
    }

    void WindowManager::fadeScreenTo(const int percent, const float time, bool clearQueue, float delay)
    {
        if (clearQueue)
            mScreenFader->clearQueue();
        mScreenFader->fadeTo(percent, time, delay);
    }

    void WindowManager::setBlindness(const int percent)
    {
        mBlindnessFader->notifyAlphaChanged(percent / 100.f);
    }

    void WindowManager::activateHitOverlay(bool interrupt)
    {
        if (!Settings::gui().mHitFader)
            return;

        if (!interrupt && !mHitFader->isEmpty())
            return;

        mHitFader->clearQueue();
        const bool falloutContent = isFalloutContentLoaded();
        mHitFader->fadeTo(falloutContent ? 20 : 100, 0.0f);
        mHitFader->fadeTo(0, falloutContent ? 0.3f : 0.5f);
    }

    void WindowManager::setWerewolfOverlay(bool set)
    {
        if (!Settings::gui().mWerewolfOverlay)
            return;

        if (mWerewolfFader)
            mWerewolfFader->notifyAlphaChanged(set ? 1.0f : 0.0f);
    }

    void WindowManager::onClipboardChanged(std::string_view type, std::string_view data)
    {
        if (type == "Text")
            SDL_SetClipboardText(MyGUI::TextIterator::getOnlyText(MyGUI::UString(data)).asUTF8().c_str());
    }

    void WindowManager::onClipboardRequested(std::string_view type, std::string& data)
    {
        if (type != "Text")
            return;
        char* text = nullptr;
        text = SDL_GetClipboardText();
        if (text)
            data = MyGUI::TextIterator::toTagsString(text);

        SDL_free(text);
    }

    void WindowManager::toggleConsole()
    {
        bool visible = mConsole->isVisible();

        if (!visible && !mGuiModes.empty())
            mKeyboardNavigation->saveFocus(mGuiModes.back());

        mConsole->setVisible(!visible);

        if (visible && !mGuiModes.empty())
            mKeyboardNavigation->restoreFocus(mGuiModes.back());

        updateVisible();
    }

    void WindowManager::toggleDebugWindow()
    {
        mDebugWindow->setVisible(!mDebugWindow->isVisible());
    }

    void WindowManager::togglePostProcessorHud()
    {
        if (!MWBase::Environment::get().getWorld()->getPostProcessor()->isEnabled())
        {
            messageBox("#{OMWEngine:PostProcessingIsNotEnabled}");
            return;
        }

        bool visible = mPostProcessorHud->isVisible();

        if (!visible && !mGuiModes.empty())
            mKeyboardNavigation->saveFocus(mGuiModes.back());

        mPostProcessorHud->setVisible(!visible);

        if (visible && !mGuiModes.empty())
            mKeyboardNavigation->restoreFocus(mGuiModes.back());

        updateVisible();
    }

    void WindowManager::toggleSettingsWindow()
    {
        bool visible = mSettingsWindow->isVisible();

        if (!visible && !mGuiModes.empty())
            mKeyboardNavigation->saveFocus(mGuiModes.back());

        mSettingsWindow->setVisible(!visible);

        if (visible && !mGuiModes.empty())
            mKeyboardNavigation->restoreFocus(mGuiModes.back());

        updateVisible();
    }

    void WindowManager::cycleSpell(bool next)
    {
        if (!isGuiMode())
            mSpellWindow->cycle(next);
    }

    void WindowManager::cycleWeapon(bool next)
    {
        if (!isGuiMode())
            mInventoryWindow->cycle(next);
    }

    void WindowManager::playSound(const ESM::RefId& soundId, float volume, float pitch)
    {
        if (soundId.empty())
            return;

        MWBase::Environment::get().getSoundManager()->playSound(
            soundId, volume, pitch, MWSound::Type::Sfx, MWSound::PlayMode::NoEnvNoScaling);
    }

    void WindowManager::updateSpellWindow()
    {
        if (mSpellWindow)
            mSpellWindow->updateSpells();
    }

    void WindowManager::setConsoleSelectedObject(const MWWorld::Ptr& object)
    {
        mConsole->setSelectedObject(object);
    }

    MWWorld::Ptr WindowManager::getConsoleSelectedObject() const
    {
        return mConsole->getSelectedObject();
    }

    void WindowManager::printToConsole(const std::string& msg, std::string_view color)
    {
        mConsole->print(msg, color);
    }

    void WindowManager::setConsoleMode(std::string_view mode)
    {
        mConsole->setConsoleMode(mode);
    }

    const std::string& WindowManager::getConsoleMode()
    {
        return mConsole->getConsoleMode();
    }

    void WindowManager::createCursors()
    {
        MyGUI::ResourceManager::EnumeratorPtr enumerator = MyGUI::ResourceManager::getInstance().getEnumerator();
        while (enumerator.next())
        {
            MyGUI::IResource* resource = enumerator.current().second;
            ResourceImageSetPointerFix* imgSetPointer = resource->castType<ResourceImageSetPointerFix>(false);
            if (!imgSetPointer)
                continue;

            const VFS::Path::Normalized path(imgSetPointer->getImageSet()->getIndexInfo(0, 0).texture);

            osg::ref_ptr<osg::Image> image;
            if (mGuiPlatform->getRenderManagerPtr()->useMissingTextureFallback()
                && !mResourceSystem->getVFS()->exists(path))
                image = MyGUIPlatform::createMissingTextureFallback(path.value());
            else
                image = mResourceSystem->getImageManager()->getImage(path);

            if (image.valid())
            {
                // everything looks good, send it to the cursor manager
                const Uint8 hotspotX = imgSetPointer->getHotSpot().left;
                const Uint8 hotspotY = imgSetPointer->getHotSpot().top;
                int rotation = imgSetPointer->getRotation();
                MyGUI::IntSize pointerSize = imgSetPointer->getSize();

                mCursorManager->createCursor(imgSetPointer->getResourceName(), rotation, image, hotspotX, hotspotY,
                    pointerSize.width, pointerSize.height);
            }
        }
    }

    void WindowManager::createTextures()
    {
        {
            MyGUI::ITexture* tex = MyGUI::RenderManager::getInstance().createTexture("white");
            tex->createManual(8, 8, MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8);
            unsigned char* data = reinterpret_cast<unsigned char*>(tex->lock(MyGUI::TextureUsage::Write));
            for (int x = 0; x < 8; ++x)
                for (int y = 0; y < 8; ++y)
                {
                    *(data++) = 255;
                    *(data++) = 255;
                    *(data++) = 255;
                }
            tex->unlock();
        }

        {
            MyGUI::ITexture* tex = MyGUI::RenderManager::getInstance().createTexture("black");
            tex->createManual(8, 8, MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8);
            unsigned char* data = reinterpret_cast<unsigned char*>(tex->lock(MyGUI::TextureUsage::Write));
            for (int x = 0; x < 8; ++x)
                for (int y = 0; y < 8; ++y)
                {
                    *(data++) = 0;
                    *(data++) = 0;
                    *(data++) = 0;
                }
            tex->unlock();
        }

        {
            MyGUI::ITexture* tex = MyGUI::RenderManager::getInstance().createTexture("transparent");
            tex->createManual(8, 8, MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);
            setMenuTransparency(Settings::gui().mMenuTransparency);
        }
    }

    void WindowManager::setMenuTransparency(float value)
    {
        MyGUI::ITexture* tex = MyGUI::RenderManager::getInstance().getTexture("transparent");
        unsigned char* data = reinterpret_cast<unsigned char*>(tex->lock(MyGUI::TextureUsage::Write));
        for (int x = 0; x < 8; ++x)
            for (int y = 0; y < 8; ++y)
            {
                *(data++) = 255;
                *(data++) = 255;
                *(data++) = 255;
                *(data++) = static_cast<unsigned char>(value * 255);
            }
        tex->unlock();
    }

    void WindowManager::addCell(MWWorld::CellStore* cell)
    {
        mLocalMapRender->addCell(cell);
    }

    void WindowManager::removeCell(MWWorld::CellStore* cell)
    {
        mLocalMapRender->removeCell(cell);
    }

    void WindowManager::writeFog(MWWorld::CellStore* cell)
    {
        mLocalMapRender->saveFogOfWar(cell);
    }

    const MWGui::TextColours& WindowManager::getTextColours()
    {
        return mTextColours;
    }

    bool WindowManager::injectKeyPress(MyGUI::KeyCode key, unsigned int text, bool repeat)
    {
        if (!mKeyboardNavigation->injectKeyPress(key, text, repeat))
        {
            MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
            bool widgetActive = MyGUI::InputManager::getInstance().injectKeyPress(key, text);
            if (!widgetActive || !focus)
                return false;
            // FIXME: MyGUI doesn't allow widgets to state if a given key was actually used, so make a guess
            if (focus->getTypeName().find("Button") != std::string::npos)
            {
                switch (key.getValue())
                {
                    case MyGUI::KeyCode::ArrowDown:
                    case MyGUI::KeyCode::ArrowUp:
                    case MyGUI::KeyCode::ArrowLeft:
                    case MyGUI::KeyCode::ArrowRight:
                    case MyGUI::KeyCode::Return:
                    case MyGUI::KeyCode::NumpadEnter:
                    case MyGUI::KeyCode::Space:
                        return true;
                    default:
                        return false;
                }
            }
            return false;
        }
        else
            return true;
    }

    bool WindowManager::injectKeyRelease(MyGUI::KeyCode key)
    {
        return MyGUI::InputManager::getInstance().injectKeyRelease(key);
    }

//## VR_PATCH BEGIN
    void WindowManager::viewerTraversals()
    {
        mViewer->eventTraversal();
        mViewer->updateTraversal();
        if (VR::getVR())
            VR::Session::instance().updateSpaces();
        mViewer->renderingTraversals();
    }

//## VR_PATCH END
    void WindowManager::GuiModeState::update(bool visible)
    {
        for (const auto& window : mWindows)
            window->setVisible(visible);
    }

    void WindowManager::watchActor(const MWWorld::Ptr& ptr)
    {
        mStatsWatcher->watchActor(ptr);
    }

    MWWorld::Ptr WindowManager::getWatchedActor() const
    {
        return mStatsWatcher->getWatchedActor();
    }

    const std::string& WindowManager::getVersionDescription() const
    {
        return mVersionDescription;
    }

    void WindowManager::handleScheduledMessageBoxes()
    {
        const auto scheduledMessageBoxes = mScheduledMessageBoxes.lock();
        for (const ScheduledMessageBox& v : *scheduledMessageBoxes)
            messageBox(v.mMessage, v.mShowInDialogueMode);
        scheduledMessageBoxes->clear();
    }

    void WindowManager::onDeleteCustomData(const MWWorld::Ptr& ptr)
    {
        for (const auto& window : mWindows)
            window->onDeleteCustomData(ptr);
    }

    void WindowManager::asyncPrepareSaveMap()
    {
        mMap->asyncPrepareSaveMap();
    }

    void WindowManager::setDisabledByLua(std::string_view windowId, bool disabled)
    {
        mLuaIdToWindow.at(windowId)->setDisabledByLua(disabled);
        updateVisible();
    }

    bool WindowManager::isWindowVisible(std::string_view windowId) const
    {
        auto it = mLuaIdToWindow.find(windowId);
        if (it == mLuaIdToWindow.end())
            throw std::logic_error("Invalid window name: " + std::string(windowId));
        return it->second->isVisible();
    }

    std::vector<std::string_view> WindowManager::getAllWindowIds() const
    {
        std::vector<std::string_view> res;
        for (const auto& [id, _] : mLuaIdToWindow)
            res.push_back(id);
        return res;
    }

    std::vector<std::string_view> WindowManager::getAllowedWindowIds(GuiMode mode) const
    {
        std::vector<std::string_view> res;
        if (mode == GM_Inventory)
        {
            if (mAllowed & GW_Map)
                res.push_back(mMap->getWindowIdForLua());
            if (mAllowed & GW_Inventory)
                res.push_back(mInventoryWindow->getWindowIdForLua());
            if (mAllowed & GW_Magic)
                res.push_back(mSpellWindow->getWindowIdForLua());
            if (mAllowed & GW_Stats)
                res.push_back(mStatsWindow->getWindowIdForLua());
        }
        else
        {
            auto it = mGuiModeStates.find(mode);
            if (it != mGuiModeStates.end())
            {
                for (const auto* w : it->second.mWindows)
                    if (!w->getWindowIdForLua().empty())
                        res.push_back(w->getWindowIdForLua());
            }
        }
        return res;
    }

    int WindowManager::getControllerMenuHeight()
    {
        int height = MyGUI::RenderManager::getInstance().getViewSize().height;
        if (mControllerButtonsOverlay != nullptr && mControllerButtonsOverlay->isVisible())
            height -= mControllerButtonsOverlay->getHeight();
        if (mInventoryTabsOverlay != nullptr && mInventoryTabsOverlay->isVisible())
            height -= mInventoryTabsOverlay->getHeight();
        return height;
    }

    void WindowManager::setControllerTooltipVisible(bool visible)
    {
        if (!Settings::gui().mControllerMenus)
            return;

        mControllerTooltipVisible = visible;
    }

    void WindowManager::setControllerTooltipEnabled(bool enabled)
    {
        if (!Settings::gui().mControllerMenus)
            return;

        mControllerTooltipEnabled = enabled;
        // When user toggles the setting, also update visibility
        mControllerTooltipVisible = enabled;
    }

    void WindowManager::restoreControllerTooltips()
    {
        // Restore tooltip visibility if user has them enabled but they were hidden by mouse movement
        if (mControllerTooltipEnabled && !mControllerTooltipVisible)
            setControllerTooltipVisible(true);
    }

    const static std::map<MWGui::GuiMode, std::string_view> modeToName{
        { MWGui::GM_Inventory, "Interface" },
        { MWGui::GM_Container, "Container" },
        { MWGui::GM_Companion, "Companion" },
        { MWGui::GM_MainMenu, "MainMenu" },
        { MWGui::GM_Journal, "Journal" },
        { MWGui::GM_Scroll, "Scroll" },
        { MWGui::GM_Book, "Book" },
        { MWGui::GM_Alchemy, "Alchemy" },
        { MWGui::GM_Repair, "Repair" },
        { MWGui::GM_Dialogue, "Dialogue" },
        { MWGui::GM_Barter, "Barter" },
        { MWGui::GM_Rest, "Rest" },
        { MWGui::GM_SpellBuying, "SpellBuying" },
        { MWGui::GM_Travel, "Travel" },
        { MWGui::GM_SpellCreation, "SpellCreation" },
        { MWGui::GM_Enchanting, "Enchanting" },
        { MWGui::GM_Recharge, "Recharge" },
        { MWGui::GM_Training, "Training" },
        { MWGui::GM_MerchantRepair, "MerchantRepair" },
        { MWGui::GM_Levelup, "LevelUp" },
        { MWGui::GM_Name, "ChargenName" },
        { MWGui::GM_Race, "ChargenRace" },
        { MWGui::GM_Birth, "ChargenBirth" },
        { MWGui::GM_Class, "ChargenClass" },
        { MWGui::GM_ClassGenerate, "ChargenClassGenerate" },
        { MWGui::GM_ClassPick, "ChargenClassPick" },
        { MWGui::GM_ClassCreate, "ChargenClassCreate" },
        { MWGui::GM_Review, "ChargenClassReview" },
        { MWGui::GM_Loading, "Loading" },
        { MWGui::GM_LoadingWallpaper, "LoadingWallpaper" },
        { MWGui::GM_Jail, "Jail" },
        { MWGui::GM_QuickKeysMenu, "QuickKeysMenu" },
        { MWGui::GM_RadialMenu, "VrRadialMenu" },
        { MWGui::GM_VrMetaMenu, "VrMetaMenu" },
    };
    
    const std::map<MWGui::GuiMode, std::string_view>& WindowManager::guiModeToName() const
    {
        return modeToName;
    }
    
    void WindowManager::skipVideo() {
        if (mVideoEnabled)
            mVideoWidget->stop();
    }
    
    void WindowManager::updateControllerButtonsOverlay()
    {
        if (!Settings::gui().mControllerMenus || !mControllerButtonsOverlay)
            return;

        if (mWindows.empty())
            return;

        WindowBase* topWin = getActiveControllerWindow();
        if (!topWin || !topWin->isVisible())
        {
            mControllerButtonsOverlay->setVisible(false);
            mInventoryTabsOverlay->setVisible(false);
            return;
        }

        // setButtons will handle setting visibility based on if any buttons are defined.
        mControllerButtonsOverlay->setButtons(topWin->getControllerButtons());
        if (getMode() == GM_Inventory && !VR::getVR())
        {
            mInventoryTabsOverlay->setVisible(true);
            mInventoryTabsOverlay->setTab(mActiveControllerWindows[GM_Inventory]);
            if (mInventoryTabsOverlay->mMainWidget != nullptr)
                MyGUI::LayerManager::getInstance().upLayerItem(mInventoryTabsOverlay->mMainWidget);
        }
        else
            mInventoryTabsOverlay->setVisible(false);
    }
}
