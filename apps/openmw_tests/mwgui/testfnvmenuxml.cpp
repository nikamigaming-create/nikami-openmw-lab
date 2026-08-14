#include <gtest/gtest.h>

#include "apps/openmw/mwgui/fnvmenuxml.hpp"

TEST(FnvMenuXmlTest, PreservesBethesdaEntitiesIncludesAndTraitExpressions)
{
    constexpr std::string_view source = R"xml(
        <!-- authored menu -->
        <menu name="ComputersMenu">
            <stackingtype> &no_click_past; </stackingtype>
            <rect name="computers_depth_rect">
                <height><copy src="screen()" trait="height"/><mult>0.75</mult></height>
                <include src="list_box.xml"/>
            </rect>
            <template name="computers_file_template"/>
        </menu>)xml";

    MWGui::FnvMenuXmlParseError error = MWGui::FnvMenuXmlParseError::UnexpectedToken;
    const std::optional<MWGui::FnvMenuXmlDocument> document = MWGui::parseFnvMenuXml(source, &error);
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(error, MWGui::FnvMenuXmlParseError::None);
    EXPECT_EQ(document->mRoot.mType, "menu");
    EXPECT_EQ(document->mRoot.getAttribute("name"), "ComputersMenu");
    ASSERT_EQ(document->mRoot.findChildren("rect").size(), 1u);
    const MWGui::FnvMenuXmlNode& rect = *document->mRoot.findChild("rect");
    EXPECT_EQ(rect.getAttribute("name"), "computers_depth_rect");
    ASSERT_NE(rect.findChild("height"), nullptr);
    EXPECT_EQ(rect.findChild("height")->findChild("copy")->getAttribute("src"), "screen()");
    EXPECT_EQ(rect.findChild("height")->findChild("mult")->mText, "0.75");
    EXPECT_EQ(rect.findChild("include")->getAttribute("src"), "list_box.xml");
}

TEST(FnvMenuXmlTest, RejectsMalformedOrMultipleRootDocuments)
{
    MWGui::FnvMenuXmlParseError error = MWGui::FnvMenuXmlParseError::None;
    EXPECT_FALSE(MWGui::parseFnvMenuXml("<menu><rect></menu>", &error));
    EXPECT_EQ(error, MWGui::FnvMenuXmlParseError::MismatchedClosingElement);
    EXPECT_FALSE(MWGui::parseFnvMenuXml("<menu/><menu/>", &error));
    EXPECT_EQ(error, MWGui::FnvMenuXmlParseError::MultipleRoots);
}

TEST(FnvMenuXmlTest, ExpandsBethesdaPrefabFragmentsWithoutMenuSpecificKnowledge)
{
    const std::optional<MWGui::FnvMenuXmlDocument> parsed
        = MWGui::parseFnvMenuXml("<menu><rect><include src=\"list_box.xml\"/></rect></menu>");
    ASSERT_TRUE(parsed.has_value());
    MWGui::FnvMenuXmlDocument document = *parsed;
    MWGui::FnvMenuXmlParseError error = MWGui::FnvMenuXmlParseError::UnexpectedToken;
    ASSERT_TRUE(MWGui::expandFnvMenuXmlIncludes(document.mRoot,
        [](std::string_view name) -> std::optional<std::string> {
            if (name == "list_box.xml")
                return "<locus>&true;</locus><hotrect name=\"highlight\"><include src=\"box.xml\"/></hotrect>";
            if (name == "box.xml")
                return "<alpha>40</alpha><target>&false;</target>";
            return std::nullopt;
        },
        &error));
    EXPECT_EQ(error, MWGui::FnvMenuXmlParseError::None);
    const MWGui::FnvMenuXmlNode& rect = *document.mRoot.findChild("rect");
    EXPECT_EQ(rect.findChild("locus")->mText, "&true;");
    const MWGui::FnvMenuXmlNode& hotrect = *rect.findChild("hotrect");
    EXPECT_EQ(hotrect.findChild("alpha")->mText, "40");
    EXPECT_EQ(hotrect.findChild("target")->mText, "&false;");
}

