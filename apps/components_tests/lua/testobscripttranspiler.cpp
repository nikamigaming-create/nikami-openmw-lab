#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <components/lua/luastate.hpp>
#include <components/testing/expecterror.hpp>
#include <components/testing/util.hpp>

namespace
{
    using namespace testing;

    std::string readDataFile(const char* name)
    {
        const auto path = std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "files" / "data" / name;
        std::ifstream stream(path);
        if (!stream)
            throw std::runtime_error("test data file not found: " + path.string());
        std::stringstream buf;
        buf << stream.rdbuf();
        return buf.str();
    }

    constexpr VFS::Path::NormalizedView lexerPath("openmw_aux/lexer.lua");
    TestingOpenMW::VFSTestFile lexerFile(readDataFile("openmw_aux/lexer.lua"));

    constexpr VFS::Path::NormalizedView parserPath("openmw_aux/obscript/parser.lua");
    TestingOpenMW::VFSTestFile parserFile(readDataFile("openmw_aux/obscript/parser.lua"));

    constexpr VFS::Path::NormalizedView transpilerPath("openmw_aux/obscript/transpiler.lua");
    TestingOpenMW::VFSTestFile transpilerFile(readDataFile("openmw_aux/obscript/transpiler.lua"));

    constexpr VFS::Path::NormalizedView driverPath("obscript/transpilertests.lua");
    TestingOpenMW::VFSTestFile driverFile(R"X(
        local parser = require('openmw_aux.obscript.parser')
        local transpiler = require('openmw_aux.obscript.transpiler')
        return {
            transpile = function(text)
                return transpiler.transpile(parser.parse(text))
            end,
        }
        )X");

    struct ObScriptTranspilerTest : Test
    {
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({
            { lexerPath, &lexerFile },
            { parserPath, &parserFile },
            { transpilerPath, &transpilerFile },
            { driverPath, &driverFile },
        });

        LuaUtil::ScriptsConfiguration mCfg;
        LuaUtil::LuaState mLua{ mVFS.get(), &mCfg };

        std::string transpile(const std::string& text)
        {
            const VFS::Path::Normalized path(driverPath);
            sol::table script = mLua.runInNewSandbox(path);
            return LuaUtil::call(script["transpile"], text).get<std::string>();
        }
    };

    TEST_F(ObScriptTranspilerTest, Preamble)
    {
        const std::string lua = transpile("scn MyScript\n");
        EXPECT_THAT(lua, HasSubstr("-- transpiled from ObScript: MyScript"));
        EXPECT_THAT(lua, HasSubstr("local obs = require('openmw_aux.obscript.runtime')"));
        EXPECT_THAT(lua, HasSubstr("local S = obs.locals(\"MyScript\")"));
        EXPECT_THAT(lua, HasSubstr("return obs.makeLocalScript()"));
    }

    TEST_F(ObScriptTranspilerTest, BlockToHandler)
    {
        const std::string lua = transpile("scn S\nbegin GameMode\nend\nbegin MenuMode 1017\nend\n");
        EXPECT_THAT(lua, HasSubstr("obs.on(\"GameMode\", function()"));
        // block arguments follow the handler function
        EXPECT_THAT(lua, HasSubstr("obs.on(\"MenuMode\", function()"));
        EXPECT_THAT(lua, HasSubstr("end, 1017)"));
    }

    TEST_F(ObScriptTranspilerTest, LocalsVersusGlobals)
    {
        const std::string lua = transpile(
            "scn S\nshort MyVar\nbegin GameMode\n"
            "set MyVar to 1\nset OtherQuestVar to 2\nend\n");
        // declared locals live on S; unknown names go through the runtime
        EXPECT_THAT(lua, HasSubstr("S.MyVar = 1"));
        EXPECT_THAT(lua, HasSubstr("obs.setv(\"OtherQuestVar\", 2)"));
    }

    TEST_F(ObScriptTranspilerTest, DigitLedLocalGetsSafeIdentifier)
    {
        const std::string lua = transpile("scn S\nshort 2ndVar\nbegin GameMode\nset 2ndVar to 5\nend\n");
        EXPECT_THAT(lua, HasSubstr("S._2ndVar = 5"));
    }

