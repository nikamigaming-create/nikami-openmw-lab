#ifndef GAME_MWCLASS_ESM4TALKINGACTIVATOR_H
#define GAME_MWCLASS_ESM4TALKINGACTIVATOR_H

#include <components/esm4/loadtact.hpp>

#include "esm4base.hpp"

namespace MWClass
{
    ESM::FormId selectFnvTalkingActivatorSound(const ESM4::TalkingActivator& activator);

    class ESM4TalkingActivator final
        : public MWWorld::RegisteredClass<ESM4TalkingActivator, ESM4Base<ESM4::TalkingActivator>>
    {
        friend MWWorld::RegisteredClass<ESM4TalkingActivator, ESM4Base<ESM4::TalkingActivator>>;

        ESM4TalkingActivator();

    public:
        std::string_view getName(const MWWorld::ConstPtr& ptr) const override;
        MWGui::ToolTipInfo getToolTipInfo(const MWWorld::ConstPtr& ptr, int count) const override;
        bool hasToolTip(const MWWorld::ConstPtr& ptr) const override;
        bool isActivator() const override;
        bool useAnim() const override;
        std::unique_ptr<MWWorld::Action> activate(
            const MWWorld::Ptr& ptr, const MWWorld::Ptr& actor) const override;
    };
}

#endif
