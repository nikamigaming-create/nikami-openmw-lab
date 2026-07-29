#include "visitor.hpp"

#include <osg/Drawable>
#include <osg/MatrixTransform>

#include <osgParticle/ParticleSystem>

#include <osgAnimation/Bone>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

namespace SceneUtil
{
    namespace
    {
        struct NormalizedNodeName
        {
            std::string mCanonical;
            std::string mSemantic;
            bool mHasSkeletonPrefix = false;
            bool mHasRootSuffix = false;
        };

        std::string_view removeExporterIndex(std::string_view value)
        {
            const std::size_t colon = value.find_last_of(':');
            if (colon == std::string_view::npos || colon + 1 == value.size())
                return value;

            for (std::size_t i = colon + 1; i < value.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(value[i])))
                    return value;
            return value.substr(0, colon);
        }

        NormalizedNodeName normalizeNodeName(std::string_view value)
        {
            NormalizedNodeName result;
            value = removeExporterIndex(value);
            result.mCanonical.reserve(value.size());
            for (unsigned char c : value)
                if (std::isalnum(c))
                    result.mCanonical.push_back(static_cast<char>(std::tolower(c)));

            result.mSemantic = result.mCanonical;
            if (result.mSemantic.starts_with("bip"))
            {
                std::size_t prefixEnd = 3;
                while (prefixEnd < result.mSemantic.size()
                    && std::isdigit(static_cast<unsigned char>(result.mSemantic[prefixEnd])))
                    ++prefixEnd;
                if (prefixEnd > 3)
                {
                    result.mHasSkeletonPrefix = true;
                    result.mSemantic.erase(0, prefixEnd);
                }
            }

            if (result.mSemantic.size() > 4 && result.mSemantic.ends_with("root"))
            {
                result.mHasRootSuffix = true;
                result.mSemantic.resize(result.mSemantic.size() - 4);
            }
            return result;
        }

        struct NumericSuffix
        {
            std::string_view mPrefix;
            std::string_view mDigits;
        };

        NumericSuffix splitNumericSuffix(std::string_view value)
        {
            std::size_t start = value.size();
            while (start > 0 && std::isdigit(static_cast<unsigned char>(value[start - 1])))
                --start;
            return { value.substr(0, start), value.substr(start) };
        }

        std::string_view trimNumericZeroes(std::string_view value)
        {
            while (value.size() > 1 && value.front() == '0')
                value.remove_prefix(1);
            return value;
        }

        bool numericSuffixEquivalent(std::string_view lhs, std::string_view rhs)
        {
            const NumericSuffix left = splitNumericSuffix(lhs);
            const NumericSuffix right = splitNumericSuffix(rhs);
            return !left.mDigits.empty() && !right.mDigits.empty() && left.mPrefix == right.mPrefix
                && trimNumericZeroes(left.mDigits) == trimNumericZeroes(right.mDigits);
        }

        bool numericSuffixReversed(std::string_view lhs, std::string_view rhs)
        {
            const NumericSuffix left = splitNumericSuffix(lhs);
            const NumericSuffix right = splitNumericSuffix(rhs);
            return left.mDigits.size() > 1 && left.mDigits.size() == right.mDigits.size()
                && left.mPrefix == right.mPrefix
                && std::equal(left.mDigits.begin(), left.mDigits.end(), right.mDigits.rbegin());
        }

