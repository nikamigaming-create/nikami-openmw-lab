#include <gtest/gtest.h>

#include <array>
#include <string>

#include "apps/openmw/mwgui/gamecontent.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

namespace
{
    TEST(GameContentTest, UsesFalloutInterfaceOnlyForNewVegas)
    {
        using MWWorld::ESM4Game;

        EXPECT_FALSE(MWGui::usesFalloutNewVegasInterface(ESM4Game::Unknown));
        EXPECT_FALSE(MWGui::usesFalloutNewVegasInterface(ESM4Game::Oblivion));
        EXPECT_FALSE(MWGui::usesFalloutNewVegasInterface(ESM4Game::Fallout3));
        EXPECT_TRUE(MWGui::usesFalloutNewVegasInterface(ESM4Game::FalloutNewVegas));
        EXPECT_FALSE(MWGui::usesFalloutNewVegasInterface(ESM4Game::Skyrim));
        EXPECT_FALSE(MWGui::usesFalloutNewVegasInterface(ESM4Game::Fallout4));
        EXPECT_FALSE(MWGui::usesFalloutNewVegasInterface(ESM4Game::Starfield));
    }

    TEST(GameContentTest, UsesSafeDefaultBeforeWorldInitialization)
    {
        EXPECT_FALSE(MWGui::usesFalloutNewVegasInterface());
    }

    TEST(GameContentTest, DetectsStartupGameFromOrderedContent)
    {
        const std::array<std::string, 3> content{ "builtin.omwscripts", "mods/example.omwaddon", "Data/FaLlOuTnV.EsM" };

        EXPECT_EQ(MWWorld::detectESM4Game(content), MWWorld::ESM4Game::FalloutNewVegas);
    }

    TEST(GameContentTest, DetectsSupportedBaseMasterNamesCaseInsensitively)
    {
        using MWWorld::ESM4Game;

        EXPECT_EQ(MWWorld::detectESM4Game("Oblivion.esm"), ESM4Game::Oblivion);
        EXPECT_EQ(MWWorld::detectESM4Game("fallout3.esm"), ESM4Game::Fallout3);
        EXPECT_EQ(MWWorld::detectESM4Game("FALLOUTNV.ESM"), ESM4Game::FalloutNewVegas);
        EXPECT_EQ(MWWorld::detectESM4Game("SkyrimVR.esm"), ESM4Game::Skyrim);
        EXPECT_EQ(MWWorld::detectESM4Game("fallout4.esm"), ESM4Game::Fallout4);
        EXPECT_EQ(MWWorld::detectESM4Game("Starfield.esm"), ESM4Game::Starfield);
        EXPECT_EQ(MWWorld::detectESM4Game("example.esp"), ESM4Game::Unknown);
    }

    TEST(GameContentTest, UsesFirstRecognizedBaseGame)
    {
        const std::array<std::string, 3> content{ "Skyrim.esm", "FalloutNV.esm", "example.esp" };

        EXPECT_EQ(MWWorld::detectESM4Game(content), MWWorld::ESM4Game::Skyrim);
    }

    TEST(GameContentTest, ReturnsUnknownWithoutRecognizedBaseGame)
    {
        const std::array<std::string, 3> content{ "builtin.omwscripts", "Morrowind.esm", "example.esp" };

        EXPECT_EQ(MWWorld::detectESM4Game(content), MWWorld::ESM4Game::Unknown);
    }
}
