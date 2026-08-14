#ifndef MWGUI_SPELLWINDOW_H
#define MWGUI_SPELLWINDOW_H

#include <memory>
<<<<<<< HEAD

#include "spellicons.hpp"
#include "spellmodel.hpp"
#include "windowpinnablebase.hpp"
=======
#include <vector>

#include <components/esm/formid.hpp>

#include "falloutquestlistpolicy.hpp"
#include "spellicons.hpp"
#include "spellmodel.hpp"
#include "windowpinnablebase.hpp"

namespace Gui
{
    class MWList;
}
>>>>>>> origin/main

namespace MWGui
{
    class SpellView;

    class SpellWindow : public WindowPinnableBase, public NoDrop
    {
    public:
        SpellWindow(DragAndDrop* drag);

        void updateSpells();

        void onFrame(float dt) override;

        /// Cycle to next/previous spell
        void cycle(bool next);

        std::string_view getWindowIdForLua() const override { return "Magic"; }

    protected:
        MyGUI::Widget* mEffectBox;

        ESM::RefId mSpellToDelete;

        void onEnchantedItemSelected(MWWorld::Ptr item, bool alreadyEquipped);
        void onSpellSelected(const ESM::RefId& spellId);
        void onModelIndexSelected(SpellModel::ModelIndex index);
        void onFilterChanged(MyGUI::EditBox* sender);
        void onDeleteClicked(MyGUI::Widget* widget);
        void onDeleteSpellAccept();
<<<<<<< HEAD
        void askDeleteSpell(const ESM::RefId& spellId);
=======
        void onFalloutQuestSelected(const std::string& name, int index);
        void askDeleteSpell(const ESM::RefId& spellId);
        bool isFalloutDataMode() const;
        void updateFalloutQuestList();
        void setFalloutQuestSelection(std::size_t index);
>>>>>>> origin/main

        void onPinToggled() override;
        void onTitleDoubleClicked() override;
        void onOpen() override;
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        void setActiveControllerWindow(bool active) override;

        SpellView* mSpellView;
        std::unique_ptr<SpellIcons> mSpellIcons;
        MyGUI::EditBox* mFilterEdit;
<<<<<<< HEAD
=======
        MyGUI::Widget* mDeleteButton;
        Gui::MWList* mFalloutQuestList;
        std::vector<FalloutQuestListRow> mFalloutQuestRows;
        std::size_t mFalloutSelectedQuestRow = 0;
>>>>>>> origin/main

    private:
        float mUpdateTimer;
    };
}

#endif
