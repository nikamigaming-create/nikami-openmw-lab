#include "statswindow.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_ProgressBar.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_TextIterator.h>
#include <MyGUI_Window.h>

#include <algorithm>
#include <array>
#include <cctype>

#include <components/debug/debuglog.hpp>

#include <components/esm3/loadbsgn.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadfact.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadrace.hpp>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/fnvplayerruntimestate.hpp"
#include "../mwworld/player.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "tooltips.hpp"

//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
//## VR_PATCH END

namespace MWGui
{
    namespace
    {
        std::string lowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        bool isFalloutStatsContent()
        {
            if (std::getenv("OPENMW_FNV_PROOF_PIPBOY_SURFACE") != nullptr)
                return true;

            if (MWBase::Environment::get().getWorld() == nullptr)
                return false;

            for (std::string file : MWBase::Environment::get().getWorld()->getContentFiles())
            {
                file = lowerAscii(std::move(file));
                if (file.find("falloutnv.esm") != std::string::npos)
                    return true;
            }

            return false;
        }

        std::string falloutAttributeLabel(const ESM::Attribute& attribute)
        {
            const std::string name = lowerAscii(attribute.mName);
            if (name.find("strength") != std::string::npos)
                return "Strength";
            if (name.find("intelligence") != std::string::npos)
                return "Intelligence";
            if (name.find("willpower") != std::string::npos)
                return "Perception";
            if (name.find("agility") != std::string::npos)
                return "Agility";
            if (name.find("endurance") != std::string::npos)
                return "Endurance";
            if (name.find("personality") != std::string::npos)
                return "Charisma";
            if (name.find("luck") != std::string::npos)
                return "Luck";
            if (name.find("speed") != std::string::npos)
                return {};
            return attribute.mName;
        }

        std::optional<std::size_t> falloutSpecialIndex(ESM::RefId id)
        {
            if (id == ESM::Attribute::Strength)
                return static_cast<std::size_t>(MWWorld::FalloutSpecial::Strength);
            if (id == ESM::Attribute::Willpower)
                return static_cast<std::size_t>(MWWorld::FalloutSpecial::Perception);
            if (id == ESM::Attribute::Endurance)
                return static_cast<std::size_t>(MWWorld::FalloutSpecial::Endurance);
            if (id == ESM::Attribute::Personality)
                return static_cast<std::size_t>(MWWorld::FalloutSpecial::Charisma);
            if (id == ESM::Attribute::Intelligence)
                return static_cast<std::size_t>(MWWorld::FalloutSpecial::Intelligence);
            if (id == ESM::Attribute::Agility)
                return static_cast<std::size_t>(MWWorld::FalloutSpecial::Agility);
            if (id == ESM::Attribute::Luck)
                return static_cast<std::size_t>(MWWorld::FalloutSpecial::Luck);
            return std::nullopt;
        }

        std::optional<std::array<float, 21>> readFalloutActorValues()
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            if (world == nullptr)
                return std::nullopt;

            const MWWorld::FalloutPlayerRuntimeState& state = world->getFalloutPlayerRuntimeState();
            if (!state.isInitialized())
                return std::nullopt;

            std::array<float, 21> result{};
            for (std::size_t index = 0; index < MWWorld::FalloutPlayerState::SpecialCount; ++index)
            {
                const auto value = state.getCurrentActorValue(
                    MWWorld::FalloutPlayerRuntimeState::SpecialActorValueBegin + static_cast<std::uint32_t>(index));
                if (!value)
                    return std::nullopt;
                result[index] = value->mValue;
            }
            for (std::size_t index = 0; index < MWWorld::FalloutPlayerState::SkillCount; ++index)
            {
                const auto value = state.getCurrentActorValue(
                    MWWorld::FalloutPlayerRuntimeState::SkillActorValueBegin + static_cast<std::uint32_t>(index));
                if (!value)
                    return std::nullopt;
                result[MWWorld::FalloutPlayerState::SpecialCount + index] = value->mValue;
            }
            return result;
        }

        int falloutDisplayValue(float value)
        {
            return static_cast<int>(value);
        }
    }

    StatsWindow::StatsWindow(DragAndDrop* drag)
//## VR_PATCH BEGIN
        : WindowPinnableBase(VR::getVR() ? "openmw_stats_window_vr.layout" : "openmw_stats_window.layout")
