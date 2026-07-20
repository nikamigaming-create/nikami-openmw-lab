#ifndef MWGUI_SPELLWINDOW_H
#define MWGUI_SPELLWINDOW_H

#include <memory>
#include <vector>

#include <components/esm/formid.hpp>

#include "spellicons.hpp"
#include "spellmodel.hpp"
#include "windowpinnablebase.hpp"

namespace Gui
{
    class MWList;
}

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
        void onFalloutQuestSelected(const std::string& name, int index);
        void askDeleteSpell(const ESM::RefId& spellId);
        bool isFalloutDataMode() const;
        void updateFalloutQuestList();
        void setFalloutQuestSelection(std::size_t index);

        void onPinToggled() override;
        void onTitleDoubleClicked() override;
        void onOpen() override;
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        void setActiveControllerWindow(bool active) override;

        SpellView* mSpellView;
        std::unique_ptr<SpellIcons> mSpellIcons;
        MyGUI::EditBox* mFilterEdit;
        MyGUI::Widget* mDeleteButton;
        Gui::MWList* mFalloutQuestList;
        std::vector<ESM::FormId> mFalloutQuestItems;
        std::size_t mFalloutSelectedQuest = 0;

    private:
        float mUpdateTimer;
    };
}

#endif
