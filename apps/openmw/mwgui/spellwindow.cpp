#include "spellwindow.hpp"

#include <algorithm>
#include <optional>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_Window.h>

#include <components/esm3/loadbsgn.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm4/loadqust.hpp>
#include <components/misc/strings/format.hpp>
#include <components/settings/values.hpp>
#include <components/widgets/list.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esm4questruntime.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/player.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/spells.hpp"
#include "../mwmechanics/spellutil.hpp"

#include "confirmationdialog.hpp"
#include "spellicons.hpp"
#include "spellview.hpp"
#include "statswindow.hpp"

//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
//## VR_PATCH END

namespace MWGui
{

    SpellWindow::SpellWindow(DragAndDrop* drag)
//## VR_PATCH BEGIN
        : WindowPinnableBase(VR::getVR() ? "openmw_spell_window_vr.layout" : "openmw_spell_window.layout")
//## VR_PATCH END
        , NoDrop(drag, mMainWidget)
        , mSpellView(nullptr)
        , mFilterEdit(nullptr)
        , mDeleteButton(nullptr)
        , mFalloutQuestList(nullptr)
        , mUpdateTimer(0.0f)
    {
        mSpellIcons = std::make_unique<SpellIcons>();

        getWidget(mDeleteButton, "DeleteSpellButton");

        getWidget(mSpellView, "SpellView");
        getWidget(mEffectBox, "EffectsBox");
        getWidget(mFilterEdit, "FilterEdit");
        getWidget(mFalloutQuestList, "FalloutQuestList");

        mSpellView->eventSpellClicked += MyGUI::newDelegate(this, &SpellWindow::onModelIndexSelected);
        mFilterEdit->eventEditTextChange += MyGUI::newDelegate(this, &SpellWindow::onFilterChanged);
        mDeleteButton->eventMouseButtonClick += MyGUI::newDelegate(this, &SpellWindow::onDeleteClicked);
        mFalloutQuestList->eventItemSelected += MyGUI::newDelegate(this, &SpellWindow::onFalloutQuestSelected);

        setCoord(498, 300, 302, 300);

        // Adjust the spell filtering widget size because of MyGUI limitations.
        int filterWidth = mSpellView->getSize().width - mDeleteButton->getSize().width - 3;
        mFilterEdit->setSize(filterWidth, mFilterEdit->getSize().height);

        if (Settings::gui().mControllerMenus)
        {
            setPinButtonVisible(false);
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB = "#{Interface:Back}";
            mControllerButtons.mR3 = "#{Interface:Info}";
        }
    }

    void SpellWindow::onPinToggled()
    {
        Settings::windows().mSpellsPin.set(mPinned);

        MWBase::Environment::get().getWindowManager()->setSpellVisibility(!mPinned);
    }