        bool defaultIndexEquivalent(std::string_view lhs, std::string_view rhs)
        {
            const NumericSuffix left = splitNumericSuffix(lhs);
            const NumericSuffix right = splitNumericSuffix(rhs);
            if (left.mPrefix != right.mPrefix)
                return false;
            if (left.mDigits.empty() == right.mDigits.empty())
                return false;
            const std::string_view digits = left.mDigits.empty() ? right.mDigits : left.mDigits;
            return trimNumericZeroes(digits) == "1";
        }
    }

    NodeNameMatch matchNodeName(std::string_view requested, std::string_view candidate)
    {
        if (requested.empty() || candidate.empty())
            return {};
        if (Misc::StringUtils::ciEqual(requested, candidate))
            return { NodeNameMatchKind::Exact, 900 };

        const NormalizedNodeName request = normalizeNodeName(requested);
        const NormalizedNodeName match = normalizeNodeName(candidate);
        if (request.mCanonical.empty() || match.mCanonical.empty())
            return {};

        const int structuralBonus = (request.mHasSkeletonPrefix == match.mHasSkeletonPrefix ? 4 : 0)
            + (request.mHasRootSuffix == match.mHasRootSuffix ? 1 : 0);
        if (request.mCanonical == match.mCanonical)
            return { NodeNameMatchKind::Canonical, 800 + structuralBonus };
        if (!request.mSemantic.empty() && request.mSemantic == match.mSemantic)
            return { NodeNameMatchKind::Semantic, 700 + structuralBonus };
        if (numericSuffixEquivalent(request.mCanonical, match.mCanonical))
            return { NodeNameMatchKind::NumericEquivalent, 650 + structuralBonus };
        if (numericSuffixReversed(request.mCanonical, match.mCanonical))
            return { NodeNameMatchKind::NumericReversed, 600 + structuralBonus };
        if (numericSuffixEquivalent(request.mSemantic, match.mSemantic))
            return { NodeNameMatchKind::NumericEquivalent, 550 + structuralBonus };
        if (numericSuffixReversed(request.mSemantic, match.mSemantic))
            return { NodeNameMatchKind::NumericReversed, 500 + structuralBonus };
        if (defaultIndexEquivalent(request.mSemantic, match.mSemantic))
            return { NodeNameMatchKind::DefaultIndex, 400 + structuralBonus };
        return {};
    }

    std::string_view getNodeNameMatchKindName(NodeNameMatchKind kind)
    {
        switch (kind)
        {
            case NodeNameMatchKind::Exact:
                return "exact";
            case NodeNameMatchKind::Canonical:
                return "canonical";
            case NodeNameMatchKind::Semantic:
                return "semantic";
            case NodeNameMatchKind::NumericEquivalent:
                return "numeric-equivalent";
            case NodeNameMatchKind::NumericReversed:
                return "numeric-reversed";
            case NodeNameMatchKind::DefaultIndex:
                return "default-index";
            case NodeNameMatchKind::None:
                return "none";
        }
        return "none";
    }

    bool FindByNameVisitor::checkGroup(osg::Group& group)
    {
        if (Misc::StringUtils::ciEqual(group.getName(), mNameToFind))
        {
            mFoundNode = &group;
            return true;
        }

        return false;
    }

    void FindByClassVisitor::apply(osg::Node& node)
    {
        if (Misc::StringUtils::ciEqual(node.className(), mNameToFind))
            mFoundNodes.push_back(&node);

        traverse(node);
    }

    void FindByNameVisitor::apply(osg::Group& group)
    {
        if (!mFoundNode && !checkGroup(group))
            traverse(group);
    }

    void FindByNameVisitor::apply(osg::MatrixTransform& node)
    {
        if (!mFoundNode && !checkGroup(node))
            traverse(node);
    }

    void FindByNameVisitor::apply(osg::Geometry&) {}

    void NodeMapVisitorBoneOnly::apply(osg::MatrixTransform& trans)
    {
        // Choose first found bone in file
        if (dynamic_cast<osgAnimation::Bone*>(&trans) != nullptr)
            mMap.emplace(trans.getName(), &trans);

        traverse(trans);
    }

    void NodeMapVisitor::apply(osg::MatrixTransform& trans)
    {
        // Choose first found node in file
        mMap.emplace(trans.getName(), &trans);
        traverse(trans);
    }

    void RemoveVisitor::remove()
    {
        for (RemoveVec::iterator it = mToRemove.begin(); it != mToRemove.end(); ++it)
        {
            if (!it->second->removeChild(it->first))
                Log(Debug::Error) << "error removing " << it->first->getName();
        }
    }

    void CleanObjectRootVisitor::apply(osg::Drawable& drw)
    {
        applyDrawable(drw);
    }

    void CleanObjectRootVisitor::apply(osg::Group& node)
    {
        applyNode(node);
    }

    void CleanObjectRootVisitor::apply(osg::MatrixTransform& node)
    {
        applyNode(node);
    }

    void CleanObjectRootVisitor::apply(osg::Node& node)
    {
        applyNode(node);
    }

    void CleanObjectRootVisitor::applyNode(osg::Node& node)
    {
        if (node.getStateSet())
            node.setStateSet(nullptr);

        if (node.getNodeMask() == 0x1 && node.getNumParents() == 1)
            mToRemove.emplace_back(&node, node.getParent(0));
        else
            traverse(node);
    }

    void CleanObjectRootVisitor::applyDrawable(osg::Node& node)
    {
        osg::NodePath::iterator parent = getNodePath().end() - 2;
        // We know that the parent is a Group because only Groups can have children.
        osg::Group* parentGroup = static_cast<osg::Group*>(*parent);

        // Try to prune nodes that would be empty after the removal
        if (parent != getNodePath().begin())
        {
            // This could be extended to remove the parent's parent, and so on if they are empty as well.
            // But for NIF files, there won't be a benefit since only TriShapes can be set to STATIC dataVariance.
            osg::Group* parentParent = static_cast<osg::Group*>(*(parent - 1));
            if (parentGroup->getNumChildren() == 1 && parentGroup->getDataVariance() == osg::Object::STATIC)
            {
                mToRemove.emplace_back(parentGroup, parentParent);
                return;
            }
        }

        mToRemove.emplace_back(&node, parentGroup);
    }

    void RemoveTriBipVisitor::apply(osg::Drawable& drw)
    {
        applyImpl(drw);
    }

    void RemoveTriBipVisitor::apply(osg::Group& node)
    {
        traverse(node);
    }

    void RemoveTriBipVisitor::apply(osg::MatrixTransform& node)
    {
        traverse(node);
    }

    void RemoveTriBipVisitor::applyImpl(osg::Node& node)
    {
        if (Misc::StringUtils::ciStartsWith(node.getName(), "tri bip"))
        {
            osg::Group* parent = static_cast<osg::Group*>(*(getNodePath().end() - 2));
            // Not safe to remove in apply(), since the visitor is still iterating the child list
            mToRemove.emplace_back(&node, parent);
        }
    }
}
