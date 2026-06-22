#include "hud.hpp"

#include <cmath>
#include <cstdlib>

#include <MyGUI_Button.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_ProgressBar.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_TextBox.h>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>
#include <components/vr/vr.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/worldmodel.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "draganddrop.hpp"
#include "inventorywindow.hpp"
#include "itemwidget.hpp"
#include "spellicons.hpp"
#include "worlditemmodel.hpp"

namespace MWGui
{
    namespace
    {
        bool hasFalloutContent()
        {
            if (std::getenv("OPENMW_FNV_PROOF_PIPBOY_SURFACE") != nullptr)
                return true;

            const MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return false;

            for (const std::string& file : world->getContentFiles())
                if (Misc::StringUtils::ciEndsWith(file, "FalloutNV.esm"))
                    return true;

            return false;
        }

        bool isFalloutAmmoLike(const MWWorld::ConstPtr& item)
        {
            if (item.getType() == ESM4::Ammunition::sRecordId)
                return true;

            const std::string id = Misc::StringUtils::lowerCase(item.getCellRef().getRefId().getRefIdString());
            return id.find("ammo") != std::string::npos || id.find("9mm") != std::string::npos
                || id.find("round") != std::string::npos;
        }

        int countFalloutAmmo(const MWWorld::Ptr& player)
        {
            int count = 0;
            const MWWorld::ContainerStore& store = player.getClass().getContainerStore(player);
            for (MWWorld::ConstContainerStoreIterator it = store.begin(); it != store.end(); ++it)
            {
                const MWWorld::ConstPtr item = *it;
                if (isFalloutAmmoLike(item))
                    count += item.getCellRef().getCount();
            }
            return count;
        }

        const char* headingFromRotation(float rotZ)
        {
            constexpr float radiansToDegrees = 57.29577951308232f;
            float degrees = std::fmod(-rotZ * radiansToDegrees + 360.f, 360.f);
            if (degrees < 0.f)
                degrees += 360.f;

            static constexpr const char* headings[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            return headings[static_cast<int>(std::floor((degrees + 22.5f) / 45.f)) % 8];
        }
    }

