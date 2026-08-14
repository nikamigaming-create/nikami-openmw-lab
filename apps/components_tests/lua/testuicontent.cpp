#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include <components/lua/luastate.hpp>
#include <components/lua_ui/content.hpp>

namespace
{
    using namespace testing;

    struct LuaUiContentTest : Test
    {
        LuaUtil::LuaState mLuaState{ nullptr, nullptr };
        sol::protected_function mNew;
        LuaUiContentTest()
        {
            mLuaState.addInternalLibSearchPath(
                std::filesystem::path{ OPENMW_PROJECT_SOURCE_DIR } / "components" / "lua_ui");
            mNew = LuaUi::loadContentConstructor(&mLuaState);
        }

        LuaUi::ContentView makeContent(sol::table source)
        {
            auto result = mNew.call(source);
            if (result.get_type() != sol::type::table)
                throw std::logic_error("Expected table");
            return LuaUi::ContentView(result.get<sol::table>());
        }

        sol::table makeTable() { return sol::table(mLuaState.unsafeState(), sol::create); }
<<<<<<< HEAD
=======

        sol::table makeTable(std::string name)
        {
            auto result = makeTable();
            result["name"] = name;
            return result;
        }
>>>>>>> origin/main
    };

    TEST_F(LuaUiContentTest, ProtectedMetatable)
    {
        sol::state_view sol = mLuaState.unsafeState();
        sol["makeContent"] = mNew;
<<<<<<< HEAD
=======
        sol["M"] = makeContent(makeTable()).getMetatable();
>>>>>>> origin/main
        std::string testScript = R"(
            assert(not pcall(function() setmetatable(makeContent{}, {}) end), 'Metatable is not protected')
            assert(getmetatable(makeContent{}) == false, 'Metatable is not protected')
        )";
        EXPECT_NO_THROW(sol.safe_script(testScript));
    }

