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
                        MWWorld::ContainerStoreIterator weaponItem = findFalloutInventoryItem(inventory, id);
                        if (weaponItem == inventory.end())
                            return completeSelection("MISSING " + selectedName);

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
                        MWWorld::ContainerStoreIterator ammunitionItem = inventory.end();
                        ESM::RefId ammunitionId;
                        if (ownedAmmo != ammoCandidates.end())
                        {
                            ammunitionId = ESM::RefId::formIdRefId(*ownedAmmo);
                            ammunitionItem = findFalloutInventoryItem(inventory, ammunitionId);
                        }
                        const bool equipAmmunition = ammunitionItem != inventory.end();

                        // Publish exactly one complete equipment state. The previous two independent equip calls
                        // rebuilt/rebound the held weapon once for the weapon slot and again for the ammo slot.
                        inventory.equip(MWWorld::InventoryStore::Slot_CarriedRight, weaponItem, !equipAmmunition);
                        if (equipAmmunition)
                        {
                            inventory.equip(MWWorld::InventoryStore::Slot_Ammunition, ammunitionItem);
                            inventory.setFalloutAmmoSelection(id, ammunitionId);
                        }
                        else if (!ammoCandidates.empty())
                            Log(Debug::Error) << "FNV Pip-Boy weapon equip: status=fail reason=no-owned-compatible-ammo"
                                              << " weapon=" << id << " authoredCandidates="
                                              << ammoCandidates.size();
                        if (!inventory.getFalloutLoadedAmmo(id).has_value())
                            inventory.setFalloutLoadedAmmo(id, 0);
                        player.getClass().getCreatureStats(player).setDrawState(MWMechanics::DrawState::Weapon);
                        MWBase::Environment::get().getMechanicsManager()->forceStateUpdate(player);
                        Log(Debug::Info) << "FNV Pip-Boy weapon equip transaction: weapon=" << id
                                         << " ammo="
                                         << (equipAmmunition ? ammunitionId.toDebugString() : std::string("none"))
                                         << " equipmentEvents=1 forceStateUpdates=1";
                        return completeSelection("EQUIPPED " + selectedName);
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
        const bool useFnvMissingGuiFallback = vfs->exists(VFS::Path::Normalized("falloutnv.esm"))
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

        auto radialMenu = std::make_unique<MWVR::RadialMenu>(
            w, h, mQuickKeysMenu, mViewer->getSceneData()->asGroup(), mResourceSystem);
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
        mFalloutPipBoyRetailRoot->setNeedMouseFocus(false);
        MyGUI::ImageBox* background = nullptr;
        MyGUI::Widget* textContainer = nullptr;
        MyGUI::Widget* unusedMessage = nullptr;
        mFalloutPipBoyRetailLayout->getWidget(background, "background");
        mFalloutPipBoyRetailLayout->getWidget(unusedMessage, "message");
        mFalloutPipBoyRetailLayout->getWidget(textContainer, "buttons");
        unusedMessage->setVisible(false);
        background->setVisible(false);
        // The base layout's full-canvas content widget is only a parent for
        // the retail tiles.  If it accepts mouse focus it masks every live
        // inventory row beneath it from the VR ray pointer.
        textContainer->setNeedMouseFocus(false);

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
                const int tileHeight = std::max(1, scaled(trait("height", fallbackHeight)));
                const MyGUI::IntCoord coord(x, y, tileWidth, tileHeight);

                if (node.mType == "image")
                {
                    MyGUI::ImageBox* image = parent->createWidget<MyGUI::ImageBox>(
                        MyGUI::WidgetStyle::Child, "ImageBox", coord, MyGUI::Align::Default);
                    widget = image;
                    const FnvMenuXmlNode* filenameNode = node.findChild("filename");
                    if (filenameNode != nullptr && !filenameNode->mText.empty())
                    {
                        const std::string texture = normalizeFnvMenuTexturePath(filenameNode->mText);
                        if (mResourceSystem->getVFS()->exists(texture))
                            setWholeImageTexture(image, texture);
                        else if (node.findChild("texatlas") != nullptr)
                        {
                            std::string atlasName(filenameNode->mText);
                            atlasName.erase(0, atlasName.find_first_not_of(" \t\r\n"));
                            atlasName.erase(atlasName.find_last_not_of(" \t\r\n") + 1);
                            Misc::StringUtils::lowerCaseInPlace(atlasName);
                            const auto found = sharedAtlas.find(atlasName);
                            if (found != sharedAtlas.end()
                                && mResourceSystem->getVFS()->exists(found->second.mTexture))
                            {
                                image->setImageTexture(found->second.mTexture);
                                image->setImageTile(found->second.mCoord.size());
                                image->setImageCoord(found->second.mCoord);
                            }
                            else
                            {
                                ++unresolvedImages;
                                Log(Debug::Error) << "FNV Pip-Boy retail atlas: status=fail node="
                                                  << node.getAttribute("name") << " entry=" << atlasName;
                            }
                        }
                        else
                        {
                            ++unresolvedImages;
                            Log(Debug::Error) << "FNV Pip-Boy retail tile: status=fail node="
                                              << node.getAttribute("name") << " missingTexture=" << texture;
                        }
                    }
                    image->setDepth(100);
                }
                else if (node.mType == "text")
                {
                    MyGUI::TextBox* text = parent->createWidget<MyGUI::TextBox>(
                        MyGUI::WidgetStyle::Child, "NormalText", coord, MyGUI::Align::Default);
                    const int fontId = static_cast<int>(std::lround(trait("font", 2.f)));
                    // Fallout_default.ini binds IDs 2 and 4 to the archived
                    // Monofonto fonts; their binary FNT headers declare base
                    // sizes 31 and 36 respectively. Preserve those authored
                    // metrics instead of deriving a size from the numeric ID.
                    const int fontHeight = fontId == 4 ? 36 : (fontId == 2 ? 31 : 31);
                    widget = configureText(text, coord, fontHeight);
                    if (const FnvMenuXmlNode* string = node.findChild("string"); string != nullptr)
                        text->setCaption(string->mText);
                }
                else
                    widget = parent->createWidget<MyGUI::Widget>(
                        MyGUI::WidgetStyle::Child, std::string{}, coord, MyGUI::Align::Default);

                widget->setNeedMouseFocus(false);
                widget->setVisible(trait("visible", 1.f) != 0.f);
                const std::string name(node.getAttribute("name"));
                if (!name.empty())
                    authoredWidgets[name] = widget;
                ++renderedTiles;
            }

            for (const FnvMenuXmlNode& child : node.mChildren)
                self(self, child, widget);
        };
        renderTiles(renderTiles, menu->mRoot, textContainer);

        const auto requireWidget = [&](std::string_view name) -> MyGUI::Widget* {
            const auto found = authoredWidgets.find(std::string(name));
            if (found != authoredWidgets.end())
                return found->second;
            Log(Debug::Error) << "FNV Pip-Boy retail tile: status=fail missingNode=" << name;
            return nullptr;
        };
        mFalloutPipBoyRetailTitle = dynamic_cast<MyGUI::TextBox*>(requireWidget("IM_Headline_Title"));
        MyGUI::Widget* list = requireWidget("IM_InventoryList");
        mFalloutPipBoyRetailListHighlight = requireWidget("lb_highlight_box");
        MyGUI::Widget* itemInfo = requireWidget("IM_ItemInfoRect");
        mFalloutPipBoyRetailItemIcon = dynamic_cast<MyGUI::ImageBox*>(requireWidget("IM_ItemIcon"));
        MyGUI::Widget* tabline = requireWidget("IM_Tabline");
        MyGUI::Widget* buttons = requireWidget("IM_ButtonRect");
        if (mFalloutPipBoyRetailTitle == nullptr || list == nullptr || mFalloutPipBoyRetailListHighlight == nullptr
            || itemInfo == nullptr
            || mFalloutPipBoyRetailItemIcon == nullptr || tabline == nullptr || buttons == nullptr)
            return;
        mFalloutPipBoyRetailItemIcon->setDepth(0);

        mFalloutPipBoyRetailBody = list->createWidget<MyGUI::TextBox>(MyGUI::WidgetStyle::Child,
            "NormalText", MyGUI::IntCoord(scaled(32.f), 0, scaled(*listWidth - 32.f), scaled(*listHeight)),
            MyGUI::Align::Default);
        configureText(mFalloutPipBoyRetailBody, mFalloutPipBoyRetailBody->getCoord(), 31);
        mFalloutPipBoyRetailBody->setVisible(false);

        const FnvMenuXmlNode* const rowTemplate = menu->mRoot.findDescendantByName("IM_InventoryListTemplateRect");
        const FnvMenuXmlNode* const rowTextTemplate
            = rowTemplate != nullptr ? rowTemplate->findDescendantByName("ListItemText") : nullptr;
        const FnvMenuXmlNode* const rowMarkerTemplate
            = rowTemplate != nullptr ? rowTemplate->findDescendantByName("IM_Template_ItemMarker") : nullptr;
        if (rowTemplate == nullptr || rowTextTemplate == nullptr || rowMarkerTemplate == nullptr)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail list template: status=fail"
                              << " row=" << (rowTemplate != nullptr) << " text=" << (rowTextTemplate != nullptr)
                              << " marker=" << (rowMarkerTemplate != nullptr);
            return;
        }

        const int rowX = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowTemplate, "x", layoutContext).value_or(32.f));
        const int rowWidth = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowTemplate, "width", layoutContext).value_or(*listWidth - 32.f));
        const int rowHeight = scaled(56.f);
        const int rowTextX = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowTextTemplate, "x", layoutContext).value_or(40.f));
        const int rowTextY = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowTextTemplate, "y", layoutContext).value_or(20.f));
        const int markerX = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowMarkerTemplate, "x", layoutContext).value_or(15.f));
        const int markerY = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowMarkerTemplate, "y", layoutContext).value_or(18.f));
        const int markerWidth = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowMarkerTemplate, "width", layoutContext).value_or(25.f));
        const int markerHeight = scaled(evaluateFnvMenuNodeScalarTrait(
            menu->mRoot, *rowMarkerTemplate, "height", layoutContext).value_or(25.f));

        constexpr int visibleRetailRows = 7;
        mFalloutPipBoyRetailItemRows.reserve(visibleRetailRows);
        mFalloutPipBoyRetailItemRowLabels.reserve(visibleRetailRows);
        mFalloutPipBoyRetailItemRowMarkers.reserve(visibleRetailRows);
        for (int index = 0; index < visibleRetailRows; ++index)
        {
            MyGUI::Widget* row = list->createWidget<MyGUI::Widget>(MyGUI::WidgetStyle::Child, std::string{},
                MyGUI::IntCoord(rowX, index * rowHeight, rowWidth, rowHeight), MyGUI::Align::Default);
            row->setNeedMouseFocus(true);
            row->setUserData(-1);
            row->eventMouseButtonClick
                += MyGUI::newDelegate(this, &WindowManager::onFalloutPipBoyRetailItemClicked);
            MyGUI::ImageBox* marker = row->createWidget<MyGUI::ImageBox>(MyGUI::WidgetStyle::Child, "ImageBox",
                MyGUI::IntCoord(markerX, markerY, markerWidth, markerHeight), MyGUI::Align::Default);
            if (const FnvMenuXmlNode* filename = rowMarkerTemplate->findChild("filename");
                filename != nullptr && !filename->mText.empty())
            {
                std::string atlasName(filename->mText);
                atlasName.erase(0, atlasName.find_first_not_of(" \t\r\n"));
                atlasName.erase(atlasName.find_last_not_of(" \t\r\n") + 1);
                Misc::StringUtils::lowerCaseInPlace(atlasName);
                const auto found = sharedAtlas.find(atlasName);
                if (found != sharedAtlas.end())
                {
                    marker->setImageTexture(found->second.mTexture);
                    marker->setImageTile(found->second.mCoord.size());
                    marker->setImageCoord(found->second.mCoord);
                }
            }
            marker->setNeedMouseFocus(false);
            MyGUI::TextBox* label = row->createWidget<MyGUI::TextBox>(MyGUI::WidgetStyle::Child, "NormalText",
                MyGUI::IntCoord(rowTextX, rowTextY, std::max(1, rowWidth - rowTextX), rowHeight - rowTextY),
                MyGUI::Align::Default);
            configureText(label, label->getCoord(), 31);
            mFalloutPipBoyRetailItemRows.push_back(row);
            mFalloutPipBoyRetailItemRowLabels.push_back(label);
            mFalloutPipBoyRetailItemRowMarkers.push_back(marker);
        }
        mFalloutPipBoyRetailListHighlight->setNeedMouseFocus(false);
        Log(Debug::Info) << "FNV Pip-Boy retail list template: status=ready source=IM_InventoryListTemplate"
                         << " rows=" << visibleRetailRows << " row=" << rowX << "," << rowWidth << ","
                         << rowHeight;
        mFalloutPipBoyRetailItemInfo = itemInfo->createWidget<MyGUI::TextBox>(MyGUI::WidgetStyle::Child,
            "NormalText", MyGUI::IntCoord(0, 0, itemInfo->getWidth(), itemInfo->getHeight()), MyGUI::Align::Default);
        configureText(mFalloutPipBoyRetailItemInfo, mFalloutPipBoyRetailItemInfo->getCoord(), 31);
        mFalloutPipBoyRetailItemInfo->setDepth(0);
        mFalloutPipBoyRetailTabs = tabline->createWidget<MyGUI::TextBox>(MyGUI::WidgetStyle::Child,
            "NormalText", MyGUI::IntCoord(0, 0, scaled(*tabWidth), scaled(60.f)), MyGUI::Align::Default);
        configureText(mFalloutPipBoyRetailTabs, mFalloutPipBoyRetailTabs->getCoord(), 31);
        mFalloutPipBoyRetailTabs->setDepth(0);
        mFalloutPipBoyRetailTabs->setTextAlign(MyGUI::Align::HCenter | MyGUI::Align::Top);
        mFalloutPipBoyRetailTabs->setNeedMouseFocus(true);
        mFalloutPipBoyRetailTabs->eventMouseButtonClick
            += MyGUI::newDelegate(this, &WindowManager::onFalloutPipBoyRetailCategoryClicked);
        mFalloutPipBoyRetailActions = buttons->createWidget<MyGUI::TextBox>(MyGUI::WidgetStyle::Child,
            "NormalText", MyGUI::IntCoord(scaled(-430.f), scaled(350.f), scaled(430.f), scaled(160.f)),
            MyGUI::Align::Default);
        configureText(mFalloutPipBoyRetailActions, mFalloutPipBoyRetailActions->getCoord(), 31);
        mFalloutPipBoyRetailActions->setDepth(0);

        mFalloutPipBoyRetailInventoryReady = true;
        const auto absoluteCoord = [](const MyGUI::Widget* widget) {
            if (widget == nullptr)
                return std::string("missing");
            const MyGUI::IntCoord coord = widget->getAbsoluteCoord();
            return std::to_string(coord.left) + ',' + std::to_string(coord.top) + ','
                + std::to_string(coord.width) + ',' + std::to_string(coord.height);
        };
        Log(Debug::Info) << "FNV Pip-Boy retail XML: status=ready path=menus/main/inventory_menu.xml"
                         << " renderer=recursive-retail-tiles tiles=" << renderedTiles
                         << " unresolvedImages=" << unresolvedImages << " canvas=" << canvasX << ',' << canvasY << ','
                         << canvasWidth << ',' << canvasHeight << " main=" << *mainX << ',' << *mainY
                         << " list=" << *listX << ',' << *listY << ',' << *listWidth << ',' << *listHeight
                         << " tab=" << *tabX << ',' << *tabY << ',' << *tabWidth
                         << " source=retail-menu-xml";
        Log(Debug::Info) << "FNV Pip-Boy retail widget rectangles: root="
                         << absoluteCoord(mFalloutPipBoyRetailRoot)
                         << " container=" << absoluteCoord(textContainer)
                         << " main=" << absoluteCoord(requireWidget("IM_MainRect"))
                         << " title=" << absoluteCoord(mFalloutPipBoyRetailTitle)
                         << " list=" << absoluteCoord(list)
                         << " itemInfo=" << absoluteCoord(itemInfo)
                         << " tab=" << absoluteCoord(tabline);
    }

    void WindowManager::initializeFalloutPipBoyRetailStats(int width, int height)
    {
        FnvMenuXmlParseError parseError = FnvMenuXmlParseError::None;
        const std::optional<FnvMenuXmlDocument> menu
            = loadFnvMenuXml(*mResourceSystem->getVFS(), "menus/main/stats_menu.xml", &parseError, true);
        if (!menu)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail stats: status=fail path=menus/main/stats_menu.xml error="
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
        const auto scaled = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };

        const FnvMenuLayoutEvaluationContext context{
            authoredWidth,
            authoredHeight,
            [](std::string_view symbol) -> std::optional<float> {
                if (symbol == "true" || symbol == "highdef" || symbol == "scale" || symbol == "pipboy"
                    || symbol == "left")
                    return 1.f;
                if (symbol == "false" || symbol == "xbox" || symbol == "right")
                    return 0.f;
                return std::nullopt;
            },
            [](std::string_view node, std::string_view trait) -> std::optional<float> {
                if (node == "globals()" && trait == "_pipboy_width")
                    return 960.f;
                if (node == "globals()" && trait == "_pipboy_height")
                    return 720.f;
                if (node == "globals()" && trait == "_background_fill_brightness")
                    return 90.f;
                if (node == "StatsMenu" && trait == "user0")
                    return 0.f;
                return std::nullopt;
            },
        };
        const auto required = [&](std::string_view node, std::string_view trait) -> std::optional<float> {
            const std::optional<float> value
                = evaluateFnvMenuNamedScalarTrait(menu->mRoot, node, trait, context);
            if (!value)
                Log(Debug::Error) << "FNV Pip-Boy retail stats: unresolved node=" << node << " trait=" << trait;
            return value;
        };
        const std::optional<float> statusX = required("stats_status_container", "x");
        const std::optional<float> statusY = required("stats_status_container", "y");
        const std::optional<float> statusWidth = required("stats_status_container", "width");
        const std::optional<float> tailX = required("stats_tailline", "x");
        const std::optional<float> tailY = required("stats_tailline", "y");
        const std::optional<float> tailWidth = required("stats_tailline", "width");
        if (!statusX || !statusY || !statusWidth || !tailX || !tailY || !tailWidth)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail stats: status=fail reason=required-authored-geometry";
            return;
        }

        mFalloutPipBoyRetailStatsLayout = std::make_unique<Layout>("openmw_fnv_menu.layout");
        mFalloutPipBoyRetailStatsRoot = mFalloutPipBoyRetailStatsLayout->mMainWidget;
        mFalloutPipBoyRetailStatsRoot->setCoord(canvasX, canvasY, canvasWidth, canvasHeight);
        MyGUI::LayerManager::getInstance().attachToLayerNode("PipBoyScreen", mFalloutPipBoyRetailStatsRoot);
        mFalloutPipBoyRetailStatsRoot->setVisible(false);
        mFalloutPipBoyRetailStatsRoot->setNeedMouseFocus(false);
        MyGUI::ImageBox* background = nullptr;
        MyGUI::Widget* content = nullptr;
        MyGUI::Widget* unusedMessage = nullptr;
        mFalloutPipBoyRetailStatsLayout->getWidget(background, "background");
        mFalloutPipBoyRetailStatsLayout->getWidget(content, "buttons");
        mFalloutPipBoyRetailStatsLayout->getWidget(unusedMessage, "message");
        background->setVisible(false);
        unusedMessage->setVisible(false);
        content->setNeedMouseFocus(false);

        const auto addText = [&](MyGUI::Widget* parent, const MyGUI::IntCoord& coord, std::string_view caption,
                                 int font = 31, MyGUI::Align align = MyGUI::Align::Left | MyGUI::Align::Top) {
            MyGUI::TextBox* text = parent->createWidget<MyGUI::TextBox>(
                MyGUI::WidgetStyle::Child, "NormalText", coord, MyGUI::Align::Default);
            text->setFontName("MonoFont");
            text->setFontHeight(std::max(1, scaled(static_cast<float>(font))));
            text->setTextColour(MyGUI::Colour(0.18f, 1.f, 0.22f));
            text->setTextAlign(align);
            text->setNeedMouseFocus(false);
            text->setCaption(std::string(caption));
            return text;
        };
        const auto addLine = [&](MyGUI::Widget* parent, int x, int y, int lineWidth, int lineHeight = 2) {
            MyGUI::Widget* line = parent->createWidget<MyGUI::Widget>(MyGUI::WidgetStyle::Child, "WhiteBox",
                MyGUI::IntCoord(scaled(static_cast<float>(x)), scaled(static_cast<float>(y)),
                    std::max(1, scaled(static_cast<float>(lineWidth))),
                    std::max(1, scaled(static_cast<float>(lineHeight)))), MyGUI::Align::Default);
            line->setColour(MyGUI::Colour(0.18f, 1.f, 0.22f));
            line->setNeedMouseFocus(false);
        };
        const auto addImage = [&](MyGUI::Widget* parent, std::string_view nodeName, std::string_view texture,
                                  const MyGUI::IntCoord& authoredCoord) -> MyGUI::ImageBox* {
            const std::string normalized = normalizeFnvMenuTexturePath(texture);
            if (!mResourceSystem->getVFS()->exists(normalized))
            {
                Log(Debug::Error) << "FNV Pip-Boy retail stats: status=fail node=" << nodeName
                                  << " missingTexture=" << normalized;
                return nullptr;
            }
            MyGUI::ImageBox* image = parent->createWidget<MyGUI::ImageBox>(MyGUI::WidgetStyle::Child, "ImageBox",
                MyGUI::IntCoord(scaled(static_cast<float>(authoredCoord.left)),
                    scaled(static_cast<float>(authoredCoord.top)), scaled(static_cast<float>(authoredCoord.width)),
                    scaled(static_cast<float>(authoredCoord.height))), MyGUI::Align::Default);
            image->setImageTexture(normalized);
            if (MyGUI::ITexture* source = MyGUI::RenderManager::getInstance().getTexture(normalized))
            {
                const MyGUI::IntSize sourceSize(source->getWidth(), source->getHeight());
                image->setImageTile(sourceSize);
                image->setImageCoord(MyGUI::IntCoord(0, 0, sourceSize.width, sourceSize.height));
            }
            image->setNeedMouseFocus(false);
            return image;
        };

        // STATUS/CND is the first executable authored slice. Geometry and
        // resource identities below are taken directly from stats_menu.xml;
        // runtime values are supplied by OpenMW just as the retail menu class
        // fills its IO traits.
        addLine(content, 50, 50, 50);
        addLine(content, 50, 50, 2, 60);
        addText(content, MyGUI::IntCoord(scaled(120.f), scaled(50.f), scaled(120.f), scaled(52.f)), "STATS", 36);
        addLine(content, 250, 50, 60);
        const auto addCard = [&](int x, int cardWidth, int valueX, std::string_view title, MyGUI::TextBox*& value) {
            addLine(content, x, 50, cardWidth);
            addLine(content, x + cardWidth - 2, 50, 2, 60);
            addText(content, MyGUI::IntCoord(scaled(static_cast<float>(x + 5)), scaled(70.f),
                scaled(static_cast<float>(std::max(1, valueX - 8))), scaled(38.f)),
                title);
            value = addText(content, MyGUI::IntCoord(scaled(static_cast<float>(x + valueX)), scaled(70.f),
                scaled(static_cast<float>(cardWidth - valueX - 5)), scaled(38.f)), "--", 26);
        };
        addCard(270, 100, 50, "LVL", mFalloutPipBoyRetailStatsLevel);
        addCard(380, 155, 42, "HP", mFalloutPipBoyRetailStatsHealth);
        addCard(545, 135, 42, "AP", mFalloutPipBoyRetailStatsActionPoints);
        addCard(690, 205, 42, "XP", mFalloutPipBoyRetailStatsExperience);

        const int bodyOriginX = static_cast<int>(*statusX + (*statusWidth - 123.f) * 0.5f);
        const int bodyOriginY = static_cast<int>(*statusY + 40.f);
        bool resourcesReady = true;
        resourcesReady &= addImage(content, "stats_player_head", "Interface\\Stats\\head.dds",
                              MyGUI::IntCoord(bodyOriginX + 19, bodyOriginY, 123, 133)) != nullptr;
        resourcesReady &= addImage(content, "stats_player_face", "Interface\\Stats\\face_00.dds",
                              MyGUI::IntCoord(bodyOriginX + 49, bodyOriginY + 39, 70, 93)) != nullptr;
        resourcesReady &= addImage(content, "stats_player_torso", "Interface\\Stats\\torso.dds",
                              MyGUI::IntCoord(bodyOriginX, bodyOriginY + 125, 148, 186)) != nullptr;
        resourcesReady &= addImage(content, "stats_player_leftarm", "Interface\\Stats\\left_arm.dds",
                              MyGUI::IntCoord(bodyOriginX + 130, bodyOriginY + 125, 145, 75)) != nullptr;
        resourcesReady &= addImage(content, "stats_player_rightarm", "Interface\\Stats\\right_arm.dds",
                              MyGUI::IntCoord(bodyOriginX - 128, bodyOriginY + 118, 139, 78)) != nullptr;
        resourcesReady &= addImage(content, "stats_player_leftleg", "Interface\\Stats\\left_leg.dds",
                              MyGUI::IntCoord(bodyOriginX + 68, bodyOriginY + 250, 104, 162)) != nullptr;
        resourcesReady &= addImage(content, "stats_player_rightleg", "Interface\\Stats\\right_leg.dds",
                              MyGUI::IntCoord(bodyOriginX - 54, bodyOriginY + 245, 122, 162)) != nullptr;
        mFalloutPipBoyRetailStatsPlayerName = addText(content,
            MyGUI::IntCoord(scaled(static_cast<float>(bodyOriginX - 30)), scaled(565.f), scaled(220.f), scaled(44.f)),
            "PLAYER", 31, MyGUI::Align::HCenter | MyGUI::Align::Top);

        addText(content, MyGUI::IntCoord(scaled(50.f), scaled(120.f), scaled(125.f), scaled(42.f)), "[CND]");
        addText(content, MyGUI::IntCoord(scaled(50.f), scaled(165.f), scaled(125.f), scaled(42.f)), "RAD");
        addText(content, MyGUI::IntCoord(scaled(50.f), scaled(210.f), scaled(125.f), scaled(42.f)), "EFF");
        addLine(content, static_cast<int>(*tailX), static_cast<int>(*tailY), static_cast<int>(*tailWidth));
        addLine(content, static_cast<int>(*tailX), static_cast<int>(*tailY - 45.f), 2, 47);
        addLine(content, static_cast<int>(*tailX + *tailWidth - 2.f), static_cast<int>(*tailY - 45.f), 2, 47);
        addText(content, MyGUI::IntCoord(scaled(55.f), scaled(647.f), scaled(845.f), scaled(48.f)),
            "[STATUS]     SPECIAL     SKILLS     PERKS     GENERAL", 31,
            MyGUI::Align::HCenter | MyGUI::Align::Top);

        if (!resourcesReady)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail stats: status=fail reason=required-authored-resource";
            return;
        }
        mFalloutPipBoyRetailStatsReady = true;
        Log(Debug::Info) << "FNV Pip-Boy retail stats: status=ready path=menus/main/stats_menu.xml"
                         << " slice=STATUS/CND geometry=authored resources=authored dynamic=live"
                         << " canvas=" << canvasX << ',' << canvasY << ',' << canvasWidth << ',' << canvasHeight;
    }

    void WindowManager::initializeFalloutPipBoyRetailMap(int width, int height)
    {
        FnvMenuXmlParseError parseError = FnvMenuXmlParseError::None;
        const std::optional<FnvMenuXmlDocument> menu
            = loadFnvMenuXml(*mResourceSystem->getVFS(), "menus/main/map_menu.xml", &parseError, true);
        if (!menu)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail map: status=fail path=menus/main/map_menu.xml error="
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
        const auto scaled = [scale](float value) { return static_cast<int>(std::lround(value * scale)); };
        const FnvMenuLayoutEvaluationContext context{
            authoredWidth,
            authoredHeight,
            [](std::string_view symbol) -> std::optional<float> {
                if (symbol == "true" || symbol == "scale" || symbol == "pipboy" || symbol == "right")
                    return 1.f;
                if (symbol == "false" || symbol == "xbox" || symbol == "left")
                    return 0.f;
                return std::nullopt;
            },
            [](std::string_view node, std::string_view trait) -> std::optional<float> {
                if (node == "globals()" && trait == "_pipboy_width")
                    return 960.f;
                if (node == "globals()" && trait == "_pipboy_height")
                    return 720.f;
                if (node == "globals()" && trait == "_line_thickness")
                    return 2.f;
                if (node == "MM_Tabline" && trait == "_CurrentTab")
                    return 1.f;
                return std::nullopt;
            },
        };
        const auto required = [&](std::string_view node, std::string_view trait) -> std::optional<float> {
            const std::optional<float> value
                = evaluateFnvMenuNamedScalarTrait(menu->mRoot, node, trait, context);
            if (!value)
                Log(Debug::Error) << "FNV Pip-Boy retail map: unresolved node=" << node << " trait=" << trait;
            return value;
        };
        const std::optional<float> mainX = required("MM_MainRect", "x");
        const std::optional<float> mainY = required("MM_MainRect", "y");
        const std::optional<float> mainWidth = required("MM_MainRect", "width");
        const std::optional<float> mainHeight = required("MM_MainRect", "height");
        const std::optional<float> clipY = required("MM_WorldMap_ClipWindow", "y");
        const std::optional<float> clipWidth = required("MM_WorldMap_ClipWindow", "width");
        const std::optional<float> clipHeight = required("MM_WorldMap_ClipWindow", "height");
        const std::optional<float> tabX = required("MM_Tabline", "x");
        const std::optional<float> tabY = required("MM_Tabline", "y");
        const std::optional<float> tabWidth = required("MM_Tabline", "width");
        if (!mainX || !mainY || !mainWidth || !mainHeight || !clipY || !clipWidth || !clipHeight
            || !tabX || !tabY || !tabWidth)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail map: status=fail reason=required-authored-geometry";
            return;
        }

        mFalloutPipBoyRetailMapLayout = std::make_unique<Layout>("openmw_fnv_menu.layout");
        mFalloutPipBoyRetailMapRoot = mFalloutPipBoyRetailMapLayout->mMainWidget;
        mFalloutPipBoyRetailMapRoot->setCoord(canvasX, canvasY, canvasWidth, canvasHeight);
        MyGUI::LayerManager::getInstance().attachToLayerNode("PipBoyScreen", mFalloutPipBoyRetailMapRoot);
        mFalloutPipBoyRetailMapRoot->setVisible(false);
        mFalloutPipBoyRetailMapRoot->setNeedMouseFocus(false);
        MyGUI::ImageBox* background = nullptr;
        MyGUI::Widget* content = nullptr;
        MyGUI::Widget* unusedMessage = nullptr;
        mFalloutPipBoyRetailMapLayout->getWidget(background, "background");
        mFalloutPipBoyRetailMapLayout->getWidget(content, "buttons");
        mFalloutPipBoyRetailMapLayout->getWidget(unusedMessage, "message");
        background->setVisible(false);
        unusedMessage->setVisible(false);
        content->setNeedMouseFocus(false);
        const auto addLine = [&](int x, int y, int lineWidth, int lineHeight = 2) {
            MyGUI::Widget* line = content->createWidget<MyGUI::Widget>(MyGUI::WidgetStyle::Child, "WhiteBox",
                MyGUI::IntCoord(scaled(static_cast<float>(x)), scaled(static_cast<float>(y)),
                    std::max(1, scaled(static_cast<float>(lineWidth))),
                    std::max(1, scaled(static_cast<float>(lineHeight)))), MyGUI::Align::Default);
            line->setColour(MyGUI::Colour(0.18f, 1.f, 0.22f));
            line->setNeedMouseFocus(false);
        };
        const auto addText = [&](const MyGUI::IntCoord& coord, std::string_view caption,
                                 MyGUI::Align align = MyGUI::Align::Left | MyGUI::Align::Top) {
            MyGUI::TextBox* text = content->createWidget<MyGUI::TextBox>(
                MyGUI::WidgetStyle::Child, "NormalText", coord, MyGUI::Align::Default);
            text->setFontName("MonoFont");
            text->setFontHeight(std::max(1, scaled(31.f)));
            text->setTextColour(MyGUI::Colour(0.18f, 1.f, 0.22f));
            text->setTextAlign(align);
            text->setNeedMouseFocus(false);
            text->setCaption(std::string(caption));
            return text;
        };
        const int x = static_cast<int>(*mainX);
        const int y = static_cast<int>(*mainY);
        const int w = static_cast<int>(*mainWidth);
        addLine(x, y, 50);
        addLine(x, y, 2, 50);
        addText(MyGUI::IntCoord(scaled(x + 70), scaled(y), scaled(110), scaled(50)), "DATA");
        addLine(x + 180, y, w - 180);
        addLine(x + w, y, 2, 50);
        addLine(x, y + static_cast<int>(*clipY + *clipHeight), w);
        addLine(static_cast<int>(*tabX), static_cast<int>(*tabY), static_cast<int>(*tabWidth));
        mFalloutPipBoyRetailMapTabs = addText(MyGUI::IntCoord(scaled(static_cast<int>(*tabX)), scaled(static_cast<int>(*tabY)),
                    scaled(static_cast<int>(*tabWidth)), scaled(50)),
            "LOCAL MAP     [WORLD MAP]     QUESTS     MISC     RADIO", MyGUI::Align::HCenter | MyGUI::Align::Top);
        mFalloutPipBoyRetailMapTabs->setNeedMouseFocus(true);
        mFalloutPipBoyRetailMapTabs->eventMouseButtonClick
            += MyGUI::newDelegate(this, &WindowManager::onFalloutPipBoyRetailMapTabClicked);

        const std::optional<float> questsX = required("MM_QuestsList", "x");
        const std::optional<float> questsY = required("MM_QuestsList", "y");
        const std::optional<float> questsWidth = required("MM_QuestsList", "width");
        const std::optional<float> questsHeight = required("MM_QuestsList", "height");
        if (!questsX || !questsY || !questsWidth || !questsHeight)
        {
            Log(Debug::Error) << "FNV Pip-Boy retail map: status=fail reason=required-authored-data-list-geometry";
            return;
        }
        mFalloutPipBoyRetailDataPanel = content->createWidget<MyGUI::Widget>(MyGUI::WidgetStyle::Child, "Widget",
            MyGUI::IntCoord(scaled(*mainX + *questsX), scaled(*mainY + *questsY), scaled(*questsWidth),
                scaled(*questsHeight)), MyGUI::Align::Default);
        mFalloutPipBoyRetailDataPanel->setNeedMouseFocus(false);
        mFalloutPipBoyRetailDataPanel->setVisible(false);
        mFalloutPipBoyRetailDataHeading = addText(MyGUI::IntCoord(scaled(*mainX + *questsX + 15.f),
            scaled(*mainY + *questsY), scaled(*questsWidth - 30.f), scaled(55.f)), "QUESTS");
        mFalloutPipBoyRetailDataRows = addText(MyGUI::IntCoord(scaled(*mainX + *questsX + 15.f),
            scaled(*mainY + *questsY + 55.f), scaled(*questsWidth - 30.f), scaled(*questsHeight - 55.f)),
            "[ ] No active quest selected");
        mFalloutPipBoyRetailDataHeading->setVisible(false);
        mFalloutPipBoyRetailDataRows->setVisible(false);

        mFalloutPipBoyRetailMapClip = MyGUI::IntCoord(canvasX + scaled(*mainX),
            canvasY + scaled(*mainY + *clipY), scaled(*clipWidth), scaled(*clipHeight));

        mFalloutPipBoyRetailMapReady = true;
        Log(Debug::Info) << "FNV Pip-Boy retail map: status=ready path=menus/main/map_menu.xml"
                         << " slice=DATA/WORLD-MAP+QUESTS/NOTES/RADIO geometry=authored dynamic=live"
                         << " main=" << *mainX << ',' << *mainY << ',' << *mainWidth << ',' << *mainHeight
                         << " clip=" << *clipY << ',' << *clipWidth << ',' << *clipHeight
                         << " tab=" << *tabX << ',' << *tabY << ',' << *tabWidth
                         << " source=retail-menu-xml";
    }

    WindowManager::~WindowManager()
    {
        try
        {
            if (VR::getVR())
                MWVR::VRGUIManager::instance().clearLua();
            LuaUi::clearGameInterface();
            LuaUi::clearMenuInterface();

            mStatsWatcher.reset();

            MyGUI::LanguageManager::getInstance().eventRequestTag.clear();
            MyGUI::PointerManager::getInstance().eventChangeMousePointer.clear();
            MyGUI::InputManager::getInstance().eventChangeKeyFocus.clear();
            MyGUI::ClipboardManager::getInstance().eventClipboardChanged.clear();
            MyGUI::ClipboardManager::getInstance().eventClipboardRequested.clear();

            mWindows.clear();
            mMessageBoxManager.reset();
            mToolTips.reset();
            mCharGen.reset();

            mKeyboardNavigation.reset();

            mFalloutPipBoyRetailLayout.reset();
            mFalloutPipBoyRetailRoot = nullptr;
            mFalloutPipBoyRetailStatsLayout.reset();
            mFalloutPipBoyRetailStatsRoot = nullptr;
            mFalloutPipBoyRetailMapLayout.reset();
            mFalloutPipBoyRetailMapRoot = nullptr;

            cleanupGarbage();

            mFontLoader.reset();

            mGui->shutdown();

            mGuiPlatform->shutdown();
        }
        catch (const MyGUI::Exception& e)
        {
            Log(Debug::Error) << "Error in the destructor: " << e.what();
        }
    }

    void WindowManager::setStore(const MWWorld::ESMStore& store)
    {
        mStore = &store;
    }

    void WindowManager::cleanupGarbage()
    {
        // Delete any dialogs which are no longer in use
        mGarbageDialogs.clear();
    }

    void WindowManager::enableScene(bool enable)
    {
//## VR_PATCH BEGIN
// VR has a different set of masks to enable/disable.
// And needs to ensure the clear color is turned to black to create a proper void, when the scene is disabled.

        unsigned int disableCullMask = MWRender::Mask_GUI | MWRender::Mask_PreCompile;
        unsigned int disableUpdateMask = disableCullMask;
        osg::Vec4 disableClearColor = osg::Vec4(0, 0, 0, 1);

        if (VR::getVR())
        {
            disableCullMask = MWRender::Mask_Pointer | MWRender::Mask_3DGUI | MWRender::Mask_PreCompile
                | MWRender::Mask_RenderToTexture | MWRender::Mask_3DGUI_NonIntersectable;
            // GUI must still be updated.
            disableUpdateMask = disableCullMask | MWRender::Mask_GUI;
        }

        if (!enable && getCullMask() != disableCullMask)
        {
            mOldUpdateMask = mViewer->getUpdateVisitor()->getTraversalMask();
            mOldCullMask = getCullMask();
            mOldClearColor = mViewer->getCamera()->getClearColor();
            mViewer->getUpdateVisitor()->setTraversalMask(disableUpdateMask);
            mViewer->getCamera()->setClearColor(disableClearColor);
            setCullMask(disableCullMask);
        }
        else if (enable && getCullMask() == disableCullMask)
        {
            mViewer->getUpdateVisitor()->setTraversalMask(mOldUpdateMask);
            mViewer->getCamera()->setClearColor(mOldClearColor);
            setCullMask(mOldCullMask);
        }
//## VR_PATCH END
    }

    void WindowManager::updateConsoleObjectPtr(const MWWorld::Ptr& currentPtr, const MWWorld::Ptr& newPtr)
    {
        mConsole->updateSelectedObjectPtr(currentPtr, newPtr);
    }

    void WindowManager::updateVisible()
    {
        bool loading = (getMode() == GM_Loading || getMode() == GM_LoadingWallpaper);

        bool mainmenucover = containsMode(GM_MainMenu)
            && MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame;

//## VR_PATCH BEGIN
// Don't enable the scene while in the void
        enableScene(!loading && !mainmenucover && !mTheVoid);
//## VR_PATCH END

        if (!mMap)
            return; // UI not created yet

        const bool falloutContent = isFalloutContentLoaded();
        // FNV VR presents its gameplay interface on the physical Pip-Boy.  The
        // generic HUD_3D layer is otherwise attached to the same wrist and can
        // cover that model with an unrelated green/pink panel.
        const bool physicalFalloutVrInterface = falloutContent && VR::getVR();
        const bool gameplayOverlayVisible
            = mHudEnabled && !loading && !mLegacyHudSuppressed && !physicalFalloutVrInterface;
        mHud->setVisible(gameplayOverlayVisible);
        mToolTips->setVisible(mHudEnabled && !loading);

        bool gameMode = !isGuiMode();

        MWBase::Environment::get().getInputManager()->changeInputMode(!gameMode);

        mInputBlocker->setVisible(gameMode);

        if (loading)
            setCursorVisible(mMessageBoxManager && mMessageBoxManager->isInteractiveMessageBox());
        else
            setCursorVisible(!gameMode);

        if (gameMode)
            setKeyFocusWidget(nullptr);

        // Fallout's modal inventory/map live on the Pip-Boy surface.  Flat FNV
        // retains the ordinary HUD, while FNV VR uses only the physical device.
        if (falloutContent)
        {
            const bool falloutGameplayHudVisible = gameplayOverlayVisible && gameMode;
            setMinimapVisibility(falloutGameplayHudVisible);
            setWeaponVisibility(falloutGameplayHudVisible);
            setSpellVisibility(false);
            setHMSVisibility(falloutGameplayHudVisible);

            static std::optional<bool> loggedFalloutGameplayHudVisible;
            if (!loggedFalloutGameplayHudVisible.has_value()
                || *loggedFalloutGameplayHudVisible != falloutGameplayHudVisible)
            {
                Log(Debug::Info) << "FNV visual assertion: gameplay HUD visible="
                                 << falloutGameplayHudVisible << " hp=1 ap=1 compass=1 weapon=1";
                loggedFalloutGameplayHudVisible = falloutGameplayHudVisible;
            }

            if (physicalFalloutVrInterface)
            {
                static bool loggedPhysicalFalloutVrInterface = false;
                if (!loggedPhysicalFalloutVrInterface)
                {
                    Log(Debug::Info)
                        << "FNV VR: generic wrist HUD suppressed; physical Pip-Boy is the sole wrist interface";
                    loggedPhysicalFalloutVrInterface = true;
                }
            }
        }
        else
        {
            // Icons of forced hidden windows are displayed
            setMinimapVisibility((mAllowed & GW_Map) && (!mMap->pinned() || (mForceHidden & GW_Map)));
            setWeaponVisibility(
                (mAllowed & GW_Inventory) && (!mInventoryWindow->pinned() || (mForceHidden & GW_Inventory)));
            setSpellVisibility((mAllowed & GW_Magic) && (!mSpellWindow->pinned() || (mForceHidden & GW_Magic)));
            setHMSVisibility((mAllowed & GW_Stats) && (!mStatsWindow->pinned() || (mForceHidden & GW_Stats)));
        }

        mInventoryWindow->setGuiMode(getMode());

        // If in game mode (or interactive messagebox), show the pinned windows
        if (mGuiModes.empty())
        {
            mMap->setVisible(mMap->pinned() && !isConsoleMode() && !(mForceHidden & GW_Map) && (mAllowed & GW_Map));
            mStatsWindow->setVisible(
                mStatsWindow->pinned() && !isConsoleMode() && !(mForceHidden & GW_Stats) && (mAllowed & GW_Stats));
            mInventoryWindow->setVisible(mInventoryWindow->pinned() && !isConsoleMode()
                && !(mForceHidden & GW_Inventory) && (mAllowed & GW_Inventory));
            mSpellWindow->setVisible(
                mSpellWindow->pinned() && !isConsoleMode() && !(mForceHidden & GW_Magic) && (mAllowed & GW_Magic));

            if (mControllerButtonsOverlay)
                mControllerButtonsOverlay->setVisible(false);
            if (mInventoryTabsOverlay)
                mInventoryTabsOverlay->setVisible(false);
            return;
        }
        else if (getMode() != GM_Inventory)
        {
            mMap->setVisible(false);
            mStatsWindow->setVisible(false);
            mSpellWindow->setVisible(false);
            mHud->setDrowningBarVisible(false);
            mInventoryWindow->setVisible(
                getMode() == GM_Container || getMode() == GM_Barter || getMode() == GM_Companion);
        }

        GuiMode mode = mGuiModes.back();

        mInventoryWindow->setTrading(mode == GM_Barter);

        if (getMode() == GM_Inventory)
        {
            // For the inventory mode, compute the effective set of windows to show.
            // This is controlled both by what windows the
            // user has opened/closed (the 'shown' variable) and by what
            // windows we are allowed to show (the 'allowed' var.)
            int eff = mShown & mAllowed & ~mForceHidden;
            const bool flatPaperDollProfiler = falloutContent && !VR::getVR()
                && std::getenv("OPENMW_FNV_PAPER_DOLL_PROFILER") != nullptr;
            if (flatPaperDollProfiler)
            {
                eff = GW_Inventory;
                static bool loggedPaperDollProfiler = false;
                if (!loggedPaperDollProfiler)
                {
                    Log(Debug::Info)
                        << "FNV/ESM4 proof: flat paper doll profiler owns inventory mode full-screen";
                    loggedPaperDollProfiler = true;
                }
            }
            else if (falloutContent && mFalloutPipBoyPhysical && !VR::getVR())
            {
                const int activeIndex = std::clamp(mActiveControllerWindows[GM_Inventory], 0, 3);
                constexpr int falloutPaneMasks[4] = { GW_Map, GW_Inventory, GW_Magic, GW_Stats };
                eff = falloutPaneMasks[activeIndex];
                Log(Debug::Verbose) << "FNV Pip-Boy physical: activePane=" << activeIndex
                                    << " visibleMask=0x" << std::hex << eff << std::dec;
            }
            auto setWindowVisibleIfChanged = [](WindowBase* window, bool visible) {
                if (window != nullptr && window->isVisible() != visible)
                    window->setVisible(visible);
            };
            setWindowVisibleIfChanged(mMap, eff & GW_Map);
            setWindowVisibleIfChanged(mInventoryWindow, eff & GW_Inventory);
            setWindowVisibleIfChanged(mSpellWindow, eff & GW_Magic);
            setWindowVisibleIfChanged(mStatsWindow, eff & GW_Stats);

            if (flatPaperDollProfiler)
            {
                const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
                setWindowCoord(mInventoryWindow, MyGUI::IntCoord(0, 0, viewSize.width, viewSize.height));
                if (mInventoryWindow->mMainWidget != nullptr)
                    MyGUI::LayerManager::getInstance().upLayerItem(mInventoryWindow->mMainWidget);
                Log(Debug::Info) << "FNV/ESM4 proof: flat paper doll profiler fullscreen rect=0,0,"
                                 << viewSize.width << "," << viewSize.height;
            }
            else if (falloutContent || std::getenv("OPENMW_FNV_PROOF_PIPBOY_SURFACE") != nullptr)
            {
                const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
                const int margin = 24;
                const bool isVr = VR::getVR();
                const int top = isVr ? std::min(std::max(88, viewSize.height / 8), 128) : 52;
                const int bottom = isVr ? 36 : 16;
                const int gap = 8;
                const int activeIndex = std::clamp(static_cast<int>(mActiveControllerWindows[GM_Inventory]), 0, 3);
                const int shelfWidth = std::min(std::max(viewSize.width / 6, 180), 260);
                const int availableWidth = viewSize.width - margin * 2;
                const int availableHeight = viewSize.height - top - bottom;
                const int activeWidth = std::max(640, availableWidth);
                const int activeHeight = std::max(360, availableHeight);
                int loggedActiveWidth = activeWidth;
                int loggedActiveHeight = activeHeight;
                int loggedActiveLeft = margin;
                int loggedActiveTop = top;
                const int shelfLeft = viewSize.width - margin - shelfWidth;
                const int shelfHeight = std::max(110, (activeHeight - gap * 2) / 3);

                WindowBase* windows[4] = { mMap, mInventoryWindow, mSpellWindow, mStatsWindow };
                if (isVr)
                {
                    const int mapActiveWidth = std::clamp(availableWidth, 860, 1080);
                    const int mapActiveHeight = std::clamp(availableHeight, 520, 680);
                    const int inactiveWidth = std::clamp(activeWidth / 2, 320, 440);
                    const int inactiveHeight = std::clamp(activeHeight / 2, 240, 340);
                    const int inactiveLeft = margin + std::max(0, (std::max(activeWidth, mapActiveWidth) - inactiveWidth) / 2);
                    const int inactiveTop = top + std::max(0, (std::max(activeHeight, mapActiveHeight) - inactiveHeight) / 2);

                    for (int i = 0; i < 4; ++i)
                    {
                        if (i == activeIndex)
                        {
                            const int paneWidth = i == 0 ? mapActiveWidth : activeWidth;
                            const int paneHeight = i == 0 ? mapActiveHeight : activeHeight;
                            const int paneLeft = std::max(margin, (viewSize.width - paneWidth) / 2);
                            const int paneTop = std::max(top, (viewSize.height - paneHeight) / 2);
                            loggedActiveLeft = paneLeft;
                            loggedActiveTop = paneTop;
                            loggedActiveWidth = paneWidth;
                            loggedActiveHeight = paneHeight;
                            setWindowCoord(windows[i], MyGUI::IntCoord(paneLeft, paneTop, paneWidth, paneHeight));
                        }
                        else
                            setWindowCoord(
                                windows[i], MyGUI::IntCoord(inactiveLeft, inactiveTop, inactiveWidth, inactiveHeight));
                    }
                }
                else
                {
                    int shelfSlot = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        if (i == activeIndex)
                        {
                            const int paneWidth = i == 0 ? std::max(activeWidth, 920) : activeWidth;
                            const int paneHeight = i == 0 ? std::max(activeHeight, 560) : activeHeight;
                            loggedActiveWidth = paneWidth;
                            loggedActiveHeight = paneHeight;
                            setWindowCoord(windows[i], MyGUI::IntCoord(margin, top, paneWidth, paneHeight));
                            if (i == 1 && mInventoryWindow != nullptr
                                && !mInventoryWindow->refreshFalloutPaneLayout())
                                Log(Debug::Error) << "FNV/ESM4 proof: active inventory item pane failed layout validation";
                            continue;
                        }

                        const int shelfTop = top + shelfSlot * (shelfHeight + gap);
                        setWindowCoord(windows[i], MyGUI::IntCoord(shelfLeft, shelfTop, shelfWidth, shelfHeight));
                        ++shelfSlot;
                    }
                }

                if (activeIndex == 0 && mMap != nullptr)
                    mMap->fitFalloutWorldMapOnce();

                if (WindowBase* activeWindow = windows[activeIndex])
                {
                    if (activeWindow->mMainWidget != nullptr)
                        MyGUI::LayerManager::getInstance().upLayerItem(activeWindow->mMainWidget);
                }
                Log(Debug::Info) << "FNV/ESM4 proof: Fallout pause panes laid out active="
                                 << activeIndex << " activeRect=" << loggedActiveLeft << "," << loggedActiveTop
                                 << "," << loggedActiveWidth << "," << loggedActiveHeight << " vrFullPanels="
                                 << isVr
                                 << " shelfWidth=" << shelfWidth;
            }
        }

        updateControllerButtonsOverlay();
        if (falloutContent && getMode() == GM_Inventory && mInventoryTabsOverlay != nullptr)
            mInventoryTabsOverlay->setVisible(false);

        switch (mode)
        {
            // FIXME: refactor chargen windows to use modes properly (or not use them at all)
            case GM_Name:
            case GM_Race:
            case GM_Class:
            case GM_ClassPick:
            case GM_ClassCreate:
            case GM_Birth:
            case GM_ClassGenerate:
            case GM_Review:
                mCharGen->spawnDialog(mode);
                break;
            default:
                break;
        }
    }

    void WindowManager::setDrowningTimeLeft(float time, float maxTime)
    {
        mHud->setDrowningTimeLeft(time, maxTime);
    }

    void WindowManager::removeDialog(std::unique_ptr<Layout>&& dialog)
    {
        if (!dialog)
            return;
        dialog->setVisible(false);
        mGarbageDialogs.push_back(std::move(dialog));
        updateControllerButtonsOverlay();
    }

    void WindowManager::exitCurrentGuiMode()
    {
        if (mDragAndDrop && mDragAndDrop->mIsOnDragAndDrop)
        {
            mDragAndDrop->finish();
            return;
        }

        if (mGuiModes.empty())
            return;

        GuiModeState& state = mGuiModeStates[mGuiModes.back()];
        for (const auto& window : state.mWindows)
        {
            if (!window->exit())
            {
                // unable to exit window, but give access to main menu
                if (!MyGUI::InputManager::getInstance().isModalAny() && getMode() != GM_MainMenu)
                    pushGuiMode(GM_MainMenu);
                return;
            }
        }

//## VR_PATCH BEGIN
        mVideoEnabled = false;

//## VR_PATCH END
        popGuiMode();
    }

    void WindowManager::interactiveMessageBox(
        std::string_view message, const std::vector<std::string>& buttons, bool block, int defaultFocus)
    {
        mMessageBoxManager->createInteractiveMessageBox(message, buttons, block, defaultFocus);
        updateVisible();

        if (block)
        {
            Misc::FrameRateLimiter frameRateLimiter
                = Misc::makeFrameRateLimiter(MWBase::Environment::get().getFrameRateLimit());
            while (mMessageBoxManager->readPressedButton(false) == -1
                && !MWBase::Environment::get().getStateManager()->hasQuitRequest())
            {
                const double dt
                    = std::chrono::duration_cast<std::chrono::duration<double>>(frameRateLimiter.getLastFrameDuration())
                          .count();

                mKeyboardNavigation->onFrame();
                mMessageBoxManager->onFrame(dt);
                MWBase::Environment::get().getInputManager()->update(dt, true, false);

                if (!mWindowVisible)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                else
//## VR_PATCH BEGIN
                    viewerTraversals();
//## VR_PATCH END
                // at the time this function is called we are in the middle of a frame,
                // so out of order calls are necessary to get a correct frameNumber for the next frame.
                // refer to the advance() and frame() order in Engine::go()
                mViewer->advance(mViewer->getFrameStamp()->getSimulationTime());

                frameRateLimiter.limit();
            }

            mMessageBoxManager->resetInteractiveMessageBox();
        }
    }

    void WindowManager::messageBox(std::string_view message, enum MWGui::ShowInDialogueMode showInDialogueMode)
    {
        if (std::getenv("OPENMW_FNV_INTERACTION_AUDIT") != nullptr)
            Log(Debug::Info) << "FNV interaction audit: rendered notification text=\"" << message << "\"";
        if (getMode() == GM_Dialogue && showInDialogueMode != MWGui::ShowInDialogueMode_Never)
        {
            MyGUI::UString text = MyGUI::LanguageManager::getInstance().replaceTags(MyGUI::UString(message));
            mDialogueWindow->addMessageBox(text);
        }
        else if (showInDialogueMode != MWGui::ShowInDialogueMode_Only)
        {
            mMessageBoxManager->createMessageBox(message);
        }
    }

    void WindowManager::scheduleMessageBox(std::string message, enum MWGui::ShowInDialogueMode showInDialogueMode)
    {
        mScheduledMessageBoxes.lock()->emplace_back(std::move(message), showInDialogueMode);
    }

    void WindowManager::staticMessageBox(std::string_view message)
    {
        mMessageBoxManager->createMessageBox(message, true);
    }

    void WindowManager::removeStaticMessageBox()
    {
        mMessageBoxManager->removeStaticMessageBox();
    }

    int WindowManager::readPressedButton()
    {
        return mMessageBoxManager->readPressedButton();
    }

    std::string_view WindowManager::getGameSettingString(std::string_view id, std::string_view defaultValue)
    {
        const ESM::GameSetting* setting = mStore->get<ESM::GameSetting>().search(id);

        if (setting && setting->mValue.getType() == ESM::VT_String)
            return setting->mValue.getString();

        return defaultValue;
    }

    void WindowManager::updateMap()
    {
        if (!mLocalMapRender)
            return;

        MWWorld::ConstPtr player = MWMechanics::getPlayer();

        osg::Vec3f playerPosition = player.getRefData().getPosition().asVec3();
        osg::Quat playerOrientation(-player.getRefData().getPosition().rot[2], osg::Vec3(0, 0, 1));

        osg::Vec3f playerdirection;
        int x, y;
        float u, v;
        mLocalMapRender->updatePlayer(playerPosition, playerOrientation, u, v, x, y, playerdirection);
        mFalloutPipBoyLocalMapX = x;
        mFalloutPipBoyLocalMapY = y;

        if (!player.getCell()->isExterior())
        {
            setActiveMap(*player.getCell()->getCell());
        }
        // else: need to know the current grid center, call setActiveMap from changeCell

        mMap->setPlayerDir(playerdirection.x(), playerdirection.y());
        mMap->setPlayerPos(x, y, u, v);
        mHud->setPlayerDir(playerdirection.x(), playerdirection.y());
        mHud->setPlayerPos(x, y, u, v);
    }

    WindowBase* WindowManager::getActiveControllerWindow()
    {
        if (!mCurrentModals.empty())
            return mCurrentModals.back();

        if (mWindows.empty())
            return nullptr;

        if (isSettingsWindowVisible())
            return mSettingsWindow;

        if (!mGuiModes.empty())
        {
            GuiMode mode = mGuiModes.back();
            GuiModeState& state = mGuiModeStates[mode];
            if (state.mWindows.size() == 0)
                return nullptr;

            int activeIndex
                = std::clamp(mActiveControllerWindows[mode], 0, static_cast<int>(state.mWindows.size()) - 1);

            // If the active window is no longer visible, find the next visible window.
            if (!state.mWindows[activeIndex]->isVisible())
                cycleActiveControllerWindow(true);

            return state.mWindows[activeIndex];
        }

        return nullptr;
    }

    void WindowManager::cycleActiveControllerWindow(bool next)
    {
        if (mGuiModes.empty())
            return;

        GuiMode mode = mGuiModes.back();
        const bool falloutPipBoyPhysical = isFalloutContentLoaded() && mode == GM_Inventory
            && (mFalloutPipBoyPhysical || VR::getVR());
        if (!Settings::gui().mControllerMenus && !falloutPipBoyPhysical)
            return;

        int winCount = mGuiModeStates[mode].mWindows.size();

        int activeIndex = 0;
        if (winCount > 1)
        {
            // Find next/previous visible window
            activeIndex = mActiveControllerWindows[mode];
            int delta = next ? 1 : -1;

            for (int i = 0; i < winCount; i++)
            {
                activeIndex = wrap(activeIndex + delta, winCount);
                if (mGuiModeStates[mode].mWindows[activeIndex]->isVisible())
                    break;
            }
        }

        if (mActiveControllerWindows[mode] != activeIndex)
            setActiveControllerWindow(mode, activeIndex);
    }

    void WindowManager::reapplyActiveControllerWindow()
    {
        if (!Settings::gui().mControllerMenus || mGuiModes.empty())
            return;

        const GuiMode mode = mGuiModes.back();
        int winCount = mGuiModeStates[mode].mWindows.size();

        for (int i = 0; i < winCount; i++)
        {
            // Set active window last so inactive windows don't stomp on changes it makes, e.g. to tooltips.
            if (i != mActiveControllerWindows[mode])
                mGuiModeStates[mode].mWindows[i]->setActiveControllerWindow(false);
        }
        if (winCount > 0)
            mGuiModeStates[mode].mWindows[mActiveControllerWindows[mode]]->setActiveControllerWindow(true);
    }

    void WindowManager::setActiveControllerWindow(GuiMode mode, int activeIndex)
    {
        const bool falloutPipBoyPhysical = isFalloutContentLoaded() && mode == GM_Inventory
            && (mFalloutPipBoyPhysical || VR::getVR());
        if (!Settings::gui().mControllerMenus && !falloutPipBoyPhysical)
            return;

        int winCount = mGuiModeStates[mode].mWindows.size();
        if (winCount == 0)
            return;

        activeIndex = std::clamp(activeIndex, 0, winCount - 1);
        mActiveControllerWindows[mode] = activeIndex;

        if (falloutPipBoyPhysical && !Settings::gui().mControllerMenus)
        {
            updateVisible();
            for (int i = 0; i < winCount; ++i)
            {
                if (i != activeIndex)
                    mGuiModeStates[mode].mWindows[i]->setActiveControllerWindow(false);
            }
            if (WindowBase* activeWindow = mGuiModeStates[mode].mWindows[activeIndex])
            {
                activeWindow->setActiveControllerWindow(true);
                if (activeWindow->mMainWidget != nullptr)
                    MyGUI::LayerManager::getInstance().upLayerItem(activeWindow->mMainWidget);
                Log(Debug::Verbose) << "FNV Pip-Boy physical: raisedPane=" << activeIndex;
            }
            MWBase::Environment::get().getInputManager()->setGamepadGuiCursorEnabled(
                mGuiModeStates[mode].mWindows[activeIndex]->isGamepadCursorAllowed());
            updateControllerButtonsOverlay();
            setCursorActive(false);
            if (mInventoryTabsOverlay != nullptr)
                mInventoryTabsOverlay->setVisible(false);

            if (winCount > 1)
                playSound(ESM::RefId::stringRefId("Menu Size"));

            return;
        }

        reapplyActiveControllerWindow();

        MWBase::Environment::get().getInputManager()->setGamepadGuiCursorEnabled(
            mGuiModeStates[mode].mWindows[activeIndex]->isGamepadCursorAllowed());

        updateControllerButtonsOverlay();
        setCursorActive(false);

        if (winCount > 1)
            playSound(ESM::RefId::stringRefId("Menu Size"));
    }

    void WindowManager::setFalloutPipBoyPresentation(bool physical)
    {
        if (!isFalloutContentLoaded() || mFalloutPipBoyPhysical == physical)
            return;

        mFalloutPipBoyPhysical = physical;
        Log(Debug::Info) << "FNV Pip-Boy presentation: mode=" << (physical ? "physical" : "analog")
                         << " activePane=" << getFalloutPipBoyActivePane();
        updateVisible();
        updateFalloutPipBoyTerminalSurface();
    }

    int WindowManager::getFalloutPipBoyActivePane() const
    {
        const auto found = mActiveControllerWindows.find(GM_Inventory);
        return found == mActiveControllerWindows.end() ? 3 : std::clamp(found->second, 0, 3);
    }

    bool WindowManager::handleFalloutPipBoyPointerClick(int x, int y)
    {
        if (!isFalloutContentLoaded() || !VR::getVR())
            return false;

        const MyGUI::IntPoint point(x, y);
        const auto hit = [&point](const MyGUI::Widget* widget) {
            return widget != nullptr && widget->getVisible() && widget->getInheritedVisible()
                && widget->getAbsoluteCoord().inside(point);
        };

        const int pane = getFalloutPipBoyActivePane();
        if (pane == 1)
        {
            for (std::size_t index = 0; index < mFalloutPipBoyRetailItemRows.size(); ++index)
            {
                MyGUI::Widget* const row = mFalloutPipBoyRetailItemRows[index];
                if (!hit(row))
                    continue;
                Log(Debug::Info) << "FNV Pip-Boy pointer: control=inventory-row visibleRow=" << index
                                 << " cursor=(" << x << ',' << y << ')';
                row->_riseMouseButtonClick();
                return true;
            }
            if (hit(mFalloutPipBoyRetailTabs))
            {
                mFalloutPipBoyRetailTabs->_riseMouseButtonClick();
                return true;
            }
        }
        else if ((pane == 0 || pane == 2) && hit(mFalloutPipBoyRetailMapTabs))
        {
            mFalloutPipBoyRetailMapTabs->_riseMouseButtonClick();
            return true;
        }

        // A click on inert Pip-Boy glass is consumed here so it cannot fall
        // through to the currently selected controller-menu action.
        return false;
    }

    bool WindowManager::handleFalloutPipBoyPhysicalControl(std::string_view control)
    {
        if (!isFalloutContentLoaded() || !VR::getVR())
            return false;

        int action = -1;
        if (Misc::StringUtils::ciEqual(control, "PipBoyButton01"))
            action = MWInput::A_QuickKey1; // STATUS
        else if (Misc::StringUtils::ciEqual(control, "PipBoyButton02"))
            action = MWInput::A_QuickKey2; // ITEMS
        else if (Misc::StringUtils::ciEqual(control, "PipBoyButton03"))
            action = MWInput::A_QuickKey3; // DATA
        else if (Misc::StringUtils::ciEqual(control, "TabKnob"))
            action = MWInput::A_QuickKey4; // MAP
        else if (Misc::StringUtils::ciEqual(control, "ScrollKnob"))
            action = MWInput::A_MoveBackward; // next visible row / map pan step
        else
            return false;

        const bool handled = handleFalloutPipBoyAction(action);
        Log(handled ? Debug::Info : Debug::Error)
            << "FNV Pip-Boy physical ray click: control=" << control << " action=" << action
            << " handled=" << handled << " pane=" << getFalloutPipBoyActivePane()
            << " submenu=" << mFalloutPipBoySubmenu << " listOffset=" << mFalloutPipBoyListOffset;
        return handled;
    }

    void WindowManager::onFalloutPipBoyRetailItemClicked(MyGUI::Widget* sender)
    {
        if (sender == nullptr || (!VR::getVR() && (!mFalloutPipBoyPhysical || !containsMode(GM_Inventory)))
            || getFalloutPipBoyActivePane() != 1)
            return;
        const int* const inventoryIndex = sender->getUserData<int>();
        if (inventoryIndex == nullptr || *inventoryIndex < 0)
            return;

        mFalloutPipBoyListOffset = *inventoryIndex;
        // A ray click on a visible row is the device's EQUIP/USE action, not
        // a synthetic desktop inventory click.
        handleFalloutPipBoyAction(MWInput::A_Activate);
    }

    void WindowManager::onFalloutPipBoyRetailCategoryClicked(MyGUI::Widget* sender)
    {
        if (sender == nullptr || (!VR::getVR() && (!mFalloutPipBoyPhysical || !containsMode(GM_Inventory)))
            || getFalloutPipBoyActivePane() != 1)
            return;
        const MyGUI::IntCoord rect = sender->getAbsoluteCoord();
        const MyGUI::IntPoint mouse = MyGUI::InputManager::getInstance().getMousePosition();
        constexpr int categoryCount = 5;
        const int category = std::clamp(
            (mouse.left - rect.left) * categoryCount / std::max(1, rect.width), 0, categoryCount - 1);
        mFalloutPipBoySubmenu = category;
        mFalloutPipBoyListOffset = 0;
        if (mInventoryWindow != nullptr)
            mInventoryWindow->setFalloutPipBoyCategory(category);
        mFalloutPipBoyInteractionPulse = 1.f;
        updateFalloutPipBoyTerminalSurface();
        Log(Debug::Info) << "FNV Pip-Boy pointer: control=inventory-category value=" << category;
    }

    void WindowManager::onFalloutPipBoyRetailMapTabClicked(MyGUI::Widget* sender)
    {
        if (sender == nullptr || (!VR::getVR() && (!mFalloutPipBoyPhysical || !containsMode(GM_Inventory))))
            return;
        const MyGUI::IntCoord rect = sender->getAbsoluteCoord();
        const MyGUI::IntPoint mouse = MyGUI::InputManager::getInstance().getMousePosition();
        constexpr int tabCount = 5;
        const int tab
            = std::clamp((mouse.left - rect.left) * tabCount / std::max(1, rect.width), 0, tabCount - 1);
        if (tab <= 1)
        {
            setActiveControllerWindow(GM_Inventory, 0);
            mFalloutPipBoyWorldMap = tab == 1;
            mFalloutPipBoySubmenu = mFalloutPipBoyWorldMap ? 1 : 0;
            mFalloutPipBoyMapZoom = 1.f;
            mFalloutPipBoyMapPanX = 0.f;
            mFalloutPipBoyMapPanY = 0.f;
        }
        else
        {
            setActiveControllerWindow(GM_Inventory, 2);
            mFalloutPipBoySubmenu = tab - 2;
            mFalloutPipBoyListOffset = 0;
        }
        mFalloutPipBoyInteractionPulse = 1.f;
        updateFalloutPipBoyTerminalSurface();
        Log(Debug::Info) << "FNV Pip-Boy pointer: control=map-data-tab value=" << tab
                         << " pane=" << getFalloutPipBoyActivePane();
    }

    bool WindowManager::handleFalloutPipBoyAction(int action)
    {
        if (!isFalloutContentLoaded()
            || (!VR::getVR() && (!mFalloutPipBoyPhysical || !containsMode(GM_Inventory))))
            return false;

        int pane = getFalloutPipBoyActivePane();
        bool changed = false;
        bool handled = true;
        const auto changePane = [this, &changed](int value) {
            setActiveControllerWindow(GM_Inventory, value);
            mFalloutPipBoySubmenu = 0;
            mFalloutPipBoyListOffset = 0;
            if (value == 1 && mInventoryWindow != nullptr)
                mInventoryWindow->setFalloutPipBoyCategory(mFalloutPipBoySubmenu);
            changed = true;
        };
        const auto changeList = [this, pane, &changed](int delta) {
            static constexpr std::array<int, 3> dataRows = { 2, 2, 1 };
            const int submenuCount = getFalloutPipBoySubmenuCount(pane);
            mFalloutPipBoySubmenu = std::clamp(mFalloutPipBoySubmenu, 0, submenuCount - 1);
            int rowCount = 1;
            if (pane == 1)
            {
                if (MWBase::World* const world = MWBase::Environment::get().getWorld())
                {
                    const MWWorld::Ptr player = world->getPlayerPtr();
                    if (!player.isEmpty())
                    {
                        MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
                        rowCount = std::max(1,
                            static_cast<int>(getFalloutPipBoyInventoryRows(
                                inventory, mFalloutPipBoySubmenu).size()));
                    }
                }
            }
            else if (pane == 2)
                rowCount = dataRows[std::clamp(mFalloutPipBoySubmenu, 0, static_cast<int>(dataRows.size()) - 1)];
            mFalloutPipBoyListOffset = std::clamp(mFalloutPipBoyListOffset + delta, 0, rowCount - 1);
            changed = true;
        };

        switch (action)
        {
            case MWInput::A_QuickKey1:
                changePane(3); // STATUS
                break;
            case MWInput::A_QuickKey2:
                changePane(1); // ITEMS
                break;
            case MWInput::A_QuickKey3:
                changePane(2); // DATA
                break;
            case MWInput::A_QuickKey4:
                changePane(0); // MAP
                break;
            case MWInput::A_Map:
            case MWInput::A_Activate:
            case MWInput::A_Use:
                if (pane == 0)
                {
                    mFalloutPipBoyWorldMap = !mFalloutPipBoyWorldMap;
                    mFalloutPipBoySubmenu = mFalloutPipBoyWorldMap ? 1 : 0;
                    mFalloutPipBoyMapZoom = 1.f;
                    mFalloutPipBoyMapPanX = 0.f;
                    mFalloutPipBoyMapPanY = 0.f;
                    changed = true;
                }
                else if (action == MWInput::A_Activate || action == MWInput::A_Use)
                {
                    // This is the device-side action boundary: physical list
                    // selection changes the actual player inventory/equipment,
                    // rather than merely moving a marker on the terminal.
                    mFalloutPipBoyLastAction
                        = executeFalloutPipBoySelection(pane, mFalloutPipBoySubmenu, mFalloutPipBoyListOffset);
                    changed = true;
                }
                else
                    handled = false;
                break;
            case MWInput::A_MoveLeft:
                if (pane == 0)
                {
                    mFalloutPipBoyMapPanX = std::clamp(mFalloutPipBoyMapPanX - 0.08f, -0.45f, 0.45f);
                    changed = true;
                }
                else
                {
                    const int count = getFalloutPipBoySubmenuCount(pane);
                    mFalloutPipBoySubmenu = (mFalloutPipBoySubmenu + count - 1) % count;
                    mFalloutPipBoyListOffset = 0;
                    if (pane == 1 && mInventoryWindow != nullptr)
                        mInventoryWindow->setFalloutPipBoyCategory(mFalloutPipBoySubmenu);
                    changed = true;
                }
                break;
            case MWInput::A_MoveRight:
                if (pane == 0)
                {
                    mFalloutPipBoyMapPanX = std::clamp(mFalloutPipBoyMapPanX + 0.08f, -0.45f, 0.45f);
                    changed = true;
                }
                else
                {
                    const int count = getFalloutPipBoySubmenuCount(pane);
                    mFalloutPipBoySubmenu = (mFalloutPipBoySubmenu + 1) % count;
                    mFalloutPipBoyListOffset = 0;
                    if (pane == 1 && mInventoryWindow != nullptr)
                        mInventoryWindow->setFalloutPipBoyCategory(mFalloutPipBoySubmenu);
                    changed = true;
                }
                break;
            case MWInput::A_MoveForward:
                if (pane == 0)
                {
                    mFalloutPipBoyMapPanY = std::clamp(mFalloutPipBoyMapPanY - 0.08f, -0.45f, 0.45f);
                    changed = true;
                }
                else
                    changeList(-1);
                break;
            case MWInput::A_MoveBackward:
                if (pane == 0)
                {
                    mFalloutPipBoyMapPanY = std::clamp(mFalloutPipBoyMapPanY + 0.08f, -0.45f, 0.45f);
                    changed = true;
                }
                else
                    changeList(1);
                break;
            case MWInput::A_ZoomIn:
                if (pane == 0)
                {
                    mFalloutPipBoyMapZoom = std::clamp(mFalloutPipBoyMapZoom * 1.25f, 1.f, 3.f);
                    changed = true;
                }
                else
                    changeList(-1);
                break;
            case MWInput::A_ZoomOut:
                if (pane == 0)
                {
                    mFalloutPipBoyMapZoom = std::clamp(mFalloutPipBoyMapZoom / 1.25f, 1.f, 3.f);
                    changed = true;
                }
                else
                    changeList(1);
                break;
            default:
                handled = false;
                break;
        }

        if (!handled)
            return false;

        if (changed)
        {
            // Keep the press visible long enough for the first-person right
            // hand to reach the real Pip-Boy button and return.
            mFalloutPipBoyInteractionPulse = 1.f;
            updateFalloutPipBoyTerminalSurface();
            Log(Debug::Info) << "FNV Pip-Boy interaction: pane=" << getFalloutPipBoyActivePane()
                             << " submenu=" << mFalloutPipBoySubmenu << " listOffset=" << mFalloutPipBoyListOffset
                             << " worldMap=" << mFalloutPipBoyWorldMap << " zoom=" << mFalloutPipBoyMapZoom
                             << " pan=" << mFalloutPipBoyMapPanX << ',' << mFalloutPipBoyMapPanY;
        }
        return true;
    }

    void WindowManager::setFalloutPipBoyMapSelection(std::string_view name, ESM::FormId marker)
    {
        mFalloutPipBoyMapSelection = std::string(name) + " " + marker.toString();
        updateFalloutPipBoyTerminalSurface();
    }

    void WindowManager::setFalloutPipBoyMapConfirmation(std::string_view text)
    {
        // The physical terminal has a deliberately small retail-sized text
        // surface.  Once the production confirmation dialog opens, show its
        // exact title in the available row instead of clipping it below the
        // selected-marker annotation.
        mFalloutPipBoyMapSelection.clear();
        mFalloutPipBoyMapConfirmation = std::string(text);
        updateFalloutPipBoyTerminalSurface();
    }

    osg::Texture2D* WindowManager::getFalloutPipBoyLocalMapTexture()
    {
        if (mLocalMapRender == nullptr)
            return nullptr;
        return mLocalMapRender->getMapTexture(mFalloutPipBoyLocalMapX, mFalloutPipBoyLocalMapY).get();
    }

    std::string WindowManager::getFalloutPipBoyTerminalHeader() const
    {
        return makeFalloutPipBoyTerminalHeader(getFalloutPipBoyActivePane());
    }

    std::string WindowManager::getFalloutPipBoyTerminalBody() const
    {
        // Screen content is supplied by the live native panes and the parsed
        // retail Tile XML renderer. There is no generated terminal-text path.
        return {};
    }

    void WindowManager::updateFalloutPipBoyTerminalSurface()
    {
        // The authored PipBoyScreen layer is the source for both flat and VR
        // device textures.  Excluding VR here left the physical model bound to
        // its blank green idle material while a separate generic panel carried
        // the live inventory.
        const bool interactive = (VR::getVR() && isFalloutContentLoaded())
            || (mFalloutPipBoyPhysical && containsMode(GM_Inventory));
        // A worn VR Pip-Boy is powered equipment, not a menu-spawned prop.
        // Keep the last live pane on its screen between interactions; raising
        // it only changes input ownership and weapon visibility.
        const bool visible = interactive || (VR::getVR() && isFalloutContentLoaded());
        auto& windows = mGuiModeStates[GM_Inventory].mWindows;
        if (windows.empty())
            return;
        const int activeIndex = std::clamp(getFalloutPipBoyActivePane(), 0, static_cast<int>(windows.size()) - 1);
        const bool useRetailInventory
            = visible && activeIndex == 1 && mFalloutPipBoyRetailInventoryReady && mFalloutPipBoyRetailRoot != nullptr;
        const bool useRetailStats
            = visible && activeIndex == 3 && mFalloutPipBoyRetailStatsReady && mFalloutPipBoyRetailStatsRoot != nullptr;
        const bool useRetailMap
            = visible && (activeIndex == 0 || activeIndex == 2) && mFalloutPipBoyRetailMapReady
                && mFalloutPipBoyRetailMapRoot != nullptr;

        for (std::size_t i = 0; i < windows.size() && i < mFalloutPipBoyOriginalLayers.size(); ++i)
        {
            MyGUI::Widget* widget = windows[i] != nullptr ? windows[i]->mMainWidget : nullptr;
            if (widget == nullptr)
                continue;

            if (mFalloutPipBoyOriginalLayers[i].empty() && widget->getLayer() != nullptr
                && widget->getLayer()->getName() != "PipBoyScreen")
                mFalloutPipBoyOriginalLayers[i] = widget->getLayer()->getName();

            // The authored inventory XML owns ITEMS. Until the other authored
            // XML files have executable tile support, keep their real live
            // panes usable instead of replacing them with a fabricated error
            // page. The fallback is logged below and must not be called retail.
            const bool renderOnDevice
                = visible && !useRetailInventory && !useRetailStats && !useRetailMap
                    && static_cast<int>(i) == activeIndex;
            const std::string targetLayer
                = renderOnDevice ? "PipBoyScreen" : mFalloutPipBoyOriginalLayers[i];
            if (!targetLayer.empty()
                && (widget->getLayer() == nullptr || widget->getLayer()->getName() != targetLayer))
            {
                MyGUI::LayerManager::getInstance().attachToLayerNode(targetLayer, widget);
                Log(Debug::Info) << "FNV Pip-Boy native pane layer: pane=" << i << " layer=" << targetLayer;
            }
            widget->setVisible(renderOnDevice);
        }

        if (mFalloutPipBoyRetailLayout != nullptr)
            mFalloutPipBoyRetailLayout->setVisible(useRetailInventory);
        if (mFalloutPipBoyRetailStatsLayout != nullptr)
            mFalloutPipBoyRetailStatsLayout->setVisible(useRetailStats);
        if (mFalloutPipBoyRetailMapLayout != nullptr)
            mFalloutPipBoyRetailMapLayout->setVisible(useRetailMap);
        if (!visible)
            return;

        if (useRetailStats)
        {
            const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
            const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
            const MWMechanics::DynamicStat<float>& health = stats.getHealth();
            mFalloutPipBoyRetailStatsLevel->setCaption(std::to_string(stats.getLevel()));
            mFalloutPipBoyRetailStatsHealth->setCaption(std::to_string(std::max(0,
                static_cast<int>(health.getCurrent()))) + "/" + std::to_string(std::max(0,
                static_cast<int>(health.getModified(false)))));
            const MWWorld::FalloutPlayerRuntimeState& runtime
                = MWBase::Environment::get().getWorld()->getFalloutPlayerRuntimeState();
            const std::optional<MWWorld::FalloutRuntimeActorValue> actionPoints
                = runtime.getCurrentActorValue(MWWorld::FalloutPlayerRuntimeState::ActionPointsActorValue);
            const std::optional<float> maxActionPoints = runtime.getMaxActionPoints();
            const std::optional<MWWorld::FalloutRuntimeActorValue> experience
                = runtime.getCurrentActorValue(MWWorld::FalloutPlayerRuntimeState::ExperienceActorValue);
            if (actionPoints && maxActionPoints)
                mFalloutPipBoyRetailStatsActionPoints->setCaption(std::to_string(std::max(0,
                    static_cast<int>(actionPoints->mValue))) + "/" + std::to_string(std::max(0,
                    static_cast<int>(*maxActionPoints))));
            else
                mFalloutPipBoyRetailStatsActionPoints->setCaption("ERR");
            if (experience)
                mFalloutPipBoyRetailStatsExperience->setCaption(
                    std::to_string(std::max(0, static_cast<int>(experience->mValue))));
            else
                mFalloutPipBoyRetailStatsExperience->setCaption("ERR");
            mFalloutPipBoyRetailStatsPlayerName->setCaption(std::string(player.getClass().getName(player)));
            return;
        }

        if (useRetailMap)
        {
            const bool dataList = activeIndex == 2;
            if (mFalloutPipBoyRetailDataPanel != nullptr)
                mFalloutPipBoyRetailDataPanel->setVisible(dataList);
            if (mFalloutPipBoyRetailDataHeading != nullptr)
                mFalloutPipBoyRetailDataHeading->setVisible(dataList);
            if (mFalloutPipBoyRetailDataRows != nullptr)
                mFalloutPipBoyRetailDataRows->setVisible(dataList);
            if (mFalloutPipBoyRetailMapTabs != nullptr)
            {
                if (!dataList)
                    mFalloutPipBoyRetailMapTabs->setCaption("LOCAL MAP     [WORLD MAP]     QUESTS     MISC     RADIO");
                else if (mFalloutPipBoySubmenu == 0)
                    mFalloutPipBoyRetailMapTabs->setCaption("LOCAL MAP     WORLD MAP     [QUESTS]     MISC     RADIO");
                else if (mFalloutPipBoySubmenu == 1)
                    mFalloutPipBoyRetailMapTabs->setCaption("LOCAL MAP     WORLD MAP     QUESTS     [MISC]     RADIO");
                else
                    mFalloutPipBoyRetailMapTabs->setCaption("LOCAL MAP     WORLD MAP     QUESTS     MISC     [RADIO]");
            }
            if (dataList && mFalloutPipBoyRetailDataHeading != nullptr && mFalloutPipBoyRetailDataRows != nullptr)
            {
                const char* heading = mFalloutPipBoySubmenu == 0 ? "QUESTS"
                    : mFalloutPipBoySubmenu == 1 ? "NOTES" : "RADIO";
                mFalloutPipBoyRetailDataHeading->setCaption(heading);
                mFalloutPipBoyRetailDataRows->setCaption(mFalloutPipBoyListOffset > 0
                    ? "[ ] Previous entry\n[>] Selected entry" : "[>] Selected entry\n[ ] Next entry");
            }
            return;
        }

        if (!useRetailInventory)
        {
            static std::array<bool, 4> loggedFallback = { false, false, false, false };
            if (!loggedFallback[activeIndex])
            {
                loggedFallback[activeIndex] = true;
                const char* authoredPath = activeIndex == 0 ? "menus/main/stats_menu.xml"
                    : activeIndex == 1                 ? "menus/main/inventory_menu.xml"
                                                       : "menus/main/map_menu.xml";
                Log(Debug::Warning) << "FNV Pip-Boy surface: pane=" << activeIndex
                                    << " source=live-openmw-fallback authoredPath=" << authoredPath
                                    << " reason=retail-xml-pane-not-executable";
            }
            return;
        }

        mFalloutPipBoyRetailTitle->setCaption("ITEMS");
        mFalloutPipBoyRetailBody->setCaption({});
        mFalloutPipBoyRetailItemIcon->setVisible(false);
        mFalloutPipBoyRetailItemInfo->setCaption({});
        if (MWBase::World* const world = MWBase::Environment::get().getWorld())
        {
            const MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty())
                return;
            MWWorld::InventoryStore& inventory = player.getClass().getInventoryStore(player);
            std::vector<MWWorld::Ptr> categoryRows
                = getFalloutPipBoyInventoryRows(inventory, mFalloutPipBoySubmenu);
            const int selectedIndex = categoryRows.empty()
                ? -1
                : std::clamp(mFalloutPipBoyListOffset, 0, static_cast<int>(categoryRows.size()) - 1);
            const int firstIndex = selectedIndex < 0
                ? 0
                : std::clamp(selectedIndex - static_cast<int>(mFalloutPipBoyRetailItemRows.size()) + 1,
                    0, std::max(0, static_cast<int>(categoryRows.size())
                        - static_cast<int>(mFalloutPipBoyRetailItemRows.size())));
            for (std::size_t index = 0; index < mFalloutPipBoyRetailItemRows.size(); ++index)
            {
                const int inventoryIndex = firstIndex + static_cast<int>(index);
                const bool hasItem = inventoryIndex < static_cast<int>(categoryRows.size());
                mFalloutPipBoyRetailItemRows[index]->setVisible(hasItem);
                mFalloutPipBoyRetailItemRows[index]->setUserData(hasItem ? inventoryIndex : -1);
                if (!hasItem)
                    continue;
                const MWWorld::Ptr& item = categoryRows[static_cast<std::size_t>(inventoryIndex)];
                const bool equipped = inventory.isEquipped(item.getCellRef().getRefId());
                std::ostringstream label;
                label << item.getClass().getName(item);
                const int count = item.getCellRef().getCount();
                if (count > 1)
                    label << " (" << count << ')';
                mFalloutPipBoyRetailItemRowLabels[index]->setCaption(label.str());
                mFalloutPipBoyRetailItemRowLabels[index]->setTextColour(inventoryIndex == selectedIndex
                        ? MyGUI::Colour(0.65f, 1.f, 0.65f)
                        : MyGUI::Colour(0.18f, 1.f, 0.22f));
                mFalloutPipBoyRetailItemRowMarkers[index]->setVisible(equipped);
            }
            if (mFalloutPipBoyRetailListHighlight != nullptr)
            {
                const int relativeIndex = selectedIndex < 0 ? 0 : selectedIndex - firstIndex;
                MyGUI::IntCoord highlight = mFalloutPipBoyRetailListHighlight->getCoord();
                highlight.top = relativeIndex * (mFalloutPipBoyRetailItemRows.empty()
                        ? highlight.height
                        : mFalloutPipBoyRetailItemRows.front()->getHeight());
                mFalloutPipBoyRetailListHighlight->setCoord(highlight);
                mFalloutPipBoyRetailListHighlight->setVisible(selectedIndex >= 0);
            }

            std::string icon;
            std::ostringstream details;
            if (selectedIndex >= 0)
            {
                const MWWorld::Ptr& selected = categoryRows[static_cast<std::size_t>(selectedIndex)];
                icon = selected.getClass().getInventoryIcon(selected);
                details << selected.getClass().getName(selected);
                if (selected.getType() == ESM4::Weapon::sRecordId)
                {
                    const ESM4::Weapon& weapon = *selected.get<ESM4::Weapon>()->mBase;
                    details
                            << "\n\nDAM  " << weapon.mData.damage
                            << "     CND  " << weapon.mData.health
                            << "\nWG   " << weapon.mData.weight
                            << "     VAL  " << weapon.mData.value
                            << "\nAMMO " << static_cast<unsigned int>(weapon.mData.clipSize);
                }
                else
                {
                    details << "\n\nWG   " << selected.getClass().getWeight(selected)
                            << "     VAL  " << selected.getClass().getValue(selected);
                }
            }
            if (!details.str().empty())
                mFalloutPipBoyRetailItemInfo->setCaption(details.str());
            if (!icon.empty())
            {
                const VFS::Manager* const vfs = mResourceSystem->getVFS();
                const std::string texture = Misc::ResourceHelpers::correctTexturePath(icon, vfs);
                if (vfs->exists(texture))
                {
                    mFalloutPipBoyRetailItemIcon->setImageTexture(texture);
                    if (MyGUI::ITexture* source = MyGUI::RenderManager::getInstance().getTexture(texture))
                    {
                        const MyGUI::IntSize sourceSize(source->getWidth(), source->getHeight());
                        mFalloutPipBoyRetailItemIcon->setImageTile(sourceSize);
                        mFalloutPipBoyRetailItemIcon->setImageCoord(
                            MyGUI::IntCoord(0, 0, sourceSize.width, sourceSize.height));
                    }
                    mFalloutPipBoyRetailItemIcon->setVisible(true);
                }
                else
                    Log(Debug::Error) << "FNV Pip-Boy retail item icon: status=fail editorIcon=" << icon
                                      << " corrected=" << texture;
            }
        }
        InventoryWindow* const inventoryWindow = getInventoryWindow();
        const std::array<std::string, 5> categoryLabels = inventoryWindow != nullptr
            ? inventoryWindow->getFalloutPipBoyCategoryLabels()
            : std::array<std::string, 5>{};
        std::ostringstream categoryTabs;
        bool categoryLabelsReady = true;
        for (std::size_t index = 0; index < categoryLabels.size(); ++index)
        {
            if (categoryLabels[index].empty())
            {
                categoryLabelsReady = false;
                static std::array<bool, 5> missingLogged{};
                if (!missingLogged[index])
                {
                    missingLogged[index] = true;
                    Log(Debug::Error) << "FNV Pip-Boy retail category label: status=fail index=" << index
                                      << " source=native-inventory-filter-caption";
                }
                continue;
            }
            if (index != 0)
                categoryTabs << "   ";
            if (static_cast<int>(index) == mFalloutPipBoySubmenu)
                categoryTabs << '[' << categoryLabels[index] << ']';
            else
                categoryTabs << categoryLabels[index];
        }
        mFalloutPipBoyRetailTabs->setCaption(categoryLabelsReady ? categoryTabs.str() : "ERROR: MISSING GMST");
        MWBase::InputManager* const input = MWBase::Environment::get().getInputManager();
        const auto binding = [input](int action, std::string_view fallback) {
            if (input == nullptr)
                return std::string(fallback);
            const std::string value = input->getActionKeyBindingName(action);
            return value.empty() ? std::string(fallback) : value;
        };
        std::ostringstream controls;
        controls << binding(MWInput::A_Activate, "E") << " EQUIP/USE  "
                 << binding(MWInput::A_MoveForward, "W") << '/'
                 << binding(MWInput::A_MoveBackward, "S") << " SELECT  "
                 << binding(MWInput::A_MoveLeft, "A") << '/'
                 << binding(MWInput::A_MoveRight, "D") << " CATEGORY  1 STATS  2 ITEMS  3 DATA  4 MAP  "
                 << binding(MWInput::A_FalloutPipBoy, "P") << " CLOSE";
        mFalloutPipBoyRetailActions->setCaption(
            mFalloutPipBoyLastAction.empty() ? controls.str() : mFalloutPipBoyLastAction + "   " + controls.str());
        static bool loggedRetailVisibility = false;
        if (!loggedRetailVisibility)
        {
            loggedRetailVisibility = true;
            const auto visibility = [](const MyGUI::Widget* widget) {
                if (widget == nullptr)
                    return std::string("missing");
                return std::to_string(widget->getVisible()) + '/'
                    + std::to_string(widget->getInheritedVisible());
            };
            Log(Debug::Info) << "FNV Pip-Boy retail live visibility: root="
                             << visibility(mFalloutPipBoyRetailRoot)
                             << " title=" << visibility(mFalloutPipBoyRetailTitle)
                             << " listRow=" << visibility(mFalloutPipBoyRetailItemRows.empty()
                                    ? nullptr : mFalloutPipBoyRetailItemRows.front())
                             << " icon=" << visibility(mFalloutPipBoyRetailItemIcon)
                             << " iconParent=" << visibility(mFalloutPipBoyRetailItemIcon != nullptr
                                    ? mFalloutPipBoyRetailItemIcon->getParent() : nullptr)
                             << " info=" << visibility(mFalloutPipBoyRetailItemInfo)
                             << " infoParent=" << visibility(mFalloutPipBoyRetailItemInfo != nullptr
                                    ? mFalloutPipBoyRetailItemInfo->getParent() : nullptr)
                             << " tabs=" << visibility(mFalloutPipBoyRetailTabs)
                             << " tabsParent=" << visibility(mFalloutPipBoyRetailTabs != nullptr
                                    ? mFalloutPipBoyRetailTabs->getParent() : nullptr);
        }
    }

    void WindowManager::interactiveFnvMenuMessageBox(const FnvMenuXmlDocument& menu,
        std::string_view frameTile, std::string_view messageTile, std::string_view buttonTile,
        std::string_view message, const std::vector<std::string>& buttons, bool block, int defaultFocus,
        const FnvHackingMenuPresentation* hacking)
    {
        mMessageBoxManager->createInteractiveFnvMenuMessageBox(
            menu, frameTile, messageTile, buttonTile, message, buttons, block, defaultFocus, hacking);
        updateVisible();

        if (block)
        {
            Misc::FrameRateLimiter frameRateLimiter
                = Misc::makeFrameRateLimiter(MWBase::Environment::get().getFrameRateLimit());
            while (mMessageBoxManager->readPressedButton(false) == -1
                && !MWBase::Environment::get().getStateManager()->hasQuitRequest())
            {
                const double dt
                    = std::chrono::duration_cast<std::chrono::duration<double>>(frameRateLimiter.getLastFrameDuration())
                          .count();
                mKeyboardNavigation->onFrame();
                mMessageBoxManager->onFrame(dt);
                MWBase::Environment::get().getInputManager()->update(dt, true, false);
                if (!mWindowVisible)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                else
                    viewerTraversals();
                mViewer->advance(mViewer->getFrameStamp()->getSimulationTime());
                frameRateLimiter.limit();
            }
            mMessageBoxManager->resetInteractiveMessageBox();
        }
    }

    void WindowManager::update(float frameDuration)
    {
        handleScheduledMessageBoxes();
        updateFalloutDialogueCamera();
        // A physical press needs time to read: approach, contact, and return.
        // The old 0.8-second envelope was shorter than the showcase's next
        // action, so the real off-hand was continuously reset and visibly
        // flapped.  Keep each device-local press a single deliberate motion.
        mFalloutPipBoyInteractionPulse = std::max(0.f, mFalloutPipBoyInteractionPulse
                - std::max(0.f, frameDuration) * 0.55f);
        updateFalloutPipBoyTerminalSurface();

        bool gameRunning
            = MWBase::Environment::get().getStateManager()->getState() != MWBase::StateManager::State_NoGame;

        if (gameRunning)
            updateMap();

        if (!mGuiModes.empty())
        {
            GuiModeState& state = mGuiModeStates[mGuiModes.back()];
            for (WindowBase* window : state.mWindows)
                window->onFrame(frameDuration);
        }
        else
        {
            // update pinned windows if visible
            for (WindowBase* window : mGuiModeStates[GM_Inventory].mWindows)
                if (window->isVisible())
                    window->onFrame(frameDuration);
        }

        // Make sure message boxes are always in front
        // This is an awful workaround for a series of awfully interwoven issues that couldn't be worked around
        // in a better way because of an impressive number of even more awfully interwoven issues.
        if (mMessageBoxManager && mMessageBoxManager->isInteractiveMessageBox()
            && mCurrentModals.back() != mMessageBoxManager->getInteractiveMessageBox())
        {
            std::vector<WindowModal*>::iterator found = std::find(
                mCurrentModals.begin(), mCurrentModals.end(), mMessageBoxManager->getInteractiveMessageBox());
            if (found != mCurrentModals.end())
            {
                WindowModal* msgbox = *found;
                std::swap(*found, mCurrentModals.back());
                MyGUI::InputManager::getInstance().addWidgetModal(msgbox->mMainWidget);
                mKeyboardNavigation->setModalWindow(msgbox->mMainWidget);
                mKeyboardNavigation->setDefaultFocus(msgbox->mMainWidget, msgbox->getDefaultKeyFocus());
            }
        }

        if (!mCurrentModals.empty())
            mCurrentModals.back()->onFrame(frameDuration);

        mKeyboardNavigation->onFrame();

        if (mMessageBoxManager)
            mMessageBoxManager->onFrame(frameDuration);

        mToolTips->onFrame(frameDuration);

        if (mLocalMapRender)
            mLocalMapRender->cleanupCameras();

        mDebugWindow->onFrame(frameDuration);

        if (isConsoleMode())
            mConsole->onFrame(frameDuration);

        if (isSettingsWindowVisible())
            mSettingsWindow->onFrame(frameDuration);

        if (mControllerButtonsOverlay && mControllerButtonsOverlay->isVisible())
            mControllerButtonsOverlay->onFrame(frameDuration);

        if (mInventoryTabsOverlay && mInventoryTabsOverlay->isVisible())
            mInventoryTabsOverlay->onFrame(frameDuration);

        if (!gameRunning)
            return;

        // We should display message about crime only once per frame, even if there are several crimes.
        // Otherwise we will get message spam when stealing several items via Take All button.
        const MWWorld::Ptr player = MWMechanics::getPlayer();
        const MWWorld::Class& playerCls = player.getClass();
        int currentBounty = playerCls.getNpcStats(player).getBounty();
        if (currentBounty != mPlayerBounty)
        {
            if (mPlayerBounty >= 0 && currentBounty > mPlayerBounty)
                messageBox("#{sCrimeMessage}");

            mPlayerBounty = currentBounty;
        }

        MWBase::LuaManager::ActorControls* playerControls
            = MWBase::Environment::get().getLuaManager()->getActorControls(player);
        bool triedToMove = playerControls
            && (playerControls->mMovement != 0 || playerControls->mSideMovement != 0 || playerControls->mJump);
        if (mMessageBoxManager && triedToMove && playerCls.getEncumbrance(player) > playerCls.getCapacity(player))
        {
            const auto& msgboxs = mMessageBoxManager->getActiveMessageBoxes();
            auto it
                = std::find_if(msgboxs.begin(), msgboxs.end(), [](const std::unique_ptr<MWGui::MessageBox>& msgbox) {
                      return (msgbox->getMessage() == "#{sNotifyMessage59}");
                  });

            // if an overencumbered messagebox is already present, reset its expiry timer,
            // otherwise create a new one.
            if (it != msgboxs.end())
                (*it)->mCurrentTime = 0;
            else
                messageBox("#{sNotifyMessage59}");
        }

        mDragAndDrop->onFrame();

        mHud->onFrame(frameDuration);

        mPostProcessorHud->onFrame(frameDuration);

        if (mCharGen)
            mCharGen->onFrame(frameDuration);

        updateActivatedQuickKey();

        mStatsWatcher->update();

        cleanupGarbage();
    }

    void WindowManager::changeCell(const MWWorld::CellStore* cell)
    {
        mMap->requestMapRender(cell);

        std::string name{ MWBase::Environment::get().getWorld()->getCellName(cell) };

        mMap->setCellName(name);
        mHud->setCellName(name);
        auto cellCommon = cell->getCell();

        if (cellCommon->isExterior())
        {
            if (!cellCommon->getNameId().empty())
                mMap->addVisitedLocation(name, cellCommon->getGridX(), cellCommon->getGridY());

            mMap->cellExplored(cellCommon->getGridX(), cellCommon->getGridY());
        }
        else
        {
            osg::Vec3f worldPos;
            if (!MWBase::Environment::get().getWorld()->findInteriorPositionInWorldSpace(cell, worldPos))
                worldPos = MWBase::Environment::get().getWorld()->getPlayer().getLastKnownExteriorPosition();
            else
                MWBase::Environment::get().getWorld()->getPlayer().setLastKnownExteriorPosition(worldPos);
            mMap->setGlobalMapPlayerPosition(worldPos.x(), worldPos.y());
        }
        setActiveMap(*cellCommon);
    }

    void WindowManager::setActiveMap(const MWWorld::Cell& cell)
    {
        mMap->setActiveCell(cell);
        mHud->setActiveCell(cell);
    }

    void WindowManager::setDrowningBarVisibility(bool visible)
    {
        mHud->setDrowningBarVisible(visible);
    }

    void WindowManager::setHMSVisibility(bool visible)
    {
        mHud->setHmsVisible(visible);
    }

    void WindowManager::setMinimapVisibility(bool visible)
    {
        mHud->setMinimapVisible(visible);
    }

    bool WindowManager::toggleFogOfWar()
    {
        mMap->toggleFogOfWar();
        return mHud->toggleFogOfWar();
    }

    void WindowManager::setFocusObject(const MWWorld::Ptr& focus)
    {
        mToolTips->setFocusObject(focus);

        const int showOwned = Settings::game().mShowOwned;
        if (mHud && (showOwned == 2 || showOwned == 3))
        {
            bool owned = mToolTips->checkOwned();
            mHud->setCrosshairOwned(owned);
        }
    }

    void WindowManager::setFocusObjectScreenCoords(float x, float y)
    {
        mToolTips->setFocusObjectScreenCoords(x, y);
    }

    bool WindowManager::toggleFullHelp()
    {
        return mToolTips->toggleFullHelp();
    }

    bool WindowManager::getFullHelp() const
    {
        return mToolTips->getFullHelp();
    }

    void WindowManager::setWeaponVisibility(bool visible)
    {
        mHud->setWeapVisible(visible);
    }

    void WindowManager::setSpellVisibility(bool visible)
    {
        mHud->setSpellVisible(visible);
        mHud->setEffectVisible(visible);
    }

    void WindowManager::setSneakVisibility(bool visible)
    {
        mHud->setSneakVisible(visible);
    }

    void WindowManager::setDragDrop(bool dragDrop)
    {
        mToolTips->setEnabled(!dragDrop);
        MWBase::Environment::get().getInputManager()->setDragDrop(dragDrop);
    }

    void WindowManager::setCursorVisible(bool visible)
    {
        mCursorVisible = visible;
    }

    void WindowManager::setCursorActive(bool active)
    {
        mCursorActive = active;
    }

    void WindowManager::onRetrieveTag(const MyGUI::UString& tag, MyGUI::UString& result)
    {
        std::string_view tagView = tag;

        constexpr std::string_view myGuiPrefix = "setting=";

        constexpr std::string_view tokenToFind = "sCell=";

        if (tagView.starts_with(myGuiPrefix))
        {
            tagView = tagView.substr(myGuiPrefix.length());
            const size_t commaPos = tagView.find(',');
            if (commaPos == std::string_view::npos)
                throw std::runtime_error("Invalid setting tag (expected comma): " + std::string(tagView));

            std::string_view settingSection = tagView.substr(0, commaPos);
            std::string_view settingTag = tagView.substr(commaPos + 1, tagView.length());

            result = Settings::get<MyGUI::Colour>(settingSection, settingTag).get().print();
        }
        else if (tagView.starts_with(tokenToFind))
        {
            std::string_view cellName = mTranslationDataStorage.translateCellName(tagView.substr(tokenToFind.length()));
            result.assign(cellName.data(), cellName.size());
            result = MyGUI::TextIterator::toTagsString(result);
        }
        else if (Gui::replaceTag(tagView, result))
        {
            return;
        }
        else
        {
            std::vector<std::string> split;
            Misc::StringUtils::split(tagView, split, ":");

            L10n::Manager& l10nManager = *MWBase::Environment::get().getL10nManager();

            // If a key has a "Context:KeyName" format, use YAML to translate data
            if (split.size() == 2)
            {
                result = l10nManager.getContext(split[0])->formatMessage(split[1], {}, {});
                return;
            }

            // If not, treat is as GMST name from legacy localization
            if (!mStore)
            {
                Log(Debug::Error) << "Error: WindowManager::onRetrieveTag: no Store set up yet, can not replace '"
                                  << tagView << "'";
                result.assign(tagView.data(), tagView.size());
                return;
            }
            const ESM::GameSetting* setting = mStore->get<ESM::GameSetting>().search(tagView);

            if (setting && setting->mValue.getType() == ESM::VT_String)
                result = setting->mValue.getString();
            else
                result.assign(tagView.data(), tagView.size());
        }
    }

    void WindowManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        bool changeRes = false;
        for (const auto& setting : changed)
        {
            if (setting.first == "GUI" && setting.second == "menu transparency")
                setMenuTransparency(Settings::gui().mMenuTransparency);
            else if (setting.first == "Video"
                && (setting.second == "resolution x" || setting.second == "resolution y"
                    || setting.second == "window mode" || setting.second == "window border"))
                changeRes = true;

            else if (setting.first == "Video" && setting.second == "vsync mode")
                mVideoWrapper->setSyncToVBlank(Settings::video().mVsyncMode);
            else if (setting.first == "Video" && (setting.second == "gamma" || setting.second == "contrast"))
                mVideoWrapper->setGammaContrast(Settings::video().mGamma, Settings::video().mContrast);
        }

        if (changeRes)
        {
            mVideoWrapper->setVideoMode(Settings::video().mResolutionX, Settings::video().mResolutionY,
                Settings::video().mWindowMode, Settings::video().mWindowBorder);
        }
    }

    void WindowManager::windowResized(int x, int y)
    {
//## VR_PATCH BEGIN
        if(VR::getVR())
            return;
//## VR_PATCH END
        Settings::video().mResolutionX.set(x);
        Settings::video().mResolutionY.set(y);

        // We only want to process changes to window-size related settings.
        Settings::CategorySettingVector filter = { { "Video", "resolution x" }, { "Video", "resolution y" } };

        // If the HUD has not been initialised, the World singleton will not be available.
        if (mHud)
        {
            MWBase::Environment::get().getWorld()->processChangedSettings(Settings::Manager::getPendingChanges(filter));
        }

        Settings::Manager::resetPendingChanges(filter);

        mGuiPlatform->getRenderManagerPtr()->setViewSize(x, y);

        // scaled size
        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        x = viewSize.width;
        y = viewSize.height;

        sizeVideo(x, y);

        if (!mHud)
            return; // UI not initialized yet

        for (const auto& [window, settings] : mTrackedWindows)
        {
            const WindowRectSettingValues& rect = settings.mIsMaximized ? settings.mMaximized : settings.mRegular;
            window->setPosition(MyGUI::IntPoint(static_cast<int>(rect.mX * x), static_cast<int>(rect.mY * y)));
            window->setSize(MyGUI::IntSize(static_cast<int>(rect.mW * x), static_cast<int>(rect.mH * y)));

            WindowBase::clampWindowCoordinates(window);
        }

        for (const auto& window : mWindows)
            window->onResChange(x, y);

        // Re-apply any controller-specific window changes.
        reapplyActiveControllerWindow();

        // TODO: check if any windows are now off-screen and move them back if so
    }

    bool WindowManager::isWindowVisible() const
    {
        return mWindowVisible;
    }

    void WindowManager::windowVisibilityChange(bool visible)
    {
        mWindowVisible = visible;
    }

    void WindowManager::windowClosed()
    {
        MWBase::Environment::get().getStateManager()->requestQuit();
    }

    void WindowManager::onCursorChange(std::string_view name)
    {
        mCursorManager->cursorChanged(name);
    }

    void WindowManager::beginFalloutDialogueCamera(const MWWorld::Ptr& target)
    {
        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (world == nullptr || VR::getVR() || target.isEmpty() || !target.getClass().isActor()
            || world->getStore().getESM4Game() != MWWorld::ESM4Game::FalloutNewVegas)
            return;

        MWWorld::Ptr player = world->getPlayerPtr();
        MWRender::Camera* const camera = world->getCamera();
        MWRender::RenderingManager* const rendering = world->getRenderingManager();
        if (camera == nullptr || rendering == nullptr || player.isEmpty() || !player.isInCell() || !target.isInCell()
            || player.getCell() != target.getCell() || camera->getMode() == MWRender::Camera::Mode::VR)
            return;

        // A second dialogue target may replace the first without returning to gameplay in between. Restore the
        // original player camera before deriving the next target's view so offsets cannot accumulate.
        endFalloutDialogueCamera();

        auto actorFocus = [&](const MWWorld::Ptr& actor) {
            osg::Vec3f focus = world->getActorHeadTransform(actor).getTrans();
            const osg::Vec3f actorPosition = actor.getRefData().getPosition().asVec3();
            if ((focus - actorPosition).length2() <= 16.f * 16.f)
            {
                const osg::Vec3f halfExtents = world->getHalfExtents(actor, true);
                focus.z() += std::max(64.f, halfExtents.z() * 2.f * 0.85f);
            }
            return focus;
        };

        const osg::Vec3f focus = actorFocus(target);
        const osg::Vec3f playerFocus = actorFocus(player);
        if (!std::isfinite(focus.x()) || !std::isfinite(focus.y()) || !std::isfinite(focus.z()))
            return;

        osg::Vec3f outward = playerFocus - focus;
        const float horizontal = std::hypot(outward.x(), outward.y());
        if (horizontal < 1.f)
            return;
        // Keep the close-up near eye level even when the player stands on steep terrain above or below the actor.
        outward.z() = std::clamp(outward.z(), -horizontal * 0.35f, horizontal * 0.35f);
        outward.normalize();

        constexpr float desiredDistance = 72.f;
        constexpr float surfaceClearance = 24.f;
        osg::Vec3f cameraPosition = focus + outward * desiredDistance;
        bool rayClamped = false;
        float obstructionDistance = desiredDistance;
        MWPhysics::RayCastingResult hit{};
        const std::array<MWWorld::Ptr, 1> ignored{ target };
        if (world->castRenderingRay(hit, focus, cameraPosition, true, true, ignored))
        {
            obstructionDistance = (hit.mHitPos - focus).length();
            const float clampedDistance = obstructionDistance - surfaceClearance;
            // A wall this close leaves no valid near-plane-safe face camera. Keep the player's normal view instead
            // of crossing geometry or photographing the inside of the actor's head.
            if (clampedDistance < 32.f)
                return;
            cameraPosition = focus + outward * clampedDistance;
            rayClamped = true;
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
