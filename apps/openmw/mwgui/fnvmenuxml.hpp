#ifndef OPENMW_MWGUI_FNVMENUXML_H
#define OPENMW_MWGUI_FNVMENUXML_H

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MWGui
{
    struct FnvHackingMenuTarget
    {
        std::size_t mBegin = 0;
        std::size_t mEnd = 0;
    };

    struct FnvHackingMenuPresentation
    {
        std::vector<std::string> mRows;
        std::vector<FnvHackingMenuTarget> mTargets;
        std::string mLog;
    };

    struct FnvMenuXmlNode
    {
        std::string mType;
        std::vector<std::pair<std::string, std::string>> mAttributes;
        std::string mText;
        std::vector<FnvMenuXmlNode> mChildren;

        [[nodiscard]] std::string_view getAttribute(std::string_view name) const;
        [[nodiscard]] const FnvMenuXmlNode* findChild(std::string_view type) const;
        [[nodiscard]] std::vector<const FnvMenuXmlNode*> findChildren(std::string_view type) const;
        [[nodiscard]] const FnvMenuXmlNode* findDescendantByName(std::string_view name) const;
    };

    enum class FnvMenuXmlParseError
    {
        None,
        EmptyDocument,
        UnexpectedToken,
        InvalidName,
        InvalidAttribute,
        UnterminatedComment,
        UnterminatedElement,
        MismatchedClosingElement,
        MultipleRoots,
    };

    struct FnvMenuXmlDocument
    {
        FnvMenuXmlNode mRoot;
    };

    [[nodiscard]] std::optional<FnvMenuXmlDocument> parseFnvMenuXml(
        std::string_view xml, FnvMenuXmlParseError* error = nullptr);
    [[nodiscard]] std::optional<std::vector<FnvMenuXmlNode>> parseFnvMenuXmlFragment(
        std::string_view xml, FnvMenuXmlParseError* error = nullptr);
    using FnvMenuXmlIncludeLoader = std::function<std::optional<std::string>(std::string_view)>;
    [[nodiscard]] bool expandFnvMenuXmlIncludes(FnvMenuXmlNode& root,
        const FnvMenuXmlIncludeLoader& loader, FnvMenuXmlParseError* error = nullptr);

    struct FnvMenuTraitEvaluationContext
    {
        std::function<std::optional<float>(std::string_view, std::string_view)> mTraitResolver;
        std::function<std::optional<float>(std::string_view)> mSymbolResolver;
    };

    [[nodiscard]] std::optional<float> evaluateFnvMenuScalarTrait(
        const FnvMenuXmlNode& trait, const FnvMenuTraitEvaluationContext& context);

    struct FnvMenuLayoutEvaluationContext
    {
        float mScreenWidth = 0.f;
        float mScreenHeight = 0.f;
        std::function<std::optional<float>(std::string_view)> mSymbolResolver;
        std::function<std::optional<float>(std::string_view, std::string_view)> mDynamicTraitResolver;
    };

    [[nodiscard]] std::optional<float> evaluateFnvMenuNamedScalarTrait(const FnvMenuXmlNode& root,
        std::string_view nodeName, std::string_view traitName, const FnvMenuLayoutEvaluationContext& context);
    [[nodiscard]] std::optional<float> evaluateFnvMenuNodeScalarTrait(const FnvMenuXmlNode& root,
        const FnvMenuXmlNode& node, std::string_view traitName, const FnvMenuLayoutEvaluationContext& context);
    [[nodiscard]] std::string normalizeFnvMenuTexturePath(std::string_view path);
    [[nodiscard]] std::string_view getFnvMenuXmlParseErrorName(FnvMenuXmlParseError error);
}

namespace VFS
{
    class Manager;
}

namespace MWGui
{
    [[nodiscard]] std::optional<FnvMenuXmlDocument> loadFnvMenuXml(
        const VFS::Manager& vfs, std::string_view path, FnvMenuXmlParseError* error = nullptr,
        bool expandIncludes = true);
}

#endif