    TEST_F(ObScriptTranspilerTest, CrossScriptVariables)
    {
        const std::string lua = transpile(
            "scn S\nshort x\nbegin GameMode\n"
            "set MyQuest.var to x\nset x to MyQuest.var\nend\n");
        EXPECT_THAT(lua, HasSubstr("obs.msetv(\"MyQuest\", \"var\", S.x)"));
        EXPECT_THAT(lua, HasSubstr("S.x = obs.mv(\"MyQuest\", \"var\")"));
    }

    TEST_F(ObScriptTranspilerTest, CallsFreeAndMember)
    {
        const std::string lua = transpile(
            "scn S\nbegin OnActivate\n"
            "ShowMessage SomeMsg\nplayer.AddItem Caps001 5\nSomeRef.Enable\nend\n");
        EXPECT_THAT(lua, HasSubstr("obs.f(\"ShowMessage\", \"SomeMsg\")"));
        EXPECT_THAT(lua, HasSubstr("obs.m(\"player\", \"AddItem\", \"Caps001\", 5)"));
        // zero-arg member command in statement position
        EXPECT_THAT(lua, HasSubstr("obs.m(\"SomeRef\", \"Enable\")"));
    }

    TEST_F(ObScriptTranspilerTest, RetailHarvestActivation)
    {
        const std::string lua = transpile(
            "scn BarrelCactusScript\n"
            "int State\n"
            "begin onActivate\n"
            "if State == 0 && GetActionRef == player\n"
            "player.additem NVFreshBarrelCactusFruit 1\n"
            "set State to 1\n"
            "endif\n"
            "end\n");
        EXPECT_THAT(lua, HasSubstr("obs.v(\"GetActionRef\") == obs.v(\"player\")"));
        EXPECT_THAT(lua, HasSubstr("obs.m(\"player\", \"additem\", \"NVFreshBarrelCactusFruit\", 1)"));
        EXPECT_THAT(lua, HasSubstr("S.State = 1"));
    }

    TEST_F(ObScriptTranspilerTest, IfChainAndTruthiness)
    {
        const std::string lua = transpile(
            "scn S\nshort x\nbegin GameMode\n"
            "if x == 1\nelseif x > 1 && x < 5\nelse\nendif\nend\n");
        EXPECT_THAT(lua, HasSubstr("if obs.b((S.x == 1)) then"));
        // &&/|| operands go through ObScript truthiness (nonzero)
        EXPECT_THAT(lua, HasSubstr("elseif obs.b((obs.b((S.x > 1)) and obs.b((S.x < 5)))) then"));
        EXPECT_THAT(lua, HasSubstr("else"));
    }

    TEST_F(ObScriptTranspilerTest, MissingComparisonOperand)
    {
        // `x >= 0 && <10`: vanilla evaluates the absent operand as 0
        const std::string lua = transpile("scn S\nshort x\nbegin GameMode\nif x >= 0 && <10\nendif\nend\n");
        EXPECT_THAT(lua, HasSubstr("(0 < 10)"));
    }

    TEST_F(ObScriptTranspilerTest, NumericLiterals)
    {
        const std::string lua = transpile("scn S\nfloat f\nbegin GameMode\nset f to .5 + 5. * -2\nend\n");
        EXPECT_THAT(lua, HasSubstr("(0.5 + (5.0 * -(2)))"));
    }

    TEST_F(ObScriptTranspilerTest, ReturnAndStray)
    {
        const std::string lua = transpile("scn S\nbegin GameMode\nreturn\nend\nDisable\n");
        EXPECT_THAT(lua, HasSubstr("do return end"));
        // statements outside any block collect into a __stray handler
        EXPECT_THAT(lua, HasSubstr("obs.on('__stray', function()"));
        EXPECT_THAT(lua, HasSubstr("obs.f(\"Disable\")"));
    }

    TEST_F(ObScriptTranspilerTest, OutputIsValidLua)
    {
        // the emitted source must itself load in the sandbox environment
        const std::string lua = transpile(
            "scn S\nshort x\nbegin GameMode\n"
            "if x >= 0 && <10\nset x to x + 1\nendif\nShowMessage Msg\nend\n");
        sol::state_view view(mLua.unsafeState());
        const sol::load_result loaded = view.load(lua);
        EXPECT_TRUE(loaded.valid());
    }
}
