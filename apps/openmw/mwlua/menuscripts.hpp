#ifndef MWLUA_MENUSCRIPTS_H
#define MWLUA_MENUSCRIPTS_H

#include <SDL_events.h>

#include <components/lua/luastate.hpp>
#include <components/lua/scriptscontainer.hpp>
#include <components/sdlutil/events.hpp>

#include "../mwbase/luamanager.hpp"

#include "inputprocessor.hpp"

namespace MWLua
{
    struct Context;

    sol::table initMenuPackage(const Context& context);

    class MenuScripts : public LuaUtil::ScriptsContainer
    {
    public:
        MenuScripts(LuaUtil::LuaState* lua)
            : LuaUtil::ScriptsContainer(lua, "Menu")
            , mInputProcessor(this)
        {
<<<<<<< HEAD
            registerEngineHandlers({ &mOnFrameHandlers, &mStateChanged, &mConsoleCommandHandlers, &mUiModeChanged });
=======
            registerEngineHandlers({ &mOnFrameHandlers, &mOnVRFrameHandlers, &mOnVRRecenter, &mStateChanged,
                &mConsoleCommandHandlers, &mUiModeChanged });
>>>>>>> origin/main
        }

        void processInputEvent(const MWBase::LuaManager::InputEvent& event)
        {
            mInputProcessor.processInputEvent(event);
        }

        void onFrame(float dt) { callEngineHandlers(mOnFrameHandlers, dt); }

<<<<<<< HEAD
=======
        void onVRFrame() { callEngineHandlers(mOnVRFrameHandlers); }
        void onVRRecenter(bool vertical, bool horizontal) { callEngineHandlers(mOnVRRecenter, vertical, horizontal); }

>>>>>>> origin/main
        void stateChanged() { callEngineHandlers(mStateChanged); }

        bool consoleCommand(const std::string& consoleMode, const std::string& command)
        {
            callEngineHandlers(mConsoleCommandHandlers, consoleMode, command);
            return !mConsoleCommandHandlers.mList.empty();
        }

        void uiModeChanged() { callEngineHandlers(mUiModeChanged); }

    private:
        friend class MWLua::InputProcessor<MenuScripts>;
        MWLua::InputProcessor<MenuScripts> mInputProcessor;
        EngineHandlerList mOnFrameHandlers{ "onFrame" };
<<<<<<< HEAD
=======
        EngineHandlerList mOnVRFrameHandlers{ "onVRFrame" };
        EngineHandlerList mOnVRRecenter{ "onVRRecenter" };
>>>>>>> origin/main
        EngineHandlerList mStateChanged{ "onStateChanged" };
        EngineHandlerList mConsoleCommandHandlers{ "onConsoleCommand" };
        EngineHandlerList mUiModeChanged{ "_onUiModeChanged" };
    };
}

#endif // MWLUA_GLOBALSCRIPTS_H
