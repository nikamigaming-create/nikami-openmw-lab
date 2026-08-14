#include "spellicons.hpp"

#include <iomanip>
<<<<<<< HEAD
#include <set>
=======
>>>>>>> origin/main
#include <sstream>

#include <MyGUI_ImageBox.h>

#include <components/esm3/loadmgef.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"

#include "tooltips.hpp"

<<<<<<< HEAD
namespace MWGui
{
    namespace
    {
        MyGUI::ImageBox* createIcon(MyGUI::Widget& parent, std::string_view name, std::string_view icon, int size)
        {
            const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();

            const MyGUI::IntCoord coord(0, 0, size, size);
            MyGUI::ImageBox* widget = parent.createWidget<MyGUI::ImageBox>("ImageBox", coord, MyGUI::Align::Default);
            widget->setImageTexture(Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(icon), vfs));

            ToolTipInfo tooltipInfo;
            tooltipInfo.caption = name;
            tooltipInfo.icon = icon;
            tooltipInfo.imageSize = size;
            tooltipInfo.wordWrap = false;

            widget->setUserData(tooltipInfo);
            widget->setUserString("ToolTipType", "ToolTipInfo");
            widget->setVisible(false);

            return widget;
        }

        std::string printEffectMagnitude(float srcMagnitude, ESM::MagicEffect::MagnitudeDisplayType displayType)
        {
            std::string result;
            if (displayType == ESM::MagicEffect::MDT_None)
                return result;

            const int magnitude = static_cast<int>(srcMagnitude);
            if (displayType == ESM::MagicEffect::MDT_TimesInt)
            {
                std::stringstream formatter;
                formatter << std::fixed << std::setprecision(1) << " " << (magnitude / 10.0f);
                result += formatter.str();
            }
            else
            {
                result += ": " + MyGUI::utility::toString(magnitude);
                if (displayType != ESM::MagicEffect::MDT_Percentage)
                    result += ' ';
            }

            std::string_view unit;
            if (displayType == ESM::MagicEffect::MDT_TimesInt)
                unit = "sXTimesINT";
            else if (displayType == ESM::MagicEffect::MDT_Percentage)
                unit = "sPercent";
            else if (displayType == ESM::MagicEffect::MDT_Feet)
                unit = "sFeet";
            else if (displayType == ESM::MagicEffect::MDT_Level)
                unit = magnitude > 1 ? "sLevels" : "sLevel";
            else if (displayType == ESM::MagicEffect::MDT_Points)
                unit = magnitude > 1 ? "sPoints" : "sPoint";

            result += MWBase::Environment::get().getWindowManager()->getGameSettingString(unit, {});

            return result;
        }
    }

    void SpellIcons::updateWidgets(MyGUI::Widget* parent, bool adjustSize)
    {
        for (auto& [effectId, widget] : mWidgetMap)
        {
            widget->setAlpha(1.f);
            widget->getUserData<ToolTipInfo>()->text.clear();
        }

        int horizontalOffset = 2;
        constexpr int verticalOffset = 2;
        constexpr int size = 16;
=======
//## VR_PATCH BEGIN
#include <components/vr/vr.hpp>
//## VR_PATCH END

namespace MWGui
{
    void SpellIcons::updateWidgets(MyGUI::Widget* parent, bool adjustSize)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);

        std::map<int, std::vector<MagicEffectInfo>> effects;
        for (const auto& params : stats.getActiveSpells())
        {
            for (const auto& effect : params.getEffects())
            {
                if (!(effect.mFlags & ESM::ActiveEffect::Flag_Applied))
                    continue;
                MagicEffectInfo newEffectSource;
                newEffectSource.mKey = MWMechanics::EffectKey(effect.mEffectId, effect.getSkillOrAttribute());
                newEffectSource.mMagnitude = static_cast<int>(effect.mMagnitude);
                newEffectSource.mPermanent = effect.mDuration == -1.f;
                newEffectSource.mRemainingTime = effect.mTimeLeft;
                newEffectSource.mSource = params.getDisplayName();
                newEffectSource.mTotalTime = effect.mDuration;
                effects[effect.mEffectId].push_back(std::move(newEffectSource));
            }
        }

        int w = 2;
        const auto& store = MWBase::Environment::get().getESMStore();
//## VR_PATCH BEGIN
        std::vector<MyGUI::ImageBox*> images;

//## VR_PATCH END
        for (const auto& [effectId, effectInfos] : effects)
        {
            const ESM::MagicEffect* effect = store->get<ESM::MagicEffect>().find(effectId);

            float remainingDuration = 0;
            float totalDuration = 0;
>>>>>>> origin/main

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        static const float fadeTime = store.get<ESM::GameSetting>().find("fMagicStartIconBlink")->mValue.getFloat();

<<<<<<< HEAD
        std::set<ESM::RefId> activeEffects;

        const MWWorld::Ptr player = MWMechanics::getPlayer();
        const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        for (const auto& params : stats.getActiveSpells())
        {
            for (const auto& source : params.getEffects())
            {
                if (!(source.mFlags & ESM::ActiveEffect::Flag_Applied))
                    continue;
                if (source.mDuration != -1.f && source.mTimeLeft <= 0.f)
                    continue;

                const ESM::RefId effectId = source.mEffectId;
                const ESM::MagicEffect& effect = *store.get<ESM::MagicEffect>().find(effectId);
                const ESM::RefId arg = source.getSkillOrAttribute();

                if (mWidgetMap.find(effectId) == mWidgetMap.end())
                    mWidgetMap[effectId] = createIcon(*parent, effect.mName, effect.mIcon, size);

                MyGUI::ImageBox& widget = *mWidgetMap[effectId];
                if (activeEffects.emplace(effectId).second)
                {
                    widget.setPosition(horizontalOffset, verticalOffset);
                    widget.setVisible(true);
                    if (source.mDuration >= fadeTime && fadeTime > 0.f)
                        widget.setAlpha(std::min(source.mTimeLeft / fadeTime, 1.f));
                    horizontalOffset += size;
                }

                std::string& desc = widget.getUserData<ToolTipInfo>()->text;
                if (!desc.empty())
                    desc += '\n';

                desc += params.getDisplayName();

                if (effect.mData.mFlags & ESM::MagicEffect::TargetSkill)
                {
                    const ESM::Skill& skill = *store.get<ESM::Skill>().find(arg);
                    desc += " (" + skill.mName + ')';
                }
                if (effect.mData.mFlags & ESM::MagicEffect::TargetAttribute)
                {
                    const ESM::Attribute& attribute = *store.get<ESM::Attribute>().find(arg);
                    desc += " (" + attribute.mName + ')';
                }
                desc += printEffectMagnitude(source.mMagnitude, effect.getMagnitudeDisplayType());
                if (source.mTimeLeft > -1 && Settings::game().mShowEffectDuration)
                    desc += MWGui::ToolTips::getDurationString(source.mTimeLeft, " #{sDuration}");
=======
            static const float fadeTime
                = store->get<ESM::GameSetting>().find("fMagicStartIconBlink")->mValue.getFloat();

            bool addNewLine = false;
            for (const MagicEffectInfo& effectInfo : effectInfos)
            {
                if (addNewLine)
                    sourcesDescription += '\n';

                // if at least one of the effect sources is permanent, the effect will never wear off
                if (effectInfo.mPermanent)
                {
                    remainingDuration = fadeTime;
                    totalDuration = fadeTime;
                }
                else
                {
                    remainingDuration = std::max(remainingDuration, effectInfo.mRemainingTime);
                    totalDuration = std::max(totalDuration, effectInfo.mTotalTime);
                }

                sourcesDescription += effectInfo.mSource;

                if (effect->mData.mFlags & ESM::MagicEffect::TargetSkill)
                {
                    const ESM::Skill* skill = store->get<ESM::Skill>().find(effectInfo.mKey.mArg);
                    sourcesDescription += " (" + skill->mName + ')';
                }
                if (effect->mData.mFlags & ESM::MagicEffect::TargetAttribute)
                {
                    const ESM::Attribute* attribute = store->get<ESM::Attribute>().find(effectInfo.mKey.mArg);
                    sourcesDescription += " (" + attribute->mName + ')';
                }
                ESM::MagicEffect::MagnitudeDisplayType displayType = effect->getMagnitudeDisplayType();
                if (displayType == ESM::MagicEffect::MDT_TimesInt)
                {
                    std::string_view timesInt
                        = MWBase::Environment::get().getWindowManager()->getGameSettingString("sXTimesINT", {});
                    std::stringstream formatter;
                    formatter << std::fixed << std::setprecision(1) << " " << (effectInfo.mMagnitude / 10.0f)
                              << timesInt;
                    sourcesDescription += formatter.str();
                }
                else if (displayType != ESM::MagicEffect::MDT_None)
                {
                    sourcesDescription += ": " + MyGUI::utility::toString(effectInfo.mMagnitude);

                    if (displayType == ESM::MagicEffect::MDT_Percentage)
                        sourcesDescription
                            += MWBase::Environment::get().getWindowManager()->getGameSettingString("spercent", {});
                    else if (displayType == ESM::MagicEffect::MDT_Feet)
                    {
                        sourcesDescription += ' ';
                        sourcesDescription
                            += MWBase::Environment::get().getWindowManager()->getGameSettingString("sfeet", {});
                    }
                    else if (displayType == ESM::MagicEffect::MDT_Level)
                    {
                        sourcesDescription += ' ';
                        if (effectInfo.mMagnitude > 1)
                            sourcesDescription
                                += MWBase::Environment::get().getWindowManager()->getGameSettingString("sLevels", {});
                        else
                            sourcesDescription
                                += MWBase::Environment::get().getWindowManager()->getGameSettingString("sLevel", {});
                    }
                    else // ESM::MagicEffect::MDT_Points
                    {
                        sourcesDescription += ' ';
                        if (effectInfo.mMagnitude > 1)
                            sourcesDescription
                                += MWBase::Environment::get().getWindowManager()->getGameSettingString("spoints", {});
                        else
                            sourcesDescription
                                += MWBase::Environment::get().getWindowManager()->getGameSettingString("spoint", {});
                    }
                }
                if (effectInfo.mRemainingTime > -1 && Settings::game().mShowEffectDuration)
                    sourcesDescription
                        += MWGui::ToolTips::getDurationString(effectInfo.mRemainingTime, " #{sDuration}");

                addNewLine = true;
            }

            if (remainingDuration > 0.f)
            {
                MyGUI::ImageBox* image;
                if (mWidgetMap.find(effectId) == mWidgetMap.end())
                {
                    image = parent->createWidget<MyGUI::ImageBox>(
                        "ImageBox", MyGUI::IntCoord(w, 2, 16, 16), MyGUI::Align::Default);
                    mWidgetMap[effectId] = image;

                    image->setImageTexture(Misc::ResourceHelpers::correctIconPath(
                        effect->mIcon, MWBase::Environment::get().getResourceSystem()->getVFS()));

                    const std::string& name = ESM::MagicEffect::indexToGmstString(effectId);

                    ToolTipInfo tooltipInfo;
                    tooltipInfo.caption = "#{" + name + "}";
                    tooltipInfo.icon = effect->mIcon;
                    tooltipInfo.imageSize = 16;
                    tooltipInfo.wordWrap = false;

                    image->setUserData(tooltipInfo);
                    image->setUserString("ToolTipType", "ToolTipInfo");
                }
                else
                    image = mWidgetMap[effectId];

                image->setPosition(w, 2);
                image->setVisible(true);
                w += 16;

                ToolTipInfo* tooltipInfo = image->getUserData<ToolTipInfo>();
                tooltipInfo->text = std::move(sourcesDescription);

                // Fade out
                if (totalDuration >= fadeTime && fadeTime > 0.f)
                    image->setAlpha(std::min(remainingDuration / fadeTime, 1.f));
//## VR_PATCH BEGIN

                images.push_back(image);
//## VR_PATCH END
            }
            else if (mWidgetMap.find(effectId) != mWidgetMap.end())
            {
                MyGUI::ImageBox* image = mWidgetMap[effectId];
                image->setVisible(false);
                image->setAlpha(1.f);
>>>>>>> origin/main
            }
        }

//## VR_PATCH BEGIN
        if (VR::getVR())
        {
            // In VR mode, the effect box grows to the right.
            // They are therefore added in the reverse order to retain positional stability.
            // TODO: What if users configure the hud to be on the right hand side?
            int reverse_w = w;
            for (auto* image : images)
            {
                reverse_w -= 16;
                image->setPosition(reverse_w, 2);
            }
        }

//## VR_PATCH END
        if (adjustSize)
        {
<<<<<<< HEAD
            const int newWidth = horizontalOffset > 2 ? horizontalOffset + 2 : 0;
            const int diff = parent->getWidth() - newWidth;
            parent->setSize(newWidth, parent->getHeight());
            parent->setPosition(parent->getLeft() + diff, parent->getTop());
        }

        for (auto& [effectId, widget] : mWidgetMap)
        {
            if (!activeEffects.contains(effectId))
                widget->setVisible(false);
=======
            int s = w + 2;
            if (effects.empty())
                s = 0;
            int diff = parent->getWidth() - s;
            parent->setSize(s, parent->getHeight());
//## VR_PATCH BEGIN
            // in VR mode, the effect box grows to the right and does not need repositioning
            if(!VR::getVR())
            parent->setPosition(parent->getLeft() + diff, parent->getTop());
//## VR_PATCH END
        }

        // hide inactive effects
        for (auto& widgetPair : mWidgetMap)
        {
            if (effects.find(widgetPair.first) == effects.end())
                widgetPair.second->setVisible(false);
>>>>>>> origin/main
        }
    }
}
