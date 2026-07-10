#include "hud.hpp"

#include <cmath>

#include <MyGUI_Button.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_ProgressBar.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_TextBox.h>

#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/debug/debuglog.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "draganddrop.hpp"
#include "inventorywindow.hpp"
#include "itemwidget.hpp"
#include "spellicons.hpp"
#include "worlditemmodel.hpp"

//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
//## VR_PATCH END

namespace MWGui
{
    namespace
    {
        bool hasFalloutContent()
        {
            if (std::getenv("OPENMW_FNV_PROOF_PIPBOY_SURFACE") != nullptr)
                return true;

            if (!MWBase::Environment::get().getWorld())
                return false;

            for (const std::string& file : MWBase::Environment::get().getWorld()->getContentFiles())
            {
                if (file.find("FalloutNV.esm") != std::string::npos || file.find("falloutnv.esm") != std::string::npos)
                    return true;
            }

            return false;
        }
    }

    HUD::HUD(CustomMarkerCollection& customMarkers, DragAndDrop* dragAndDrop, MWRender::LocalMap* localMapRender)
//## VR_PATCH BEGIN
        : WindowBase(VR::getVR() ? "openmw_hud_vr.layout" : "openmw_hud.layout")
//## VR_PATCH END
        , LocalMapBase(customMarkers, localMapRender, Settings::map().mLocalMapHudFogOfWar)
        , mHealth(nullptr)
        , mMagicka(nullptr)
        , mStamina(nullptr)
        , mDrowning(nullptr)
        , mWeapImage(nullptr)
        , mSpellImage(nullptr)
        , mWeapStatus(nullptr)
        , mSpellStatus(nullptr)
        , mEffectBox(nullptr)
        , mMinimap(nullptr)
        , mCrosshair(nullptr)
        , mCellNameBox(nullptr)
        , mDrowningBar(nullptr)
        , mDrowningFlash(nullptr)
        , mHealthManaStaminaBaseLeft(0)
        , mWeapBoxBaseLeft(0)
        , mSpellBoxBaseLeft(0)
        , mMinimapBoxBaseRight(0)
        , mEffectBoxBaseRight(0)
        , mDragAndDrop(dragAndDrop)
        , mCellNameTimer(0.0f)
        , mWeaponSpellTimer(0.f)
        , mMapVisible(true)
        , mWeaponVisible(true)
        , mSpellVisible(true)
        , mWorldMouseOver(false)
        , mEnemyActorId(-1)
        , mEnemyHealthTimer(-1)
        , mIsDrowning(false)
        , mDrowningFlashTheta(0.f)
    {
//## VR_PATCH BEGIN
// Hud size shouldn't depend on window size in VR
        if(VR::getVR())
        {
            mMainWidgetBaseSize = mMainWidget->getSize();
            mMainWidget->setSize(mMainWidgetBaseSize);
        }
        else
        {
            mMainWidget->setSize(MyGUI::RenderManager::getInstance().getViewSize());
            mMainWidgetBaseSize = mMainWidget->getSize();
        }

//## VR_PATCH END
        // Energy bars
        getWidget(mHealthFrame, "HealthFrame");
        getWidget(mHealth, "Health");
        getWidget(mMagicka, "Magicka");
        getWidget(mStamina, "Stamina");
        getWidget(mEnemyHealth, "EnemyHealth");
        mHealthManaStaminaBaseLeft = mHealthFrame->getLeft();

        MyGUI::Widget *healthFrame, *magickaFrame, *fatigueFrame;
        getWidget(healthFrame, "HealthFrame");
        getWidget(magickaFrame, "MagickaFrame");
        getWidget(fatigueFrame, "FatigueFrame");
        healthFrame->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onHMSClicked);
        magickaFrame->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onHMSClicked);
        fatigueFrame->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onHMSClicked);

        bool falloutContent = hasFalloutContent();

        if (falloutContent)
        {
            if (mHealth)
            {
                mHealth->changeWidgetSkin("MW_EnergyBar_Green");
                mHealth->setColour(MyGUI::Colour(0.25f, 1.f, 0.25f, 1.f));
            }
            if (mMagicka)
            {
                mMagicka->changeWidgetSkin("MW_EnergyBar_Green");
                mMagicka->setColour(MyGUI::Colour(0.25f, 1.f, 0.25f, 1.f));
            }
            if (mStamina) { mStamina->changeWidgetSkin("MW_EnergyBar_Green"); }
            if (fatigueFrame && falloutContent) fatigueFrame->setVisible(false);
            if (mStamina && falloutContent && VR::getVR()) mStamina->setVisible(false);
            Log(Debug::Info) << "FNV/ESM4 proof: HUD Fallout bars applied HP/AP"
                             << (VR::getVR() ? "/stamina wrist" : "")
                             << " green theme";
        }

        // Drowning bar
        getWidget(mDrowningBar, "DrowningBar");
        getWidget(mDrowningFrame, "DrowningFrame");
        getWidget(mDrowning, "Drowning");
        getWidget(mDrowningFlash, "Flash");
        mDrowning->setProgressRange(200);

        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();

        // Item and spell images and status bars
        getWidget(mWeapBox, "WeapBox");
        getWidget(mWeapImage, "WeapImage");
        getWidget(mWeapStatus, "WeapStatus");
        mWeapBoxBaseLeft = mWeapBox->getLeft();
        mWeapBox->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onWeaponClicked);

        getWidget(mSpellBox, "SpellBox");
        getWidget(mSpellImage, "SpellImage");
        getWidget(mSpellStatus, "SpellStatus");
        mSpellBoxBaseLeft = mSpellBox->getLeft();
        mSpellBox->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onMagicClicked);

        getWidget(mSneakBox, "SneakBox");
        mSneakBoxBaseLeft = mSneakBox->getLeft();

        getWidget(mEffectBox, "EffectBox");
        mEffectBoxBaseRight = viewSize.width - mEffectBox->getRight();

        getWidget(mMinimapBox, "MiniMapBox");
        mMinimapBoxBaseRight = viewSize.width - mMinimapBox->getRight();
        getWidget(mMinimap, "MiniMap");
        getWidget(mCompass, "Compass");
        getWidget(mMinimapButton, "MiniMapButton");
        mMinimapButton->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onMapClicked);

        getWidget(mCellNameBox, "CellName");
        getWidget(mWeaponSpellBox, "WeaponSpellName");
        try
        {
            getWidget(mCompassHeading, "CompassHeading");
        }
        catch (const MyGUI::Exception& e)
        {
            mCompassHeading = nullptr;
            Log(Debug::Info) << "FNV/ESM4 proof: optional CompassHeading HUD widget unavailable: " << e.what();
        }

        getWidget(mCrosshair, "Crosshair");

        LocalMapBase::init(mMinimap, mCompass);

        if (falloutContent && VR::getVR())
        {
            mLocalMapZoom = 0.82f;
            if (mMinimapBox->getChildCount() > 0)
            {
                mMinimapBox->getChildAt(0)->setAlpha(0.8f);
                mMinimapBox->getChildAt(0)->setCoord(0, 0, 150, 150);
            }
            mMinimapBox->setCoord(8, 82, 150, 150);
            mMinimap->setCoord(5, 5, 140, 140);
            mCompass->setCoord(39, 39, 72, 72);
            mCompass->setColour(MyGUI::Colour(0.25f, 1.f, 0.25f, 1.f));
            mCompass->setVisible(false);
            if (mCompassHeading != nullptr)
            {
                mCompassHeading->setVisible(true);
                mCompassHeading->setCoord(39, 63, 72, 24);
            }
            mMinimapButton->setCoord(0, 0, 140, 140);
            Log(Debug::Info) << "FNV/ESM4 proof: using square Fallout local map on VR wrist HUD zoom="
                             << mLocalMapZoom;
        }
        else if (falloutContent && !VR::getVR())
        {
            const int margin = 18;
            const int barWidth = 160;
            const int barHeight = 18;
            const int barGap = 7;
            const int mapSize = 150;
            const int mapInset = 5;
            const int iconSize = 54;
            const int iconStatusHeight = 7;

            const int barTop = viewSize.height - margin - (barHeight * 2 + barGap);
            mHealthFrame->setCoord(margin, barTop, barWidth, barHeight);
            mHealth->setCoord(0, 0, barWidth, barHeight);
            magickaFrame->setCoord(margin, barTop + barHeight + barGap, barWidth, barHeight);
            mMagicka->setCoord(0, 0, barWidth, barHeight);

            const int iconTop = viewSize.height - margin - iconSize - iconStatusHeight;
            mWeapBox->setCoord(margin + barWidth + 18, iconTop, iconSize, iconSize + iconStatusHeight);
            mSpellBox->setCoord(margin + barWidth + 18 + iconSize + 8, iconTop, iconSize, iconSize + iconStatusHeight);
            mSneakBox->setCoord(margin + barWidth + 18 + (iconSize + 8) * 2, iconTop, iconSize, iconSize);

            mMinimapBox->setCoord(viewSize.width - margin - mapSize, viewSize.height - margin - mapSize, mapSize, mapSize);
            if (mMinimapBox->getChildCount() > 0)
                mMinimapBox->getChildAt(0)->setCoord(0, 0, mapSize, mapSize);
            mMinimap->setCoord(mapInset, mapInset, mapSize - mapInset * 2, mapSize - mapInset * 2);
            mCompass->setCoord((mapSize - 72) / 2, (mapSize - 72) / 2, 72, 72);
            mMinimapButton->setCoord(0, 0, mapSize - mapInset * 2, mapSize - mapInset * 2);

            mEffectBox->setPosition(viewSize.width - margin - mapSize - 28, viewSize.height - margin - 24);

            mHealthManaStaminaBaseLeft = mHealthFrame->getLeft();
            mWeapBoxBaseLeft = mWeapBox->getLeft();
            mSpellBoxBaseLeft = mSpellBox->getLeft();
            mSneakBoxBaseLeft = mSneakBox->getLeft();
            mMinimapBoxBaseRight = viewSize.width - mMinimapBox->getRight();
            mEffectBoxBaseRight = viewSize.width - mEffectBox->getRight();
            Log(Debug::Info) << "FNV/ESM4 proof: flat Fallout HUD scaled HP/AP and compass to "
                             << barWidth << "x" << barHeight << " bars, " << mapSize << "x" << mapSize
                             << " minimap";
        }

        mMainWidget->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onWorldClicked);
        mMainWidget->eventMouseMove += MyGUI::newDelegate(this, &HUD::onWorldMouseOver);
        mMainWidget->eventMouseLostFocus += MyGUI::newDelegate(this, &HUD::onWorldMouseLostFocus);

        mSpellIcons = std::make_unique<SpellIcons>();
    }

    HUD::~HUD()
    {
        mMainWidget->eventMouseLostFocus.clear();
        mMainWidget->eventMouseMove.clear();
        mMainWidget->eventMouseButtonClick.clear();
    }

    void HUD::setValue(std::string_view id, const MWMechanics::DynamicStat<float>& value)
    {
        int current = static_cast<int>(value.getCurrent());
        int modified = static_cast<int>(value.getModified());
        // Fatigue can be negative
        if (id != "FBar")
            current = std::max(0, current);

        MyGUI::Widget* w;
        std::string valStr = MyGUI::utility::toString(current) + " / " + MyGUI::utility::toString(modified);
        if (id == "HBar")
        {
            mHealth->setProgressRange(std::max(0, modified));
            mHealth->setProgressPosition(std::max(0, current));
            getWidget(w, "HealthFrame");
            w->setUserString("Caption_HealthDescription", "#{sHealthDesc}\n" + valStr);
            if (VR::getVR() && hasFalloutContent())
            {
                MyGUI::TextBox* valueText;
                getWidget(valueText, "HealthValue");
                valueText->setCaption(valStr);
            }
        }
        else if (id == "MBar")
        {
            mMagicka->setProgressRange(std::max(0, modified));
            mMagicka->setProgressPosition(std::max(0, current));
            getWidget(w, "MagickaFrame");
            w->setUserString("Caption_HealthDescription", "#{sMagDesc}\n" + valStr);
            if (VR::getVR() && hasFalloutContent())
            {
                MyGUI::TextBox* valueText;
                getWidget(valueText, "MagickaValue");
                valueText->setCaption(valStr);
            }
        }
        else if (id == "FBar")
        {
            mStamina->setProgressRange(std::max(0, modified));
            mStamina->setProgressPosition(std::max(0, current));
            getWidget(w, "FatigueFrame");
            w->setUserString("Caption_HealthDescription", "#{sFatDesc}\n" + valStr);
            if (VR::getVR() && hasFalloutContent())
            {
                MyGUI::TextBox* valueText;
                getWidget(valueText, "StaminaValue");
                valueText->setCaption("");
                valueText->setVisible(false);
            }
        }

        if (VR::getVR() && hasFalloutContent())
        {
            static int logged = 0;
            if (logged < 12)
            {
                Log(Debug::Info) << "FNV/ESM4 proof: wrist HUD stat feed id=" << id
                                 << " value=" << current << "/" << modified;
                ++logged;
            }
        }
    }

    void HUD::setDrowningTimeLeft(float time, float maxTime)
    {
        size_t progress = static_cast<size_t>(time / maxTime * 200);
        mDrowning->setProgressPosition(progress);

        bool isDrowning = (progress == 0);
        if (isDrowning && !mIsDrowning) // Just started drowning
            mDrowningFlashTheta = 0.0f; // Start out on bright red every time.

        mDrowningFlash->setVisible(isDrowning);
        mIsDrowning = isDrowning;
    }

    void HUD::setDrowningBarVisible(bool visible)
    {
        mDrowningBar->setVisible(visible);
    }

    void HUD::dropDraggedItem(float mouseX, float mouseY)
    {
        if (!mDragAndDrop->mIsOnDragAndDrop)
            return;

        MWBase::Environment::get().getWorld()->breakInvisibility(MWMechanics::getPlayer());

        WorldItemModel drop(mouseX, mouseY);
        mDragAndDrop->drop(&drop, nullptr);
    }

    void HUD::onWorldClicked(MyGUI::Widget* /*sender*/)
    {
        if (!MWBase::Environment::get().getWindowManager()->isGuiMode())
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (mDragAndDrop->mIsOnDragAndDrop)
        {
            const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            const MyGUI::IntPoint cursorPosition = MyGUI::InputManager::getInstance().getMousePosition();
            const float cursorX = cursorPosition.left / static_cast<float>(viewSize.width);
            const float cursorY = cursorPosition.top / static_cast<float>(viewSize.height);

            // drop item into the gameworld
            WorldItemModel worldItemModel(cursorX, cursorY);
            mDragAndDrop->drop(&worldItemModel, nullptr);

            winMgr->changePointer("arrow");
        }
        else
        {
            GuiMode mode = winMgr->getMode();

            if (!winMgr->isConsoleMode() && (mode != GM_Container) && (mode != GM_Inventory))
                return;

            MWWorld::Ptr object = MWBase::Environment::get().getWorld()->getFacedObject();

            if (winMgr->isConsoleMode())
                winMgr->setConsoleSelectedObject(object);
            else // if ((mode == GM_Container) || (mode == GM_Inventory))
            {
                // pick up object
                if (!object.isEmpty())
                    winMgr->getInventoryWindow()->pickUpObject(object);
            }
        }
    }

    void HUD::onWorldMouseOver(MyGUI::Widget* /*sender*/, int x, int y)
    {
        if (mDragAndDrop->mIsOnDragAndDrop)
        {
            mWorldMouseOver = false;

            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            MyGUI::IntPoint cursorPosition = MyGUI::InputManager::getInstance().getMousePosition();
            float mouseX = cursorPosition.left / float(viewSize.width);
            float mouseY = cursorPosition.top / float(viewSize.height);

            MWBase::World* world = MWBase::Environment::get().getWorld();

            // if we can't drop the object at the wanted position, show the "drop on ground" cursor.
            bool canDrop = world->canPlaceObject(mouseX, mouseY);

            if (!canDrop)
                MWBase::Environment::get().getWindowManager()->changePointer("drop_ground");
            else
                MWBase::Environment::get().getWindowManager()->changePointer("arrow");
        }
        else
        {
            MWBase::Environment::get().getWindowManager()->changePointer("arrow");
            mWorldMouseOver = true;
        }
    }

    void HUD::onWorldMouseLostFocus(MyGUI::Widget* /*sender*/, MyGUI::Widget* newWidget)
    {
        MWBase::Environment::get().getWindowManager()->changePointer("arrow");
        mWorldMouseOver = false;
    }

    void HUD::onHMSClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Stats);
    }

    void HUD::onMapClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Map);
    }

    void HUD::onWeaponClicked(MyGUI::Widget* /*sender*/)
    {
        const MWWorld::Ptr& player = MWMechanics::getPlayer();
        if (player.getClass().getNpcStats(player).isWerewolf())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sWerewolfRefusal}");
            return;
        }

        MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Inventory);
    }

    void HUD::onMagicClicked(MyGUI::Widget* /*sender*/)
    {
        const MWWorld::Ptr& player = MWMechanics::getPlayer();
        if (player.getClass().getNpcStats(player).isWerewolf())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sWerewolfRefusal}");
            return;
        }

        MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Magic);
    }

    void HUD::setCellName(const std::string& cellName)
    {
        if (mCellName != cellName)
        {
            mCellNameTimer = 5.0f;
            mCellName = cellName;

            mCellNameBox->setCaptionWithReplacing("#{sCell=" + mCellName + "}");
            mCellNameBox->setVisible(mMapVisible);
        }
    }

    void HUD::setPlayerDir(float x, float y)
    {
        LocalMapBase::setPlayerDir(x, y);

        if (!VR::getVR() || !hasFalloutContent() || mCompassHeading == nullptr)
            return;

        static constexpr const char* headings[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        constexpr float pi = 3.14159265358979323846f;
        const float angle = std::atan2(x, y);
        int index = static_cast<int>(std::floor((angle + pi / 8.f) / (pi / 4.f)));
        index = (index % 8 + 8) % 8;
        mCompassHeading->setCaption(headings[index]);
        mCompassHeading->setVisible(true);
    }

    void HUD::onFrame(float dt)
    {
        LocalMapBase::onFrame(dt);

        if (VR::getVR() && hasFalloutContent())
        {
            if (!MWBase::Environment::get().getWorld()->getPlayerPtr().isEmpty())
            {
                const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
                MyGUI::TextBox* ammoText;
                getWidget(ammoText, "ArmorValue");
                MWWorld::InventoryStore& inv = player.getClass().getInventoryStore(player);
                MWWorld::ContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                ammoText->setCaption(ammo != inv.end()
                        ? "AMMO " + MyGUI::utility::toString(ammo->getCellRef().getCount())
                        : "AMMO --");
            }

            static int loggedFalloutVrHudFrames = 0;
            if (loggedFalloutVrHudFrames < 16)
            {
                ++loggedFalloutVrHudFrames;
                MyGUI::Widget *healthFrame, *magickaFrame, *fatigueFrame;
                getWidget(healthFrame, "HealthFrame");
                getWidget(magickaFrame, "MagickaFrame");
                getWidget(fatigueFrame, "FatigueFrame");
                Log(Debug::Info) << "FNV/ESM4 diag: wrist HUD widgets healthFrame=" << healthFrame->getVisible()
                                 << " health=" << mHealth->getVisible()
                                 << " apFrame=" << magickaFrame->getVisible()
                                 << " ap=" << mMagicka->getVisible()
                                 << " staminaFrame=" << fatigueFrame->getVisible()
                                 << " stamina=" << mStamina->getVisible()
                                 << " minimapBox=" << mMinimapBox->getVisible()
                                 << " mapVisible=" << mMapVisible
                                 << " cellName=" << mCellNameBox->getVisible();
            }
        }

        mCellNameTimer -= dt;
        mWeaponSpellTimer -= dt;
        if (mCellNameTimer < 0)
            mCellNameBox->setVisible(false);
        if (mWeaponSpellTimer < 0)
            mWeaponSpellBox->setVisible(false);

        mEnemyHealthTimer -= dt;
        if (mEnemyHealth->getVisible() && mEnemyHealthTimer < 0)
        {
            mEnemyHealth->setVisible(false);
            mWeaponSpellBox->setPosition(mWeaponSpellBox->getPosition() + MyGUI::IntPoint(0, 20));
        }

        mSpellIcons->updateWidgets(mEffectBox, true);

        if (mEnemyActorId != -1 && mEnemyHealth->getVisible())
        {
            updateEnemyHealthBar();
        }

        if (mDrowningBar->getVisible())
            mDrowningBar->setPosition(
                mMainWidget->getWidth() / 2 - mDrowningFrame->getWidth() / 2, mMainWidget->getTop());

        if (mIsDrowning)
        {
            mDrowningFlashTheta += dt * osg::PI * 2;

            float intensity = (cos(mDrowningFlashTheta) + 2.0f) / 3.0f;

            mDrowningFlash->setAlpha(intensity);
        }
    }

    void HUD::setSelectedSpell(const ESM::RefId& spellId, int successChancePercent)
    {
        const ESM::Spell* spell = MWBase::Environment::get().getESMStore()->get<ESM::Spell>().find(spellId);

        const std::string& spellName = spell->mName;
        if (spellName != mSpellName && mSpellVisible)
        {
            mWeaponSpellTimer = 5.0f;
            mSpellName = spellName;
            mWeaponSpellBox->setCaption(mSpellName);
            mWeaponSpellBox->setVisible(true);
        }

        mSpellStatus->setProgressRange(100);
        mSpellStatus->setProgressPosition(successChancePercent);

        mSpellBox->setUserString("ToolTipType", "Spell");
        mSpellBox->setUserString("Spell", spellId.serialize());
        mSpellBox->setUserData(MyGUI::Any::Null);

        if (!spell->mEffects.mList.empty())
        {
            // use the icon of the first effect
            const ESM::MagicEffect* effect = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(
                spell->mEffects.mList.front().mData.mEffectID);
            std::string icon = effect->mIcon;
            std::replace(icon.begin(), icon.end(), '/', '\\');
            size_t slashPos = icon.rfind('\\');
            icon.insert(slashPos + 1, "b_");
            icon = Misc::ResourceHelpers::correctIconPath(
                icon, MWBase::Environment::get().getResourceSystem()->getVFS());
            mSpellImage->setSpellIcon(icon);
        }
        else
            mSpellImage->setSpellIcon({});
    }

    void HUD::setSelectedEnchantItem(const MWWorld::Ptr& item, int chargePercent)
    {
        std::string_view itemName = item.getClass().getName(item);
        if (itemName != mSpellName && mSpellVisible)
        {
            mWeaponSpellTimer = 5.0f;
            mSpellName = itemName;
            mWeaponSpellBox->setCaption(mSpellName);
            mWeaponSpellBox->setVisible(true);
        }

        mSpellStatus->setProgressRange(100);
        mSpellStatus->setProgressPosition(chargePercent);

        mSpellBox->setUserString("ToolTipType", "ItemPtr");
        mSpellBox->setUserData(MWWorld::Ptr(item));

        mSpellImage->setItem(item);
    }

    void HUD::setSelectedWeapon(const MWWorld::Ptr& item, int durabilityPercent)
    {
        std::string_view itemName = item.getClass().getName(item);
        if (itemName != mWeaponName && mWeaponVisible)
        {
            mWeaponSpellTimer = 5.0f;
            mWeaponName = itemName;
            mWeaponSpellBox->setCaption(mWeaponName);
            mWeaponSpellBox->setVisible(true);
        }

        mWeapBox->clearUserStrings();
        mWeapBox->setUserString("ToolTipType", "ItemPtr");
        mWeapBox->setUserData(MWWorld::Ptr(item));

        mWeapStatus->setProgressRange(100);
        mWeapStatus->setProgressPosition(durabilityPercent);

        mWeapImage->setItem(item);
    }

    void HUD::unsetSelectedSpell()
    {
        std::string_view spellName = "#{Interface:None}";
        if (spellName != mSpellName && mSpellVisible)
        {
            mWeaponSpellTimer = 5.0f;
            mSpellName = spellName;
            mWeaponSpellBox->setCaptionWithReplacing(mSpellName);
            mWeaponSpellBox->setVisible(true);
        }

        mSpellStatus->setProgressRange(100);
        mSpellStatus->setProgressPosition(0);
        mSpellImage->setItem(MWWorld::Ptr());
        mSpellBox->clearUserStrings();
        mSpellBox->setUserData(MyGUI::Any::Null);
    }

    void HUD::unsetSelectedWeapon()
    {
        std::string itemName = "#{sSkillHandtohand}";
        if (itemName != mWeaponName && mWeaponVisible)
        {
            mWeaponSpellTimer = 5.0f;
            mWeaponName = itemName;
            mWeaponSpellBox->setCaptionWithReplacing(mWeaponName);
            mWeaponSpellBox->setVisible(true);
        }

        mWeapStatus->setProgressRange(100);
        mWeapStatus->setProgressPosition(0);

        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr player = world->getPlayerPtr();

        mWeapImage->setItem(MWWorld::Ptr());
        std::string icon = (player.getClass().getNpcStats(player).isWerewolf()) ? "icons\\k\\tx_werewolf_hand.dds"
                                                                                : "icons\\k\\stealth_handtohand.dds";
        mWeapImage->setIcon(icon);

        mWeapBox->clearUserStrings();
        mWeapBox->setUserString("ToolTipType", "Layout");
        mWeapBox->setUserString("ToolTipLayout", "HandToHandToolTip");
        mWeapBox->setUserString("Caption_HandToHandText", itemName);
        mWeapBox->setUserString("ImageTexture_HandToHandImage", icon);
        mWeapBox->setUserData(MyGUI::Any::Null);
    }

    void HUD::setCrosshairVisible(bool visible)
    {
        mCrosshair->setVisible(visible);
    }

    void HUD::setCrosshairOwned(bool owned)
    {
        if (owned)
        {
            mCrosshair->changeWidgetSkin("HUD_Crosshair_Owned");
        }
        else
        {
            mCrosshair->changeWidgetSkin("HUD_Crosshair");
        }
    }

    void HUD::setHmsVisible(bool visible)
    {
        MyGUI::Widget *healthFrame, *magickaFrame, *fatigueFrame;
        getWidget(healthFrame, "HealthFrame");
        getWidget(magickaFrame, "MagickaFrame");
        getWidget(fatigueFrame, "FatigueFrame");
        if ((VR::getVR() || hasFalloutContent()) && !visible)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log(Debug::Info) << "FNV/ESM4 diag: keeping Fallout HUD health/AP visible despite hide request";
            }
            visible = true;
        }
        const bool falloutVr = VR::getVR() && hasFalloutContent();
        healthFrame->setVisible(visible);
        magickaFrame->setVisible(visible);
        fatigueFrame->setVisible(falloutVr ? false : visible);
        mHealth->setVisible(visible);
        mMagicka->setVisible(visible);
        mStamina->setVisible(falloutVr ? false : visible);
        updatePositions();
    }

    void HUD::setWeapVisible(bool visible)
    {
        mWeapBox->setVisible(visible);
        updatePositions();
    }

    void HUD::setSpellVisible(bool visible)
    {
        mSpellBox->setVisible(visible);
        updatePositions();
    }

    void HUD::setSneakVisible(bool visible)
    {
        mSneakBox->setVisible(visible);
        updatePositions();
    }

    void HUD::setEffectVisible(bool visible)
    {
        mEffectBox->setVisible(visible);
        updatePositions();
    }

    void HUD::setMinimapVisible(bool visible)
    {
        if ((VR::getVR() || hasFalloutContent()) && !visible)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log(Debug::Info) << "FNV/ESM4 diag: keeping Fallout HUD compass/minimap visible despite hide request";
            }
            visible = true;
        }
        mMinimapBox->setVisible(visible);
        updatePositions();
    }

    void HUD::updatePositions()
    {
        int weapDx = 0, spellDx = 0, sneakDx = 0;
        if (!mHealth->getVisible())
            sneakDx = spellDx = weapDx = mWeapBoxBaseLeft - mHealthManaStaminaBaseLeft;

        if (!mWeapBox->getVisible())
        {
            spellDx += mSpellBoxBaseLeft - mWeapBoxBaseLeft;
            sneakDx = spellDx;
        }

        if (!mSpellBox->getVisible())
            sneakDx += mSneakBoxBaseLeft - mSpellBoxBaseLeft;

        mWeaponVisible = mWeapBox->getVisible();
        mSpellVisible = mSpellBox->getVisible();
        if (!mWeaponVisible && !mSpellVisible)
            mWeaponSpellBox->setVisible(false);

        mWeapBox->setPosition(mWeapBoxBaseLeft - weapDx, mWeapBox->getTop());
        mSpellBox->setPosition(mSpellBoxBaseLeft - spellDx, mSpellBox->getTop());
        mSneakBox->setPosition(mSneakBoxBaseLeft - sneakDx, mSneakBox->getTop());

//## VR_PATCH BEGIN
// Hud size shouldn't depend on window size in VR
        if(VR::getVR())
        {
            // in VR mode, the effect box grows to the right and does not need repositioning
            // VR-TODO: If the user by preference attaches the HUD to the right instead, it should grow to the left again
            int width = std::max(mMainWidgetBaseSize.width, mEffectBox->getSize().width);
            mMainWidget->setSize(width, mMainWidget->getHeight());
            mMapVisible = mMinimapBox->getVisible();
        }
        else
        {
            const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            // effect box can have variable width -> variable left coordinate
            int effectsDx = 0;
            if (!mMinimapBox->getVisible())
                effectsDx = mEffectBoxBaseRight - mMinimapBoxBaseRight;
    
            mMapVisible = mMinimapBox->getVisible();
            if (!mMapVisible)
                mCellNameBox->setVisible(false);
    
            mEffectBox->setPosition(
                (viewSize.width - mEffectBoxBaseRight) - mEffectBox->getWidth() + effectsDx, mEffectBox->getTop());
        }
    }

    void HUD::updateEnemyHealthBar()
    {
        MWWorld::Ptr enemy = MWBase::Environment::get().getWorld()->searchPtrViaActorId(mEnemyActorId);
        if (enemy.isEmpty())
            return;
        MWMechanics::CreatureStats& stats = enemy.getClass().getCreatureStats(enemy);
        mEnemyHealth->setProgressRange(100);
        // Health is usually cast to int before displaying. Actors die whenever they are < 1 health.
        // Therefore any value < 1 should show as an empty health bar. We do the same in statswindow :)
        mEnemyHealth->setProgressPosition(static_cast<size_t>(stats.getHealth().getRatio() * 100));

        static const float fNPCHealthBarFade = MWBase::Environment::get()
                                                   .getESMStore()
                                                   ->get<ESM::GameSetting>()
                                                   .find("fNPCHealthBarFade")
                                                   ->mValue.getFloat();
        if (fNPCHealthBarFade > 0.f)
            mEnemyHealth->setAlpha(std::clamp(mEnemyHealthTimer / fNPCHealthBarFade, 0.f, 1.f));
    }

    void HUD::setEnemy(const MWWorld::Ptr& enemy)
    {
        mEnemyActorId = enemy.getClass().getCreatureStats(enemy).getActorId();
        mEnemyHealthTimer = MWBase::Environment::get()
                                .getESMStore()
                                ->get<ESM::GameSetting>()
                                .find("fNPCHealthBarTime")
                                ->mValue.getFloat();
        if (!mEnemyHealth->getVisible())
            mWeaponSpellBox->setPosition(mWeaponSpellBox->getPosition() - MyGUI::IntPoint(0, 20));
        mEnemyHealth->setVisible(true);
        updateEnemyHealthBar();
    }

    void HUD::clear()
    {
        mEnemyActorId = -1;
        mEnemyHealthTimer = -1;

        mWeaponSpellTimer = 0.f;
        mWeaponName = std::string();
        mSpellName = std::string();
        mWeaponSpellBox->setVisible(false);

        mWeapStatus->setProgressRange(100);
        mWeapStatus->setProgressPosition(0);
        mSpellStatus->setProgressRange(100);
        mSpellStatus->setProgressPosition(0);

        mWeapImage->setItem(MWWorld::Ptr());
        mSpellImage->setItem(MWWorld::Ptr());

        mWeapBox->clearUserStrings();
        mWeapBox->setUserData(MyGUI::Any::Null);
        mSpellBox->clearUserStrings();
        mSpellBox->setUserData(MyGUI::Any::Null);

        mActiveCell = nullptr;
        mHasALastActiveCell = false;
    }

    void HUD::customMarkerCreated(MyGUI::Widget* marker)
    {
        marker->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onMapClicked);
    }

    void HUD::doorMarkerCreated(MyGUI::Widget* marker)
    {
        marker->eventMouseButtonClick += MyGUI::newDelegate(this, &HUD::onMapClicked);
    }

}
