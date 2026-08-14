#ifndef MWLUA_SOUNDBINDINGS_H
#define MWLUA_SOUNDBINDINGS_H

#include <sol/forward.hpp>

<<<<<<< HEAD
namespace ESM
{
    struct Sound;
}

=======
>>>>>>> origin/main
namespace MWLua
{
    struct Context;

    sol::table initCoreSoundBindings(const Context& context);

    sol::table initAmbientPackage(const Context& context);
<<<<<<< HEAD

    void addMutableSoundType(sol::state_view& lua);

    ESM::Sound tableToSound(const sol::table&);
=======
>>>>>>> origin/main
}

#endif // MWLUA_SOUNDBINDINGS_H