//## VR_PATCH END
        , NoDrop(drag, mMainWidget)
        , mSkillView(nullptr)
        , mReputation(0)
        , mBounty(0)
        , mChanged(true)
        , mMinFullWidth(mMainWidget->getSize().width)
    {

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        MyGUI::Widget* attributeView = getWidget("AttributeView");
        MyGUI::IntCoord coord{ 0, 0, 204, 18 };
        const MyGUI::Align alignment = MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch;
        const bool falloutContent = isFalloutStatsContent();
        for (const ESM::Attribute& attribute : store.get<ESM::Attribute>())
        {
            const std::string displayName = falloutContent ? falloutAttributeLabel(attribute) : attribute.mName;
            if (displayName.empty())
                continue;

            auto* box = attributeView->createWidget<MyGUI::Button>({}, coord, alignment);
            box->setUserString("ToolTipType", "Layout");
            box->setUserString("ToolTipLayout", "AttributeToolTip");
            box->setUserString("Caption_AttributeName", displayName);
            box->setUserString("Caption_AttributeDescription", attribute.mDescription);
            box->setUserString("ImageTexture_AttributeImage", attribute.mIcon);
            coord.top += coord.height;
            auto* name = box->createWidget<MyGUI::TextBox>("SandText", { 0, 0, 160, 18 }, alignment);
            name->setNeedMouseFocus(false);
            name->setCaption(displayName);
            auto* value = box->createWidget<MyGUI::TextBox>(
                "SandTextRight", { 160, 0, 44, 18 }, MyGUI::Align::Right | MyGUI::Align::Top);
            value->setNeedMouseFocus(false);
            mAttributeWidgets.emplace(attribute.mId, value);
        }

        getWidget(mSkillView, "SkillView");
        getWidget(mLeftPane, "LeftPane");
        getWidget(mRightPane, "RightPane");

        for (const ESM::Skill& skill : store.get<ESM::Skill>())
        {
            mSkillValues.emplace(skill.mId, MWMechanics::SkillValue());
            mSkillWidgetMap.emplace(skill.mId, std::make_pair<MyGUI::TextBox*, MyGUI::TextBox*>(nullptr, nullptr));
        }

        MyGUI::Window* t = mMainWidget->castType<MyGUI::Window>();
        t->eventWindowChangeCoord += MyGUI::newDelegate(this, &StatsWindow::onWindowResize);

        if (Settings::gui().mControllerMenus)
        {
            setPinButtonVisible(false);
            mControllerButtons.mLStick = "#{Interface:Mouse}";
            mControllerButtons.mRStick = "#{Interface:ScrollDown}";
            mControllerButtons.mB = "#{Interface:Back}";
        }

        if (falloutContent)
        {
            refreshFalloutActorValues();
            Log(Debug::Info) << "FNV/ESM4 proof: stats panel labels loaded HP/AP/WG";
        }

        onWindowResize(t);
    }

    void StatsWindow::onMouseWheel(MyGUI::Widget* /*sender*/, int rel)
    {
        if (mSkillView->getViewOffset().top + rel * 0.3 > 0)
            mSkillView->setViewOffset(MyGUI::IntPoint(0, 0));
        else
            mSkillView->setViewOffset(
                MyGUI::IntPoint(0, static_cast<int>(mSkillView->getViewOffset().top + rel * 0.3)));
    }

    void StatsWindow::onWindowResize(MyGUI::Window* window)
    {
        int windowWidth = window->getSize().width;
        int windowHeight = window->getSize().height;

        // initial values defined in openmw_stats_window.layout, if custom options are not present in .layout, a default
        // is loaded
        float leftPaneRatio = 0.44f;
        if (mLeftPane->isUserString("LeftPaneRatio"))
            leftPaneRatio = MyGUI::utility::parseFloat(mLeftPane->getUserString("LeftPaneRatio"));

        int leftOffsetWidth = 24;
        if (mLeftPane->isUserString("LeftOffsetWidth"))
            leftOffsetWidth = MyGUI::utility::parseInt(mLeftPane->getUserString("LeftOffsetWidth"));

        float rightPaneRatio = 1.f - leftPaneRatio;
        int minLeftWidth = static_cast<int>(mMinFullWidth * leftPaneRatio);
        int minLeftOffsetWidth = minLeftWidth + leftOffsetWidth;

        // if there's no space for right pane
        mRightPane->setVisible(windowWidth >= minLeftOffsetWidth);
        if (!mRightPane->getVisible())
        {
            mLeftPane->setCoord(MyGUI::IntCoord(0, 0, windowWidth - leftOffsetWidth, windowHeight));
        }
        // if there's some space for right pane
        else if (windowWidth < mMinFullWidth)
        {
            mLeftPane->setCoord(MyGUI::IntCoord(0, 0, minLeftWidth, windowHeight));
            mRightPane->setCoord(MyGUI::IntCoord(minLeftWidth, 0, windowWidth - minLeftWidth, windowHeight));
        }
        // if there's enough space for both panes
        else
        {
            mLeftPane->setCoord(MyGUI::IntCoord(0, 0, static_cast<int>(leftPaneRatio * windowWidth), windowHeight));
            mRightPane->setCoord(MyGUI::IntCoord(static_cast<int>(leftPaneRatio * windowWidth), 0,
                static_cast<int>(rightPaneRatio * windowWidth), windowHeight));
        }

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mSkillView->setVisibleVScroll(false);
        mSkillView->setCanvasSize(mSkillView->getWidth(), mSkillView->getCanvasSize().height);
        mSkillView->setVisibleVScroll(true);
    }

    void StatsWindow::setBar(const std::string& name, const std::string& tname, int val, int max)
    {
        MyGUI::ProgressBar* pt;
        getWidget(pt, name);

        std::stringstream out;
        out << val << "/" << max;
        setText(tname, out.str());

        pt->setProgressRange(std::max(0, max));
        pt->setProgressPosition(std::max(0, val));
    }

    void StatsWindow::setPlayerName(const std::string& playerName)
    {
        mMainWidget->castType<MyGUI::Window>()->setCaption(playerName);
    }

    void StatsWindow::setAttribute(ESM::RefId id, const MWMechanics::AttributeValue& value)
    {
        if (isFalloutStatsContent())
        {
            refreshFalloutActorValues();
            const std::optional<std::size_t> index = falloutSpecialIndex(id);
            const auto widget = mAttributeWidgets.find(id);
            if (index && mFalloutActorValues && widget != mAttributeWidgets.end())
            {
                widget->second->setCaption(std::to_string(falloutDisplayValue((*mFalloutActorValues)[*index])));
                widget->second->_setWidgetState("normal");
            }
            mChanged = true;
            return;
        }

        auto it = mAttributeWidgets.find(id);
        if (it != mAttributeWidgets.end())
        {
            MyGUI::TextBox* box = it->second;
            box->setCaption(std::to_string(static_cast<int>(value.getModified())));
            if (value.getModified() > value.getBase())
                box->_setWidgetState("increased");
            else if (value.getModified() < value.getBase())
                box->_setWidgetState("decreased");
            else
                box->_setWidgetState("normal");
        }
    }

    bool StatsWindow::refreshFalloutActorValues()
    {
        const std::optional<std::array<float, 21>> values = readFalloutActorValues();
        if (values == mFalloutActorValues)
            return false;

        mFalloutActorValues = values;
        if (mFalloutActorValues)
        {
            for (const auto& [id, widget] : mAttributeWidgets)
            {
                const std::optional<std::size_t> index = falloutSpecialIndex(id);
                if (!index)
                    continue;
                widget->setCaption(std::to_string(falloutDisplayValue((*mFalloutActorValues)[*index])));
                widget->_setWidgetState("normal");
            }
        }
        mChanged = true;
        return true;
    }

    void StatsWindow::setValue(std::string_view id, const MWMechanics::DynamicStat<float>& value)
    {
        int current = static_cast<int>(value.getCurrent());
        int modified = static_cast<int>(value.getModified(false));

        // Fatigue can be negative
        if (id != "FBar")
            current = std::max(0, current);

        setBar(std::string(id), std::string(id) + "T", current, modified);

        // health, magicka, fatigue tooltip
        MyGUI::Widget* w;
        std::string valStr = MyGUI::utility::toString(current) + " / " + MyGUI::utility::toString(modified);
        if (id == "HBar")
        {
            getWidget(w, "Health");
            w->setUserString("Caption_HealthDescription", "#{sHealthDesc}\n" + valStr);
        }
        else if (id == "MBar")
        {
            getWidget(w, "Magicka");
            w->setUserString("Caption_HealthDescription", "#{sMagDesc}\n" + valStr);
        }
        else if (id == "FBar")
        {
            getWidget(w, "Fatigue");
            w->setUserString("Caption_HealthDescription", "#{sFatDesc}\n" + valStr);
        }
    }

    void StatsWindow::setValue(std::string_view id, const std::string& value)
    {
        if (id == "name")
            setPlayerName(value);
        else if (id == "race")
            setText("RaceText", value);
        else if (id == "class")
            setText("ClassText", value);
    }

    void StatsWindow::setValue(std::string_view id, int value)
    {
        if (id == "level")
        {
            std::ostringstream text;
            text << value;
            setText("LevelText", text.str());
        }
    }

    void setSkillProgress(MyGUI::Widget* w, float progress, ESM::RefId skillId)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();

        float progressRequirement = player.getClass().getNpcStats(player).getSkillProgressRequirement(
            skillId, *esmStore.get<ESM::Class>().find(player.get<ESM::NPC>()->mBase->mClass));

        // This is how vanilla MW displays the progress bar (I think). Note it's slightly inaccurate,
        // due to the int casting in the skill levelup logic. Also the progress label could in rare cases
        // reach 100% without the skill levelling up.
        // Leaving the original display logic for now, for consistency with ess-imported savegames.
        int progressPercent = int(float(progress) / float(progressRequirement) * 100.f + 0.5f);

        w->setUserString("Caption_SkillProgressText", MyGUI::utility::toString(progressPercent) + "/100");
        w->setUserString("RangePosition_SkillProgress", MyGUI::utility::toString(progressPercent));
    }

    void StatsWindow::setValue(ESM::RefId id, const MWMechanics::SkillValue& value)
    {
        mSkillValues[id] = value;
        std::pair<MyGUI::TextBox*, MyGUI::TextBox*> widgets = mSkillWidgetMap[id];
        MyGUI::TextBox* valueWidget = widgets.second;
        MyGUI::TextBox* nameWidget = widgets.first;
        if (valueWidget && nameWidget)
        {
            int modified = value.getModified(), base = value.getBase();
            std::string text = MyGUI::utility::toString(modified);
            std::string state = "normal";
            if (modified > base)
                state = "increased";
            else if (modified < base)
                state = "decreased";

            int widthBefore = valueWidget->getTextSize().width;

            valueWidget->setCaption(text);
            valueWidget->_setWidgetState(state);

            int widthAfter = valueWidget->getTextSize().width;
            if (widthBefore != widthAfter)
            {
                valueWidget->setCoord(valueWidget->getLeft() - (widthAfter - widthBefore), valueWidget->getTop(),
                    valueWidget->getWidth() + (widthAfter - widthBefore), valueWidget->getHeight());
                nameWidget->setSize(nameWidget->getWidth() - (widthAfter - widthBefore), nameWidget->getHeight());
            }

            if (value.getBase() < 100)
            {
                nameWidget->setUserString("Visible_SkillMaxed", "false");
                nameWidget->setUserString("UserData^Hidden_SkillMaxed", "true");
                nameWidget->setUserString("Visible_SkillProgressVBox", "true");
                nameWidget->setUserString("UserData^Hidden_SkillProgressVBox", "false");

                valueWidget->setUserString("Visible_SkillMaxed", "false");
                valueWidget->setUserString("UserData^Hidden_SkillMaxed", "true");
                valueWidget->setUserString("Visible_SkillProgressVBox", "true");
                valueWidget->setUserString("UserData^Hidden_SkillProgressVBox", "false");

                setSkillProgress(nameWidget, value.getProgress(), id);
                setSkillProgress(valueWidget, value.getProgress(), id);
            }
            else
            {
                nameWidget->setUserString("Visible_SkillMaxed", "true");
                nameWidget->setUserString("UserData^Hidden_SkillMaxed", "false");
                nameWidget->setUserString("Visible_SkillProgressVBox", "false");
                nameWidget->setUserString("UserData^Hidden_SkillProgressVBox", "true");

                valueWidget->setUserString("Visible_SkillMaxed", "true");
                valueWidget->setUserString("UserData^Hidden_SkillMaxed", "false");
                valueWidget->setUserString("Visible_SkillProgressVBox", "false");
                valueWidget->setUserString("UserData^Hidden_SkillProgressVBox", "true");
            }
        }
    }

    void StatsWindow::configureSkills(const std::vector<ESM::RefId>& major, const std::vector<ESM::RefId>& minor)
    {
        mMajorSkills = major;
        mMinorSkills = minor;

        // Update misc skills with the remaining skills not in major or minor
        std::set<ESM::RefId> skillSet;
        std::copy(major.begin(), major.end(), std::inserter(skillSet, skillSet.begin()));
        std::copy(minor.begin(), minor.end(), std::inserter(skillSet, skillSet.begin()));
        mMiscSkills.clear();
        const auto& store = MWBase::Environment::get().getWorld()->getStore().get<ESM::Skill>();
        for (const auto& skill : store)
        {
            if (!skillSet.contains(skill.mId))
                mMiscSkills.push_back(skill.mId);
        }

        updateSkillArea();
    }

    void StatsWindow::onFrame(float dt)
    {
        NoDrop::onFrame(dt);

        MWWorld::Ptr player = MWMechanics::getPlayer();
        const MWMechanics::NpcStats& playerStats = player.getClass().getNpcStats(player);
        const auto& store = MWBase::Environment::get().getESMStore();

        std::stringstream detail;
        bool first = true;
        for (const auto& attribute : store->get<ESM::Attribute>())
        {
            float mult = playerStats.getLevelupAttributeMultiplier(attribute.mId);
            mult = std::min(mult, 100 - playerStats.getAttribute(attribute.mId).getBase());
            if (mult > 1)
            {
                if (!first)
                    detail << '\n';
                detail << attribute.mName << " x" << MyGUI::utility::toString(mult);
                first = false;
            }
        }
        std::string detailText = detail.str();

        // level progress
        MyGUI::Widget* levelWidget;
        for (int i = 0; i < 2; ++i)
        {
            int max = store->get<ESM::GameSetting>().find("iLevelUpTotal")->mValue.getInteger();
            getWidget(levelWidget, i == 0 ? "Level_str" : "LevelText");

            levelWidget->setUserString(
                "RangePosition_LevelProgress", MyGUI::utility::toString(playerStats.getLevelProgress()));
            levelWidget->setUserString("Range_LevelProgress", MyGUI::utility::toString(max));
            levelWidget->setUserString("Caption_LevelProgressText",
                MyGUI::utility::toString(playerStats.getLevelProgress()) + "/" + MyGUI::utility::toString(max));
            levelWidget->setUserString("Caption_LevelDetailText", detailText);
        }

        setFactions(playerStats.getFactionRanks());
        setExpelled(playerStats.getExpelled());

        const auto& signId = MWBase::Environment::get().getWorld()->getPlayer().getBirthSign();

        setBirthSign(signId);
        setReputation(playerStats.getReputation());
        setBounty(playerStats.getBounty());

        if (isFalloutStatsContent())
            refreshFalloutActorValues();

        if (mChanged)
            updateSkillArea();
    }

    void StatsWindow::setFactions(const FactionList& factions)
    {
        if (mFactions != factions)
        {
            mFactions = factions;
            mChanged = true;
        }
    }

    void StatsWindow::setExpelled(const std::set<ESM::RefId>& expelled)
    {
        if (mExpelled != expelled)
        {
            mExpelled = expelled;
            mChanged = true;
        }
    }

    void StatsWindow::setBirthSign(const ESM::RefId& signId)
    {
        if (signId != mBirthSignId)
        {
            mBirthSignId = signId;
            mChanged = true;
        }
    }

    void StatsWindow::addSeparator(MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        MyGUI::ImageBox* separator = mSkillView->createWidget<MyGUI::ImageBox>("MW_HLine",
            MyGUI::IntCoord(10, coord1.top, coord1.width + coord2.width - 4, 18),
            MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
        separator->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);
        mSkillWidgets.push_back(separator);

        coord1.top += separator->getHeight();
        coord2.top += separator->getHeight();
    }

    void StatsWindow::addGroup(std::string_view label, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        MyGUI::TextBox* groupWidget = mSkillView->createWidget<MyGUI::TextBox>("SandBrightText",
            MyGUI::IntCoord(0, coord1.top, coord1.width + coord2.width, coord1.height),
            MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
        groupWidget->setCaption(MyGUI::UString(label));
        groupWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);
        mSkillWidgets.push_back(groupWidget);

        const int lineHeight = Settings::gui().mFontSize + 2;
        coord1.top += lineHeight;
        coord2.top += lineHeight;
    }

    std::pair<MyGUI::TextBox*, MyGUI::TextBox*> StatsWindow::addValueItem(std::string_view text,
        const std::string& value, const std::string& state, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        MyGUI::TextBox *skillNameWidget, *skillValueWidget;

        skillNameWidget = mSkillView->createWidget<MyGUI::TextBox>(
            "SandText", coord1, MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
        skillNameWidget->setCaption(MyGUI::UString(text));
        skillNameWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);

        skillValueWidget = mSkillView->createWidget<MyGUI::TextBox>(
            "SandTextRight", coord2, MyGUI::Align::Right | MyGUI::Align::Top);
        skillValueWidget->setCaption(value);
        skillValueWidget->_setWidgetState(state);
        skillValueWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);

        // resize dynamically according to text size
        int textWidthPlusMargin = skillValueWidget->getTextSize().width + 12;
        skillValueWidget->setCoord(
            coord2.left + coord2.width - textWidthPlusMargin, coord2.top, textWidthPlusMargin, coord2.height);
        skillNameWidget->setSize(skillNameWidget->getSize() + MyGUI::IntSize(coord2.width - textWidthPlusMargin, 0));

        mSkillWidgets.push_back(skillNameWidget);
        mSkillWidgets.push_back(skillValueWidget);

        const int lineHeight = Settings::gui().mFontSize + 2;
        coord1.top += lineHeight;
        coord2.top += lineHeight;

        return std::make_pair(skillNameWidget, skillValueWidget);
    }

    MyGUI::Widget* StatsWindow::addItem(const std::string& text, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        MyGUI::TextBox* skillNameWidget;

        skillNameWidget = mSkillView->createWidget<MyGUI::TextBox>("SandText", coord1, MyGUI::Align::Default);

        skillNameWidget->setCaption(text);
        skillNameWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);

        int textWidth = skillNameWidget->getTextSize().width;
        skillNameWidget->setSize(textWidth, skillNameWidget->getHeight());

        mSkillWidgets.push_back(skillNameWidget);

        const int lineHeight = Settings::gui().mFontSize + 2;
        coord1.top += lineHeight;
        coord2.top += lineHeight;

        return skillNameWidget;
    }

    void StatsWindow::addSkills(const std::vector<ESM::RefId>& skills, const std::string& titleId,
        const std::string& titleDefault, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        // Add a line separator if there are items above
        if (!mSkillWidgets.empty())
        {
            addSeparator(coord1, coord2);
        }

        addGroup(
            MWBase::Environment::get().getWindowManager()->getGameSettingString(titleId, titleDefault), coord1, coord2);

        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        for (const ESM::RefId& skillId : skills)
        {
            const ESM::Skill* skill = esmStore.get<ESM::Skill>().search(skillId);
            if (!skill) // Skip unknown skills
                continue;

            auto skillValue = mSkillValues.find(skill->mId);
            if (skillValue == mSkillValues.end())
            {
                Log(Debug::Error) << "Failed to update stats window: can not find value for skill " << skill->mId;
                continue;
            }

            const ESM::Attribute* attr
                = esmStore.get<ESM::Attribute>().find(ESM::Attribute::indexToRefId(skill->mData.mAttribute));

            std::pair<MyGUI::TextBox*, MyGUI::TextBox*> widgets
                = addValueItem(skill->mName, {}, "normal", coord1, coord2);
            mSkillWidgetMap[skill->mId] = std::move(widgets);

            for (int i = 0; i < 2; ++i)
            {
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipType", "Layout");
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipLayout", "SkillToolTip");
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString(
                    "Caption_SkillName", MyGUI::TextIterator::toTagsString(skill->mName));
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString(
                    "Caption_SkillDescription", skill->mDescription);
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("Caption_SkillAttribute",
                    "#{sGoverningAttribute}: " + MyGUI::TextIterator::toTagsString(attr->mName));
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ImageTexture_SkillImage", skill->mIcon);
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("Range_SkillProgress", "100");
            }

            setValue(skill->mId, skillValue->second);
        }
    }

    void StatsWindow::updateSkillArea()
    {
        mChanged = false;

        for (MyGUI::Widget* widget : mSkillWidgets)
        {
            MyGUI::Gui::getInstance().destroyWidget(widget);
        }
        mSkillWidgets.clear();

        const int valueSize = 40;
        MyGUI::IntCoord coord1(10, 0, mSkillView->getWidth() - (10 + valueSize) - 24, 18);
        MyGUI::IntCoord coord2(coord1.left + coord1.width, coord1.top, valueSize, coord1.height);

        if (isFalloutStatsContent() && updateFalloutStatsArea())
            return;

        if (!mMajorSkills.empty())
            addSkills(mMajorSkills, "sSkillClassMajor", "Major Skills", coord1, coord2);

        if (!mMinorSkills.empty())
            addSkills(mMinorSkills, "sSkillClassMinor", "Minor Skills", coord1, coord2);

        if (!mMiscSkills.empty())
            addSkills(mMiscSkills, "sSkillClassMisc", "Misc Skills", coord1, coord2);

        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::ESMStore& store = world->getStore();
        const ESM::NPC* player = world->getPlayerPtr().get<ESM::NPC>()->mBase;

        // race tooltip
        const ESM::Race* playerRace = store.get<ESM::Race>().find(player->mRace);

        MyGUI::Widget* raceWidget;
        getWidget(raceWidget, "RaceText");
        ToolTips::createRaceToolTip(raceWidget, playerRace);
        getWidget(raceWidget, "Race_str");
        ToolTips::createRaceToolTip(raceWidget, playerRace);

        // class tooltip
        MyGUI::Widget* classWidget;

        const ESM::Class* playerClass = store.get<ESM::Class>().find(player->mClass);

        getWidget(classWidget, "ClassText");
        ToolTips::createClassToolTip(classWidget, *playerClass);
        getWidget(classWidget, "Class_str");
        ToolTips::createClassToolTip(classWidget, *playerClass);

        if (!mFactions.empty())
        {
            MWWorld::Ptr playerPtr = MWMechanics::getPlayer();
            const MWMechanics::NpcStats& playerStats = playerPtr.getClass().getNpcStats(playerPtr);
            const std::set<ESM::RefId>& expelled = playerStats.getExpelled();

            bool firstFaction = true;
            for (const auto& [factionId, factionRank] : mFactions)
            {
                const ESM::Faction* faction = store.get<ESM::Faction>().find(factionId);
                if (faction->mData.mIsHidden == 1)
                    continue;

                if (firstFaction)
                {
                    // Add a line separator if there are items above
                    if (!mSkillWidgets.empty())
                        addSeparator(coord1, coord2);

                    addGroup(MWBase::Environment::get().getWindowManager()->getGameSettingString("sFaction", "Faction"),
                        coord1, coord2);

                    firstFaction = false;
                }

                MyGUI::Widget* w = addItem(faction->mName, coord1, coord2);

                std::string text;

                text += std::string("#{fontcolourhtml=header}") + faction->mName;

                if (expelled.find(factionId) != expelled.end())
                    text += "\n#{fontcolourhtml=normal}#{sExpelled}";
                else
                {
                    const auto rank = static_cast<size_t>(std::max(0, factionRank));
                    if (rank < faction->mRanks.size())
                        text += std::string("\n#{fontcolourhtml=normal}") + faction->mRanks[rank];
                    if (rank + 1 < faction->mRanks.size() && !faction->mRanks[rank + 1].empty())
                    {
                        // player doesn't have max rank yet
                        text += std::string("\n\n#{fontcolourhtml=header}#{sNextRank} ") + faction->mRanks[rank + 1];

                        const ESM::RankData& rankData = faction->mData.mRankData[rank + 1];
                        const ESM::Attribute* attr1 = store.get<ESM::Attribute>().find(
                            ESM::Attribute::indexToRefId(faction->mData.mAttribute[0]));
                        const ESM::Attribute* attr2 = store.get<ESM::Attribute>().find(
                            ESM::Attribute::indexToRefId(faction->mData.mAttribute[1]));

                        text += "\n#{fontcolourhtml=normal}" + MyGUI::TextIterator::toTagsString(attr1->mName) + ": "
                            + MyGUI::utility::toString(rankData.mAttribute1) + ", "
                            + MyGUI::TextIterator::toTagsString(attr2->mName) + ": "
                            + MyGUI::utility::toString(rankData.mAttribute2);

                        text += "\n\n#{fontcolourhtml=header}#{sFavoriteSkills}";
                        text += "\n#{fontcolourhtml=normal}";
                        bool firstSkill = true;
                        for (int id : faction->mData.mSkills)
                        {
                            const ESM::Skill* skill = store.get<ESM::Skill>().search(ESM::Skill::indexToRefId(id));
                            if (skill)
                            {
                                if (!firstSkill)
                                    text += ", ";

                                firstSkill = false;
                                text += MyGUI::TextIterator::toTagsString(skill->mName);
                            }
                        }

                        text += "\n";

                        if (rankData.mPrimarySkill > 0)
                            text += "\n#{sNeedOneSkill} " + MyGUI::utility::toString(rankData.mPrimarySkill);
                        if (rankData.mFavouredSkill > 0)
                            text += " #{sand} #{sNeedTwoSkills} " + MyGUI::utility::toString(rankData.mFavouredSkill);
                    }
                }

                w->setUserString("ToolTipType", "Layout");
                w->setUserString("ToolTipLayout", "FactionToolTip");
                w->setUserString("Caption_FactionText", text);
            }
        }

        if (!mBirthSignId.empty())
        {
            // Add a line separator if there are items above
            if (!mSkillWidgets.empty())
                addSeparator(coord1, coord2);

            bool falloutContent = std::getenv("OPENMW_FNV_PROOF_PIPBOY_SURFACE") != nullptr;
            if (!falloutContent && MWBase::Environment::get().getWorld())
            {
                for (const std::string& file : MWBase::Environment::get().getWorld()->getContentFiles())
                {
                    if (file.find("FalloutNV.esm") != std::string::npos || file.find("falloutnv.esm") != std::string::npos)
                    {
                        falloutContent = true;
                        break;
                    }
                }
            }

            std::string signStr = falloutContent ? "TRAITS" : std::string(MWBase::Environment::get().getWindowManager()->getGameSettingString("sBirthSign", "Sign"));
            addGroup(signStr, coord1, coord2);
            if (falloutContent)
                Log(Debug::Info) << "FNV/ESM4 proof: stats birthsign group applied TRAITS";
            const ESM::BirthSign* sign = store.get<ESM::BirthSign>().find(mBirthSignId);
            MyGUI::Widget* w = addItem(sign->mName, coord1, coord2);

            ToolTips::createBirthsignToolTip(w, mBirthSignId);
        }

        // Add a line separator if there are items above
        if (!mSkillWidgets.empty())
            addSeparator(coord1, coord2);

        addValueItem(MWBase::Environment::get().getWindowManager()->getGameSettingString("sReputation", "Reputation"),
            MyGUI::utility::toString(static_cast<int>(mReputation)), "normal", coord1, coord2);

        for (int i = 0; i < 2; ++i)
        {
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipType", "Layout");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipLayout", "TextToolTip");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("Caption_Text", "#{sSkillsMenuReputationHelp}");
        }

        addValueItem(MWBase::Environment::get().getWindowManager()->getGameSettingString("sBounty", "Bounty"),
            MyGUI::utility::toString(static_cast<int>(mBounty)), "normal", coord1, coord2);

        for (int i = 0; i < 2; ++i)
        {
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipType", "Layout");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipLayout", "TextToolTip");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("Caption_Text", "#{sCrimeHelp}");
        }

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mSkillView->setVisibleVScroll(false);
        mSkillView->setCanvasSize(mSkillView->getWidth(), std::max(mSkillView->getHeight(), coord1.top));
        mSkillView->setVisibleVScroll(true);
    }

    bool StatsWindow::updateFalloutStatsArea()
    {
        const int valueSize = 40;
        MyGUI::IntCoord coord1(10, 0, mSkillView->getWidth() - (10 + valueSize) - 24, 18);
        MyGUI::IntCoord coord2(coord1.left + coord1.width, coord1.top, valueSize, coord1.height);

        refreshFalloutActorValues();
        if (!mFalloutActorValues)
            return false;

        addGroup("S.P.E.C.I.A.L.", coord1, coord2);
        const std::array<std::pair<std::string_view, MWWorld::FalloutSpecial>, 7> special{ {
            { "Strength", MWWorld::FalloutSpecial::Strength },
            { "Perception", MWWorld::FalloutSpecial::Perception },
            { "Endurance", MWWorld::FalloutSpecial::Endurance },
            { "Charisma", MWWorld::FalloutSpecial::Charisma },
            { "Intelligence", MWWorld::FalloutSpecial::Intelligence },
            { "Agility", MWWorld::FalloutSpecial::Agility },
            { "Luck", MWWorld::FalloutSpecial::Luck },
        } };
        for (const auto& [label, value] : special)
        {
            const std::size_t index = static_cast<std::size_t>(value);
            addValueItem(label, MyGUI::utility::toString(falloutDisplayValue((*mFalloutActorValues)[index])),
                "normal", coord1, coord2);
        }

        addSeparator(coord1, coord2);
        addGroup("SKILLS", coord1, coord2);
        const std::array<std::pair<std::string_view, MWWorld::FalloutSkill>, 13> falloutSkills{ {
            { "Barter", MWWorld::FalloutSkill::Barter },
            { "Energy Weapons", MWWorld::FalloutSkill::EnergyWeapons },
            { "Explosives", MWWorld::FalloutSkill::Explosives },
            { "Guns", MWWorld::FalloutSkill::SmallGuns },
            { "Lockpick", MWWorld::FalloutSkill::Lockpick },
            { "Medicine", MWWorld::FalloutSkill::Medicine },
            { "Melee Weapons", MWWorld::FalloutSkill::MeleeWeapons },
            { "Repair", MWWorld::FalloutSkill::Repair },
            { "Science", MWWorld::FalloutSkill::Science },
            { "Sneak", MWWorld::FalloutSkill::Sneak },
            { "Speech", MWWorld::FalloutSkill::Speech },
            { "Survival", MWWorld::FalloutSkill::SurvivalOrThrowing },
            { "Unarmed", MWWorld::FalloutSkill::Unarmed },
        } };
        for (const auto& [label, value] : falloutSkills)
        {
            const std::size_t index
                = MWWorld::FalloutPlayerState::SpecialCount + static_cast<std::size_t>(value);
            addValueItem(label, MyGUI::utility::toString(falloutDisplayValue((*mFalloutActorValues)[index])),
                "normal", coord1, coord2);
        }

        addSeparator(coord1, coord2);
        addGroup("REPUTATION", coord1, coord2);
        addValueItem("Reputation", MyGUI::utility::toString(static_cast<int>(mReputation)), "normal", coord1, coord2);
        addValueItem("Bounty", MyGUI::utility::toString(static_cast<int>(mBounty)), "normal", coord1, coord2);

        mSkillView->setVisibleVScroll(false);
        mSkillView->setCanvasSize(mSkillView->getWidth(), std::max(mSkillView->getHeight(), coord1.top));
        mSkillView->setVisibleVScroll(true);

        static bool loggedFalloutStatsArea = false;
        if (!loggedFalloutStatsArea)
        {
            Log(Debug::Info)
                << "FNV/ESM4 proof: Fallout stats area applied SPECIAL/SKILLS Barter Guns Lockpick Medicine Science";
            loggedFalloutStatsArea = true;
        }

        return true;
    }

    void StatsWindow::onPinToggled()
    {
        Settings::windows().mStatsPin.set(mPinned);

        MWBase::Environment::get().getWindowManager()->setHMSVisibility(!mPinned);
    }

    void StatsWindow::onTitleDoubleClicked()
    {
        if (Settings::gui().mControllerMenus)
            return;
        else if (MyGUI::InputManager::getInstance().isShiftPressed())
        {
            MWBase::Environment::get().getWindowManager()->toggleMaximized(this);
            MyGUI::Window* t = mMainWidget->castType<MyGUI::Window>();
            onWindowResize(t);
        }
        else if (!mPinned)
            MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Stats);
    }

    bool StatsWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();

        return true;
    }

    void StatsWindow::setActiveControllerWindow(bool active)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->getMode() == MWGui::GM_Inventory)
        {
            // Fill the screen, or limit to a certain size on large screens. Size chosen to
            // show all stats.
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            int width = std::min(viewSize.width, getIdealWidth());
            int height = std::min(winMgr->getControllerMenuHeight(), getIdealHeight());
            int x = (viewSize.width - width) / 2;
            int y = (viewSize.height - height) / 2;

            MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
            window->setCoord(x, active ? y : viewSize.height + 1, width, height);

            if (active)
                onWindowResize(window);
        }

        WindowBase::setActiveControllerWindow(active);
    }
}
