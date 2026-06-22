#ifndef OPENMW_COMPONENTS_WIDGETS_MYGUICOMPAT_H
#define OPENMW_COMPONENTS_WIDGETS_MYGUICOMPAT_H

#include <MyGUI_InputManager.h>
#include <MyGUI_Prerequest.h>
#include <MyGUI_UString.h>

#include <string>
#include <string_view>

namespace Gui
{
#if MYGUI_VERSION < MYGUI_DEFINE_VERSION(3, 4, 3)
    using MyGUIStringParam = const std::string&;
#define OPENMW_MYGUI_RENDER_TARGET_CONST
#define OPENMW_MYGUI_VERTEX_FORMAT_CONST
#define OPENMW_MYGUI_CHECK_TEXTURE_OVERRIDE
#define OPENMW_MYGUI_REGISTER_SHADER_OVERRIDE
#define OPENMW_MYGUI_VERTEX_BUFFER_CONST
#define OPENMW_MYGUI_DATA_MANAGER_CONST
#define OPENMW_MYGUI_DATA_PATH_RETURN const std::string&
#define OPENMW_MYGUI_LOG_STRING_PARAM const std::string&
#define OPENMW_MYGUI_LOG_FILE_PARAM const char*
#define OPENMW_MYGUI_TEXTURE_CONST
#define OPENMW_MYGUI_SET_SHADER_OVERRIDE
#else
    using MyGUIStringParam = std::string_view;
#define OPENMW_MYGUI_RENDER_TARGET_CONST const
#define OPENMW_MYGUI_VERTEX_FORMAT_CONST const
#define OPENMW_MYGUI_CHECK_TEXTURE_OVERRIDE override
#define OPENMW_MYGUI_REGISTER_SHADER_OVERRIDE override
#define OPENMW_MYGUI_VERTEX_BUFFER_CONST const
#define OPENMW_MYGUI_DATA_MANAGER_CONST const
#define OPENMW_MYGUI_DATA_PATH_RETURN std::string
#define OPENMW_MYGUI_LOG_STRING_PARAM std::string_view
#define OPENMW_MYGUI_LOG_FILE_PARAM std::string_view
#define OPENMW_MYGUI_TEXTURE_CONST const
#define OPENMW_MYGUI_SET_SHADER_OVERRIDE override
#endif

    inline MyGUI::UString makeMyGUIUString(std::string_view value)
    {
        return MyGUI::UString(std::string(value));
    }

    inline MyGUI::UString makeMyGUIUString(const std::string& value)
    {
        return MyGUI::UString(value);
    }

    inline MyGUI::UString makeMyGUIUString(const char* value)
    {
        return MyGUI::UString(value);
    }

    inline const MyGUI::UString& makeMyGUIUString(const MyGUI::UString& value)
    {
        return value;
    }

    inline bool isMyGUIAltPressed()
    {
#if MYGUI_VERSION >= MYGUI_DEFINE_VERSION(3, 4, 3)
        return MyGUI::InputManager::getInstance().isAltPressed();
#else
        return false;
#endif
    }
}

#endif
