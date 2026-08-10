#include "fnvmenuxml.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <cmath>
#include <charconv>
#include <istream>
#include <set>

#include <components/misc/strings/algorithm.hpp>
#include <components/vfs/manager.hpp>

namespace
{
    bool isNameCharacter(char value)
    {
        const unsigned char character = static_cast<unsigned char>(value);
        return std::isalnum(character) || value == '_' || value == '-' || value == ':' || value == '.';
    }

    void trim(std::string& value)
    {
        const auto isSpace = [](unsigned char character) { return std::isspace(character); };
        value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
        value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    }

    class Parser
    {
        std::string_view mXml;
        std::size_t mPosition = 0;
        MWGui::FnvMenuXmlParseError mError = MWGui::FnvMenuXmlParseError::None;

        bool startsWith(std::string_view value) const
        {
            return mXml.substr(mPosition, value.size()) == value;
        }

        void skipSpace()
        {
            while (mPosition < mXml.size() && std::isspace(static_cast<unsigned char>(mXml[mPosition])))
                ++mPosition;
        }

        bool skipComment()
        {
            if (!startsWith("<!--"))
                return false;
            const std::size_t end = mXml.find("-->", mPosition + 4);
            if (end == std::string_view::npos)
            {
                mError = MWGui::FnvMenuXmlParseError::UnterminatedComment;
                mPosition = mXml.size();
            }
            else
                mPosition = end + 3;
            return true;
        }

        void skipTrivia()
        {
            while (true)
            {
                skipSpace();
                if (!skipComment())
                    return;
            }
        }

        std::string parseName()
        {
            const std::size_t begin = mPosition;
            while (mPosition < mXml.size() && isNameCharacter(mXml[mPosition]))
                ++mPosition;
            if (begin == mPosition)
                mError = MWGui::FnvMenuXmlParseError::InvalidName;
            return std::string(mXml.substr(begin, mPosition - begin));
        }

        bool parseAttributes(MWGui::FnvMenuXmlNode& node)
        {
            while (mError == MWGui::FnvMenuXmlParseError::None)
            {
                skipSpace();
                if (mPosition >= mXml.size() || mXml[mPosition] == '>' || startsWith("/>"))
                    return true;
                std::string name = parseName();
                skipSpace();
                if (name.empty() || mPosition >= mXml.size() || mXml[mPosition++] != '=')
                {
                    mError = MWGui::FnvMenuXmlParseError::InvalidAttribute;
                    return false;
                }
                skipSpace();
                if (mPosition >= mXml.size() || (mXml[mPosition] != '\'' && mXml[mPosition] != '"'))
                {
                    mError = MWGui::FnvMenuXmlParseError::InvalidAttribute;
                    return false;
                }
                const char quote = mXml[mPosition++];
                const std::size_t begin = mPosition;
                const std::size_t end = mXml.find(quote, begin);
                if (end == std::string_view::npos)
                {
                    mError = MWGui::FnvMenuXmlParseError::InvalidAttribute;
                    return false;
                }
                node.mAttributes.emplace_back(std::move(name), std::string(mXml.substr(begin, end - begin)));
                mPosition = end + 1;
            }
            return false;
        }

