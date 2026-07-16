#include "creatureanimation.hpp"

#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/ColorMask>
#include <osg/ComputeBoundsVisitor>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/FrameStamp>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osg/State>
#include <osg/Switch>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/TriangleFunctor>
#include <osg/Uniform>
#include <osg/Viewport>
#include <osgSim/MultiSwitch>
#include <osgUtil/CullVisitor>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm4/loadbptd.hpp>
#include <components/esm4/loadcrea.hpp>
#include <components/esm4/loadlvlc.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/attach.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include <vector>

#include "../mwmechanics/weapontype.hpp"

#include "../mwbase/environment.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

namespace MWRender
{
    namespace
    {
        bool hasSuffix(std::string_view value, std::string_view suffix)
        {
            return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
        }

        std::string toLowerAscii(std::string_view value)
        {
            std::string result(value);
            std::transform(result.begin(), result.end(), result.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        bool isLikelySkeletonNif(std::string_view value)
        {
            const std::string lowered = toLowerAscii(value);
            return lowered.ends_with("skeleton.nif") || lowered.find("/skeleton") != std::string::npos
                || lowered.find("\\skeleton") != std::string::npos;
        }

        bool isMarkerCreatureModel(std::string_view value)
        {
            const std::string lowered = toLowerAscii(value);
            return lowered.ends_with("marker_creature.nif") || lowered.find("/marker_creature.nif") != std::string::npos
                || lowered.find("\\marker_creature.nif") != std::string::npos;
        }

        bool findCreatureKf(const VFS::Manager& vfs, const std::string& path, std::string& normalizedPath)
        {
            VFS::Path::Normalized normalized(path);
            if (!vfs.exists(normalized))
                return false;

            normalizedPath = normalized.value();
            return true;
        }

        struct FalloutCreatureRepresentativePoseSource
        {
            std::string_view mGroup;
            std::string_view mRelativeKf;
        };

        using FalloutCreatureRepresentativePoseSources
            = std::array<FalloutCreatureRepresentativePoseSource, 7>;

        // Keep stand last. Some special-idle KFs also advertise a generic idle group, and animation sources added
        // later win. Loading mtidle last preserves the neutral gameplay idle while each selected KF still owns its
        // explicit proof-facing semantic alias.
        constexpr FalloutCreatureRepresentativePoseSources sSuperMutantRepresentativePoseSources = { {
            { "kneel", "idleanims/specialidle_sitcycle.kf" },
            { "prone", "idleanims/specialidle_sleepcycle.kf" },
            { "walk", "locomotion/mtforward.kf" },
            { "talk", "talking.kf" },
            { "shoot", "2hrattack3.kf" },
            { "wave", "idleanims/specialidle_mtponder.kf" },
            { "stand", "mtidle.kf" },
        } };

        constexpr FalloutCreatureRepresentativePoseSources sProtectronRepresentativePoseSources = { {
            { "kneel", "idleanims/mtspecialidle_deactivateloop.kf" },
            { "prone", "idleanims/mtspecialidle_knockdownfacedown.kf" },
            { "walk", "locomotion/mtforward.kf" },
            { "talk", "talking.kf" },
            { "shoot", "specialanims/1hpattackright.kf" },
            { "wave", "idleanims/specialidle_salutes.kf" },
            { "stand", "mtidle.kf" },
        } };

        const FalloutCreatureRepresentativePoseSources* getFalloutCreatureRepresentativePoseSources(
            std::string_view directory)
        {
            std::string normalizedDirectory(directory);
            VFS::Path::normalizeFilenameInPlace(normalizedDirectory);
            if (!normalizedDirectory.ends_with('/'))
                normalizedDirectory.push_back('/');

            // Exact directory equality is intentional. Fallout creature KFs are skeleton-specific even when their
            // filenames are identical, so never borrow a pose source from another creature/robot rig.
            if (normalizedDirectory == "meshes/creatures/smspinebreaker/")
                return &sSuperMutantRepresentativePoseSources;
            if (normalizedDirectory == "meshes/creatures/protectron/")
                return &sProtectronRepresentativePoseSources;
            return nullptr;
        }

        std::vector<std::string> collectDiscoveredCreatureKfs(
            const VFS::Manager& vfs, std::string_view directory, std::string_view probeToken, const std::string& editorId)
        {
            std::vector<std::string> paths;
            unsigned int logged = 0;
            const bool logCandidates = std::getenv("OPENMW_FNV_CREATURE_KF_DIAG") != nullptr;
            for (const VFS::Path::Normalized& name : vfs.getRecursiveDirectoryIterator(directory))
            {
                const std::string_view value = name.view();
                if (!hasSuffix(value, ".kf"))
                    continue;

                paths.push_back(name.value());

                if (logCandidates && logged < 24)
                {
                    Log(Debug::Verbose) << "FNV/ESM4 diag: creature KF candidate " << editorId << " path=" << name;
                    ++logged;
                }
            }

            if (paths.empty() && !probeToken.empty())
            {
                for (const VFS::Path::Normalized& name : vfs.getRecursiveDirectoryIterator())
                {
                    const std::string_view value = name.view();
                    if (value.find(probeToken) == std::string_view::npos || !hasSuffix(value, ".kf"))
                        continue;

                    paths.push_back(name.value());

                    if (logCandidates && logged < 24)
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 diag: creature KF global candidate " << editorId
                                         << " path=" << name;
                        ++logged;
                    }
                }
            }

            auto sourcePriority = [&](const std::string& path) {
                const std::string loweredPath = toLowerAscii(path);
                // Animation::play deliberately gives the last-added source precedence for duplicate groups.
                // Procedure/special-idle KFs often advertise a generic "idle" alias in addition to their
                // specific group; allowing one of those files to override mtidle makes a forced neutral proof
                // run the special's accumulated root motion (the raven fly-away moved its visual root forever
                // while the reference stayed staged).  Keep special sources available for their unique groups,
                // but make the authored locomotion idle the canonical duplicate-group source for every creature.
                if (loweredPath.ends_with("/mtidle.kf") || loweredPath.ends_with("\\mtidle.kf"))
                    return 100;
                if (loweredPath.find("/idleanims/") != std::string::npos
                    || loweredPath.find("\\idleanims\\") != std::string::npos)
                    return 0;
                return 50;
            };
            std::stable_sort(paths.begin(), paths.end(), [&](const std::string& lhs, const std::string& rhs) {
                return sourcePriority(lhs) < sourcePriority(rhs);
            });

            return paths;
        }

        const ESM4::Creature* searchCreatureTemplate(ESM::FormId id, int depth = 0)
        {
            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr || id.isZeroOrUnset() || depth > 8)
                return nullptr;

            const ESM::RecNameInts foundType = static_cast<ESM::RecNameInts>(store->find(id));
            if (foundType == ESM::RecNameInts::REC_CREA4)
                return store->get<ESM4::Creature>().search(id);

            if (foundType != ESM::RecNameInts::REC_LVLC4)
                return nullptr;

            const ESM4::LevelledCreature* list = store->get<ESM4::LevelledCreature>().search(id);
            if (list == nullptr || list->mLvlObject.empty())
                return nullptr;

            const ESM4::LVLO* selected = nullptr;
            for (const ESM4::LVLO& entry : list->mLvlObject)
            {
                if (entry.item == 0)
                    continue;
                if (selected == nullptr || entry.level <= 1)
                    selected = &entry;
                if (entry.level <= 1)
                    break;
            }

            return selected == nullptr ? nullptr : searchCreatureTemplate(ESM::FormId::fromUint32(selected->item), depth + 1);
        }

        std::vector<const ESM4::Creature*> collectCreatureRenderingTemplates(const ESM4::Creature& creature)
        {
            std::vector<const ESM4::Creature*> records;
            const ESM4::Creature* current = &creature;
            for (int depth = 0; current != nullptr && depth < 16; ++depth)
            {
                if (std::find(records.begin(), records.end(), current) != records.end())
                    break;
                records.push_back(current);
                if (current->mBaseTemplate.isZeroOrUnset())
                    break;

                const ESM4::Creature* templated = searchCreatureTemplate(current->mBaseTemplate);
                if (templated == nullptr || templated == current)
                    break;

                current = templated;
            }
            return records;
        }

        bool appendUniquePath(std::vector<std::string>& paths, const std::string& path)
        {
            if (path.empty() || std::find(paths.begin(), paths.end(), path) != paths.end())
                return false;

            paths.push_back(path);
            return true;
        }

        VFS::Path::Normalized correctCreatureBodyPath(const std::string& path)
        {
            VFS::Path::Normalized normalized(path);
            if (normalized.value().starts_with("meshes/"))
                return normalized;
            return Misc::ResourceHelpers::correctMeshPath(normalized);
        }

        void appendDiagnosticDirectoryBodyNif(
            const VFS::Manager& vfs, std::string_view directory, std::vector<std::string>& paths)
        {
            std::vector<std::string> candidates;
            for (const VFS::Path::Normalized& name : vfs.getRecursiveDirectoryIterator(directory))
            {
                const std::string_view value = name.view();
                if (!hasSuffix(value, ".nif") || isLikelySkeletonNif(value))
                    continue;

                candidates.push_back(name.value());
            }

            for (const std::string& candidate : candidates)
            {
                const std::string loweredCandidate = toLowerAscii(candidate);
                if (loweredCandidate.find("skullcap") != std::string::npos
                    || loweredCandidate.find("eyes") != std::string::npos
                    || loweredCandidate.find("rex") != std::string::npos
                    || loweredCandidate.find("cyberdog") != std::string::npos
                    || loweredCandidate.find("static") != std::string::npos)
                    continue;

                appendUniquePath(paths, candidate);
                return;
            }
        }

        std::vector<std::string> resolveCreatureRelativePaths(
            const std::vector<std::string>& paths, std::string_view visualModel)
        {
            std::vector<std::string> resolved = paths;
            const std::size_t slash = visualModel.find_last_of("/\\");
            if (slash == std::string_view::npos)
                return resolved;

            const std::string directory(visualModel.substr(0, slash + 1));
            for (std::string& path : resolved)
                if (!path.empty() && path.find_first_of("/\\") == std::string::npos)
                    path = directory + path;
            return resolved;
        }

        std::vector<std::string> collectCreatureBodyNifs(
            const std::vector<std::string>& authoredNifs, const std::vector<ESM::FormId>& authoredBodyParts,
            const VFS::Manager& vfs, std::string_view animationDirectory)
        {
            std::vector<std::string> paths;
            for (const std::string& bodyNif : authoredNifs)
            {
                if (!isLikelySkeletonNif(bodyNif))
                    appendUniquePath(paths, bodyNif);
            }

            const MWWorld::ESMStore* store = MWBase::Environment::get().getESMStore();
            if (store == nullptr)
                return paths;

            const auto& bodyPartStore = store->get<ESM4::BodyPartData>();
            for (ESM::FormId bodyPartId : authoredBodyParts)
            {
                const ESM4::BodyPartData* bodyPartData = bodyPartStore.search(bodyPartId);
                if (bodyPartData == nullptr)
                    continue;

                if (!isLikelySkeletonNif(bodyPartData->mModel))
                    appendUniquePath(paths, bodyPartData->mModel);
            }

            // Directory guessing cannot establish record identity and previously
            // contained creature-name special cases. Keep it diagnostic-only so it
            // cannot mask missing TPLT/NIFZ/PNAM resolution in normal rendering.
            if (paths.empty() && std::getenv("OPENMW_FNV_CREATURE_DIRECTORY_FALLBACK") != nullptr)
                appendDiagnosticDirectoryBodyNif(vfs, animationDirectory, paths);

            return paths;
        }

