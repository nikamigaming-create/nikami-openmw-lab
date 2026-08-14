#include "ref.hpp"

#include <components/interpreter/runtime.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/worldmodel.hpp"

#include "interpretercontext.hpp"

#include <string_view>

MWWorld::Ptr MWScript::ExplicitRef::operator()(Interpreter::Runtime& runtime, bool required, bool activeOnly) const
{
    const std::string_view text = runtime.getStringLiteral(runtime[0].mInteger);
    ESM::RefId id = ESM::RefId::deserializeText(text);
    if (id.empty())
        id = ESM::RefId::stringRefId(text);
    runtime.pop();

    if (const ESM::FormId* formId = id.getIf<ESM::FormId>())
    {
        MWWorld::Ptr placed = MWBase::Environment::get().getWorldModel()->getPtr(*formId);
        if (!placed.isEmpty())
            return placed;
    }

    if (required)
        return MWBase::Environment::get().getWorld()->getPtr(id, activeOnly);
    else
        return MWBase::Environment::get().getWorld()->searchPtr(id, activeOnly);
}

MWWorld::Ptr MWScript::ImplicitRef::operator()(Interpreter::Runtime& runtime, bool required, bool activeOnly) const
{
    MWScript::InterpreterContext& context = static_cast<MWScript::InterpreterContext&>(runtime.getContext());

    return context.getReference(required);
}