        std::optional<MWGui::FnvMenuXmlNode> parseElement()
        {
            if (mPosition >= mXml.size() || mXml[mPosition++] != '<' || startsWith("/"))
            {
                mError = MWGui::FnvMenuXmlParseError::UnexpectedToken;
                return std::nullopt;
            }
            MWGui::FnvMenuXmlNode node;
            node.mType = parseName();
            if (node.mType.empty() || !parseAttributes(node))
                return std::nullopt;
            if (startsWith("/>"))
            {
                mPosition += 2;
                return node;
            }
            if (mPosition >= mXml.size() || mXml[mPosition++] != '>')
            {
                mError = MWGui::FnvMenuXmlParseError::UnexpectedToken;
                return std::nullopt;
            }

            while (mPosition < mXml.size())
            {
                if (startsWith("<!--"))
                {
                    skipComment();
                    continue;
                }
                if (startsWith("</"))
                {
                    mPosition += 2;
                    const std::string closingName = parseName();
                    skipSpace();
                    if (closingName != node.mType)
                    {
                        mError = MWGui::FnvMenuXmlParseError::MismatchedClosingElement;
                        return std::nullopt;
                    }
                    if (mPosition >= mXml.size() || mXml[mPosition++] != '>')
                    {
                        mError = MWGui::FnvMenuXmlParseError::UnexpectedToken;
                        return std::nullopt;
                    }
                    trim(node.mText);
                    return node;
                }
                if (mXml[mPosition] == '<')
                {
                    std::optional<MWGui::FnvMenuXmlNode> child = parseElement();
                    if (!child)
                        return std::nullopt;
                    node.mChildren.push_back(std::move(*child));
                }
                else
                {
                    const std::size_t begin = mPosition;
                    const std::size_t end = mXml.find('<', begin);
                    if (end == std::string_view::npos)
                    {
                        mError = MWGui::FnvMenuXmlParseError::UnterminatedElement;
                        return std::nullopt;
                    }
                    node.mText.append(mXml.substr(begin, end - begin));
                    mPosition = end;
                }
            }
            mError = MWGui::FnvMenuXmlParseError::UnterminatedElement;
            return std::nullopt;
        }

    public:
        explicit Parser(std::string_view xml)
            : mXml(xml)
        {
        }

        std::optional<MWGui::FnvMenuXmlDocument> parse()
        {
            skipTrivia();
            if (mPosition == mXml.size())
            {
                mError = MWGui::FnvMenuXmlParseError::EmptyDocument;
                return std::nullopt;
            }
            std::optional<MWGui::FnvMenuXmlNode> root = parseElement();
            if (!root)
                return std::nullopt;
            skipTrivia();
            if (mPosition != mXml.size())
            {
                mError = MWGui::FnvMenuXmlParseError::MultipleRoots;
                return std::nullopt;
            }
            return MWGui::FnvMenuXmlDocument{ std::move(*root) };
        }

        std::optional<std::vector<MWGui::FnvMenuXmlNode>> parseFragment()
        {
            std::vector<MWGui::FnvMenuXmlNode> result;
            skipTrivia();
            while (mPosition < mXml.size())
            {
                std::optional<MWGui::FnvMenuXmlNode> node = parseElement();
                if (!node)
                    return std::nullopt;
                result.push_back(std::move(*node));
                skipTrivia();
            }
            if (result.empty())
            {
                mError = MWGui::FnvMenuXmlParseError::EmptyDocument;
                return std::nullopt;
            }
            return result;
        }

        MWGui::FnvMenuXmlParseError getError() const { return mError; }
    };
}

namespace MWGui
{
    namespace
    {
        std::optional<float> parseScalar(
            std::string_view text, const FnvMenuTraitEvaluationContext& context)
        {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1);
            if (text.empty())
                return 0.f;
            if (text.front() == '&' && text.back() == ';')
                return context.mSymbolResolver ? context.mSymbolResolver(text.substr(1, text.size() - 2)) : std::nullopt;
            float value = 0.f;
            const char* end = text.data() + text.size();
            const std::from_chars_result parsed = std::from_chars(text.data(), end, value);
            return parsed.ec == std::errc{} && parsed.ptr == end ? std::optional<float>(value) : std::nullopt;
        }

