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
            transpileRegistration = function(text)
                return transpiler.transpileRegistration(parser.parse(text))
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

        std::string transpileRegistration(const std::string& text)
        {
            const VFS::Path::Normalized path(driverPath);
            sol::table script = mLua.runInNewSandbox(path);
            return LuaUtil::call(script["transpileRegistration"], text).get<std::string>();
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

    TEST_F(ObScriptTranspilerTest, RegistrationFormHasNoPerScriptFooter)
    {
        const std::string lua
            = transpileRegistration("scn SharedUdf\nbegin Function { value }\nSetFunctionValue value\nend\n");
        EXPECT_THAT(lua, HasSubstr("obs.udf(\"SharedUdf\""));
        EXPECT_THAT(lua, Not(HasSubstr("return obs.makeLocalScript()")));
        sol::state_view view(mLua.unsafeState());
        EXPECT_TRUE(view.load(lua).valid());
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
        EXPECT_THAT(lua, HasSubstr("S.myvar = 1"));
        EXPECT_THAT(lua, HasSubstr("obs.setv(\"OtherQuestVar\", 2)"));
    }

    TEST_F(ObScriptTranspilerTest, DigitLedLocalGetsSafeIdentifier)
    {
        const std::string lua = transpile("scn S\nshort 2ndVar\nbegin GameMode\nset 2ndVar to 5\nend\n");
        EXPECT_THAT(lua, HasSubstr("S._2ndvar = 5"));
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
        EXPECT_THAT(lua, HasSubstr("obs.f(\"ShowMessage\", obs.arg(\"SomeMsg\"))"));
        EXPECT_THAT(lua, HasSubstr("obs.m(\"player\", \"AddItem\", obs.arg(\"Caps001\"), 5)"));
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
        EXPECT_THAT(lua,
            HasSubstr("obs.boolnum(obs.eq(obs.v(\"GetActionRef\"), obs.v(\"player\")))"));
        EXPECT_THAT(lua,
            HasSubstr("obs.m(\"player\", \"additem\", obs.arg(\"NVFreshBarrelCactusFruit\"), 1)"));
        EXPECT_THAT(lua, HasSubstr("S.state = 1"));
    }

    TEST_F(ObScriptTranspilerTest, RetailGoodspringsTriggerRetainsPlayerFilterAndQuestMutation)
    {
        const std::string lua = transpile(
            "scn GSDocMitchellExitTriggerScript\n"
            "begin OnTriggerEnter player\n"
            "if GetStage VCG01 == 110\n"
            "SetStage VCG01 115\n"
            "endif\n"
            "end\n");
        EXPECT_THAT(lua, HasSubstr("obs.on(\"OnTriggerEnter\", function()"));
        EXPECT_THAT(lua, HasSubstr("end, obs.arg(\"player\"))"));
        EXPECT_THAT(lua,
            HasSubstr("obs.boolnum(obs.eq(obs.f(\"GetStage\", obs.arg(\"VCG01\")), 110))"));
        EXPECT_THAT(lua, HasSubstr("obs.f(\"SetStage\", obs.arg(\"VCG01\"), 115)"));
    }

    TEST_F(ObScriptTranspilerTest, IfChainAndTruthiness)
    {
        const std::string lua = transpile(
            "scn S\nshort x\nbegin GameMode\n"
            "if x == 1\nelseif x > 1 && x < 5\nelse\nendif\nend\n");
        EXPECT_THAT(lua, HasSubstr("if obs.b(obs.boolnum(obs.eq(S.x, 1))) then"));
        // &&/|| operands go through ObScript truthiness (nonzero), preserve
        // short-circuiting, and return the numeric 0/1 that ObScript expects.
        EXPECT_THAT(lua,
            HasSubstr("elseif obs.b(obs.boolnum(obs.b(obs.boolnum((S.x > 1))) and "
                      "obs.b(obs.boolnum((S.x < 5))))) then"));
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

    TEST_F(ObScriptTranspilerTest, WorldToScreenPreservesOutputArguments)
    {
        const std::string lua = transpile(
            "scn JVOCoordinates\n"
            "float fX\nfloat fY\nfloat fZ\nfloat dX\nfloat dY\nfloat dZ\nref target\n"
            "begin Function {}\n"
            "WorldToScreen fX fY fZ dX dY dZ 2 target\n"
            "end\n");
        EXPECT_THAT(lua,
            HasSubstr("obs.f(\"WorldToScreen\", obs.out(S, \"fx\"), "
                      "obs.out(S, \"fy\"), obs.out(S, \"fz\"), "
                      "S.dx, S.dy, S.dz, 2, S.target)"));
    }

    TEST_F(ObScriptTranspilerTest, ParenthesizedCommandArgumentDoesNotConsumeFollowingArguments)
    {
        const std::string lua = transpile(
            "scn JDCMainLoopEventHandler\n"
            "float fPlayerSpread\n"
            "float fWeaponSpread\n"
            "float fSpreadDegrees\n"
            "begin Function {}\n"
            "set fSpreadDegrees to Clamp "
            "(5.955 * (2.0 * fPlayerSpread + 0.2 * fWeaponSpread "
            "+ 0.024 * fWeaponSpread * fWeaponSpread)) 0 89.9\n"
            "end\n");
        EXPECT_THAT(lua,
            HasSubstr("obs.f(\"Clamp\", "
                      "(5.955 * (((2.0 * S.fplayerspread) + (0.2 * S.fweaponspread)) "
                      "+ ((0.024 * S.fweaponspread) * S.fweaponspread))), 0, 89.9)"));
        EXPECT_THAT(lua, Not(HasSubstr("obs.fx(")));
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

    TEST_F(ObScriptTranspilerTest, XnvseLoopsArraysStringsAndBits)
    {
        const std::string lua = transpile(
            "scn JAM\n"
            "array_var values\n"
            "string_var label\n"
            "int i\n"
            "begin GameMode\n"
            "values[i] = 0b101\n"
            "label += \"value=\" + $values[i]\n"
            "while i < 3\n"
            "eval i += 1\n"
            "if i & 1\n"
            "continue\n"
            "endif\n"
            "loop\n"
            "end\n");
        EXPECT_THAT(lua, HasSubstr("obs.setindex(S.values, S.i, 5)"));
        EXPECT_THAT(lua, HasSubstr("obs.add(S.label"));
        EXPECT_THAT(lua, HasSubstr("while obs.b(obs.boolnum((S.i < 3))) do"));
        EXPECT_THAT(lua, HasSubstr("obs.bit(\"&\", S.i, 1)"));

        sol::state_view view(mLua.unsafeState());
        const sol::load_result loaded = view.load(lua);
        EXPECT_TRUE(loaded.valid());
    }

    TEST_F(ObScriptTranspilerTest, XnvseNamedAndAnonymousUdfs)
    {
        const std::string lua = transpile(
            "scn JamHandler\n"
            "ref callback\n"
            "int value\n"
            "begin Function {value}\n"
            "SetFunctionValue value\n"
            "end\n"
            "begin GameMode\n"
            "callback = (begin function {value}\n"
            "SetFunctionValue value + 1\n"
            "end)\n"
            "Call callback 7\n"
            "end\n");
        EXPECT_THAT(lua, HasSubstr("obs.udf(\"JamHandler\", function(__obsArg1)"));
        EXPECT_THAT(lua, HasSubstr("obs.lambda(\"JamHandler#lambda1\", function(__obsArg1)"));
        EXPECT_THAT(lua, HasSubstr("obs.f(\"Call\", S.callback, 7)"));

        sol::state_view view(mLua.unsafeState());
        const sol::load_result loaded = view.load(lua);
        EXPECT_TRUE(loaded.valid());
    }

    TEST_F(ObScriptTranspilerTest, XnvseArrowLambdaAndPair)
    {
        const std::string lua = transpile(
            "scn JAM\n"
            "ref callback\n"
            "int refresh\n"
            "array_var event\n"
            "begin GameMode\n"
            "callback = ({} => refresh = 1)\n"
            "event = ar_Map \"CurrentItem\"::(refresh)\n"
            "end\n");
        EXPECT_THAT(lua, HasSubstr("obs.lambda(\"JAM#lambda1\", function()"));
        EXPECT_THAT(lua, HasSubstr("obs.pair(\"CurrentItem\", S.refresh)"));

        sol::state_view view(mLua.unsafeState());
        const sol::load_result loaded = view.load(lua);
        EXPECT_TRUE(loaded.valid());
    }
}
