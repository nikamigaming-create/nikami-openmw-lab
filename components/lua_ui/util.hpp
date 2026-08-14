#ifndef OPENMW_LUAUI_WIDGETLIST
#define OPENMW_LUAUI_WIDGETLIST

<<<<<<< HEAD
#include <sol/table.hpp>
#include <string>
#include <unordered_map>
#include <vector>
=======
#include <string>
#include <unordered_map>
>>>>>>> origin/main

namespace LuaUi
{
    void registerAllWidgets();

    const std::unordered_map<std::string, std::string>& widgetTypeToName();

    void clearGameInterface();
    void clearMenuInterface();
<<<<<<< HEAD

    bool warnUnused(std::vector<std::string>& warnings, sol::object table, const std::string& tableName,
        const std::vector<std::string_view>& usedKeys, bool generateWarningStrings);
=======
>>>>>>> origin/main
}

#endif // OPENMW_LUAUI_WIDGETLIST
