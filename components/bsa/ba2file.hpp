#ifndef OPENMW_COMPONENTS_BSA_BA2FILE_HPP
#define OPENMW_COMPONENTS_BSA_BA2FILE_HPP

#include <cstdint>
<<<<<<< HEAD
#include <string_view>

namespace Bsa
{
    uint32_t generateHash(std::string_view name);
=======
#include <string>

namespace Bsa
{
    uint32_t generateHash(const std::string& name);
>>>>>>> origin/main
    uint32_t generateExtensionHash(std::string_view extension);

    enum class BA2Version : std::uint32_t
    {
        Fallout4 = 1,
        StarfieldGeneral = 2,
        StarfieldDDS = 3,
        Fallout4NextGen_v7 = 7,
        Fallout4NextGen_v8 = 8,
    };
}

#endif