    void SpellWindow::onTitleDoubleClicked()
    {
        if (Settings::gui().mControllerMenus)
            return;
        else if (MyGUI::InputManager::getInstance().isShiftPressed())
            MWBase::Environment::get().getWindowManager()->toggleMaximized(this);
        else if (!mPinned)
            MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Magic);
    }

    void SpellWindow::onOpen()
    {
        const bool fallout = isFalloutDataMode();
        mFalloutQuestList->setVisible(fallout);
        mSpellView->setVisible(!fallout);
        mEffectBox->getParent()->setVisible(!fallout);
        mFilterEdit->setVisible(!fallout);
        mDeleteButton->getParent()->setVisible(!fallout);
        if (fallout)
        {
            setTitle("DATA / QUESTS");
            updateFalloutQuestList();
            return;
        }

        // Reset the filter focus when opening the window
        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        if (focus == mFilterEdit)
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(nullptr);

        updateSpells();
    }

    void SpellWindow::onFrame(float dt)
    {
        NoDrop::onFrame(dt);
        if (isFalloutDataMode())
            return;

        mUpdateTimer += dt;
        if (0.5f < mUpdateTimer)
        {
            mUpdateTimer = 0;
            mSpellView->incrementalUpdate();
        }

        // Update effects if the time is unpaused for any reason (e.g. the window is pinned)
        if (!MWBase::Environment::get().getWorld()->getTimeManager()->isPaused())
            mSpellIcons->updateWidgets(mEffectBox, false);
    }

    void SpellWindow::updateSpells()
    {
        if (isFalloutDataMode())
        {
            updateFalloutQuestList();
            return;
        }

        mSpellIcons->updateWidgets(mEffectBox, false);

        mSpellView->setModel(new SpellModel(MWMechanics::getPlayer(), mFilterEdit->getCaption()));
    }

    void SpellWindow::onEnchantedItemSelected(MWWorld::Ptr item, bool alreadyEquipped)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);

        // retrieve ContainerStoreIterator to the item
        MWWorld::ContainerStoreIterator it = store.begin();
        for (; it != store.end(); ++it)
        {
            if (*it == item)
            {
                break;
            }
        }
        if (it == store.end())
            throw std::runtime_error("can't find selected item");

        // equip, if it can be equipped and is not already equipped
        if (!alreadyEquipped && !item.getClass().getEquipmentSlots(item).first.empty())
        {
            MWBase::Environment::get().getWindowManager()->useItem(item);
            // make sure that item was successfully equipped
            if (!store.isEquipped(item))
                return;
        }

        store.setSelectedEnchantItem(it);
        // to reset WindowManager::mSelectedSpell immediately
        MWBase::Environment::get().getWindowManager()->setSelectedEnchantItem(*it);

        updateSpells();
    }

    void SpellWindow::askDeleteSpell(const ESM::RefId& spellId)
    {
        // delete spell, if allowed
        const ESM::Spell* spell = MWBase::Environment::get().getESMStore()->get<ESM::Spell>().find(spellId);

        MWWorld::Ptr player = MWMechanics::getPlayer();
        const ESM::RefId& raceId = player.get<ESM::NPC>()->mBase->mRace;
        const ESM::Race* race = MWBase::Environment::get().getESMStore()->get<ESM::Race>().find(raceId);
        // can't delete racial spells, birthsign spells or powers
        bool isInherent = race->mPowers.exists(spell->mId) || spell->mData.mType == ESM::Spell::ST_Power;
        const ESM::RefId& signId = MWBase::Environment::get().getWorld()->getPlayer().getBirthSign();
        if (!isInherent && !signId.empty())
        {
            const ESM::BirthSign* sign = MWBase::Environment::get().getESMStore()->get<ESM::BirthSign>().find(signId);
            isInherent = sign->mPowers.exists(spell->mId);
        }

        const auto windowManager = MWBase::Environment::get().getWindowManager();
        if (isInherent)
        {
            windowManager->messageBox("#{sDeleteSpellError}");
        }
        else
        {
            // ask for confirmation
            mSpellToDelete = spellId;
            ConfirmationDialog* dialog = windowManager->getConfirmationDialog();
            std::string question{ windowManager->getGameSettingString("sQuestionDeleteSpell", "Delete %s?") };
            question = Misc::StringUtils::format(question, spell->mName);
            dialog->askForConfirmation(question);
            dialog->eventOkClicked.clear();
            dialog->eventOkClicked += MyGUI::newDelegate(this, &SpellWindow::onDeleteSpellAccept);
            dialog->eventCancelClicked.clear();
        }
    }

    void SpellWindow::onModelIndexSelected(SpellModel::ModelIndex index)
    {
        if (isFalloutDataMode())
            return;

        const Spell& spell = mSpellView->getModel()->getItem(index);
        if (spell.mType == Spell::Type_EnchantedItem)
        {
            onEnchantedItemSelected(spell.mItem, spell.mActive);
        }
        else
        {
            if (MyGUI::InputManager::getInstance().isShiftPressed())
                askDeleteSpell(spell.mId);
            else
                onSpellSelected(spell.mId);
        }
    }

    void SpellWindow::onFilterChanged(MyGUI::EditBox* sender)
    {
        if (isFalloutDataMode())
            return;
        mSpellView->setModel(new SpellModel(MWMechanics::getPlayer(), sender->getCaption()));
    }

    void SpellWindow::onDeleteClicked(MyGUI::Widget* widget)
    {
        SpellModel::ModelIndex selected = mSpellView->getModel()->getSelectedIndex();
        if (selected < 0)
            return;

        const Spell& spell = mSpellView->getModel()->getItem(selected);
        if (spell.mType != Spell::Type_EnchantedItem)
            askDeleteSpell(spell.mId);
    }

    void SpellWindow::onSpellSelected(const ESM::RefId& spellId)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);
        store.setSelectedEnchantItem(store.end());
        MWBase::Environment::get().getWindowManager()->setSelectedSpell(
            spellId, int(MWMechanics::getSpellSuccessChance(spellId, player)));

        updateSpells();
    }

    void SpellWindow::onDeleteSpellAccept()
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        MWMechanics::Spells& spells = stats.getSpells();

        if (MWBase::Environment::get().getWindowManager()->getSelectedSpell() == mSpellToDelete)
            MWBase::Environment::get().getWindowManager()->unsetSelectedSpell();

        spells.remove(mSpellToDelete);

        updateSpells();
    }

    void SpellWindow::cycle(bool next)
    {
        if (isFalloutDataMode())
            return;

        MWWorld::Ptr player = MWMechanics::getPlayer();

        if (MWBase::Environment::get().getMechanicsManager()->isAttackingOrSpell(player))
            return;

        const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        if (stats.isParalyzed() || stats.getKnockedDown() || stats.isDead() || stats.getHitRecovery())
            return;

        mSpellView->setModel(new SpellModel(MWMechanics::getPlayer()));
        int itemCount = mSpellView->getModel()->getItemCount();
        if (itemCount == 0)
            return;

        SpellModel::ModelIndex nextIndex;
        SpellModel::ModelIndex currentIndex = mSpellView->getModel()->getSelectedIndex();

        // If we have a selected index, search for a valid selection in the target direction
        if (currentIndex >= 0)
        {
            MWWorld::ContainerStore store;
            const Spell& currentSpell = mSpellView->getModel()->getItem(currentIndex);

            nextIndex = currentIndex;
            for (int i = 0; i < itemCount; i++)
            {
                nextIndex += next ? 1 : -1;
                nextIndex = (nextIndex + itemCount) % itemCount;

                // We can keep this selection if:
                //   * we're not switching off of an enchanted item
                //   * we're not switching to an enchanted item
                //   * the next item wouldn't stack with the current item
                if (currentSpell.mType != Spell::Type_EnchantedItem)
                    break;

                const Spell& nextSpell = mSpellView->getModel()->getItem(nextIndex);
                if (nextSpell.mType != Spell::Type_EnchantedItem || !store.stacks(currentSpell.mItem, nextSpell.mItem))
                    break;
            }
        }
        // Otherwise, the first selection is always index 0
        else
            nextIndex = 0;

        // Only trigger the selection event if the selection is actually changing.
        // The itemCount check earlier ensures we have at least one spell to select.
        if (nextIndex != currentIndex)
        {
            const Spell& selectedSpell = mSpellView->getModel()->getItem(nextIndex);
            if (selectedSpell.mType == Spell::Type_EnchantedItem)
                onEnchantedItemSelected(selectedSpell.mItem, selectedSpell.mActive);
            else
                onSpellSelected(selectedSpell.mId);
        }
    }

    bool SpellWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (isFalloutDataMode())
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_B)
                MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
            else if (!mFalloutQuestItems.empty() && arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
                setFalloutQuestSelection((mFalloutSelectedQuest + mFalloutQuestItems.size() - 1)
                    % mFalloutQuestItems.size());
            else if (!mFalloutQuestItems.empty() && arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                setFalloutQuestSelection((mFalloutSelectedQuest + 1) % mFalloutQuestItems.size());
            else if (!mFalloutQuestItems.empty() && arg.button == SDL_CONTROLLER_BUTTON_A)
            {
                MWBase::Environment::get().getWorld()->getESM4QuestRuntime().forceActiveQuest(
                    mFalloutQuestItems[mFalloutSelectedQuest]);
                updateFalloutQuestList();
            }
            return true;
        }

        if (arg.button == SDL_CONTROLLER_BUTTON_B)
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
        else
            mSpellView->onControllerButton(arg.button);

        return true;
    }

    void SpellWindow::setActiveControllerWindow(bool active)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->getMode() == MWGui::GM_Inventory)
        {
            // Fill the screen, or limit to a certain size on large screens. Size chosen to
            // match the size of the stats window.
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            int width = std::min(viewSize.width, StatsWindow::getIdealWidth());
            int height = std::min(winMgr->getControllerMenuHeight(), StatsWindow::getIdealHeight());
            int x = (viewSize.width - width) / 2;
            int y = (viewSize.height - height) / 2;

            MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
            window->setCoord(x, active ? y : viewSize.height + 1, width, height);

            MWBase::Environment::get().getWindowManager()->setControllerTooltipVisible(
                active && Settings::gui().mControllerTooltips);
        }

        if (isFalloutDataMode())
            setFalloutQuestSelection(mFalloutSelectedQuest);
        else
            mSpellView->setActiveControllerWindow(active);

        WindowBase::setActiveControllerWindow(active);
    }

    bool SpellWindow::isFalloutDataMode() const
    {
        const MWBase::World* world = MWBase::Environment::get().getWorld();
        return world != nullptr && world->getStore().getESM4Game() == MWWorld::ESM4Game::FalloutNewVegas;
    }

    void SpellWindow::updateFalloutQuestList()
    {
        if (mFalloutQuestList == nullptr || !isFalloutDataMode())
            return;

        const MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::ESM4QuestRuntime& runtime = world.getESM4QuestRuntime();
        const std::optional<ESM::FormId> activeQuest = runtime.getActiveQuest();

        struct VisibleQuest
        {
            const ESM4::Quest* mQuest = nullptr;
            const MWWorld::ESM4QuestState* mState = nullptr;
        };
        std::vector<VisibleQuest> visible;
        for (const ESM4::Quest& quest : world.getStore().get<ESM4::Quest>())
        {
            const MWWorld::ESM4QuestState* state = runtime.search(quest.mId);
            if (state == nullptr)
                continue;
            const bool hasDisplayedObjective = std::ranges::any_of(state->mObjectiveStatus, [](const auto& objective) {
                return (objective.second & MWWorld::ESM4QuestState::Objective_Displayed) != 0;
            });
            if ((state->mFlags & MWWorld::ESM4QuestState::Flag_ShownInPipBoy) == 0 && !hasDisplayedObjective)
                continue;
            visible.push_back({ &quest, state });
        }
        std::ranges::sort(visible, [activeQuest](const VisibleQuest& left, const VisibleQuest& right) {
            const bool leftActive = activeQuest && left.mQuest->mId == *activeQuest;
            const bool rightActive = activeQuest && right.mQuest->mId == *activeQuest;
            if (leftActive != rightActive)
                return leftActive;
            return left.mQuest->mQuestName < right.mQuest->mQuestName;
        });

        mFalloutQuestList->clear();
        mFalloutQuestItems.clear();
        for (const VisibleQuest& entry : visible)
        {
            const ESM4::Quest& quest = *entry.mQuest;
            const MWWorld::ESM4QuestState& state = *entry.mState;
            const std::string_view title = quest.mQuestName.empty() ? std::string_view(quest.mEditorId)
                                                                    : std::string_view(quest.mQuestName);
            std::string header = activeQuest && quest.mId == *activeQuest ? "> " : "  ";
            if ((state.mFlags & MWWorld::ESM4QuestState::Flag_Failed) != 0)
                header += "[FAILED] ";
            else if ((state.mFlags & MWWorld::ESM4QuestState::Flag_Completed) != 0)
                header += "[DONE] ";
            header += title;
            mFalloutQuestList->addItem(header, 4);
            mFalloutQuestItems.push_back(quest.mId);

            for (const ESM4::QuestObjective& objective : quest.mObjectives)
            {
                const auto status = state.mObjectiveStatus.find(objective.mIndex);
                if (status == state.mObjectiveStatus.end()
                    || (status->second & MWWorld::ESM4QuestState::Objective_Displayed) == 0)
                    continue;
                const bool completed = (status->second & MWWorld::ESM4QuestState::Objective_Completed) != 0;
                std::string text = completed ? "      [x] " : "      [ ] ";
                text += objective.mDescription.empty()
                    ? "Objective " + std::to_string(objective.mIndex)
                    : objective.mDescription;
                mFalloutQuestList->addItem(text, 2);
                mFalloutQuestItems.push_back(quest.mId);
            }
        }
        if (mFalloutQuestItems.empty())
            mFalloutQuestList->addItem("No active quests in the loaded save", 8);
        mFalloutQuestList->adjustSize();
        mFalloutSelectedQuest = std::min(mFalloutSelectedQuest,
            mFalloutQuestItems.empty() ? std::size_t(0) : mFalloutQuestItems.size() - 1);
        setFalloutQuestSelection(mFalloutSelectedQuest);
    }

    void SpellWindow::onFalloutQuestSelected(const std::string& /*name*/, int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= mFalloutQuestItems.size())
            return;
        mFalloutSelectedQuest = static_cast<std::size_t>(index);
        MWBase::Environment::get().getWorld()->getESM4QuestRuntime().forceActiveQuest(
            mFalloutQuestItems[mFalloutSelectedQuest]);
        updateFalloutQuestList();
    }

    void SpellWindow::setFalloutQuestSelection(std::size_t index)
    {
        if (mFalloutQuestItems.empty())
            return;
        mFalloutSelectedQuest = std::min(index, mFalloutQuestItems.size() - 1);
        for (std::size_t i = 0; i < mFalloutQuestItems.size(); ++i)
        {
            if (MyGUI::Button* item = mFalloutQuestList->getItemWidget(mFalloutQuestList->getItemNameAt(i)))
            {
                item->setStateSelected(i == mFalloutSelectedQuest);
                // The TES3 list skin applies a dim default colour. Override it at full
                // opacity so FNV quest/objective text remains legible over the world view.
                item->setTextColour(i == mFalloutSelectedQuest ? MyGUI::Colour(1.f, 0.95f, 0.55f, 1.f)
                                                               : MyGUI::Colour(1.f, 0.82f, 0.22f, 1.f));
            }
        }
    }
}