        osg::Vec3f boundingBoxExtent(const osg::BoundingBox& box)
        {
            return osg::Vec3f(box.xMax() - box.xMin(), box.yMax() - box.yMin(), box.zMax() - box.zMin());
        }

        void logCreatureBounds(std::string_view label, const std::string& editorId, const std::string& path,
            osg::Node& node)
        {
            osg::ComputeBoundsVisitor boundsVisitor;
            node.accept(boundsVisitor);
            const osg::BoundingBox box = boundsVisitor.getBoundingBox();
            if (!box.valid())
            {
                Log(Debug::Warning) << "FNV/ESM4 diag: creature " << label << " bounds invalid for "
                                    << editorId << " path=" << path;
                return;
            }

            const osg::Vec3f center = box.center();
            const osg::Vec3f extent = boundingBoxExtent(box);
            Log(Debug::Verbose) << "FNV/ESM4 diag: creature " << label << " bounds for " << editorId
                             << " path=" << path << " center=(" << center.x() << "," << center.y() << ","
                             << center.z() << ") extent=(" << extent.x() << "," << extent.y() << ","
                             << extent.z() << ")";
        }

        class ForceFalloutCreatureBodyVisibleVisitor : public osg::NodeVisitor
        {
        public:
            ForceFalloutCreatureBodyVisibleVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                node.setNodeMask(~0u);
                traverse(node);
            }

            void apply(osg::Drawable& drawable) override
            {
                drawable.setNodeMask(~0u);
                drawable.setCullingActive(false);

                if (SceneUtil::RigGeometry* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable))
                {
                    // Creature body NIFs are loaded from meshes/creatures rather
                    // than the meshes/characters path marked by NPC assembly.
                    // Without this bit RigGeometry treats their skin-to-skeleton
                    // wrapper as a generic model transform and applies it again,
                    // separating the authored body partitions.
                    rig->setFalloutCharacterSkinning(true);
                    ++mRigGeometryCount;
                }
                else if (dynamic_cast<osg::Geometry*>(&drawable) != nullptr)
                    ++mStaticGeometryCount;
                else
                    ++mOtherDrawableCount;

                for (osg::Node* node : getNodePath())
                {
                    if (node != nullptr)
                        node->setNodeMask(~0u);
                }
            }

