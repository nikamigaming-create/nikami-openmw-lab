#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <components/lua/luastate.hpp>
#include <components/testing/util.hpp>

namespace
{
    constexpr VFS::Path::NormalizedView sActivationHandlersPath("scripts/omw/activationhandlers.lua");

    std::string readActivationHandlers()
    {
        const std::filesystem::path path = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files" / "data"
            / "scripts" / "omw" / "activationhandlers.lua";
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            throw std::runtime_error("Unable to read " + path.string());
        return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
    }

    class ActivationHandlersTest : public testing::Test
    {
    protected:
        TestingOpenMW::VFSTestFile mActivationHandlers{ readActivationHandlers() };
        std::unique_ptr<VFS::Manager> mVfs
            = TestingOpenMW::createTestVFS({ { sActivationHandlersPath, &mActivationHandlers } });
        LuaUtil::ScriptsConfiguration mConfiguration;
        LuaUtil::LuaState mLua{ mVfs.get(), &mConfiguration };
    };

    TEST_F(ActivationHandlersTest, Esm4DoorsDelegateToStandardActivationWithoutDirectLuaDoorActions)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::state_view lua = view.sol();

            int worldPausedCalls = 0;
            int standardActivationCalls = 0;
            int invisibilityRemovalCalls = 0;
            int isTeleportCalls = 0;
            int recordCalls = 0;
            int isOpenCalls = 0;
            int activateDoorCalls = 0;
            int destinationCalls = 0;
            int soundCalls = 0;
            int actorTeleportCalls = 0;

            sol::table sound = lua.create_table();
            sound["playSound3d"] = [&](sol::variadic_args) { ++soundCalls; };
            sol::table core = lua.create_table();
            core["sound"] = sound;

            sol::table doorType = lua.create_table();
            doorType["isTeleport"] = [&](sol::table door) {
                ++isTeleportCalls;
                return door["teleportShape"].get<bool>();
            };
            doorType["record"] = [&](sol::object) {
                ++recordCalls;
                return lua.create_table_with("openSound", "open", "closeSound", "close");
            };
            doorType["isOpen"] = [&](sol::object) {
                ++isOpenCalls;
                return false;
            };
            doorType["activateDoor"] = [&](sol::object) { ++activateDoorCalls; };
            const auto destination = [&](sol::object) {
                ++destinationCalls;
                return std::string("destination");
            };
            doorType["destCell"] = destination;
            doorType["destPosition"] = destination;
            doorType["destRotation"] = destination;

            sol::table effects = lua.create_table();
            effects["remove"] = [&](sol::object, std::string_view effect) {
                EXPECT_EQ(effect, "invisibility");
                ++invisibilityRemovalCalls;
            };
            sol::table actorType = lua.create_table();
            actorType["activeEffects"] = [effects](sol::object) { return effects; };

            sol::table types = lua.create_table();
            types["Actor"] = actorType;
            types["ESM4Book"] = lua.create_table();
            types["ESM4Door"] = doorType;
            types["Player"] = lua.create_table();

            sol::table world = lua.create_table();
            world["isWorldPaused"] = [&] {
                ++worldPausedCalls;
                return false;
            };
            world["_runStandardActivationAction"] = [&](sol::object, sol::object) {
                ++standardActivationCalls;
            };

            const std::map<std::string, sol::main_object> packages{
                { "openmw.core", core },
                { "openmw.types", types },
                { "openmw.world", world },
            };
            sol::table script = mLua.runInNewSandbox(
                VFS::Path::Normalized(sActivationHandlersPath), "activation-handlers-test", packages);
            sol::table engineHandlers = script["engineHandlers"];

            sol::table actor = lua.create_table();
            actor["teleport"] = [&](sol::variadic_args) { ++actorTeleportCalls; };
            for (const bool teleportShape : { false, true })
            {
                sol::table door = lua.create_table();
                door["id"] = teleportShape ? "teleport-door" : "ordinary-door";
                door["type"] = doorType;
                door["teleportShape"] = teleportShape;
                LuaUtil::call(engineHandlers["onActivate"], door, actor);
            }

            EXPECT_EQ(worldPausedCalls, 2);
            EXPECT_EQ(standardActivationCalls, 2);
            EXPECT_EQ(invisibilityRemovalCalls, 2);
            EXPECT_EQ(isTeleportCalls, 0);
            EXPECT_EQ(recordCalls, 0);
            EXPECT_EQ(isOpenCalls, 0);
            EXPECT_EQ(activateDoorCalls, 0);
            EXPECT_EQ(destinationCalls, 0);
            EXPECT_EQ(soundCalls, 0);
            EXPECT_EQ(actorTeleportCalls, 0);
        });
    }
}
