#ifndef OPENMW_LUAUI_ELEMENT
#define OPENMW_LUAUI_ELEMENT

<<<<<<< HEAD
#include <vector>

=======
>>>>>>> origin/main
#include "widget.hpp"

namespace LuaUi
{
    struct Element
    {
<<<<<<< HEAD
        static std::shared_ptr<Element> make(sol::table layout, bool menu, sol::optional<sol::table> options);
=======
        static std::shared_ptr<Element> make(sol::table layout, bool menu);
>>>>>>> origin/main
        static void erase(Element* element);

        template <class Callback>
        static void forEach(bool menu, Callback callback)
        {
            auto& container = menu ? sMenuElements : sGameElements;
            for (auto& [_, element] : container)
                callback(element.get());
        }

<<<<<<< HEAD
        static const std::vector<std::string_view>& allLayoutProperties();

        WidgetExtension* mRoot;
        sol::main_object mLayout;
        std::string mLayer;
        bool mWarnedOnce{ false };

        // From options
        bool mNoWarnUnused{ false };
=======
        WidgetExtension* mRoot;
        sol::main_object mLayout;
        std::string mLayer;
>>>>>>> origin/main

        enum State
        {
            New,
            Created,
            Update,
            Destroy,
            Destroyed,
        };
        State mState;

        void create(uint64_t dept = 0);

        void update();

        void destroy();

        friend void clearGameInterface();
        friend void clearMenuInterface();

<<<<<<< HEAD
        void checkWarnings();

    private:
        Element(sol::table layout, sol::optional<sol::table> options);
=======
    private:
        Element(sol::table layout);
>>>>>>> origin/main
        sol::table layout() { return LuaUtil::cast<sol::table>(mLayout); }
        static std::map<Element*, std::shared_ptr<Element>> sGameElements;
        static std::map<Element*, std::shared_ptr<Element>> sMenuElements;
    };
}

#endif // !OPENMW_LUAUI_ELEMENT
