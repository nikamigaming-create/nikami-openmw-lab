#ifndef GAME_MWCLASS_ESM4ACTIVATOR_H
#define GAME_MWCLASS_ESM4ACTIVATOR_H

#include <components/esm4/loadacti.hpp>

#include "esm4base.hpp"

namespace MWClass
{
    class ESM4Activator final
        : public MWWorld::RegisteredClass<ESM4Activator, ESM4Base<ESM4::Activator>>
    {
        friend MWWorld::RegisteredClass<ESM4Activator, ESM4Base<ESM4::Activator>>;

        ESM4Activator();

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
