#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPENNVCOMPATIBILITY_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_OPENNVCOMPATIBILITY_H

#include <components/settings/settingvalue.hpp>

#include <string>

namespace Settings
{
    struct OpenNVCompatibilityCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<std::string> mScriptCommandMappings{
            mIndex, "OpenNV Compatibility", "script command mappings" };
        SettingValue<bool> mDeferLoadingInputUpdate{
            mIndex, "OpenNV Compatibility", "defer loading input update" };
    };
}

#endif
