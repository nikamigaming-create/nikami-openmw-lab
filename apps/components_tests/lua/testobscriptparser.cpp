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

    // The tests run the real shipped modules, read from the source tree.
    constexpr VFS::Path::NormalizedView lexerPath("openmw_aux/lexer.lua");
    TestingOpenMW::VFSTestFile lexerFile(readDataFile("openmw_aux/lexer.lua"));

    constexpr VFS::Path::NormalizedView parserPath("openmw_aux/obscript/parser.lua");
    TestingOpenMW::VFSTestFile parserFile(readDataFile("openmw_aux/obscript/parser.lua"));

    constexpr VFS::Path::NormalizedView driverPath("obscript/parsertests.lua");
    TestingOpenMW::VFSTestFile driverFile(R"X(
        local parser = require('openmw_aux.obscript.parser')
        return {
            parse = parser.parse,
            tokenKinds = function(text)
                local kinds = {}
                for _, t in ipairs(parser.tokenize(text)) do
                    kinds[#kinds + 1] = t.kind
                end
                return table.concat(kinds, ' ')
            end,
        }
        )X");

    struct ObScriptParserTest : Test
    {
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({
            { lexerPath, &lexerFile },
            { parserPath, &parserFile },
            { driverPath, &driverFile },
        });

        LuaUtil::ScriptsConfiguration mCfg;
        LuaUtil::LuaState mLua{ mVFS.get(), &mCfg };

        sol::table parse(const std::string& text)
        {
            const VFS::Path::Normalized path(driverPath);
            sol::table script = mLua.runInNewSandbox(path);
            return LuaUtil::call(script["parse"], text).get<sol::table>();
        }
    };

    TEST_F(ObScriptParserTest, Tokenize)
    {
        const VFS::Path::Normalized path(driverPath);
        sol::table script = mLua.runInNewSandbox(path);
        // comments, junk decoration and whitespace are skipped;
        // keywords are recognized case-insensitively;
        // a NEWLINE + EOF pair is appended.
        EXPECT_EQ(LuaUtil::call(script["tokenKinds"], "SET x to 3.5 ; comment").get<std::string>(),
            "KEYWORD NAME KEYWORD FLOAT NEWLINE EOF");
        EXPECT_EQ(LuaUtil::call(script["tokenKinds"], "x != 1 !!!").get<std::string>(),
            "NAME OP INT NEWLINE EOF");
        // digit-led identifiers lex as names, not numbers
        EXPECT_EQ(LuaUtil::call(script["tokenKinds"], "12GaCoinShot .2ndFloor").get<std::string>(),
            "NAME OP NAME NEWLINE EOF");
    }

    TEST_F(ObScriptParserTest, ScriptHeader)
    {
        sol::table ast = parse("ScriptName MyScript trailing junk ignored\nshort a\nfloat b\n");
        EXPECT_EQ(ast["kind"].get<std::string>(), "Script");
        EXPECT_EQ(ast["name"].get<std::string>(), "MyScript");
        sol::table vars = ast["variables"];
        EXPECT_EQ(vars.size(), 2);
        EXPECT_EQ(vars[1].get<sol::table>()["type"].get<std::string>(), "short");
        EXPECT_EQ(vars[1].get<sol::table>()["name"].get<std::string>(), "a");
        EXPECT_EQ(vars[2].get<sol::table>()["type"].get<std::string>(), "float");
    }

    TEST_F(ObScriptParserTest, BlockArguments)
    {
        sol::table ast = parse("scn S\nbegin MenuMode 1017\nend\nbegin OnTriggerEnter player\nend\n");
        sol::table blocks = ast["blocks"];
        EXPECT_EQ(blocks.size(), 2);
        sol::table menuMode = blocks[1];
        EXPECT_EQ(menuMode["event"].get<std::string>(), "MenuMode");
        EXPECT_EQ(menuMode["args"].get<sol::table>()[1].get<sol::table>()["value"].get<int>(), 1017);
        sol::table trigger = blocks[2];
        EXPECT_EQ(trigger["args"].get<sol::table>()[1].get<sol::table>()["value"].get<std::string>(), "player");
    }

    TEST_F(ObScriptParserTest, IfElseChain)
    {
        // covers: elseif; the two-word 'else if' variant; junk after bare else
        sol::table ast = parse("scn S\nbegin GameMode\n"
                               "if x == 0\nelseif x == 1\nelse if x == 2\nelse junk ignored\nendif\n"
                               "end\n");
        sol::table ifStmt = ast["blocks"].get<sol::table>()[1].get<sol::table>()["body"].get<sol::table>()[1];
        EXPECT_EQ(ifStmt["kind"].get<std::string>(), "If");
        sol::table clauses = ifStmt["clauses"];
        EXPECT_EQ(clauses.size(), 4);
        EXPECT_EQ(clauses[3].get<sol::table>()["cond"].get<sol::table>()["kind"].get<std::string>(), "BinOp");
        const sol::object elseCond = clauses[4].get<sol::table>().get<sol::object>("cond");
        EXPECT_FALSE(elseCond.valid()); // the bare 'else' clause has no condition
    }

    TEST_F(ObScriptParserTest, StrayTerminators)
    {
        // vanilla tolerates unmatched endif/else/elseif; they parse as no-ops
        sol::table ast = parse("scn S\nbegin GameMode\nendif\nelse\nelseif garbage\nend\n");
        sol::table body = ast["blocks"].get<sol::table>()[1].get<sol::table>()["body"];
        EXPECT_EQ(body.size(), 3);
        EXPECT_EQ(body[1].get<sol::table>()["kind"].get<std::string>(), "StrayKeyword");
        EXPECT_EQ(body[1].get<sol::table>()["keyword"].get<std::string>(), "endif");
        EXPECT_EQ(body[3].get<sol::table>()["keyword"].get<std::string>(), "elseif");
    }

    TEST_F(ObScriptParserTest, RefSetStatement)
    {
        // `SomeRef.Set Var to Expr` - keyword used as member name
        sol::table ast = parse("scn S\nbegin GameMode\nMyRef.Set Foo to 1 + 2\nend\n");
        sol::table stmt = ast["blocks"].get<sol::table>()[1].get<sol::table>()["body"].get<sol::table>()[1];
        EXPECT_EQ(stmt["kind"].get<std::string>(), "Set");
        sol::table target = stmt["target"];
        EXPECT_EQ(target["kind"].get<std::string>(), "Member");
        EXPECT_EQ(target["base"].get<sol::table>()["value"].get<std::string>(), "MyRef");
        EXPECT_EQ(target["member"].get<sol::table>()["value"].get<std::string>(), "Foo");
        EXPECT_EQ(stmt["value"].get<sol::table>()["op"].get<std::string>(), "+");
    }

    TEST_F(ObScriptParserTest, CommaSeparatedArguments)
    {
        // commas may separate bare arguments: PlaceAtMe Foo 1, 0, 0
        sol::table ast = parse("scn S\nbegin GameMode\nPlaceAtMe SomeCreature 1, 0, 0\nend\n");
        sol::table stmt = ast["blocks"].get<sol::table>()[1].get<sol::table>()["body"].get<sol::table>()[1];
        EXPECT_EQ(stmt["kind"].get<std::string>(), "ExprStatement");
        sol::table call = stmt["expr"];
        EXPECT_EQ(call["kind"].get<std::string>(), "Call");
        EXPECT_EQ(call["args"].get<sol::table>().size(), 4);
    }

    TEST_F(ObScriptParserTest, BareExpressionStatement)
    {
        // an expression alone on a line is a valid (no-op) statement
        sol::table ast = parse("scn S\nbegin GameMode\ngetfurnituremarkerid == 26\nend\n");
        sol::table stmt = ast["blocks"].get<sol::table>()[1].get<sol::table>()["body"].get<sol::table>()[1];
        EXPECT_EQ(stmt["kind"].get<std::string>(), "ExprStatement");
        EXPECT_EQ(stmt["expr"].get<sol::table>()["op"].get<std::string>(), "==");
    }

    TEST_F(ObScriptParserTest, JunkAndSeparatorLines)
    {
        // decorative separator lines and junk characters are ignored
        sol::table ast = parse("scn S\nbegin GameMode\n=====\n-----\n` ! @ # [ ] ~\nEnable\nend\n");
        sol::table body = ast["blocks"].get<sol::table>()[1].get<sol::table>()["body"];
        int junk = 0, exprs = 0;
        for (const auto& [_, stmt] : body)
        {
            std::string kind = stmt.as<sol::table>()["kind"].get<std::string>();
            if (kind == "JunkLine")
                ++junk;
            else if (kind == "ExprStatement")
                ++exprs;
        }
        EXPECT_EQ(junk, 2); // '=====' and '-----'; the pure-junk-character line lexes to nothing
        EXPECT_EQ(exprs, 1); // Enable
    }

    TEST_F(ObScriptParserTest, OperatorPrecedence)
    {
        sol::table ast = parse("scn S\nbegin GameMode\nset x to 1 + 2 * 3\nend\n");
        sol::table value = ast["blocks"].get<sol::table>()[1].get<sol::table>()["body"].get<sol::table>()[1]
                               .get<sol::table>()["value"];
        EXPECT_EQ(value["op"].get<std::string>(), "+");
        EXPECT_EQ(value["right"].get<sol::table>()["op"].get<std::string>(), "*");
        EXPECT_EQ(value["right"].get<sol::table>()["left"].get<sol::table>()["value"].get<int>(), 2);
    }

    TEST_F(ObScriptParserTest, ErrorHandling)
    {
        const VFS::Path::Normalized path(driverPath);
        sol::table script = mLua.runInNewSandbox(path);
        EXPECT_ERROR(LuaUtil::call(script["parse"], "scn\n"), "line 1: expected NAME");
    }
}