            unsigned int mRigGeometryCount = 0;
            unsigned int mStaticGeometryCount = 0;
            unsigned int mOtherDrawableCount = 0;
        };

        class CreatureRigTopologyVisitor : public osg::NodeVisitor
        {
        public:
            explicit CreatureRigTopologyVisitor(const SceneUtil::Skeleton* expectedSkeleton)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mExpectedSkeleton(expectedSkeleton)
            {
            }

            void apply(osg::Node& node) override { traverse(node); }

            void apply(osg::Drawable& drawable) override
            {
                const osg::NodePathList paths = drawable.getParentalNodePaths();
                ++mDrawableCount;
                if (!paths.empty())
                    ++mParentedDrawableCount;
                bool hasVisiblePath = false;
                for (const osg::NodePath& path : paths)
                {
                    unsigned int effectiveMask = drawable.getNodeMask();
                    for (const osg::Node* node : path)
                        if (node != nullptr)
                            effectiveMask &= node->getNodeMask();
                    hasVisiblePath = hasVisiblePath || effectiveMask != 0;
                }
                if (hasVisiblePath)
                    ++mVisibleDrawableCount;

                if (dynamic_cast<SceneUtil::RigGeometry*>(&drawable) == nullptr)
                    return;

                ++mRigGeometryCount;
                if (!paths.empty())
                    ++mParentedRigGeometryCount;
                bool hasSkeletonPath = false;
                for (const osg::NodePath& path : paths)
                {
                    for (const osg::Node* node : path)
                        if (node != nullptr)
                            hasSkeletonPath = hasSkeletonPath || node == mExpectedSkeleton;
                }
                if (hasSkeletonPath)
                    ++mSkeletonAnchoredRigGeometryCount;
                if (hasVisiblePath)
                    ++mVisibleRigGeometryCount;
            }

            const SceneUtil::Skeleton* mExpectedSkeleton;
            unsigned int mDrawableCount = 0;
            unsigned int mParentedDrawableCount = 0;
            unsigned int mVisibleDrawableCount = 0;
            unsigned int mRigGeometryCount = 0;
            unsigned int mParentedRigGeometryCount = 0;
            unsigned int mSkeletonAnchoredRigGeometryCount = 0;
            unsigned int mVisibleRigGeometryCount = 0;
        };

        void auditCreatureRigTopology(osg::Node* bodyNode, const SceneUtil::Skeleton* skeleton,
            const osg::Node* objectRoot, const std::string& editorId, const VFS::Path::Normalized& bodyPath,
            bool requireVisible)
        {
            if (bodyNode == nullptr)
                return;

            CreatureRigTopologyVisitor visitor(skeleton);
            bodyNode->accept(visitor);
            const bool drawableGate = visitor.mDrawableCount != 0
                && visitor.mParentedDrawableCount == visitor.mDrawableCount
                && (!requireVisible || visitor.mVisibleDrawableCount == visitor.mDrawableCount);
            const bool rigGate = visitor.mRigGeometryCount == 0
                || (visitor.mParentedRigGeometryCount == visitor.mRigGeometryCount
                    && visitor.mSkeletonAnchoredRigGeometryCount == visitor.mRigGeometryCount
                    && visitor.mVisibleRigGeometryCount == visitor.mRigGeometryCount);
            const bool passed = drawableGate && rigGate;
            Log(passed ? Debug::Verbose : Debug::Error)
                << "FNV/ESM4 diag: creature rig topology gate"
                << " editor=" << editorId << " path=" << bodyPath
                << " drawables=" << visitor.mDrawableCount
                << " parentedDrawables=" << visitor.mParentedDrawableCount
                << " visibleDrawables=" << visitor.mVisibleDrawableCount
                << " requireVisible=" << requireVisible
                << " rigged=" << visitor.mRigGeometryCount
                << " parented=" << visitor.mParentedRigGeometryCount
                << " skeletonAnchored=" << visitor.mSkeletonAnchoredRigGeometryCount
                << " visible=" << visitor.mVisibleRigGeometryCount
                << " skeleton='" << (skeleton == nullptr ? std::string("<none>") : skeleton->getName()) << "'"
                << " objectRoot='" << (objectRoot == nullptr ? std::string("<none>") : objectRoot->getName()) << "'"
                << " rootWrapped=" << (skeleton != nullptr && skeleton != objectRoot)
                << " status=" << (passed ? "passed" : "failed");
        }

        void forceFalloutCreatureBodyVisible(
            osg::Node* bodyNode, const std::string& editorId, const VFS::Path::Normalized& bodyPath)
        {
            if (bodyNode == nullptr)
                return;

            ForceFalloutCreatureBodyVisibleVisitor visitor;
            bodyNode->accept(visitor);
            Log(Debug::Verbose) << "FNV/ESM4 diag: forced creature body render mask for " << editorId
                             << " path=" << bodyPath
                             << " rigged=" << visitor.mRigGeometryCount
                             << " static=" << visitor.mStaticGeometryCount
                             << " other=" << visitor.mOtherDrawableCount;
        }

        bool isFalloutCreatureDestructionAddon(const VFS::Path::Normalized& bodyPath)
        {
            const std::string lowered = toLowerAscii(bodyPath.value());
            // Protectrons and similar Fallout creatures list their post-destruction
            // replacements beside the intact body in NIFZ.  They are not ordinary
            // PNAM equipment layers and must stay hidden until limb/destruction state
            // explicitly selects them.
            return lowered.find("blowaway") != std::string::npos;
        }

        bool shouldForceFalloutCreatureAddonVisible(
            const VFS::Path::Normalized& bodyPath, std::string_view nifPrn)
        {
            if (nifPrn.empty())
                return true;

            const std::string lowered = toLowerAscii(bodyPath.value());
            // Prn attachment roots in FO3/FNV are commonly flagged hidden even
            // when their intact render geometry is the actor's active layer.
            // Restore those layers, but leave authored transition/static screens
            // and effects under controller ownership. This makes an actor-specific
            // face/voice add-on visible without also forcing white noise or smoke.
            return !isFalloutCreatureDestructionAddon(bodyPath)
                && lowered.find("screenstatic") == std::string::npos
                && lowered.find("smoke") == std::string::npos
                && lowered.find("particle") == std::string::npos
                && lowered.find("effect") == std::string::npos;
        }

        std::string creatureScreenGlMode(const osg::StateSet* stateSet, GLenum mode)
        {
            if (stateSet == nullptr)
                return "inherit";

            const osg::StateAttribute::GLModeValue value = stateSet->getMode(mode);
            std::ostringstream stream;
            stream << (((value & osg::StateAttribute::ON) != 0) ? "on"
                                                                : ((value & osg::StateAttribute::OFF) != 0) ? "off"
                                                                                                            : "inherit")
                   << "(0x" << std::hex << value << std::dec << ")";
            return stream.str();
        }

        std::string creatureScreenCallbackChain(const osg::Callback* callback)
        {
            std::ostringstream stream;
            bool first = true;
            for (const osg::Callback* current = callback; current != nullptr; current = current->getNestedCallback())
            {
                if (!first)
                    stream << ">";
                first = false;
                stream << current->libraryName() << "::" << current->className();
            }
            return first ? std::string("none") : stream.str();
        }

        std::string creatureScreenProgramSummary(const osg::StateSet* stateSet)
        {
            const auto* program = stateSet == nullptr
                ? nullptr
                : dynamic_cast<const osg::Program*>(stateSet->getAttribute(osg::StateAttribute::PROGRAM));
            if (program == nullptr)
                return "none";

            std::ostringstream stream;
            stream << "name='" << program->getName() << "',shaders=[";
            for (unsigned int i = 0; i < program->getNumShaders(); ++i)
            {
                if (i != 0)
                    stream << ";";
                const osg::Shader* shader = program->getShader(i);
                if (shader == nullptr)
                {
                    stream << "null";
                    continue;
                }

                const std::string& source = shader->getShaderSource();
                stream << "{type=" << static_cast<int>(shader->getType()) << ",name='" << shader->getName()
                       << "',bytes=" << source.size()
                       << ",screenUniform="
                       << (source.find("creatureScreenDebugSolidColor") != std::string::npos)
                       << ",directMagenta="
                       << (source.find("OPENMW_FNV_DIRECT_MAGENTA_COVERAGE") != std::string::npos) << "}";
            }
            stream << "]";
            return stream.str();
        }

        std::string creatureScreenStateSummary(const osg::StateSet* stateSet)
        {
            if (stateSet == nullptr)
                return "none";

            std::ostringstream stream;
            stream << "cull=" << creatureScreenGlMode(stateSet, GL_CULL_FACE)
                   << ",depthTest=" << creatureScreenGlMode(stateSet, GL_DEPTH_TEST)
                   << ",blend=" << creatureScreenGlMode(stateSet, GL_BLEND)
                   << ",binMode=" << static_cast<int>(stateSet->getRenderBinMode())
                   << ",bin=" << stateSet->getBinNumber() << ":" << stateSet->getBinName()
                   << ",nestBins=" << stateSet->getNestRenderBins()
                   << ",program={" << creatureScreenProgramSummary(stateSet) << "}";

            if (const auto* depth
                = dynamic_cast<const osg::Depth*>(stateSet->getAttribute(osg::StateAttribute::DEPTH)))
            {
                stream << ",depth={func=" << static_cast<int>(depth->getFunction())
                       << ",write=" << depth->getWriteMask() << ",range=(" << depth->getZNear() << ","
                       << depth->getZFar() << ")}";
            }
            else
                stream << ",depth={none}";

            if (const auto* blend
                = dynamic_cast<const osg::BlendFunc*>(stateSet->getAttribute(osg::StateAttribute::BLENDFUNC)))
            {
                stream << ",blendFunc={srcRGB=0x" << std::hex << blend->getSourceRGB()
                       << ",dstRGB=0x" << blend->getDestinationRGB() << ",srcA=0x" << blend->getSourceAlpha()
                       << ",dstA=0x" << blend->getDestinationAlpha() << std::dec << "}";
            }
            else
                stream << ",blendFunc={none}";

            if (const auto* cull
                = dynamic_cast<const osg::CullFace*>(stateSet->getAttribute(osg::StateAttribute::CULLFACE)))
                stream << ",cullFace=0x" << std::hex << cull->getMode() << std::dec;
            else
                stream << ",cullFace=none";

            if (const auto* material
                = dynamic_cast<const osg::Material*>(stateSet->getAttribute(osg::StateAttribute::MATERIAL)))
            {
                const osg::Vec4f diffuse = material->getDiffuse(osg::Material::FRONT);
                const osg::Vec4f emission = material->getEmission(osg::Material::FRONT);
                stream << ",material={diffuse=(" << diffuse.r() << "," << diffuse.g() << "," << diffuse.b()
                       << "," << diffuse.a() << "),emission=(" << emission.r() << "," << emission.g() << ","
                       << emission.b() << "," << emission.a() << ")}";
            }
            else
                stream << ",material={none}";

            const auto* texture = dynamic_cast<const osg::Texture2D*>(
                stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
            const osg::Image* image = texture == nullptr ? nullptr : texture->getImage();
            stream << ",texture=" << (image == nullptr ? std::string("none") : image->getFileName());

            if (const auto* texMat = dynamic_cast<const osg::TexMat*>(
                    stateSet->getTextureAttribute(0, osg::StateAttribute::TEXMAT)))
            {
                const osg::Matrixf& matrix = texMat->getMatrix();
                stream << ",texMat={(" << matrix(0, 0) << "," << matrix(0, 1) << "," << matrix(0, 3) << ");("
                       << matrix(1, 0) << "," << matrix(1, 1) << "," << matrix(1, 3) << ");trans=("
                       << matrix(3, 0) << "," << matrix(3, 1) << ")}";
            }
            else
                stream << ",texMat={none}";

            if (const osg::Uniform* uniform = stateSet->getUniform("useNoLightingEmission"))
            {
                bool value = false;
                stream << ",useNoLightingEmission=" << (uniform->get(value) ? (value ? "1" : "0") : "invalid");
            }
            if (const osg::Uniform* uniform = stateSet->getUniform("useNoLightingVertexColor"))
            {
                bool value = false;
                stream << ",useNoLightingVertexColor=" << (uniform->get(value) ? (value ? "1" : "0") : "invalid");
            }
            if (const osg::Uniform* uniform = stateSet->getUniform("emissiveMult"))
            {
                float value = 0.f;
                stream << ",emissiveMult=" << (uniform->get(value) ? value : -1.f);
            }
            stream << ",updateCallbacks=" << creatureScreenCallbackChain(stateSet->getUpdateCallback());
            return stream.str();
        }

        struct FalloutCreatureScreenTriangleAudit
        {
            void operator()(const osg::Vec3& v0, const osg::Vec3& v1, const osg::Vec3& v2, bool = false)
            {
                ++mTriangles;
                const float twiceArea = ((v1 - v0) ^ (v2 - v0)).length();
                if (!std::isfinite(twiceArea))
                {
                    ++mNonFinite;
                    return;
                }
                if (twiceArea <= 1e-6f)
                    ++mDegenerate;
                mMinTwiceArea = std::min(mMinTwiceArea, twiceArea);
                mMaxTwiceArea = std::max(mMaxTwiceArea, twiceArea);
            }

            unsigned int mTriangles = 0;
            unsigned int mDegenerate = 0;
            unsigned int mNonFinite = 0;
            float mMinTwiceArea = std::numeric_limits<float>::max();
            float mMaxTwiceArea = 0.f;
        };

        class FalloutCreatureScreenStateVisitor : public osg::NodeVisitor
        {
        public:
            FalloutCreatureScreenStateVisitor(std::string_view editorId, unsigned int frame)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mEditorId(editorId)
                , mFrame(frame)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                const std::string loweredName = toLowerAscii(drawable.getName());
                if (loweredName != "screen01:0" && loweredName.find("screenreflection") == std::string::npos
                    && loweredName != "screenstatic:0" && loweredName != "screen01root:0")
                    return;

                osg::Node::NodeMask effectiveMask = drawable.getNodeMask();
                std::ostringstream pathState;
                bool first = true;
                for (const osg::Node* node : getNodePath())
                {
                    if (node == nullptr)
                        continue;
                    effectiveMask &= node->getNodeMask();
                    if (!first)
                        pathState << ";";
                    first = false;
                    pathState << "'" << node->getName() << "'={type=" << node->className() << ",mask=0x"
                              << std::hex << node->getNodeMask() << std::dec
                              << ",cullingActive=" << node->getCullingActive()
                              << ",updateCallbacks=" << creatureScreenCallbackChain(node->getUpdateCallback())
                              << ",state={" << creatureScreenStateSummary(node->getStateSet()) << "}}";
                }

                const osg::Geometry* geometry = dynamic_cast<const osg::Geometry*>(&drawable);
                const osg::BoundingBox box = drawable.getBoundingBox();
                const osg::Matrix actorMatrix = osg::computeLocalToWorld(getNodePath());
                const osg::Vec3f actorCenter = box.valid() ? box.center() * actorMatrix : osg::Vec3f();
                const osg::Vec3f translation = actorMatrix.getTrans();
                const osg::Quat rotation = actorMatrix.getRotate();
                unsigned int primitiveSets = geometry == nullptr ? 0 : geometry->getNumPrimitiveSets();
                unsigned int vertices = geometry == nullptr || geometry->getVertexArray() == nullptr
                    ? 0
                    : geometry->getVertexArray()->getNumElements();
                unsigned int texCoords = geometry == nullptr || geometry->getTexCoordArray(0) == nullptr
                    ? 0
                    : geometry->getTexCoordArray(0)->getNumElements();
                osg::TriangleFunctor<FalloutCreatureScreenTriangleAudit> triangleAudit;
                drawable.accept(triangleAudit);
                const float minTwiceArea
                    = triangleAudit.mTriangles == 0 ? 0.f : triangleAudit.mMinTwiceArea;

                std::ostringstream primitiveSummary;
                if (geometry != nullptr)
                {
                    for (unsigned int i = 0; i < geometry->getNumPrimitiveSets(); ++i)
                    {
                        if (i != 0)
                            primitiveSummary << ";";
                        const osg::PrimitiveSet* primitive = geometry->getPrimitiveSet(i);
                        primitiveSummary << "{mode=0x" << std::hex
                                         << (primitive == nullptr ? 0 : primitive->getMode()) << std::dec
                                         << ",indices="
                                         << (primitive == nullptr ? 0 : primitive->getNumIndices()) << "}";
                    }
                }

                Log(Debug::Info) << "FNV/ESM4 CREATURE SCREEN LIVE STATE editor=" << mEditorId
                                 << " frame=" << mFrame << " drawable='" << drawable.getName() << "'"
                                 << " mask=0x" << std::hex << drawable.getNodeMask() << " effectiveMask=0x"
                                 << effectiveMask << std::dec << " cullingActive=" << drawable.getCullingActive()
                                 << " vertices=" << vertices << " texCoords=" << texCoords
                                 << " primitiveSets=" << primitiveSets << " primitives=[" << primitiveSummary.str()
                                 << "] triangles=" << triangleAudit.mTriangles
                                 << " degenerateTriangles=" << triangleAudit.mDegenerate
                                 << " nonFiniteTriangles=" << triangleAudit.mNonFinite
                                 << " twiceAreaRange=(" << minTwiceArea << "," << triangleAudit.mMaxTwiceArea
                                 << ") actorCenter=(" << actorCenter.x() << ","
                                 << actorCenter.y() << "," << actorCenter.z() << ") actorMatrixT=("
                                 << translation.x() << "," << translation.y() << "," << translation.z()
                                 << ") actorMatrixQ=(" << rotation.x() << "," << rotation.y() << ","
                                 << rotation.z() << "," << rotation.w() << ") drawableState={"
                                 << creatureScreenStateSummary(drawable.getStateSet()) << "} path={"
                                 << pathState.str() << "}";
            }

        private:
            std::string mEditorId;
            unsigned int mFrame = 0;
        };

        struct FalloutCreatureScreenClipVertex
        {
            osg::Vec4d mClip;
            osg::Vec3d mNdc;
            unsigned int mOutcode = 0;
            bool mFinite = false;
            bool mDivisible = false;
        };

        FalloutCreatureScreenClipVertex transformFalloutCreatureScreenVertex(
            const osg::Vec3d& vertex, const osg::Matrixd& modelViewProjection)
        {
            FalloutCreatureScreenClipVertex result;
            result.mClip = osg::Vec4d(vertex.x(), vertex.y(), vertex.z(), 1.0) * modelViewProjection;
            result.mFinite = std::isfinite(result.mClip.x()) && std::isfinite(result.mClip.y())
                && std::isfinite(result.mClip.z()) && std::isfinite(result.mClip.w());
            if (!result.mFinite)
                return result;

            const double w = result.mClip.w();
            if (result.mClip.x() < -w)
                result.mOutcode |= 1u << 0;
            if (result.mClip.x() > w)
                result.mOutcode |= 1u << 1;
            if (result.mClip.y() < -w)
                result.mOutcode |= 1u << 2;
            if (result.mClip.y() > w)
                result.mOutcode |= 1u << 3;
            if (result.mClip.z() < -w)
                result.mOutcode |= 1u << 4;
            if (result.mClip.z() > w)
                result.mOutcode |= 1u << 5;

            result.mDivisible = std::abs(w) > 1e-12;
            if (result.mDivisible)
            {
                result.mNdc.set(result.mClip.x() / w, result.mClip.y() / w, result.mClip.z() / w);
                result.mDivisible = std::isfinite(result.mNdc.x()) && std::isfinite(result.mNdc.y())
                    && std::isfinite(result.mNdc.z());
            }
            return result;
        }

        struct FalloutCreatureScreenVertexAudit
        {
            void add(const osg::Vec3d& vertex, const osg::Matrixd& modelViewProjection, const osg::Viewport* viewport)
            {
                ++mVertices;
                const FalloutCreatureScreenClipVertex transformed
                    = transformFalloutCreatureScreenVertex(vertex, modelViewProjection);
                if (!transformed.mFinite)
                {
                    ++mNonFinite;
                    return;
                }

                ++mFinite;
                if (transformed.mClip.w() <= 0.0)
                    ++mBehindEye;
                if (!transformed.mDivisible)
                {
                    ++mZeroW;
                    return;
                }
                if (transformed.mOutcode == 0)
                    ++mInside;
                for (unsigned int plane = 0; plane < 6; ++plane)
                    if ((transformed.mOutcode & (1u << plane)) != 0)
                        ++mOutside[plane];

                const osg::Vec3d& ndc = transformed.mNdc;
                if (!mHasNdc)
                {
                    mNdcMin = ndc;
                    mNdcMax = ndc;
                    mHasNdc = true;
                }
                else
                {
                    for (unsigned int axis = 0; axis < 3; ++axis)
                    {
                        mNdcMin[axis] = std::min(mNdcMin[axis], ndc[axis]);
                        mNdcMax[axis] = std::max(mNdcMax[axis], ndc[axis]);
                    }
                }

                if (viewport != nullptr)
                {
                    const osg::Vec2d window(viewport->x() + (ndc.x() + 1.0) * viewport->width() * 0.5,
                        viewport->y() + (ndc.y() + 1.0) * viewport->height() * 0.5);
                    if (!mHasWindow)
                    {
                        mWindowMin = window;
                        mWindowMax = window;
                        mHasWindow = true;
                    }
                    else
                    {
                        for (unsigned int axis = 0; axis < 2; ++axis)
                        {
                            mWindowMin[axis] = std::min(mWindowMin[axis], window[axis]);
                            mWindowMax[axis] = std::max(mWindowMax[axis], window[axis]);
                        }
                    }
                }
            }

            unsigned int mVertices = 0;
            unsigned int mFinite = 0;
            unsigned int mNonFinite = 0;
            unsigned int mZeroW = 0;
            unsigned int mBehindEye = 0;
            unsigned int mInside = 0;
            std::array<unsigned int, 6> mOutside{};
            bool mHasNdc = false;
            osg::Vec3d mNdcMin;
            osg::Vec3d mNdcMax;
            bool mHasWindow = false;
            osg::Vec2d mWindowMin;
            osg::Vec2d mWindowMax;
        };

        double falloutCreatureScreenClipDistance(const osg::Vec4d& vertex, unsigned int plane)
        {
            switch (plane)
            {
                case 0:
                    return vertex.x() + vertex.w();
                case 1:
                    return vertex.w() - vertex.x();
                case 2:
                    return vertex.y() + vertex.w();
                case 3:
                    return vertex.w() - vertex.y();
                case 4:
                    return vertex.z() + vertex.w();
                default:
                    return vertex.w() - vertex.z();
            }
        }

        std::vector<osg::Vec4d> clipFalloutCreatureScreenTriangle(std::vector<osg::Vec4d> polygon)
        {
            for (unsigned int plane = 0; plane < 6 && !polygon.empty(); ++plane)
            {
                std::vector<osg::Vec4d> clipped;
                clipped.reserve(polygon.size() + 1);
                osg::Vec4d previous = polygon.back();
                double previousDistance = falloutCreatureScreenClipDistance(previous, plane);
                bool previousInside = previousDistance >= 0.0;
                for (const osg::Vec4d& current : polygon)
                {
                    const double currentDistance = falloutCreatureScreenClipDistance(current, plane);
                    const bool currentInside = currentDistance >= 0.0;
                    if (currentInside != previousInside)
                    {
                        const double denominator = previousDistance - currentDistance;
                        if (std::abs(denominator) > 1e-18)
                        {
                            const double ratio = previousDistance / denominator;
                            clipped.push_back(previous + (current - previous) * ratio);
                        }
                    }
                    if (currentInside)
                        clipped.push_back(current);
                    previous = current;
                    previousDistance = currentDistance;
                    previousInside = currentInside;
                }
                polygon = std::move(clipped);
            }
            return polygon;
        }

        struct FalloutCreatureScreenRenderTriangleAudit
        {
            void operator()(const osg::Vec3& v0, const osg::Vec3& v1, const osg::Vec3& v2, bool = false)
            {
                ++mTriangles;
                const FalloutCreatureScreenClipVertex transformed[]
                    = { transformFalloutCreatureScreenVertex(v0, mModelViewProjection),
                          transformFalloutCreatureScreenVertex(v1, mModelViewProjection),
                          transformFalloutCreatureScreenVertex(v2, mModelViewProjection) };
                if (!transformed[0].mFinite || !transformed[1].mFinite || !transformed[2].mFinite)
                {
                    ++mNonFinite;
                    return;
                }

                std::vector<osg::Vec4d> polygon
                    = clipFalloutCreatureScreenTriangle(
                        { transformed[0].mClip, transformed[1].mClip, transformed[2].mClip });
                if (polygon.size() < 3)
                {
                    ++mClippedAway;
                    return;
                }

                std::vector<osg::Vec2d> ndc;
                ndc.reserve(polygon.size());
                for (const osg::Vec4d& vertex : polygon)
                {
                    if (std::abs(vertex.w()) <= 1e-12)
                    {
                        ++mNonFinite;
                        return;
                    }
                    ndc.emplace_back(vertex.x() / vertex.w(), vertex.y() / vertex.w());
                }

                double twiceNdcArea = 0.0;
                for (std::size_t i = 0; i < ndc.size(); ++i)
                {
                    const osg::Vec2d& current = ndc[i];
                    const osg::Vec2d& next = ndc[(i + 1) % ndc.size()];
                    twiceNdcArea += current.x() * next.y() - next.x() * current.y();
                }
                const double ndcArea = std::abs(twiceNdcArea) * 0.5;
                const double windowArea = mViewport == nullptr
                    ? ndcArea
                    : ndcArea * mViewport->width() * mViewport->height() * 0.25;
                if (!std::isfinite(windowArea))
                {
                    ++mNonFinite;
                    return;
                }
                if (windowArea <= 1e-6)
                {
                    ++mZeroArea;
                    return;
                }

                ++mRasterable;
                if (windowArea >= 0.5)
                    ++mAtLeastHalfPixel;
                mMinWindowArea = std::min(mMinWindowArea, windowArea);
                mMaxWindowArea = std::max(mMaxWindowArea, windowArea);
            }

            osg::Matrixd mModelViewProjection;
            const osg::Viewport* mViewport = nullptr;
            unsigned int mTriangles = 0;
            unsigned int mNonFinite = 0;
            unsigned int mClippedAway = 0;
            unsigned int mZeroArea = 0;
            unsigned int mRasterable = 0;
            unsigned int mAtLeastHalfPixel = 0;
            double mMinWindowArea = std::numeric_limits<double>::max();
            double mMaxWindowArea = 0.0;
        };

        std::string falloutCreatureScreenMatrixSummary(const osg::Matrixd& matrix)
        {
            std::ostringstream stream;
            stream << "[";
            for (unsigned int row = 0; row < 4; ++row)
            {
                if (row != 0)
                    stream << ";";
                stream << "(";
                for (unsigned int column = 0; column < 4; ++column)
                {
                    if (column != 0)
                        stream << ",";
                    stream << matrix(row, column);
                }
                stream << ")";
            }
            stream << "]";
            return stream.str();
        }

        class FalloutCreatureScreenDrawCallback : public osg::Drawable::DrawCallback
        {
        public:
            explicit FalloutCreatureScreenDrawCallback(std::string editorId)
                : mEditorId(std::move(editorId))
            {
            }

            void drawImplementation(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const override
            {
                const unsigned int call = mCalls.fetch_add(1, std::memory_order_relaxed) + 1;
                osg::State* state = renderInfo.getState();
                const GLenum errorBeforeDraw = glGetError();
                if (call <= 12)
                {
                    const osg::FrameStamp* frameStamp = state == nullptr ? nullptr : state->getFrameStamp();
                    const osg::Camera* camera = renderInfo.getCurrentCamera();
                    const osg::Viewport* viewport = state == nullptr ? nullptr : state->getCurrentViewport();
                    const osg::Matrixd modelView
                        = state == nullptr ? osg::Matrixd::identity() : state->getModelViewMatrix();
                    const osg::Matrixd projection
                        = state == nullptr ? osg::Matrixd::identity() : state->getProjectionMatrix();
                    const osg::Matrixd modelViewProjection = modelView * projection;

                    FalloutCreatureScreenVertexAudit vertexAudit;
                    std::string vertexArrayType = "none";
                    osg::TriangleFunctor<FalloutCreatureScreenRenderTriangleAudit> triangleAudit;
                    triangleAudit.mModelViewProjection = modelViewProjection;
                    triangleAudit.mViewport = viewport;
                    const auto* geometry = dynamic_cast<const osg::Geometry*>(drawable);
                    if (geometry != nullptr)
                    {
                        const osg::Array* vertices = geometry->getVertexArray();
                        if (vertices != nullptr)
                            vertexArrayType = vertices->className();
                        if (const auto* vec3 = dynamic_cast<const osg::Vec3Array*>(vertices))
                            for (const osg::Vec3& vertex : *vec3)
                                vertexAudit.add(vertex, modelViewProjection, viewport);
                        else if (const auto* vec3d = dynamic_cast<const osg::Vec3dArray*>(vertices))
                            for (const osg::Vec3d& vertex : *vec3d)
                                vertexAudit.add(vertex, modelViewProjection, viewport);

                        geometry->accept(triangleAudit);
                    }

                    GLint currentProgram = -1;
                    GLint framebuffer = -1;
                    GLint glViewport[4] = { -1, -1, -1, -1 };
                    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
#ifdef GL_DRAW_FRAMEBUFFER_BINDING
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
#elif defined(GL_FRAMEBUFFER_BINDING)
                    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
#endif
                    glGetIntegerv(GL_VIEWPORT, glViewport);

                    const osg::Program::PerContextProgram* pcp
                        = state == nullptr ? nullptr : state->getLastAppliedProgramObject();
                    const osg::Program* appliedProgram = pcp == nullptr ? nullptr : pcp->getProgram();
                    const unsigned int programHandle = pcp == nullptr ? 0 : pcp->getHandle();
                    GLint glLinkStatus = -1;
                    GLint glValidateStatus = -1;
                    std::string programInfoLog;
                    if (pcp != nullptr)
                    {
                        pcp->getInfoLog(programInfoLog);
                        if (programHandle != 0 && state != nullptr)
                        {
                            osg::GLExtensions* extensions = osg::GLExtensions::Get(state->getContextID(), true);
                            extensions->glGetProgramiv(programHandle, GL_LINK_STATUS, &glLinkStatus);
                            extensions->glGetProgramiv(programHandle, GL_VALIDATE_STATUS, &glValidateStatus);
                        }
                    }

                    const double minWindowArea
                        = triangleAudit.mRasterable == 0 ? 0.0 : triangleAudit.mMinWindowArea;
                    Log(Debug::Info) << "FNV/ESM4 CREATURE SCREEN DRAW COVERAGE editor=" << mEditorId
                                     << " drawable='" << (drawable == nullptr ? std::string("<null>")
                                                                              : drawable->getName())
                                     << " call=" << call << " frame="
                                     << (frameStamp == nullptr ? 0 : frameStamp->getFrameNumber())
                                     << " context=" << (state == nullptr ? 0 : state->getContextID())
                                     << " camera='" << (camera == nullptr ? std::string("<null>") : camera->getName())
                                     << "' cameraRenderOrder="
                                     << (camera == nullptr ? -1 : static_cast<int>(camera->getRenderOrder()))
                                     << ":" << (camera == nullptr ? 0 : camera->getRenderOrderNum())
                                     << " cameraTarget="
                                     << (camera == nullptr ? -1
                                                           : static_cast<int>(camera->getRenderTargetImplementation()))
                                     << " stateViewport=("
                                     << (viewport == nullptr ? 0.0 : viewport->x()) << ","
                                     << (viewport == nullptr ? 0.0 : viewport->y()) << ","
                                     << (viewport == nullptr ? 0.0 : viewport->width()) << ","
                                     << (viewport == nullptr ? 0.0 : viewport->height()) << ")"
                                     << " glViewport=(" << glViewport[0] << "," << glViewport[1] << ","
                                     << glViewport[2] << "," << glViewport[3] << ") framebuffer=" << framebuffer
                                     << " program={current=" << currentProgram << ",handle=" << programHandle
                                     << ",name='"
                                     << (appliedProgram == nullptr ? std::string("<null>") : appliedProgram->getName())
                                     << "',pcpLinked=" << (pcp == nullptr ? 0 : pcp->isLinked())
                                     << ",needsLink=" << (pcp == nullptr ? 0 : pcp->needsLink())
                                     << ",glLink=" << glLinkStatus << ",glValidate=" << glValidateStatus
                                     << ",info='" << programInfoLog << "'}"
                                     << " vertices={array=" << vertexArrayType << ",total=" << vertexAudit.mVertices
                                     << ",finite=" << vertexAudit.mFinite
                                     << ",nonFinite=" << vertexAudit.mNonFinite << ",zeroW=" << vertexAudit.mZeroW
                                     << ",behindEye=" << vertexAudit.mBehindEye << ",inside=" << vertexAudit.mInside
                                     << ",outsideLTRBNF=(" << vertexAudit.mOutside[0] << ","
                                     << vertexAudit.mOutside[1] << "," << vertexAudit.mOutside[2] << ","
                                     << vertexAudit.mOutside[3] << "," << vertexAudit.mOutside[4] << ","
                                     << vertexAudit.mOutside[5] << "),ndcBounds=("
                                     << (vertexAudit.mHasNdc ? vertexAudit.mNdcMin.x() : 0.0) << ","
                                     << (vertexAudit.mHasNdc ? vertexAudit.mNdcMin.y() : 0.0) << ","
                                     << (vertexAudit.mHasNdc ? vertexAudit.mNdcMin.z() : 0.0) << ")->("
                                     << (vertexAudit.mHasNdc ? vertexAudit.mNdcMax.x() : 0.0) << ","
                                     << (vertexAudit.mHasNdc ? vertexAudit.mNdcMax.y() : 0.0) << ","
                                     << (vertexAudit.mHasNdc ? vertexAudit.mNdcMax.z() : 0.0) << "),windowBounds=("
                                     << (vertexAudit.mHasWindow ? vertexAudit.mWindowMin.x() : 0.0) << ","
                                     << (vertexAudit.mHasWindow ? vertexAudit.mWindowMin.y() : 0.0) << ")->("
                                     << (vertexAudit.mHasWindow ? vertexAudit.mWindowMax.x() : 0.0) << ","
                                     << (vertexAudit.mHasWindow ? vertexAudit.mWindowMax.y() : 0.0) << ")}"
                                     << " triangles={total=" << triangleAudit.mTriangles
                                     << ",nonFinite=" << triangleAudit.mNonFinite
                                     << ",clippedAway=" << triangleAudit.mClippedAway
                                     << ",zeroArea=" << triangleAudit.mZeroArea
                                     << ",rasterable=" << triangleAudit.mRasterable
                                     << ",halfPixel=" << triangleAudit.mAtLeastHalfPixel
                                     << ",windowAreaRange=(" << minWindowArea << "," << triangleAudit.mMaxWindowArea
                                     << ")} modelView=" << falloutCreatureScreenMatrixSummary(modelView)
                                     << " projection=" << falloutCreatureScreenMatrixSummary(projection)
                                     << " glErrorBefore=0x" << std::hex << errorBeforeDraw << std::dec;
                }

                if (drawable != nullptr)
                    drawable->drawImplementation(renderInfo);

                if (call <= 12)
                {
                    const GLenum errorAfterDraw = glGetError();
                    Log(Debug::Info) << "FNV/ESM4 CREATURE SCREEN DRAW RESULT editor=" << mEditorId
                                     << " call=" << call << " glErrorAfter=0x" << std::hex << errorAfterDraw
                                     << std::dec;
                }
            }

        private:
            std::string mEditorId;
            mutable std::atomic_uint mCalls{ 0 };
        };

        osg::ref_ptr<osg::Program> createFalloutCreatureScreenCoverageProgram()
        {
            static constexpr const char* vertexSource = R"glsl(
#version 120
// OPENMW_FNV_DIRECT_MAGENTA_COVERAGE
void main()
{
    gl_Position = ftransform();
}
)glsl";
            static constexpr const char* fragmentSource = R"glsl(
#version 120
// OPENMW_FNV_DIRECT_MAGENTA_COVERAGE
void main()
{
    gl_FragData[0] = vec4(1.0, 0.0, 1.0, 1.0);
}
)glsl";

            osg::ref_ptr<osg::Program> program = new osg::Program;
            program->setName("OPENMW FNV Screen01 direct magenta coverage");
            program->addShader(new osg::Shader(osg::Shader::VERTEX, vertexSource));
            program->addShader(new osg::Shader(osg::Shader::FRAGMENT, fragmentSource));
            return program;
        }

        std::string falloutCreatureScreenNodePathSummary(const osg::NodePath& path)
        {
            std::ostringstream stream;
            for (std::size_t i = 0; i < path.size(); ++i)
            {
                if (i != 0)
                    stream << ">";
                const osg::Node* node = path[i];
                stream << (node == nullptr ? std::string("<null>") : node->getName()) << "["
                       << (node == nullptr ? std::string("<null>") : node->className()) << "]";
            }
            return stream.str();
        }

        std::string falloutCreatureScreenNodeGateSummary(const osg::Node* node, const osg::NodeVisitor* visitor)
        {
            if (node == nullptr)
                return "none";

            std::ostringstream stream;
            if (const auto* switchNode = dynamic_cast<const osg::Switch*>(node))
            {
                stream << "Switch{default=" << switchNode->getNewChildDefaultValue() << ",children=[";
                for (unsigned int i = 0; i < switchNode->getNumChildren(); ++i)
                {
                    if (i != 0)
                        stream << ";";
                    const osg::Node* child = switchNode->getChild(i);
                    stream << i << ":'" << (child == nullptr ? std::string("<null>") : child->getName())
                           << "'=" << switchNode->getValue(i);
                }
                stream << "]}";
                return stream.str();
            }

            if (const auto* multiSwitch = dynamic_cast<const osgSim::MultiSwitch*>(node))
            {
                const unsigned int active = multiSwitch->getActiveSwitchSet();
                stream << "MultiSwitch{active=" << active << ",sets="
                       << multiSwitch->getSwitchSetList().size() << ",children=[";
                for (unsigned int i = 0; i < multiSwitch->getNumChildren(); ++i)
                {
                    if (i != 0)
                        stream << ";";
                    const osg::Node* child = multiSwitch->getChild(i);
                    const bool enabled = active < multiSwitch->getSwitchSetList().size()
                        && i < multiSwitch->getValueList(active).size() && multiSwitch->getValue(active, i);
                    stream << i << ":'" << (child == nullptr ? std::string("<null>") : child->getName())
                           << "'=" << enabled;
                }
                stream << "]}";
                return stream.str();
            }

            if (const auto* lod = dynamic_cast<const osg::LOD*>(node))
            {
                stream << "LOD{mode=" << static_cast<int>(lod->getRangeMode()) << ",center=(" << lod->getCenter().x()
                       << "," << lod->getCenter().y() << "," << lod->getCenter().z() << "),distance="
                       << (visitor == nullptr ? -1.f : visitor->getDistanceToViewPoint(lod->getCenter(), true))
                       << ",ranges=[";
                for (unsigned int i = 0; i < lod->getNumRanges(); ++i)
                {
                    if (i != 0)
                        stream << ";";
                    stream << i << ":(" << lod->getMinRange(i) << "," << lod->getMaxRange(i) << ")";
                }
                stream << "]}";
                return stream.str();
            }

            return "none";
        }

        std::string falloutCreatureScreenIncomingGateSummary(const osg::Node* node, const osg::NodePath& path,
            const osg::NodeVisitor* visitor)
        {
            if (node == nullptr || path.size() < 2)
                return "root";
            const osg::Node* parentNode = path[path.size() - 2];
            const osg::Group* parent = parentNode == nullptr ? nullptr : parentNode->asGroup();
            if (parent == nullptr)
                return "parent-not-group";

            const unsigned int childIndex = parent->getChildIndex(node);
            std::ostringstream stream;
            stream << "parent='" << parent->getName() << "' index=" << childIndex;
            if (const auto* switchNode = dynamic_cast<const osg::Switch*>(parent))
                stream << " switchEnabled="
                       << (childIndex < switchNode->getNumChildren() ? switchNode->getValue(childIndex) : false);
            else if (const auto* multiSwitch = dynamic_cast<const osgSim::MultiSwitch*>(parent))
            {
                const unsigned int active = multiSwitch->getActiveSwitchSet();
                stream << " multiSwitchSet=" << active << " enabled="
                       << (active < multiSwitch->getSwitchSetList().size()
                               && childIndex < multiSwitch->getValueList(active).size()
                               && multiSwitch->getValue(active, childIndex));
            }
            else if (const auto* lod = dynamic_cast<const osg::LOD*>(parent))
            {
                stream << " lodDistance="
                       << (visitor == nullptr ? -1.f : visitor->getDistanceToViewPoint(lod->getCenter(), true));
                if (childIndex < lod->getNumRanges())
                    stream << " range=(" << lod->getMinRange(childIndex) << "," << lod->getMaxRange(childIndex)
                           << ")";
            }
            return stream.str();
        }

        class FalloutCreatureScreenCullPathCallback
            : public SceneUtil::NodeCallback<FalloutCreatureScreenCullPathCallback>
        {
        public:
            FalloutCreatureScreenCullPathCallback() = default;

            FalloutCreatureScreenCullPathCallback(std::string editorId, std::string expectedPath, unsigned int index)
                : mEditorId(std::move(editorId))
                , mExpectedPath(std::move(expectedPath))
                , mIndex(index)
            {
            }

            FalloutCreatureScreenCullPathCallback(
                const FalloutCreatureScreenCullPathCallback& copy, const osg::CopyOp& copyop)
                : SceneUtil::NodeCallback<FalloutCreatureScreenCullPathCallback>(copy, copyop)
                , mEditorId(copy.mEditorId)
                , mExpectedPath(copy.mExpectedPath)
                , mIndex(copy.mIndex)
            {
            }

            META_Object(MWRender, FalloutCreatureScreenCullPathCallback)

            void operator()(osg::Node* node, osg::NodeVisitor* visitor)
            {
                const unsigned int call = mCalls.fetch_add(1, std::memory_order_relaxed) + 1;
                const bool shouldLog = call <= 12;
                const osg::NodePath emptyPath;
                const osg::NodePath& path = visitor == nullptr ? emptyPath : visitor->getNodePath();
                auto* cullVisitor = dynamic_cast<osgUtil::CullVisitor*>(visitor);
                const osg::Camera* camera = cullVisitor == nullptr ? nullptr : cullVisitor->getCurrentCamera();
                if (shouldLog)
                {
                    Log(Debug::Info) << "FNV/ESM4 CREATURE SCREEN CULL PATH ENTER editor=" << mEditorId
                                     << " index=" << mIndex << " call=" << call << " node='"
                                     << (node == nullptr ? std::string("<null>") : node->getName()) << "' type="
                                     << (node == nullptr ? std::string("<null>") : node->className())
                                     << " visitorType="
                                     << (visitor == nullptr ? -1 : static_cast<int>(visitor->getVisitorType()))
                                     << " camera='"
                                     << (camera == nullptr ? std::string("<null>") : camera->getName())
                                     << "' traversalMask=0x" << std::hex
                                     << (visitor == nullptr ? 0u : visitor->getTraversalMask())
                                     << " overrideMask=0x"
                                     << (visitor == nullptr ? 0u : visitor->getNodeMaskOverride()) << std::dec
                                     << " nodeMask=0x" << std::hex
                                     << (node == nullptr ? 0u : node->getNodeMask()) << std::dec
                                     << " validMask="
                                     << (visitor != nullptr && node != nullptr && visitor->validNodeMask(*node))
                                     << " cullingActive=" << (node != nullptr && node->getCullingActive())
                                     << " parents=" << (node == nullptr ? 0u : node->getNumParents())
                                     << " gate={" << falloutCreatureScreenNodeGateSummary(node, visitor) << "}"
                                     << " incoming={"
                                     << falloutCreatureScreenIncomingGateSummary(node, path, visitor) << "}"
                                     << " expectedPath={" << mExpectedPath << "} actualPath={"
                                     << falloutCreatureScreenNodePathSummary(path) << "}";
                }

                traverse(node, visitor);

                if (shouldLog)
                    Log(Debug::Info) << "FNV/ESM4 CREATURE SCREEN CULL PATH EXIT editor=" << mEditorId
                                     << " index=" << mIndex << " call=" << call << " node='"
                                     << (node == nullptr ? std::string("<null>") : node->getName()) << "'";
            }

        private:
            std::string mEditorId;
            std::string mExpectedPath;
            unsigned int mIndex = 0;
            std::atomic_uint mCalls{ 0 };
        };

        class FalloutCreatureScreenLayerDiagnosticVisitor : public osg::NodeVisitor
        {
        public:
            FalloutCreatureScreenLayerDiagnosticVisitor(
                std::string_view mode, std::string_view editorId, osg::Node* objectRoot)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mMode(mode)
                , mEditorId(editorId)
                , mObjectRoot(objectRoot)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                const std::string loweredName = toLowerAscii(drawable.getName());
                const bool face = loweredName == "screen01:0";
                const bool reflection = loweredName.find("screenreflection") != std::string::npos;
                const bool staticLayer = loweredName == "screenstatic:0";
                const bool rootGeometry = loweredName == "screen01root:0";
                if (!face && !reflection && !staticLayer && !rootGeometry)
                    return;

                if (mMode.starts_with("face-only"))
                    drawable.setNodeMask(face ? ~0u : 0u);
                if (staticLayer && mMode.find("hide-static") != std::string::npos)
                {
                    drawable.setNodeMask(0u);
                    Log(Debug::Info) << "FNV/ESM4 screen layer diagnostic hid static layer editor=" << mEditorId
                                     << " drawable='" << drawable.getName() << "'";
                }
                if (reflection && mMode.find("hide-reflection") != std::string::npos)
                {
                    drawable.setNodeMask(0u);
                    Log(Debug::Info) << "FNV/ESM4 screen layer diagnostic hid reflection layer editor=" << mEditorId
                                     << " drawable='" << drawable.getName() << "'";
                }

                if (!face || getNodePath().empty())
                    return;

                if (mMode.find("cull-path") != std::string::npos)
                    installCullPath(drawable);

                if (mMode.find("direct-program") != std::string::npos)
                {
                    drawable.setNodeMask(~0u);
                    drawable.setCullingActive(false);
                    for (osg::Node* node : getNodePath())
                    {
                        if (node == nullptr)
                            continue;
                        node->setNodeMask(~0u);
                        node->setCullingActive(false);
                    }

                    osg::StateSet* drawableState = drawable.getOrCreateStateSet();
                    constexpr osg::StateAttribute::GLModeValue forced
                        = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
                    constexpr osg::StateAttribute::GLModeValue forcedOff
                        = osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;
                    drawableState->setAttributeAndModes(createFalloutCreatureScreenCoverageProgram(), forced);
                    drawableState->setAttributeAndModes(new osg::ColorMask(true, true, true, true), forced);
                    drawableState->setMode(GL_ALPHA_TEST, forcedOff);
                    drawableState->setMode(GL_BLEND, forcedOff);
                    drawableState->setMode(GL_CULL_FACE, forcedOff);
                    drawableState->setMode(GL_DEPTH_TEST, forcedOff);
                    drawableState->setMode(GL_SCISSOR_TEST, forcedOff);
                    drawableState->setMode(GL_STENCIL_TEST, forcedOff);
                    if (drawable.getDrawCallback() == nullptr)
                        drawable.setDrawCallback(new FalloutCreatureScreenDrawCallback(mEditorId));
                    else
                        Log(Debug::Warning) << "FNV/ESM4 direct screen coverage skipped draw callback for editor="
                                            << mEditorId << " drawable='" << drawable.getName()
                                            << "' because an authored callback is already installed";
                    Log(Debug::Info) << "FNV/ESM4 direct screen coverage installed editor=" << mEditorId
                                     << " drawable='" << drawable.getName() << "'";
                }

                osg::Node* stateNode = getNodePath().back();
                if (stateNode == &drawable && getNodePath().size() > 1)
                    stateNode = getNodePath()[getNodePath().size() - 2];
                if (stateNode == nullptr)
                    return;

                if (mMode.find("cull-off") != std::string::npos)
                    stateNode->getOrCreateStateSet()->setMode(
                        GL_CULL_FACE, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                if (mMode.find("depth-off") != std::string::npos)
                    stateNode->getOrCreateStateSet()->setMode(
                        GL_DEPTH_TEST, osg::StateAttribute::OFF | osg::StateAttribute::OVERRIDE);
                if (mMode.find("solid") != std::string::npos)
                    stateNode->getOrCreateStateSet()->addUniform(new osg::Uniform(
                        "creatureScreenDebugSolidColor", true), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            }

        private:
            void installCullPath(osg::Drawable& drawable)
            {
                osg::NodePath expectedPath = getNodePath();
                if (expectedPath.empty() || expectedPath.back() != &drawable)
                    expectedPath.push_back(&drawable);
                const std::string expectedSummary = falloutCreatureScreenNodePathSummary(expectedPath);

                osg::NodePath nodes;
                if (mObjectRoot != nullptr)
                    nodes.push_back(mObjectRoot);
                for (osg::Node* node : expectedPath)
                    if (node != nullptr && std::find(nodes.begin(), nodes.end(), node) == nodes.end())
                        nodes.push_back(node);

                for (unsigned int index = 0; index < nodes.size(); ++index)
                {
                    osg::Node* node = nodes[index];
                    if (!mCullCallbackNodes.insert(node).second)
                        continue;

                    node->addCullCallback(
                        new FalloutCreatureScreenCullPathCallback(mEditorId, expectedSummary, index));

                    const osg::NodePathList parentalPaths = node->getParentalNodePaths();
                    std::ostringstream parentSummary;
                    const std::size_t pathLimit = std::min<std::size_t>(parentalPaths.size(), 6);
                    for (std::size_t parentPath = 0; parentPath < pathLimit; ++parentPath)
                    {
                        if (parentPath != 0)
                            parentSummary << ";";
                        parentSummary << "{" << falloutCreatureScreenNodePathSummary(parentalPaths[parentPath])
                                      << "}";
                    }

                    osg::NodePath prefix;
                    const auto expected = std::find(expectedPath.begin(), expectedPath.end(), node);
                    if (expected != expectedPath.end())
                        prefix.assign(expectedPath.begin(), expected + 1);
                    Log(Debug::Info) << "FNV/ESM4 CREATURE SCREEN CULL PATH INSTALLED editor=" << mEditorId
                                     << " index=" << index << " node='" << node->getName() << "' type="
                                     << node->className() << " mask=0x" << std::hex << node->getNodeMask()
                                     << std::dec << " cullingActive=" << node->getCullingActive()
                                     << " parents=" << node->getNumParents() << " gate={"
                                     << falloutCreatureScreenNodeGateSummary(node, nullptr) << "} incoming={"
                                     << falloutCreatureScreenIncomingGateSummary(node, prefix, nullptr)
                                     << "} parentalPaths=" << parentalPaths.size() << " sample=["
                                     << parentSummary.str() << "] expectedPath={" << expectedSummary << "}";
                }
            }

            std::string mMode;
            std::string mEditorId;
            osg::Node* mObjectRoot = nullptr;
            std::set<osg::Node*> mCullCallbackNodes;
        };

        class FalloutCreatureScreenStateCallback
            : public SceneUtil::NodeCallback<FalloutCreatureScreenStateCallback>
        {
        public:
            FalloutCreatureScreenStateCallback() = default;

            explicit FalloutCreatureScreenStateCallback(std::string editorId)
                : mEditorId(std::move(editorId))
            {
            }

            FalloutCreatureScreenStateCallback(
                const FalloutCreatureScreenStateCallback& copy, const osg::CopyOp& copyop)
                : SceneUtil::NodeCallback<FalloutCreatureScreenStateCallback>(copy, copyop)
                , mEditorId(copy.mEditorId)
                , mFrame(copy.mFrame)
            {
            }

            META_Object(MWRender, FalloutCreatureScreenStateCallback)

            void operator()(osg::Node* node, osg::NodeVisitor* visitor)
            {
                traverse(node, visitor);
                ++mFrame;
                if (mFrame != 1 && mFrame != 60 && mFrame != 240)
                    return;

                FalloutCreatureScreenStateVisitor probe(mEditorId, mFrame);
                node->accept(probe);
            }

        private:
            std::string mEditorId;
            unsigned int mFrame = 0;
        };
    }

    CreatureAnimation::CreatureAnimation(
        const MWWorld::Ptr& ptr, const std::string& model, Resource::ResourceSystem* resourceSystem, bool animated)
        : ActorAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), resourceSystem)
    {
        std::string actorModel = model;
        std::vector<const ESM4::Creature*> creatureTemplates;
        ESM4::CreatureVisualTemplate creatureVisuals;
        if (mPtr.getType() == ESM4::Creature::sRecordId)
        {
            const ESM4::Creature* base = mPtr.get<ESM4::Creature>()->mBase;
            creatureTemplates = collectCreatureRenderingTemplates(*base);
            creatureVisuals = ESM4::resolveCreatureVisualTemplate(creatureTemplates);
            std::string visualModel;
            if (creatureVisuals.mModel != nullptr)
                visualModel = creatureVisuals.mModel->mModel;
            else if (creatureVisuals.mNif != nullptr && !creatureVisuals.mNif->mNif.empty())
                visualModel = creatureVisuals.mNif->mNif.front();
            if (!visualModel.empty())
                actorModel = Misc::ResourceHelpers::correctActorModelPath(
                    correctCreatureBodyPath(visualModel), resourceSystem->getVFS());
        }

        if (!actorModel.empty())
        {
            // Fallout creature skeleton NIFs commonly contain only the authored bone hierarchy; their skinned
            // drawables live in separate NIFZ/PNAM body files. The NIF loader therefore cannot infer skinning while
            // loading skeleton.nif and returns a plain Group. Force a SceneUtil::Skeleton master before attaching
            // those body rigs so update and cull visitors have a real skeleton ancestor to resolve.
            const bool forceSkeleton = mPtr.getType() == ESM4::Creature::sRecordId;
            setObjectRoot(actorModel, forceSkeleton, false, true);

            if (mPtr.getType() == ESM::Creature::sRecordId)
            {
                MWWorld::LiveCellRef<ESM::Creature>* ref = mPtr.get<ESM::Creature>();
                if ((ref->mBase->mFlags & ESM::Creature::Bipedal))
                    addAnimSource(Settings::models().mXbaseanim.get(), actorModel);

                if (animated)
                    addAnimSource(actorModel, actorModel);
            }
            else if (mPtr.getType() == ESM4::Creature::sRecordId)
            {
                MWWorld::LiveCellRef<ESM4::Creature>* ref = mPtr.get<ESM4::Creature>();
                const bool markerCreatureRoot = isMarkerCreatureModel(actorModel);
                if (!markerCreatureRoot)
                    addAnimSource(actorModel, actorModel);

                std::string animationDirectory = actorModel;
                const std::size_t slash = animationDirectory.find_last_of("/\\");
                animationDirectory = slash == std::string::npos ? std::string() : animationDirectory.substr(0, slash + 1);
                std::string normalizedDirectory = animationDirectory;
                VFS::Path::normalizeFilenameInPlace(normalizedDirectory);
                unsigned int attachedBodyNifs = 0;
                const VFS::Manager* vfs = resourceSystem->getVFS();
                std::vector<std::pair<std::string, std::string>> representativePoseKfs;
                if (const FalloutCreatureRepresentativePoseSources* poseSources
                    = getFalloutCreatureRepresentativePoseSources(normalizedDirectory))
                {
                    representativePoseKfs.reserve(poseSources->size());
                    for (const FalloutCreatureRepresentativePoseSource& pose : *poseSources)
                    {
                        std::string normalizedPath;
                        const std::string path = normalizedDirectory + std::string(pose.mRelativeKf);
                        if (findCreatureKf(*vfs, path, normalizedPath))
                            representativePoseKfs.emplace_back(pose.mGroup, std::move(normalizedPath));
                        else
                            Log(Debug::Warning) << "FNV/ESM4 diag: selected same-rig creature pose KF missing"
                                                << " editor=" << ref->mBase->mEditorId << " group=" << pose.mGroup
                                                << " path=" << path << " skeleton=" << actorModel;
                    }
                }
                const auto isRepresentativePoseKf = [&](std::string_view path) {
                    const VFS::Path::Normalized normalized(path);
                    return std::any_of(representativePoseKfs.begin(), representativePoseKfs.end(),
                        [&](const auto& pose) { return pose.second == normalized.value(); });
                };
                const std::vector<std::string> authoredNifs = creatureVisuals.mNif == nullptr
                    ? std::vector<std::string>()
                    : resolveCreatureRelativePaths(creatureVisuals.mNif->mNif, actorModel);
                const std::vector<ESM::FormId> authoredBodyParts = creatureVisuals.mBodyParts == nullptr
                    ? std::vector<ESM::FormId>()
                    : creatureVisuals.mBodyParts->mBodyParts;
                const std::vector<std::string> authoredKfs = creatureVisuals.mKf == nullptr
                    ? std::vector<std::string>()
                    : resolveCreatureRelativePaths(creatureVisuals.mKf->mKf, actorModel);
                const std::vector<std::string> bodyNifs
                    = collectCreatureBodyNifs(authoredNifs, authoredBodyParts, *vfs, animationDirectory);
                for (const std::string& bodyNif : bodyNifs)
                {
                    if (bodyNif.empty())
                        continue;
                    const VFS::Path::Normalized bodyPath = correctCreatureBodyPath(bodyNif);
                    osg::ref_ptr<const osg::Node> bodyTemplate
                        = resourceSystem->getSceneManager()->getTemplate(bodyPath);
                    std::string nifPrn;
                    bodyTemplate->getUserValue("OpenMW.NifPrn", nifPrn);
                    osg::ref_ptr<osg::Node> bodyNode;
                    osg::Group* attachNode = mObjectRoot;
                    if (!nifPrn.empty())
                    {
                        attachNode = getBoneByName(nifPrn);
                        if (attachNode == nullptr)
                        {
                            // A PNAM/Prn is an authored skeletal relationship, not an optional hint.  Root-attaching
                            // an unresolved part makes it render in actor space while its siblings animate in bone
                            // space, producing the characteristic exploded creature.  Keep the failure local and
                            // visible in telemetry instead of corrupting the whole assembly.
                            Log(Debug::Error) << "FNV/ESM4 diag: unresolved creature body Prn; part not attached"
                                              << " editor=" << ref->mBase->mEditorId << " path=" << bodyPath
                                              << " prn=" << nifPrn << " skeleton=" << actorModel;
                            continue;
                        }
                        const auto* attachTransform = dynamic_cast<const osg::MatrixTransform*>(attachNode);
                        Log(Debug::Verbose) << "FNV/ESM4 diag: resolved creature body Prn"
                                         << " editor=" << ref->mBase->mEditorId << " path=" << bodyPath
                                         << " requested='" << nifPrn << "' actual='" << attachNode->getName() << "'"
                                         << " resolution="
                                         << (toLowerAscii(nifPrn) == toLowerAscii(attachNode->getName()) ? "exact"
                                                                                                         : "normalized")
                                         << " local="
                                         << (attachTransform == nullptr
                                                 ? std::string("<group>")
                                                 : falloutCreatureScreenMatrixSummary(attachTransform->getMatrix()));
                    }
                    // Use the same attachment path for root and PNAM parts.  In particular, rigged root parts must
                    // be copied onto the actor's master skeleton; adding an independent NIF instance here creates a
                    // second, unanimated skeleton and lets the body drift away from its authored parents.
                    // A Fallout actor root can be wrapped for coordinate conversion. RigGeometry must remain below
                    // the actual Skeleton, not beside it under that wrapper, because its update/cull initialization
                    // discovers the master skeleton by walking its parental path.
                    osg::Node* attachmentMaster
                        = mSkeleton != nullptr ? static_cast<osg::Node*>(mSkeleton) : mObjectRoot.get();
                    bodyNode = SceneUtil::attach(
                        bodyTemplate, attachmentMaster, {}, attachNode, resourceSystem->getSceneManager());
                    const bool destructionAddon = isFalloutCreatureDestructionAddon(bodyPath);
                    if (destructionAddon)
                    {
                        // Keep the authored replacement in the assembly so a future
                        // destruction-state implementation can select it, but do not
                        // render it on the intact actor.
                        bodyNode->setNodeMask(0u);
                        Log(Debug::Verbose) << "FNV/ESM4 diag: hid inactive creature destruction add-on"
                                         << " editor=" << ref->mBase->mEditorId << " path=" << bodyPath
                                         << " prn=" << nifPrn;
                    }
                    const bool requireVisible = shouldForceFalloutCreatureAddonVisible(bodyPath, nifPrn);
                    if (requireVisible)
                        forceFalloutCreatureBodyVisible(bodyNode, ref->mBase->mEditorId, bodyPath);
                    auditCreatureRigTopology(
                        bodyNode, mSkeleton, mObjectRoot, ref->mBase->mEditorId, bodyPath, requireVisible);
                    if (std::getenv("OPENMW_FNV_CREATURE_BODY_DIAG") != nullptr)
                    {
                        Log(Debug::Verbose) << "FNV/ESM4 diag: attached creature body nif "
                                         << ref->mBase->mEditorId << " nifSource="
                                         << (creatureVisuals.mNif == nullptr ? std::string("<none>")
                                                                            : creatureVisuals.mNif->mEditorId)
                                         << " bodyPartSource="
                                         << (creatureVisuals.mBodyParts == nullptr
                                                 ? std::string("<none>")
                                                 : creatureVisuals.mBodyParts->mEditorId)
                                         << " path=" << bodyPath << " prn="
                                         << (nifPrn.empty() ? std::string("<root>") : nifPrn);
                        logCreatureBounds("body", ref->mBase->mEditorId, bodyPath.value(), *bodyNode);
                    }
                    ++attachedBodyNifs;
                }
                // addAnimSource can materialize the node map before the static add-ons
                // are assembled. Rebuild it so subsequent creature KF targets resolve
                // nodes inside authored Prn subtrees such as robot screens.
                mNodeMap.clear();
                mNodeMapCreated = false;
                if (std::getenv("OPENMW_FNV_CREATURE_BODY_DIAG") != nullptr && mObjectRoot != nullptr)
                    logCreatureBounds("root", ref->mBase->mEditorId, actorModel, *mObjectRoot);
                unsigned int fallbackKfs = 0;
                unsigned int discoveredKfs = 0;
                unsigned int selectedPoseKfs = 0;
                if (!markerCreatureRoot)
                {
                    for (const std::string& kf : authoredKfs)
                    {
                        if (!kf.empty() && !isRepresentativePoseKf(kf))
                            addAnimSource(kf, actorModel);
                    }

                    static constexpr std::string_view fallbackNames[] = {
                        "skeleton.kf",
                        "idle.kf",
                        "forward.kf",
                        "backward.kf",
                        "left.kf",
                        "right.kf",
                        "walkforward.kf",
                        "runforward.kf",
                        "attackleft.kf",
                        "attackright.kf",
                        "attack1.kf",
                    };
                    for (std::string_view fallback : fallbackNames)
                    {
                        std::string path = animationDirectory + std::string(fallback);
                        std::string normalizedPath;
                        if (findCreatureKf(*vfs, path, normalizedPath))
                        {
                            if (!isRepresentativePoseKf(normalizedPath))
                            {
                                addAnimSource(normalizedPath, actorModel);
                                ++fallbackKfs;
                            }
                        }
                    }

                    std::string probeToken = normalizedDirectory;
                    if (probeToken.ends_with('/'))
                        probeToken.pop_back();
                    const std::vector<std::string> discoveredKfPaths
                        = collectDiscoveredCreatureKfs(*vfs, normalizedDirectory, probeToken, ref->mBase->mEditorId);
                    for (const std::string& path : discoveredKfPaths)
                    {
                        if (isRepresentativePoseKf(path))
                            continue;
                        addAnimSource(path, actorModel);
                        ++discoveredKfs;
                    }

                    // Add exactly one selected same-rig KF for each representative semantic after recursive
                    // discovery. Later sources win, so transitions remain available under their authored groups
                    // without being able to steal stand/kneel/prone/walk/talk/shoot/wave from these exact assets.
                    for (const auto& [group, path] : representativePoseKfs)
                    {
                        if (addSingleAnimSource(path, actorModel, false, {}, group) != nullptr)
                            ++selectedPoseKfs;
                        else
                            Log(Debug::Warning) << "FNV/ESM4 diag: selected same-rig creature pose KF did not bind"
                                                << " editor=" << ref->mBase->mEditorId << " group=" << group
                                                << " path=" << path << " skeleton=" << actorModel;
                    }
                }
                else
                    Log(Debug::Warning) << "FNV/ESM4 diag: skipped marker-creature animation binding for "
                                        << ref->mBase->mEditorId << " modelSource="
                                        << (creatureVisuals.mModel == nullptr ? std::string("<none>")
                                                                              : creatureVisuals.mModel->mEditorId)
                                        << " model=" << actorModel;

                if (std::getenv("OPENMW_FNV_CREATURE_SCREEN_STATE_DIAG") != nullptr && mObjectRoot != nullptr)
                    mObjectRoot->addUpdateCallback(
                        new FalloutCreatureScreenStateCallback(ref->mBase->mEditorId));
                if (const char* layerDiagnostic = std::getenv("OPENMW_FNV_CREATURE_SCREEN_LAYER_DIAG");
                    layerDiagnostic != nullptr && mObjectRoot != nullptr)
                {
                    FalloutCreatureScreenLayerDiagnosticVisitor layerVisitor(
                        layerDiagnostic, ref->mBase->mEditorId, mObjectRoot);
                    mObjectRoot->accept(layerVisitor);
                }

                Log(Debug::Verbose) << "FNV/ESM4 diag: inserted creature animation for "
                                 << ref->mBase->mEditorId << " model=" << actorModel
                                 << " animated=" << animated << " templateDepth=" << creatureTemplates.size()
                                 << " modelSource="
                                 << (creatureVisuals.mModel == nullptr ? std::string("<none>")
                                                                       : creatureVisuals.mModel->mEditorId)
                                 << " nifSource="
                                 << (creatureVisuals.mNif == nullptr ? std::string("<none>")
                                                                     : creatureVisuals.mNif->mEditorId)
                                 << " bodyPartSource="
                                 << (creatureVisuals.mBodyParts == nullptr ? std::string("<none>")
                                                                           : creatureVisuals.mBodyParts->mEditorId)
                                 << " kfCount=" << authoredKfs.size()
                                 << " bodyPartCount=" << authoredBodyParts.size()
                                 << " attachedBodyNifs=" << attachedBodyNifs
                                 << " fallbackKfs=" << fallbackKfs
                                 << " discoveredKfs=" << discoveredKfs
                                 << " selectedPoseKfs=" << selectedPoseKfs;
            }
        }
    }

    CreatureWeaponAnimation::CreatureWeaponAnimation(
        const MWWorld::Ptr& ptr, const std::string& model, Resource::ResourceSystem* resourceSystem, bool animated)
        : ActorAnimation(ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), resourceSystem)
        , mShowWeapons(false)
        , mShowCarriedLeft(false)
    {
        MWWorld::LiveCellRef<ESM::Creature>* ref = mPtr.get<ESM::Creature>();

        if (!model.empty())
        {
            setObjectRoot(model, true, false, true);

            if ((ref->mBase->mFlags & ESM::Creature::Bipedal))
                addAnimSource(Settings::models().mXbaseanim.get(), model);

            if (animated)
                addAnimSource(model, model);

            mPtr.getClass().getInventoryStore(mPtr).setInvListener(this);

            updateParts();
        }

        mWeaponAnimationTime = std::make_shared<WeaponAnimationTime>(this);
    }

    void CreatureWeaponAnimation::showWeapons(bool showWeapon)
    {
        if (showWeapon != mShowWeapons)
        {
            mShowWeapons = showWeapon;
            updateParts();
        }
    }

    void CreatureWeaponAnimation::showCarriedLeft(bool show)
    {
        if (show != mShowCarriedLeft)
        {
            mShowCarriedLeft = show;
            updateParts();
        }
    }

    void CreatureWeaponAnimation::updateParts()
    {
        mAmmunition.reset();
        mWeapon.reset();
        mShield.reset();

        updateHolsteredWeapon(!mShowWeapons);
        updateQuiver();
        updateHolsteredShield(mShowCarriedLeft);

        if (mShowWeapons)
            updatePart(mWeapon, MWWorld::InventoryStore::Slot_CarriedRight);
        if (mShowCarriedLeft)
            updatePart(mShield, MWWorld::InventoryStore::Slot_CarriedLeft);
    }

    void CreatureWeaponAnimation::updatePart(PartHolderPtr& scene, int slot)
    {
        if (!mObjectRoot)
            return;

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator it = inv.getSlot(slot);

        if (it == inv.end())
        {
            scene.reset();
            return;
        }
        MWWorld::ConstPtr item = *it;

        std::string_view bonename;
        VFS::Path::Normalized itemModel = item.getClass().getCorrectedModel(item);
        if (slot == MWWorld::InventoryStore::Slot_CarriedRight)
        {
            if (item.getType() == ESM::Weapon::sRecordId)
            {
                int type = item.get<ESM::Weapon>()->mBase->mData.mType;
                bonename = MWMechanics::getWeaponType(type)->mAttachBone;
                if (bonename != "Weapon Bone")
                {
                    const NodeMap& nodeMap = getNodeMap();
                    NodeMap::const_iterator found = nodeMap.find(bonename);
                    if (found == nodeMap.end())
                        bonename = "Weapon Bone";
                }
            }
            else
                bonename = "Weapon Bone";
        }
        else
        {
            bonename = "Shield Bone";
            if (item.getType() == ESM::Armor::sRecordId)
            {
                itemModel = getShieldMesh(item, false);
            }
        }

        try
        {
            osg::ref_ptr<osg::Node> attached
                = attach(itemModel, bonename, bonename, item.getType() == ESM::Light::sRecordId);

            scene = std::make_unique<PartHolder>(attached);

            if (!item.getClass().getEnchantment(item).empty())
                mGlowUpdater
                    = SceneUtil::addEnchantedGlow(attached, mResourceSystem, item.getClass().getEnchantmentColor(item));

            // Crossbows start out with a bolt attached
            // FIXME: code duplicated from NpcAnimation
            if (slot == MWWorld::InventoryStore::Slot_CarriedRight && item.getType() == ESM::Weapon::sRecordId
                && item.get<ESM::Weapon>()->mBase->mData.mType == ESM::Weapon::MarksmanCrossbow)
            {
                const ESM::WeaponType* weaponInfo = MWMechanics::getWeaponType(ESM::Weapon::MarksmanCrossbow);
                MWWorld::ConstContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                if (ammo != inv.end() && ammo->get<ESM::Weapon>()->mBase->mData.mType == weaponInfo->mAmmoType)
                    attachArrow();
                else
                    mAmmunition.reset();
            }
            else
                mAmmunition.reset();

            std::shared_ptr<SceneUtil::ControllerSource> source;

            if (slot == MWWorld::InventoryStore::Slot_CarriedRight)
                source = mWeaponAnimationTime;
            else
                source = mAnimationTimePtr[0];

            SceneUtil::AssignControllerSourcesVisitor assignVisitor(std::move(source));
            attached->accept(assignVisitor);

            if (item.getType() == ESM::Light::sRecordId)
                addExtraLight(scene->getNode()->asGroup(), SceneUtil::LightCommon(*item.get<ESM::Light>()->mBase));
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Can not add creature part: " << e.what();
        }
    }

    bool CreatureWeaponAnimation::isArrowAttached() const
    {
        return mAmmunition != nullptr;
    }

    void CreatureWeaponAnimation::detachArrow()
    {
        WeaponAnimation::detachArrow(mPtr);
        updateQuiver();
    }

    void CreatureWeaponAnimation::attachArrow()
    {
        WeaponAnimation::attachArrow(mPtr);

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator ammo = inv.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
        if (ammo != inv.end() && !ammo->getClass().getEnchantment(*ammo).empty())
        {
            osg::Group* bone = getArrowBone();
            if (bone != nullptr && bone->getNumChildren())
                SceneUtil::addEnchantedGlow(
                    bone->getChild(0), mResourceSystem, ammo->getClass().getEnchantmentColor(*ammo));
        }

        updateQuiver();
    }

    void CreatureWeaponAnimation::releaseArrow(float attackStrength)
    {
        WeaponAnimation::releaseArrow(mPtr, attackStrength);
        updateQuiver();
    }

    osg::Group* CreatureWeaponAnimation::getArrowBone()
    {
        if (!mWeapon)
            return nullptr;

        if (!mPtr.getClass().hasInventoryStore(mPtr))
            return nullptr;

        const MWWorld::InventoryStore& inv = mPtr.getClass().getInventoryStore(mPtr);
        MWWorld::ConstContainerStoreIterator weapon = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        if (weapon == inv.end() || weapon->getType() != ESM::Weapon::sRecordId)
            return nullptr;

        int type = weapon->get<ESM::Weapon>()->mBase->mData.mType;
        int ammoType = MWMechanics::getWeaponType(type)->mAmmoType;
        if (ammoType == ESM::Weapon::None)
            return nullptr;

        // Try to find and attachment bone in actor's skeleton, otherwise fall back to the ArrowBone in weapon's mesh
        osg::Group* bone = getBoneByName(MWMechanics::getWeaponType(ammoType)->mAttachBone);
        if (bone == nullptr)
        {
            SceneUtil::FindByNameVisitor findVisitor("ArrowBone");
            mWeapon->getNode()->accept(findVisitor);
            bone = findVisitor.mFoundNode;
        }
        return bone;
    }

    osg::Node* CreatureWeaponAnimation::getWeaponNode()
    {
        return mWeapon ? mWeapon->getNode().get() : nullptr;
    }

    Resource::ResourceSystem* CreatureWeaponAnimation::getResourceSystem()
    {
        return mResourceSystem;
    }

    void CreatureWeaponAnimation::addControllers()
    {
        Animation::addControllers();
        if (mObjectRoot)
            WeaponAnimation::addControllers(mNodeMap, mActiveControllers, mObjectRoot.get());
    }

    osg::Vec3f CreatureWeaponAnimation::runAnimation(float duration)
    {
        osg::Vec3f ret = Animation::runAnimation(duration);

        WeaponAnimation::configureControllers(mPtr.getRefData().getPosition().rot[0] + getBodyPitchRadians());

        return ret;
    }

}