TEST(FnvMenuXmlTest, AcceptsUtf8BomOnBethesdaPrefabFragments)
{
    MWGui::FnvMenuXmlParseError error = MWGui::FnvMenuXmlParseError::UnexpectedToken;
    const std::optional<std::vector<MWGui::FnvMenuXmlNode>> fragment
        = MWGui::parseFnvMenuXmlFragment("\xEF\xBB\xBF<!-- authored prefab --><width>60</width>", &error);
    ASSERT_TRUE(fragment.has_value());
    ASSERT_EQ(fragment->size(), 1u);
    EXPECT_EQ(fragment->front().mType, "width");
    EXPECT_EQ(fragment->front().mText, "60");
    EXPECT_EQ(error, MWGui::FnvMenuXmlParseError::None);
}

TEST(FnvMenuXmlTest, AcceptsShippedGeckMissingOpenTagBracket)
{
    MWGui::FnvMenuXmlParseError error = MWGui::FnvMenuXmlParseError::UnexpectedToken;
    const std::optional<std::vector<MWGui::FnvMenuXmlNode>> fragment
        = MWGui::parseFnvMenuXmlFragment("<systemcolor</systemcolor><locus>&true;</locus>", &error);
    ASSERT_TRUE(fragment.has_value());
    ASSERT_EQ(fragment->size(), 2u);
    EXPECT_EQ(fragment->front().mType, "systemcolor");
    EXPECT_TRUE(fragment->front().mText.empty());
    EXPECT_EQ(error, MWGui::FnvMenuXmlParseError::None);
}

TEST(FnvMenuXmlTest, EvaluatesAuthoredScalarTraitOperationSequences)
{
    const std::optional<MWGui::FnvMenuXmlDocument> parsed = MWGui::parseFnvMenuXml(R"xml(
        <height>
            <copy src="screen()" trait="height"/>
            <mult>0.75</mult>
            <onlyif>&highdef;</onlyif>
        </height>)xml");
    ASSERT_TRUE(parsed.has_value());
    const MWGui::FnvMenuTraitEvaluationContext context{
        [](std::string_view source, std::string_view trait) -> std::optional<float> {
            return source == "screen()" && trait == "height" ? std::optional<float>(1080.f) : std::nullopt;
        },
        [](std::string_view symbol) -> std::optional<float> {
            return symbol == "highdef" ? std::optional<float>(1.f) : std::nullopt;
        },
    };
    EXPECT_EQ(MWGui::evaluateFnvMenuScalarTrait(parsed->mRoot, context), 810.f);

    const std::optional<MWGui::FnvMenuXmlDocument> conditional = MWGui::parseFnvMenuXml(R"xml(
        <brightness>
            <copy>255</copy><onlyif>&highdef;</onlyif>
            <add><copy>175</copy><onlyifnot>&highdef;</onlyifnot></add>
        </brightness>)xml");
    ASSERT_TRUE(conditional.has_value());
    EXPECT_EQ(MWGui::evaluateFnvMenuScalarTrait(conditional->mRoot, context), 255.f);

    const std::optional<MWGui::FnvMenuXmlDocument> inverted
        = MWGui::parseFnvMenuXml("<visible><not src=\"screen()\" trait=\"height\"/></visible>");
    ASSERT_TRUE(inverted.has_value());
    EXPECT_EQ(MWGui::evaluateFnvMenuScalarTrait(inverted->mRoot, context), 0.f);

    const std::optional<MWGui::FnvMenuXmlDocument> bounds
        = MWGui::parseFnvMenuXml("<x><copy>17</copy><mod>10</mod><min>9</min><max>8</max></x>");
    ASSERT_TRUE(bounds.has_value());
    EXPECT_EQ(MWGui::evaluateFnvMenuScalarTrait(bounds->mRoot, context), 8.f);
}