<<<<<<< HEAD
    TEST_F(LuaUiContentTest, Insert)
    {
        mLuaState.protectedCall([&](LuaUtil::LuaView& state) {
            sol::state_view& sol = state.sol();
            sol["makeContent"] = mNew;
            EXPECT_NO_THROW(sol.safe_script(R"(
                local content = makeContent({ {}, {}, {} })
                content:insert(2, { name = 'inserted' })
                assert(#content == 4, 'Not inserted')
                local inserted = content:indexOf('inserted')
                local index = content:indexOf(content[inserted])
                assert(index ~= nil, 'Failed to find inserted')
                assert(index == 2, 'Inserted at the wrong index')
                )"));
        });
    }

    TEST_F(LuaUiContentTest, MakeHole)
    {
        mLuaState.protectedCall([&](LuaUtil::LuaView& state) {
            sol::state_view& sol = state.sol();
            sol["makeContent"] = mNew;
            EXPECT_NO_THROW(sol.safe_script(R"(
                local content = makeContent({ {}, {} })
                assert(not pcall(function() content[4] = {} end), 'Allowed to make hole')
                )"));
        });
    }

=======
>>>>>>> origin/main
    TEST_F(LuaUiContentTest, Create)
    {
        auto table = makeTable();
        table.add(makeTable());
        table.add(makeTable());
        table.add(makeTable());
        LuaUi::ContentView content = makeContent(table);
        EXPECT_EQ(content.size(), 3);
    }

<<<<<<< HEAD
=======
    TEST_F(LuaUiContentTest, Insert)
    {
        auto table = makeTable();
        table.add(makeTable());
        table.add(makeTable());
        table.add(makeTable());
        LuaUi::ContentView content = makeContent(table);
        content.insert(2, makeTable("inserted"));
        EXPECT_EQ(content.size(), 4);
        auto inserted = content.at("inserted");
        auto index = content.indexOf(inserted);
        EXPECT_TRUE(index.has_value());
        EXPECT_EQ(index.value(), 2);
    }

    TEST_F(LuaUiContentTest, MakeHole)
    {
        auto table = makeTable();
        table.add(makeTable());
        table.add(makeTable());
        LuaUi::ContentView content = makeContent(table);
        sol::table t = makeTable();
        EXPECT_ANY_THROW(content.assign(3, t));
    }

>>>>>>> origin/main
    TEST_F(LuaUiContentTest, WrongType)
    {
        auto table = makeTable();
        table.add(makeTable());
        table.add("a");
        table.add(makeTable());
        EXPECT_ANY_THROW(makeContent(table));
    }

    TEST_F(LuaUiContentTest, NameAccess)
    {
<<<<<<< HEAD
        mLuaState.protectedCall([&](LuaUtil::LuaView& state) {
            sol::state_view& sol = state.sol();
            sol["makeContent"] = mNew;
            EXPECT_NO_THROW(sol.safe_script(R"(
                local content = makeContent({ {}, { name = 'a' } })
                assert(content:indexOf('a') ~= nil, 'Could not find named table')
                content['a'] = nil
                assert(#content == 1, 'Failed to remove')
                content:add({ name = 'b' })
                content['b'] = {}
                assert(#content == 2, 'Failed to insert')
                content:add({ name = 'c' })
                content:add({ name = 'c' })
                content['c'] = nil
                assert(content:indexOf('c') == nil, 'Failed to remove value inserted twice'..#content)
                )"));
        });
=======
        auto table = makeTable();
        table.add(makeTable());
        table.add(makeTable("a"));
        LuaUi::ContentView content = makeContent(table);
        EXPECT_NO_THROW(content.at("a"));
        content.remove("a");
        EXPECT_EQ(content.size(), 1);
        content.assign(content.size(), makeTable("b"));
        content.assign("b", makeTable());
        EXPECT_ANY_THROW(content.at("b"));
        EXPECT_EQ(content.size(), 2);
        content.assign(content.size(), makeTable("c"));
        content.assign(content.size(), makeTable("c"));
        content.remove("c");
        EXPECT_ANY_THROW(content.at("c"));
>>>>>>> origin/main
    }

    TEST_F(LuaUiContentTest, IndexOf)
    {
<<<<<<< HEAD
        mLuaState.protectedCall([&](LuaUtil::LuaView& state) {
            sol::state_view& sol = state.sol();
            sol["makeContent"] = mNew;
            EXPECT_NO_THROW(sol.safe_script(R"(
                local content = makeContent({ {}, {}, {} })
                local child = {}
                content[3] = child
                assert(content:indexOf(child) == 3, 'Failed to assign')
                assert(content:indexOf({}) == nil, 'Found non-existent child')
                )"));
        });
=======
        auto table = makeTable();
        table.add(makeTable());
        table.add(makeTable());
        table.add(makeTable());
        LuaUi::ContentView content = makeContent(table);
        auto child = makeTable();
        content.assign(2, child);
        EXPECT_EQ(content.indexOf(child).value(), 2);
        EXPECT_TRUE(!content.indexOf(makeTable()).has_value());
>>>>>>> origin/main
    }

    TEST_F(LuaUiContentTest, BoundsChecks)
    {
<<<<<<< HEAD
        {
            auto table = makeTable();
            LuaUi::ContentView content = makeContent(table);
            EXPECT_ANY_THROW(content.at(0));
            EXPECT_EQ(content.size(), 0);
        }
        {
            auto table = makeTable();
            table[1] = makeTable();
            LuaUi::ContentView content = makeContent(table);
            EXPECT_EQ(content.size(), 1);
            EXPECT_ANY_THROW(content.at(1));
            content.at(0);
        }
=======
        auto table = makeTable();
        LuaUi::ContentView content = makeContent(table);
        EXPECT_ANY_THROW(content.at(0));
        EXPECT_EQ(content.size(), 0);
        content.assign(content.size(), makeTable());
        EXPECT_EQ(content.size(), 1);
        content.assign(content.size(), makeTable());
        EXPECT_EQ(content.size(), 2);
        content.assign(content.size(), makeTable());
        EXPECT_EQ(content.size(), 3);
        EXPECT_ANY_THROW(content.at(3));
        EXPECT_ANY_THROW(content.remove(3));
        content.remove(2);
        EXPECT_EQ(content.size(), 2);
        EXPECT_ANY_THROW(content.at(2));
>>>>>>> origin/main
    }
}