    HUD::HUD(CustomMarkerCollection& customMarkers, DragAndDrop* dragAndDrop, MWRender::LocalMap* localMapRender)
        : WindowBase("openmw_hud.layout")
        , LocalMapBase(customMarkers, localMapRender, Settings::map().mLocalMapHudFogOfWar)
        , mDragAndDrop(dragAndDrop)
    {
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

        const bool falloutContent = hasFalloutContent();
        if (falloutContent)
        {
            mHealth->changeWidgetSkin("MW_EnergyBar_Green");
            mHealth->setColour(MyGUI::Colour(0.25f, 1.f, 0.25f, 1.f));
            mMagicka->changeWidgetSkin("MW_EnergyBar_Green");
            mMagicka->setColour(MyGUI::Colour(0.25f, 1.f, 0.25f, 1.f));
            mStamina->changeWidgetSkin("MW_EnergyBar_Green");
            fatigueFrame->setVisible(false);
            mStamina->setVisible(false);
            Log(Debug::Info) << "FNV/ESM4 proof: HUD Fallout bars applied HP/AP green theme; fatigue hidden";
        }

        // Drowning bar
        getWidget(mDrowningBar, "DrowningBar");
        getWidget(mDrowningFrame, "DrowningFrame");
        getWidget(mDrowning, "Drowning");
        getWidget(mDrowningFlash, "Flash");
        mDrowning->setProgressRange(200);

        const MyGUI::IntSize viewSize
            = VR::getVR() ? mMainWidget->getSize() : MyGUI::RenderManager::getInstance().getViewSize();

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
        getWidget(mHealthLabel, "HealthLabel");
        getWidget(mHealthValue, "HealthValue");
        getWidget(mMagickaLabel, "MagickaLabel");
        getWidget(mMagickaValue, "MagickaValue");
        getWidget(mAmmoValue, "AmmoValue");
        getWidget(mCompassHeading, "CompassHeading");
        mAmmoValue->setVisible(falloutContent);
        mCompassHeading->setVisible(falloutContent);

        getWidget(mCrosshair, "Crosshair");

        LocalMapBase::init(mMinimap, mCompass);

        if (falloutContent)
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
            mHealthLabel->setCoord(margin, barTop - 1, 28, barHeight + 2);
            mHealthFrame->setCoord(margin + 30, barTop, barWidth, barHeight);
            mHealth->setCoord(0, 0, barWidth, barHeight);
            mHealthValue->setCoord(margin + 30 + barWidth + 8, barTop - 1, 82, barHeight + 2);
            mMagickaLabel->setCoord(margin, barTop + barHeight + barGap - 1, 28, barHeight + 2);
            magickaFrame->setCoord(margin + 30, barTop + barHeight + barGap, barWidth, barHeight);
            mMagicka->setCoord(0, 0, barWidth, barHeight);
            mMagickaValue->setCoord(margin + 30 + barWidth + 8, barTop + barHeight + barGap - 1, 82, barHeight + 2);
            mAmmoValue->setCoord(margin + 30 + barWidth + 96, barTop - 1, 110, barHeight + 2);

            const int iconTop = viewSize.height - margin - iconSize - iconStatusHeight;
            mWeapBox->setCoord(margin + barWidth + 250, iconTop, iconSize, iconSize + iconStatusHeight);
            mSpellBox->setCoord(
                margin + barWidth + 250 + iconSize + 8, iconTop, iconSize, iconSize + iconStatusHeight);
            mSneakBox->setCoord(margin + barWidth + 250 + (iconSize + 8) * 2, iconTop, iconSize, iconSize);
            mWeapBox->setVisible(false);
            mSpellBox->setVisible(false);

            mMinimapBox->setCoord(
                viewSize.width - margin - mapSize, viewSize.height - margin - mapSize, mapSize, mapSize);
            if (mMinimapBox->getChildCount() > 0)
                mMinimapBox->getChildAt(0)->setCoord(0, 0, mapSize, mapSize);
            mMinimap->setCoord(mapInset, mapInset, mapSize - mapInset * 2, mapSize - mapInset * 2);
            mCompass->setCoord((mapSize - 72) / 2, (mapSize - 72) / 2, 72, 72);
            mCompass->setVisible(false);
            mCompassHeading->setCoord((mapSize - 72) / 2, (mapSize - 24) / 2, 72, 24);
            mMinimapButton->setCoord(0, 0, mapSize - mapInset * 2, mapSize - mapInset * 2);

            mEffectBox->setPosition(viewSize.width - margin - mapSize - 28, viewSize.height - margin - 24);

            mHealthManaStaminaBaseLeft = mHealthFrame->getLeft();
            mWeapBoxBaseLeft = mWeapBox->getLeft();
            mSpellBoxBaseLeft = mSpellBox->getLeft();
            mSneakBoxBaseLeft = mSneakBox->getLeft();
            mMinimapBoxBaseRight = viewSize.width - mMinimapBox->getRight();
            mEffectBoxBaseRight = viewSize.width - mEffectBox->getRight();
            Log(Debug::Info) << "FNV/ESM4 proof: flat Fallout HUD scaled HP/AP and compass to " << barWidth << "x"
                             << barHeight << " bars, " << mapSize << "x" << mapSize << " minimap";
            Log(Debug::Info)
                << "FNV/ESM4 proof: flat Fallout HUD readouts active HP/AP/AMMO/text compass";
            Log(Debug::Info)
                << "FNV/ESM4 proof: flat Fallout HUD hides unbacked Morrowind weapon/spell icon slots";
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
            mHealthValue->setCaption(MyGUI::utility::toString(current) + "/" + MyGUI::utility::toString(modified));
            getWidget(w, "HealthFrame");
            w->setUserString("Caption_HealthDescription", "#{sHealthDesc}\n" + valStr);
        }
        else if (id == "MBar")
        {
            mMagicka->setProgressRange(std::max(0, modified));
            mMagicka->setProgressPosition(std::max(0, current));
            mMagickaValue->setCaption(MyGUI::utility::toString(current) + "/" + MyGUI::utility::toString(modified));
            getWidget(w, "MagickaFrame");
            w->setUserString("Caption_HealthDescription", "#{sMagDesc}\n" + valStr);
        }
        else if (id == "FBar")
        {
            mStamina->setProgressRange(std::max(0, modified));
            mStamina->setProgressPosition(std::max(0, current));
            getWidget(w, "FatigueFrame");
            w->setUserString("Caption_HealthDescription", "#{sFatDesc}\n" + valStr);
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
        if (hasFalloutContent())
            visible = false;
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

            MWWorld::Ptr object = MWBase::Environment::get().getWorld()->getFocusObject();

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

    void HUD::onFrame(float dt)
    {
        LocalMapBase::onFrame(dt);

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

        if (mEnemyActor.isSet() && mEnemyHealth->getVisible())
        {
            updateEnemyHealthBar();
        }

        if (mDrowningBar->getVisible())
            mDrowningBar->setPosition(
                mMainWidget->getWidth() / 2 - mDrowningFrame->getWidth() / 2, mMainWidget->getTop());

        if (mIsDrowning)
        {
            mDrowningFlashTheta += dt * osg::PIf * 2;

            float intensity = (cos(mDrowningFlashTheta) + 2.0f) / 3.0f;

            mDrowningFlash->setAlpha(intensity);
        }

        if (hasFalloutContent())
        {
            const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
            const int ammoCount = countFalloutAmmo(player);
            mAmmoValue->setCaption(ammoCount > 0 ? "AMMO " + MyGUI::utility::toString(ammoCount) : "AMMO --");
            mCompassHeading->setCaption(headingFromRotation(player.getRefData().getPosition().rot[2]));
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
            const VFS::Path::Normalized iconPath = Misc::ResourceHelpers::correctBigIconPath(
                VFS::Path::toNormalized(effect->mIcon), *MWBase::Environment::get().getResourceSystem()->getVFS());
            mSpellImage->setSpellIcon(iconPath);
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
        if (itemName != mWeaponName && (mWeaponVisible || hasFalloutContent()))
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
        healthFrame->setVisible(visible);
        magickaFrame->setVisible(visible);
        fatigueFrame->setVisible(hasFalloutContent() ? false : visible);
        mHealth->setVisible(visible);
        mMagicka->setVisible(visible);
        mStamina->setVisible(hasFalloutContent() ? false : visible);
        mHealthLabel->setVisible(visible);
        mHealthValue->setVisible(visible);
        mMagickaLabel->setVisible(visible);
        mMagickaValue->setVisible(visible);
        mAmmoValue->setVisible(hasFalloutContent() && visible);
        updatePositions();
    }

    void HUD::setWeapVisible(bool visible)
    {
        if (hasFalloutContent())
            visible = false;
        mWeapBox->setVisible(visible);
        updatePositions();
    }

    void HUD::setSpellVisible(bool visible)
    {
        if (hasFalloutContent())
            visible = false;
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
        if (hasFalloutContent() && !visible && MWBase::Environment::get().getWindowManager()->getMode() == GM_None)
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                Log(Debug::Info) << "FNV/ESM4 proof: keeping flat Fallout compass/minimap visible";
            }
            visible = true;
        }
        mMinimapBox->setVisible(visible);
        mCompassHeading->setVisible(visible && hasFalloutContent());
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

    void HUD::updateEnemyHealthBar()
    {
        MWWorld::Ptr enemy = MWBase::Environment::get().getWorldModel()->getPtr(mEnemyActor);
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
        mEnemyActor = enemy.getCellRef().getRefNum();
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
        mEnemyActor = {};
        mEnemyHealthTimer = -1;

        mWeaponSpellTimer = 0.f;
        mWeaponName.clear();
        mSpellName.clear();
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
