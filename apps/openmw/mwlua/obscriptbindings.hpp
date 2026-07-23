#ifndef MWLUA_OBSCRIPTBINDINGS_H
#define MWLUA_OBSCRIPTBINDINGS_H

#include <sol/forward.hpp>

namespace MWLua
{
    struct Context;

    sol::table initCoreObScriptBindings(const Context& context);
}

#endif // MWLUA_OBSCRIPTBINDINGS_H