TEST(FnvMenuXmlTest, ResolvesNamedTileDependenciesForAuthoredScreenGeometry)
{
    const std::optional<MWGui::FnvMenuXmlDocument> parsed = MWGui::parseFnvMenuXml(R"xml(
        <menu name="ComputersMenu">
            <rect name="computers_depth_rect">
                <height><copy src="screen()" trait="height"/><mult>0.75</mult></height>
                <width><copy src="me()" trait="height"/><mult>4</mult><div>3</div></width>
                <text name="child"><x>100</x><y><copy src="parent()" trait="height"/><sub>30</sub></y></text>
            </rect>
        </menu>)xml");
    ASSERT_TRUE(parsed.has_value());
    const MWGui::FnvMenuLayoutEvaluationContext context{ 1920.f, 1080.f };
    EXPECT_EQ(MWGui::evaluateFnvMenuNamedScalarTrait(
                  parsed->mRoot, "computers_depth_rect", "height", context),
        810.f);
    EXPECT_EQ(MWGui::evaluateFnvMenuNamedScalarTrait(
                  parsed->mRoot, "computers_depth_rect", "width", context),
        1080.f);
    EXPECT_EQ(MWGui::evaluateFnvMenuNamedScalarTrait(parsed->mRoot, "child", "y", context), 780.f);
}

TEST(FnvMenuXmlTest, EvaluatesAnExactAuthoredNodeWithoutDependingOnUniqueNames)
{
    const std::optional<MWGui::FnvMenuXmlDocument> parsed = MWGui::parseFnvMenuXml(
        "<menu><rect name=\"duplicate\"><x>10</x></rect><rect name=\"duplicate\"><x>20</x></rect></menu>");
    ASSERT_TRUE(parsed);
    const MWGui::FnvMenuLayoutEvaluationContext context{ 960.f, 720.f, {} };
    ASSERT_EQ(parsed->mRoot.mChildren.size(), 2u);
    EXPECT_EQ(MWGui::evaluateFnvMenuNodeScalarTrait(
                  parsed->mRoot, parsed->mRoot.mChildren[1], "x", context),
        20.f);
}

TEST(FnvMenuXmlTest, ResolvesRetailGlobalsThroughTheDynamicTraitContract)
{
    const std::optional<MWGui::FnvMenuXmlDocument> parsed = MWGui::parseFnvMenuXml(
        "<menu><image name=\"background\"><width><copy src=\"globals()\" trait=\"_pipboy_width\"/></width></image></menu>");
    ASSERT_TRUE(parsed);
    const MWGui::FnvMenuLayoutEvaluationContext context{ 960.f, 720.f, {},
        [](std::string_view node, std::string_view trait) -> std::optional<float> {
            return node == "globals()" && trait == "_pipboy_width" ? std::optional<float>(960.f) : std::nullopt;
        } };
    EXPECT_EQ(MWGui::evaluateFnvMenuNamedScalarTrait(parsed->mRoot, "background", "width", context), 960.f);
}

TEST(FnvMenuXmlTest, LocalTraitsOverrideExpandedPrefabTraits)
{
    const std::optional<MWGui::FnvMenuXmlDocument> parsed = MWGui::parseFnvMenuXml(R"xml(
        <menu><hotrect name="IM_InventoryList"><include src="list_box.xml"/><width>400</width></hotrect></menu>)xml");
    ASSERT_TRUE(parsed.has_value());
    MWGui::FnvMenuXmlDocument document = *parsed;
    ASSERT_TRUE(MWGui::expandFnvMenuXmlIncludes(document.mRoot,
        [](std::string_view name) -> std::optional<std::string> {
            return name == "list_box.xml" ? std::optional<std::string>("<width>300</width><height>250</height>")
                                          : std::nullopt;
        }));
    const MWGui::FnvMenuLayoutEvaluationContext context{ 960.f, 720.f };
    EXPECT_EQ(MWGui::evaluateFnvMenuNamedScalarTrait(document.mRoot, "IM_InventoryList", "width", context), 400.f);
    EXPECT_EQ(MWGui::evaluateFnvMenuNamedScalarTrait(document.mRoot, "IM_InventoryList", "height", context), 250.f);
}

TEST(FnvMenuXmlTest, NormalizesAuthoredTextureNamesIntoTheFalloutVfsNamespace)
{
    EXPECT_EQ(MWGui::normalizeFnvMenuTexturePath(" Interface\\Shared\\Background\\pipboy.dds "),
        "textures\\Interface\\Shared\\Background\\pipboy.dds");
    EXPECT_EQ(MWGui::normalizeFnvMenuTexturePath("textures/interface/shared/background/pipboy.dds"),
        "textures\\interface\\shared\\background\\pipboy.dds");
    EXPECT_TRUE(MWGui::normalizeFnvMenuTexturePath(" \r\n\t ").empty());
}