        std::optional<float> evaluateSequence(
            const FnvMenuXmlNode& node, const FnvMenuTraitEvaluationContext& context)
        {
            if (node.mChildren.empty())
                return parseScalar(node.mText, context);

            float current = 0.f;
            bool initialized = false;
            for (const FnvMenuXmlNode& operation : node.mChildren)
            {
                const auto operand = [&]() -> std::optional<float> {
                    const std::string_view source = operation.getAttribute("src");
                    const std::string_view trait = operation.getAttribute("trait");
                    if (!source.empty() || !trait.empty())
                    {
                        if (source.empty() || trait.empty() || !context.mTraitResolver)
                            return std::nullopt;
                        return context.mTraitResolver(source, trait);
                    }
                    return evaluateSequence(operation, context);
                }();
                if (!operand)
                    return std::nullopt;

                if (operation.mType == "copy" || operation.mType == "ref")
                    current = *operand;
                else if (operation.mType == "add")
                    current += *operand;
                else if (operation.mType == "sub")
                    current -= *operand;
                else if (operation.mType == "mult" || operation.mType == "mul")
                    current *= *operand;
                else if (operation.mType == "div")
                {
                    if (*operand == 0.f)
                        return std::nullopt;
                    current /= *operand;
                }
                else if (operation.mType == "mod")
                {
                    if (*operand == 0.f)
                        return std::nullopt;
                    current = std::fmod(current, *operand);
                }
                else if (operation.mType == "min")
                    current = std::min(current, *operand);
                else if (operation.mType == "max")
                    current = std::max(current, *operand);
                else if (operation.mType == "floor")
                    current = std::floor(current);
                else if (operation.mType == "ceil")
                    current = std::ceil(current);
                else if (operation.mType == "eq")
                    current = current == *operand ? 1.f : 0.f;
                else if (operation.mType == "neq")
                    current = current != *operand ? 1.f : 0.f;
                else if (operation.mType == "gt")
                    current = current > *operand ? 1.f : 0.f;
                else if (operation.mType == "gte")
                    current = current >= *operand ? 1.f : 0.f;
                else if (operation.mType == "lt")
                    current = current < *operand ? 1.f : 0.f;
                else if (operation.mType == "lte")
                    current = current <= *operand ? 1.f : 0.f;
                else if (operation.mType == "and")
                    current = current != 0.f && *operand != 0.f ? 1.f : 0.f;
                else if (operation.mType == "or")
                    current = current != 0.f || *operand != 0.f ? 1.f : 0.f;
                else if (operation.mType == "not")
                    current = *operand == 0.f ? 1.f : 0.f;
                else if (operation.mType == "onlyif")
                    current = *operand != 0.f ? current : 0.f;
                else if (operation.mType == "onlyifnot")
                    current = *operand == 0.f ? current : 0.f;
                else
                    return std::nullopt;
                initialized = true;
            }
            return initialized ? std::optional<float>(current) : parseScalar(node.mText, context);
        }
    }

    std::string_view FnvMenuXmlNode::getAttribute(std::string_view name) const
    {
        const auto found = std::find_if(mAttributes.begin(), mAttributes.end(),
            [name](const auto& value) { return value.first == name; });
        return found == mAttributes.end() ? std::string_view{} : std::string_view(found->second);
    }

    const FnvMenuXmlNode* FnvMenuXmlNode::findChild(std::string_view type) const
    {
        const auto found = std::find_if(
            mChildren.begin(), mChildren.end(), [type](const FnvMenuXmlNode& value) { return value.mType == type; });
        return found == mChildren.end() ? nullptr : &*found;
    }

    std::vector<const FnvMenuXmlNode*> FnvMenuXmlNode::findChildren(std::string_view type) const
    {
        std::vector<const FnvMenuXmlNode*> result;
        for (const FnvMenuXmlNode& child : mChildren)
        {
            if (child.mType == type)
                result.push_back(&child);
        }
        return result;
    }

    const FnvMenuXmlNode* FnvMenuXmlNode::findDescendantByName(std::string_view name) const
    {
        if (getAttribute("name") == name)
            return this;
        for (const FnvMenuXmlNode& child : mChildren)
        {
            if (const FnvMenuXmlNode* result = child.findDescendantByName(name))
                return result;
        }
        return nullptr;
    }

    std::optional<FnvMenuXmlDocument> parseFnvMenuXml(std::string_view xml, FnvMenuXmlParseError* error)
    {
        Parser parser(xml);
        std::optional<FnvMenuXmlDocument> result = parser.parse();
        if (error != nullptr)
            *error = parser.getError();
        return result;
    }

    std::optional<std::vector<FnvMenuXmlNode>> parseFnvMenuXmlFragment(
        std::string_view xml, FnvMenuXmlParseError* error)
    {
        Parser parser(xml);
        std::optional<std::vector<FnvMenuXmlNode>> result = parser.parseFragment();
        if (error != nullptr)
            *error = parser.getError();
        return result;
    }

    bool expandFnvMenuXmlIncludes(
        FnvMenuXmlNode& root, const FnvMenuXmlIncludeLoader& loader, FnvMenuXmlParseError* error)
    {
        constexpr std::size_t maximumDepth = 32;
        const auto expand = [&](auto&& self, FnvMenuXmlNode& node, std::size_t depth) -> bool {
            if (depth > maximumDepth)
            {
                if (error != nullptr)
                    *error = FnvMenuXmlParseError::UnexpectedToken;
                return false;
            }
            for (std::size_t index = 0; index < node.mChildren.size();)
            {
                FnvMenuXmlNode& child = node.mChildren[index];
                if (child.mType != "include")
                {
                    if (!self(self, child, depth))
                        return false;
                    ++index;
                    continue;
                }

                const std::string_view source = child.getAttribute("src");
                const std::optional<std::string> contents = source.empty() ? std::nullopt : loader(source);
                FnvMenuXmlParseError parseError = FnvMenuXmlParseError::None;
                const std::optional<std::vector<FnvMenuXmlNode>> fragment
                    = contents ? parseFnvMenuXmlFragment(*contents, &parseError) : std::nullopt;
                if (!fragment)
                {
                    if (error != nullptr)
                        *error = contents ? parseError : FnvMenuXmlParseError::UnexpectedToken;
                    return false;
                }
                std::vector<FnvMenuXmlNode> expanded = std::move(*fragment);
                for (FnvMenuXmlNode& included : expanded)
                {
                    if (!self(self, included, depth + 1))
                        return false;
                }
                node.mChildren.erase(node.mChildren.begin() + index);
                node.mChildren.insert(node.mChildren.begin() + index,
                    std::make_move_iterator(expanded.begin()), std::make_move_iterator(expanded.end()));
                index += expanded.size();
            }
            return true;
        };

        if (error != nullptr)
            *error = FnvMenuXmlParseError::None;
        return expand(expand, root, 0);
    }

    std::optional<float> evaluateFnvMenuScalarTrait(
        const FnvMenuXmlNode& trait, const FnvMenuTraitEvaluationContext& context)
    {
        return evaluateSequence(trait, context);
    }

    std::optional<float> evaluateFnvMenuNamedScalarTrait(const FnvMenuXmlNode& root,
        std::string_view nodeName, std::string_view traitName, const FnvMenuLayoutEvaluationContext& context)
    {
        const auto findParent = [&root](const FnvMenuXmlNode* target) -> const FnvMenuXmlNode* {
            const auto visit = [&](auto&& self, const FnvMenuXmlNode& node) -> const FnvMenuXmlNode* {
                for (const FnvMenuXmlNode& child : node.mChildren)
                {
                    if (&child == target)
                        return &node;
                    if (const FnvMenuXmlNode* parent = self(self, child))
                        return parent;
                }
                return nullptr;
            };
            return target == &root ? nullptr : visit(visit, root);
        };

        const auto nodeDisplayName = [&root](const FnvMenuXmlNode& node) -> std::string_view {
            const std::string_view name = node.getAttribute("name");
            return name.empty() && &node == &root ? root.getAttribute("name") : name;
        };

        const FnvMenuXmlNode* initial = root.findDescendantByName(nodeName);
        if (initial == nullptr)
            return std::nullopt;
        std::set<std::pair<const FnvMenuXmlNode*, std::string>> active;
        const auto evaluate = [&](auto&& self, const FnvMenuXmlNode& node,
                                  std::string_view requestedTrait) -> std::optional<float> {
            const std::string displayName(nodeDisplayName(node));
            if (context.mDynamicTraitResolver)
            {
                if (std::optional<float> value = context.mDynamicTraitResolver(displayName, requestedTrait))
                    return value;
            }
            const auto key = std::pair{ &node, std::string(requestedTrait) };
            if (!active.insert(key).second)
                return std::nullopt;
            const FnvMenuXmlNode* trait = node.findChild(requestedTrait);
            if (trait == nullptr)
            {
                active.erase(key);
                return 0.f;
            }
            const FnvMenuTraitEvaluationContext scalarContext{
                [&](std::string_view source, std::string_view sourceTrait) -> std::optional<float> {
                    if (source == "screen()")
                    {
                        if (sourceTrait == "width")
                            return context.mScreenWidth;
                        if (sourceTrait == "height")
                            return context.mScreenHeight;
                        return std::nullopt;
                    }
                    const FnvMenuXmlNode* sourceNode = nullptr;
                    if (source == "me()")
                        sourceNode = &node;
                    else if (source == "parent()")
                        sourceNode = findParent(&node);
                    else
                    {
                        const auto unwrap = [source](std::string_view prefix) -> std::string_view {
                            return source.starts_with(prefix) && source.ends_with(')')
                                ? source.substr(prefix.size(), source.size() - prefix.size() - 1)
                                : std::string_view{};
                        };
                        std::string_view reference = unwrap("sibling(");
                        if (reference.empty())
                            reference = unwrap("child(");
                        if (reference.empty())
                            reference = source;
                        sourceNode = root.findDescendantByName(reference);
                    }
                    return sourceNode == nullptr ? std::nullopt : self(self, *sourceNode, sourceTrait);
                },
                context.mSymbolResolver,
            };
            const std::optional<float> result = evaluateFnvMenuScalarTrait(*trait, scalarContext);
            active.erase(key);
            return result;
        };
        return evaluate(evaluate, *initial, traitName);
    }

    std::string normalizeFnvMenuTexturePath(std::string_view path)
    {
        const std::size_t begin = path.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos)
            return {};
        const std::size_t end = path.find_last_not_of(" \t\r\n");
        std::string result(path.substr(begin, end - begin + 1));
        std::replace(result.begin(), result.end(), '/', '\\');
        if (!Misc::StringUtils::ciStartsWith(result, "textures\\"))
            result.insert(0, "textures\\");
        return result;
    }

    std::optional<FnvMenuXmlDocument> loadFnvMenuXml(
        const VFS::Manager& vfs, std::string_view path, FnvMenuXmlParseError* error)
    {
        const auto read = [&vfs](std::string_view candidate) -> std::optional<std::string> {
            const Files::IStreamPtr stream = vfs.find(VFS::Path::Normalized(candidate));
            if (!stream)
                return std::nullopt;
            return std::string(std::istreambuf_iterator<char>(*stream), {});
        };

        const std::optional<std::string> contents = read(path);
        if (!contents)
        {
            if (error != nullptr)
                *error = FnvMenuXmlParseError::UnexpectedToken;
            return std::nullopt;
        }
        std::optional<FnvMenuXmlDocument> document = parseFnvMenuXml(*contents, error);
        if (!document)
            return std::nullopt;

        const std::size_t slash = path.find_last_of("/\\");
        const std::string parent = slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash + 1));
        const bool expanded = expandFnvMenuXmlIncludes(document->mRoot,
            [&](std::string_view source) -> std::optional<std::string> {
                if (std::optional<std::string> direct = read(parent + std::string(source)))
                    return direct;
                return read("menus/prefabs/" + std::string(source));
            },
            error);
        return expanded ? document : std::nullopt;
    }

    std::string_view getFnvMenuXmlParseErrorName(FnvMenuXmlParseError error)
    {
        switch (error)
        {
            case FnvMenuXmlParseError::None: return "none";
            case FnvMenuXmlParseError::EmptyDocument: return "empty document";
            case FnvMenuXmlParseError::UnexpectedToken: return "unexpected token";
            case FnvMenuXmlParseError::InvalidName: return "invalid name";
            case FnvMenuXmlParseError::InvalidAttribute: return "invalid attribute";
            case FnvMenuXmlParseError::UnterminatedComment: return "unterminated comment";
            case FnvMenuXmlParseError::UnterminatedElement: return "unterminated element";
            case FnvMenuXmlParseError::MismatchedClosingElement: return "mismatched closing element";
            case FnvMenuXmlParseError::MultipleRoots: return "multiple roots";
        }
        return "unknown";
    }
}
