#ifndef OPENMW_GAME_MWGUI_HUD_H
#define OPENMW_GAME_MWGUI_HUD_H

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "mapwindow.hpp"
#include "spellicons.hpp"
#include "statswatcher.hpp"

namespace MWWorld
{
    class Ptr;
}

namespace MWGui
{
    class DragAndDrop;
    class ItemWidget;
    class SpellWidget;

    struct FalloutVatsBodyPartDisplay
    {
        std::string_view mName;
        unsigned int mHitChance = 0;
        float mViewportX = 0.5f;
        float mViewportY = 0.5f;
        bool mSelected = false;
    };

    class HUD : public WindowBase, public LocalMapBase, public StatsListener
    {
    public:
        HUD(CustomMarkerCollection& customMarkers, DragAndDrop* dragAndDrop, MWRender::LocalMap* localMapRender);
        virtual ~HUD();
        void setValue(std::string_view id, const MWMechanics::DynamicStat<float>& value) override;

        /// Set time left for the player to start drowning
        /// @param time time left to start drowning
        /// @param maxTime how long we can be underwater (in total) until drowning starts
        void setDrowningTimeLeft(float time, float maxTime);
        void setDrowningBarVisible(bool visible);

        void setHmsVisible(bool visible);
        void setWeapVisible(bool visible);
        void setSpellVisible(bool visible);
        void setSneakVisible(bool visible);

        void setEffectVisible(bool visible);
        void setMinimapVisible(bool visible);

        void setSelectedSpell(const ESM::RefId& spellId, int successChancePercent);
        void setSelectedEnchantItem(const MWWorld::Ptr& item, int chargePercent);
        const MWWorld::Ptr& getSelectedEnchantItem();
        void setSelectedWeapon(const MWWorld::Ptr& item, int durabilityPercent);
        void unsetSelectedSpell();
        void unsetSelectedWeapon();

        void setCrosshairVisible(bool visible);
        void setCrosshairOwned(bool owned);

        void onFrame(float dt) override;

        void setCellName(const std::string& cellName);
        void setPlayerDir(float x, float y);

        bool getWorldMouseOver() { return mWorldMouseOver; }

        MyGUI::Widget* getEffectBox() { return mEffectBox; }

        void setEnemy(const MWWorld::Ptr& enemy);
        void setFalloutVatsVisible(bool visible, std::string_view targetName = {},
            std::span<const FalloutVatsBodyPartDisplay> bodyParts = {},
            float actionPointsBefore = 0.f, float actionPointsAfter = 0.f,
            std::size_t queuedAttacks = 0, std::size_t availableShots = 0, bool executing = false);

        void clear() override;

        void dropDraggedItem(float mouseX, float mouseY);

    private:
        DragAndDrop* mDragAndDrop;
        MyGUI::ProgressBar *mHealth, *mMagicka, *mStamina, *mEnemyHealth, *mDrowning;
        MyGUI::Widget* mHealthFrame;
        MyGUI::Widget *mWeapBox, *mSpellBox, *mSneakBox;
        ItemWidget* mWeapImage;
        SpellWidget* mSpellImage;
        MyGUI::ProgressBar *mWeapStatus, *mSpellStatus;
        MyGUI::Widget *mEffectBox, *mMinimapBox;
        MyGUI::Button* mMinimapButton;
        MyGUI::ScrollView* mMinimap;
        MyGUI::ImageBox* mCrosshair;
        MyGUI::TextBox* mCellNameBox;
        MyGUI::TextBox* mWeaponSpellBox;
        MyGUI::TextBox* mCompassHeading;
        MyGUI::Widget* mFalloutVatsOverlay = nullptr;
        MyGUI::TextBox* mFalloutVatsTarget = nullptr;
        MyGUI::TextBox* mFalloutVatsActionPoints = nullptr;
        MyGUI::TextBox* mFalloutVatsInstructions = nullptr;
        struct FalloutVatsBodyPartWidgets
        {
            MyGUI::Widget* mFrame = nullptr;
            MyGUI::TextBox* mText = nullptr;
        };
        std::vector<FalloutVatsBodyPartWidgets> mFalloutVatsBodyPartWidgets;
        MyGUI::Widget *mDrowningBar, *mDrowningFrame, *mDrowningFlash;

        std::string mCellName;
        std::string mWeaponName;
        std::string mSpellName;
        std::unique_ptr<SpellIcons> mSpellIcons;
        ESM::RefNum mEnemyActor;

        // bottom left elements
        int mHealthManaStaminaBaseLeft, mWeapBoxBaseLeft, mSpellBoxBaseLeft, mSneakBoxBaseLeft;
        // bottom right elements
        int mMinimapBoxBaseRight, mEffectBoxBaseRight;

        float mCellNameTimer = 0.f;
        float mWeaponSpellTimer = 0.f;
        float mEnemyHealthTimer = -1;
        float mDrowningFlashTheta = 0.f;

        bool mMapVisible = true;
        bool mWeaponVisible = true;
        bool mSpellVisible = true;
        bool mWorldMouseOver = false;
        bool mIsDrowning = false;

        void onWorldClicked(MyGUI::Widget* sender);
        void onWorldMouseOver(MyGUI::Widget* sender, int x, int y);
        void onWorldMouseLostFocus(MyGUI::Widget* sender, MyGUI::Widget* newWidget);
        void onHMSClicked(MyGUI::Widget* sender);
        void onWeaponClicked(MyGUI::Widget* sender);
        void onMagicClicked(MyGUI::Widget* sender);
        void onMapClicked(MyGUI::Widget* sender);

        // LocalMapBase
        void customMarkerCreated(MyGUI::Widget* marker) override;
        void doorMarkerCreated(MyGUI::Widget* marker) override;

        void updateEnemyHealthBar();

        void updatePositions();
    };
}

#endif
