/// Program to test .nif files both on the FileSystem and in BSA archives.

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <components/bgsm/file.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/files/constrainedfilestream.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/sceneutil/keyframe.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/bsaarchive.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <boost/program_options.hpp>

// Create local aliases for brevity
namespace bpo = boost::program_options;

enum class FileType
{
    BSA,
    BA2,
    BGEM,
    BGSM,
    NIF,
    KF,
    BTO,
    BTR,
    RDT,
    PSA,
    Unknown,
};

enum class FileClass
{
    Archive,
    Material,
    NIF,
    Unknown,
};

std::pair<FileType, FileClass> classifyFile(const std::filesystem::path& filename)
{
    const std::string extension = Misc::StringUtils::lowerCase(Files::pathToUnicodeString(filename.extension()));
    if (extension == ".bsa")
        return { FileType::BSA, FileClass::Archive };
    if (extension == ".ba2")
        return { FileType::BA2, FileClass::Archive };
    if (extension == ".bgem")
        return { FileType::BGEM, FileClass::Material };
    if (extension == ".bgsm")
        return { FileType::BGSM, FileClass::Material };
    if (extension == ".nif")
        return { FileType::NIF, FileClass::NIF };
    if (extension == ".kf")
        return { FileType::KF, FileClass::NIF };
    if (extension == ".bto")
        return { FileType::BTO, FileClass::NIF };
    if (extension == ".btr")
        return { FileType::BTR, FileClass::NIF };
    if (extension == ".rdt")
        return { FileType::RDT, FileClass::NIF };
    if (extension == ".psa")
        return { FileType::PSA, FileClass::NIF };

    return { FileType::Unknown, FileClass::Unknown };
}

std::string getFileTypeName(FileType fileType)
{
    switch (fileType)
    {
        case FileType::BSA:
            return "BSA";
        case FileType::BA2:
            return "BA2";
        case FileType::BGEM:
            return "BGEM";
        case FileType::BGSM:
            return "BGSM";
        case FileType::NIF:
            return "NIF";
        case FileType::KF:
            return "KF";
        case FileType::BTO:
            return "BTO";
        case FileType::BTR:
            return "BTR";
        case FileType::RDT:
            return "RDT";
        case FileType::PSA:
            return "PSA";
        case FileType::Unknown:
        default:
            return {};
    }
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (char ch : value)
    {
        switch (ch)
        {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << ch;
                break;
        }
    }
    return out.str();
}

std::string formatVec3Json(const osg::Vec3f& value)
{
    std::ostringstream out;
    out << std::setprecision(9) << "[" << value.x() << "," << value.y() << "," << value.z() << "]";
    return out.str();
}

std::string formatQuatJson(const osg::Quat& value)
{
    std::ostringstream out;
    out << std::setprecision(9) << "[" << value.x() << "," << value.y() << "," << value.z() << "," << value.w()
        << "]";
    return out.str();
}

std::string formatMatrixJson(const osg::Matrixf& matrix)
{
    std::ostringstream out;
    out << std::setprecision(9) << "[";
    for (int row = 0; row < 4; ++row)
    {
        if (row != 0)
            out << ",";
        out << "[";
        for (int column = 0; column < 4; ++column)
        {
            if (column != 0)
                out << ",";
            out << matrix(row, column);
        }
        out << "]";
    }
    out << "]";
    return out.str();
}

std::string getNifStringPaletteValue(const Nif::NiStringPalettePtr& palette, uint32_t offset)
{
    if (palette.empty() || offset == std::numeric_limits<uint32_t>::max())
        return {};

    const std::string& text = palette->mPalette;
    if (offset >= text.size())
        return {};

    const std::size_t end = text.find('\0', offset);
    if (end == std::string::npos)
        return text.substr(offset);

    return text.substr(offset, end - offset);
}

std::string resolveNifControlledBlockTargetName(
    const Nif::NiControllerSequence& sequence, const Nif::ControlledBlock& block)
{
    if (!block.mNodeName.empty())
        return block.mNodeName;
    if (!block.mTargetName.empty())
        return block.mTargetName;

    std::string targetName = getNifStringPaletteValue(block.mStringPalette, block.mNodeNameOffset);
    if (!targetName.empty())
        return targetName;

    targetName = getNifStringPaletteValue(sequence.mStringPalette, block.mNodeNameOffset);
    if (!targetName.empty())
        return targetName;

    return {};
}

bool isLoadedFNVTransformInterpolator(int recType)
{
    return recType == Nif::RC_NiTransformInterpolator || recType == Nif::RC_NiBSplineTransformInterpolator
        || recType == Nif::RC_NiBSplineCompTransformInterpolator || recType == Nif::RC_NiBlendTransformInterpolator;
}

osg::Matrixf makeLocalMatrixLike(osg::MatrixTransform* transform, const osg::Quat& rotation, const osg::Vec3f& translation)
{
    if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(transform))
    {
        NifOsg::MatrixTransform probe(*nifTransform, osg::CopyOp::SHALLOW_COPY);
        probe.setRotation(rotation);
        probe.setTranslation(translation);
        return probe.getMatrix();
    }

    return osg::Matrixf::rotate(rotation) * osg::Matrixf::translate(translation);
}

osg::Quat fnvDumpHalfTurn(char axis)
{
    switch (axis)
    {
        case 'x':
            return osg::Quat(osg::PI, osg::Vec3f(1.f, 0.f, 0.f));
        case 'z':
            return osg::Quat(osg::PI, osg::Vec3f(0.f, 0.f, 1.f));
        default:
            return osg::Quat();
    }
}

bool isFNVLowerBodyDumpBone(const std::string& lowerName)
{
    return lowerName.find("thigh") != std::string::npos || lowerName.find("calf") != std::string::npos
        || lowerName.find("foot") != std::string::npos || lowerName.find("toe") != std::string::npos;
}

bool isFNVCoreDumpBone(const std::string& lowerName)
{
    return lowerName == "bip01" || lowerName == "bip01 nonaccum" || lowerName.find("pelvis") != std::string::npos
        || lowerName.find("spine") != std::string::npos || lowerName.find("neck") != std::string::npos
        || lowerName.find("head") != std::string::npos;
}

bool isFNVArmDumpBone(const std::string& lowerName)
{
    return lowerName.find("clavicle") != std::string::npos || lowerName.find("upperarm") != std::string::npos
        || lowerName.find("forearm") != std::string::npos || lowerName.find("hand") != std::string::npos
        || lowerName.find("finger") != std::string::npos || lowerName.find("thumb") != std::string::npos
        || lowerName.find("foretwist") != std::string::npos || lowerName.find("uparmtwist") != std::string::npos;
}

bool isFNVUpperArmDumpBone(const std::string& lowerName)
{
    return lowerName.find("clavicle") != std::string::npos || lowerName.find("upperarm") != std::string::npos
        || lowerName.find("uparmtwist") != std::string::npos;
}

bool isFNVForearmDumpBone(const std::string& lowerName)
{
    return lowerName.find("forearm") != std::string::npos || lowerName.find("foretwist") != std::string::npos;
}

bool isFNVHandDumpBone(const std::string& lowerName)
{
    return lowerName.find("hand") != std::string::npos || lowerName.find("finger") != std::string::npos
        || lowerName.find("thumb") != std::string::npos;
}

bool isFNVArmOrHandDumpBone(const std::string& lowerName)
{
    return isFNVUpperArmDumpBone(lowerName) || isFNVForearmDumpBone(lowerName) || isFNVHandDumpBone(lowerName);
}

double quatDeltaDegrees(osg::Quat left, osg::Quat right)
{
    const double leftLength = std::sqrt(
        left.x() * left.x() + left.y() * left.y() + left.z() * left.z() + left.w() * left.w());
    const double rightLength = std::sqrt(
        right.x() * right.x() + right.y() * right.y() + right.z() * right.z() + right.w() * right.w());
    if (leftLength <= 0.0 || rightLength <= 0.0)
        return 0.0;

    double dot = (left.x() * right.x() + left.y() * right.y() + left.z() * right.z() + left.w() * right.w())
        / (leftLength * rightLength);
    dot = std::clamp(std::abs(dot), 0.0, 1.0);
    return 2.0 * std::acos(dot) * 180.0 / osg::PI;
}

osg::Quat composeFNVTransformDumpRotation(
    const std::string& mode, const std::string& lowerName, const osg::Quat& keyRotation, const osg::Quat& bindRotation)
{
    if (mode == "bindCoreRawLimbs" && isFNVCoreDumpBone(lowerName))
        return bindRotation;
    if (mode == "bindCoreBindLowerRawUpper"
        && (isFNVCoreDumpBone(lowerName) || isFNVLowerBodyDumpBone(lowerName)))
        return bindRotation;
    if (mode == "bindCoreBindLowerSplitUpper")
    {
        if (isFNVCoreDumpBone(lowerName) || isFNVLowerBodyDumpBone(lowerName))
            return bindRotation;
        if (isFNVArmDumpBone(lowerName))
            return keyRotation * fnvDumpHalfTurn('x') * bindRotation;
    }
    if (mode == "bindCoreRawLowerBindUpper")
    {
        if (isFNVCoreDumpBone(lowerName) || isFNVArmDumpBone(lowerName))
            return bindRotation;
        if (isFNVLowerBodyDumpBone(lowerName))
            return keyRotation;
    }
    if (mode == "rawCoreBindLowerRawUpper" && isFNVLowerBodyDumpBone(lowerName))
        return bindRotation;
    if (mode == "bindCoreBindLowerBindUpper" && (isFNVCoreDumpBone(lowerName) || isFNVLowerBodyDumpBone(lowerName)
                                                    || isFNVArmOrHandDumpBone(lowerName)))
        return bindRotation;
    if (mode == "bindCoreBindLowerBindArmsRawHands")
    {
        if (isFNVCoreDumpBone(lowerName) || isFNVLowerBodyDumpBone(lowerName) || isFNVUpperArmDumpBone(lowerName)
            || isFNVForearmDumpBone(lowerName))
            return bindRotation;
        if (isFNVHandDumpBone(lowerName))
            return keyRotation;
    }
    if (mode == "bindCoreBindLowerBindUpperRawForearmsHands")
    {
        if (isFNVCoreDumpBone(lowerName) || isFNVLowerBodyDumpBone(lowerName) || isFNVUpperArmDumpBone(lowerName))
            return bindRotation;
        if (isFNVForearmDumpBone(lowerName) || isFNVHandDumpBone(lowerName))
            return keyRotation;
    }
    if (mode == "bindCoreBindLowerRawUpperBindHands")
    {
        if (isFNVCoreDumpBone(lowerName) || isFNVLowerBodyDumpBone(lowerName) || isFNVHandDumpBone(lowerName))
            return bindRotation;
        if (isFNVUpperArmDumpBone(lowerName) || isFNVForearmDumpBone(lowerName))
            return keyRotation;
    }
    if (mode == "bindCoreBindLowerRawClavicleBindArms")
    {
        if (isFNVCoreDumpBone(lowerName) || isFNVLowerBodyDumpBone(lowerName) || isFNVForearmDumpBone(lowerName)
            || isFNVHandDumpBone(lowerName))
            return bindRotation;
        if (lowerName.find("clavicle") != std::string::npos || lowerName.find("upperarm") != std::string::npos
            || lowerName.find("uparmtwist") != std::string::npos)
            return keyRotation;
    }
    if (mode == "bind")
        return bindRotation;
    if (mode == "rawKey")
        return keyRotation;
    if (mode == "bindThenKey")
        return bindRotation * keyRotation;
    if (mode == "keyThenBind")
        return keyRotation * bindRotation;
    if (mode == "splitKeyXZThenBind")
        return keyRotation * fnvDumpHalfTurn(isFNVLowerBodyDumpBone(lowerName) ? 'z' : 'x') * bindRotation;
    return keyRotation;
}

osg::Matrixf parentWorldMatrix(osg::MatrixTransform* transform, const std::unordered_map<osg::MatrixTransform*, osg::Matrixf>& worlds)
{
    if (transform == nullptr || transform->getNumParents() == 0)
        return osg::Matrixf::identity();

    osg::Node* parent = transform->getParent(0);
    while (parent != nullptr)
    {
        if (auto* parentTransform = dynamic_cast<osg::MatrixTransform*>(parent))
        {
            const auto found = worlds.find(parentTransform);
            if (found != worlds.end())
                return found->second;
        }
        if (parent->getNumParents() == 0)
            break;
        parent = parent->getParent(0);
    }
    return osg::Matrixf::identity();
}

struct TransformDumpNode
{
    std::string mName;
    std::string mLowerName;
    std::string mParentName;
    osg::MatrixTransform* mTransform = nullptr;
    osg::MatrixTransform* mParentTransform = nullptr;
    osg::Matrixf mBindLocal;
    osg::Matrixf mBindWorld;
};

class ConstantTransformDumpTimeSource : public SceneUtil::ControllerSource
{
public:
    explicit ConstantTransformDumpTimeSource(float time)
        : mTime(time)
    {
    }

    float getValue(osg::NodeVisitor*) override { return mTime; }

private:
    float mTime;
};

class TransformDumpVisitor : public osg::NodeVisitor
{
public:
    TransformDumpVisitor(std::vector<TransformDumpNode>& nodes,
        std::unordered_map<osg::MatrixTransform*, osg::Matrixf>& worldByNode)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        , mNodes(nodes)
        , mWorldByNode(worldByNode)
    {
    }

    void apply(osg::MatrixTransform& node) override
    {
        const osg::Matrixf parentWorld = mWorldStack.empty() ? osg::Matrixf::identity() : mWorldStack.back();
        const osg::Matrixf local = node.getMatrix();
        const osg::Matrixf world = local * parentWorld;
        mWorldByNode[&node] = world;

        const bool keep = !node.getName().empty() && Misc::StringUtils::ciStartsWith(node.getName(), "Bip01");
        if (keep)
        {
            TransformDumpNode item;
            item.mName = node.getName();
            item.mLowerName = Misc::StringUtils::lowerCase(node.getName());
            item.mParentName = mNameStack.empty() ? std::string() : mNameStack.back();
            item.mTransform = &node;
            item.mParentTransform = mTransformStack.empty() ? nullptr : mTransformStack.back();
            item.mBindLocal = local;
            item.mBindWorld = world;
            mNodes.push_back(item);
        }

        mWorldStack.push_back(world);
        mNameStack.push_back(node.getName());
        mTransformStack.push_back(&node);
        traverse(node);
        mTransformStack.pop_back();
        mNameStack.pop_back();
        mWorldStack.pop_back();
    }

private:
    std::vector<TransformDumpNode>& mNodes;
    std::unordered_map<osg::MatrixTransform*, osg::Matrixf>& mWorldByNode;
    std::vector<osg::Matrixf> mWorldStack;
    std::vector<std::string> mNameStack;
    std::vector<osg::MatrixTransform*> mTransformStack;
};

bool isImportantFNVHumanBone(const std::string& lowerName)
{
    static const std::set<std::string> sImportant = {
        "bip01",
        "bip01 nonaccum",
        "bip01 pelvis",
        "bip01 spine",
        "bip01 spine1",
        "bip01 spine2",
        "bip01 neck",
        "bip01 head",
        "bip01 l clavicle",
        "bip01 r clavicle",
        "bip01 l upperarm",
        "bip01 r upperarm",
        "bip01 l forearm",
        "bip01 r forearm",
        "bip01 l hand",
        "bip01 r hand",
        "bip01 l finger0",
        "bip01 l finger01",
        "bip01 l finger02",
        "bip01 l finger1",
        "bip01 l finger11",
        "bip01 l finger12",
        "bip01 l finger2",
        "bip01 l finger21",
        "bip01 l finger22",
        "bip01 l finger3",
        "bip01 l finger31",
        "bip01 l finger32",
        "bip01 l finger4",
        "bip01 l finger41",
        "bip01 l finger42",
        "bip01 r finger0",
        "bip01 r finger01",
        "bip01 r finger02",
        "bip01 r finger1",
        "bip01 r finger11",
        "bip01 r finger12",
        "bip01 r finger2",
        "bip01 r finger21",
        "bip01 r finger22",
        "bip01 r finger3",
        "bip01 r finger31",
        "bip01 r finger32",
        "bip01 r finger4",
        "bip01 r finger41",
        "bip01 r finger42",
        "bip01 l thigh",
        "bip01 r thigh",
        "bip01 l calf",
        "bip01 r calf",
        "bip01 l foot",
        "bip01 r foot",
        "bip01 l toe0",
        "bip01 r toe0",
    };
    return sImportant.count(lowerName) != 0;
}

std::unique_ptr<Nif::NIFFile> readNifFile(const std::filesystem::path& path)
{
    auto file = std::make_unique<Nif::NIFFile>(VFS::Path::Normalized(Files::pathToUnicodeString(path)));
    Nif::Reader reader(*file, nullptr);
    reader.parse(Files::openConstrainedFileStream(path));
    return file;
}

float fnvMatrixMaxAbsDelta(const osg::Matrixf& left, const osg::Matrixf& right)
{
    float result = 0.f;
    const float* leftPtr = left.ptr();
    const float* rightPtr = right.ptr();
    for (int i = 0; i < 16; ++i)
        result = std::max(result, std::abs(leftPtr[i] - rightPtr[i]));
    return result;
}

osg::Vec3f fnvAuditExtent(const osg::BoundingBox& box)
{
    if (!box.valid())
        return osg::Vec3f();
    return osg::Vec3f(box.xMax() - box.xMin(), box.yMax() - box.yMin(), box.zMax() - box.zMin());
}

void collectNifWorldMatrices(const Nif::NiAVObject* node, const osg::Matrixf& parentWorld,
    std::unordered_map<std::string, osg::Matrixf>& worldsByLowerName)
{
    if (node == nullptr)
        return;

    const osg::Matrixf local = node->mTransform.toMatrix();
    const osg::Matrixf world = local * parentWorld;
    if (!node->mName.empty())
        worldsByLowerName[Misc::StringUtils::lowerCase(node->mName)] = world;

    const Nif::NiNode* niNode = dynamic_cast<const Nif::NiNode*>(node);
    if (niNode == nullptr)
        return;

    for (const auto& child : niNode->mChildren)
        if (!child.empty())
            collectNifWorldMatrices(child.getPtr(), world, worldsByLowerName);
}

void collectSkinnedGeometries(const Nif::NiAVObject* node, std::vector<const Nif::NiGeometry*>& geometries)
{
    if (node == nullptr)
        return;

    if (const Nif::NiGeometry* geometry = dynamic_cast<const Nif::NiGeometry*>(node))
        if (!geometry->mSkin.empty() && !geometry->mSkin->mData.empty() && !geometry->mData.empty())
            geometries.push_back(geometry);

    const Nif::NiNode* niNode = dynamic_cast<const Nif::NiNode*>(node);
    if (niNode == nullptr)
        return;

    for (const auto& child : niNode->mChildren)
        if (!child.empty())
            collectSkinnedGeometries(child.getPtr(), geometries);
}

void collectNifGeometries(const Nif::NiAVObject* node, std::vector<const Nif::NiGeometry*>& geometries)
{
    if (node == nullptr)
        return;

    if (const Nif::NiGeometry* geometry = dynamic_cast<const Nif::NiGeometry*>(node))
        if (!geometry->mData.empty())
            geometries.push_back(geometry);

    const Nif::NiNode* niNode = dynamic_cast<const Nif::NiNode*>(node);
    if (niNode == nullptr)
        return;

    for (const auto& child : niNode->mChildren)
        if (!child.empty())
            collectNifGeometries(child.getPtr(), geometries);
}

struct FnvGeometryDumpInfo
{
    const Nif::NiGeometry* mGeometry = nullptr;
    osg::Matrixf mWorldTransform;
};

void collectNifGeometryInfos(const Nif::NiAVObject* node, const osg::Matrixf& parentWorld,
    std::vector<FnvGeometryDumpInfo>& geometries)
{
    if (node == nullptr)
        return;

    const osg::Matrixf local = node->mTransform.toMatrix();
    const osg::Matrixf world = local * parentWorld;

    if (const Nif::NiGeometry* geometry = dynamic_cast<const Nif::NiGeometry*>(node))
        if (!geometry->mData.empty())
            geometries.push_back({ geometry, world });

    const Nif::NiNode* niNode = dynamic_cast<const Nif::NiNode*>(node);
    if (niNode == nullptr)
        return;

    for (const auto& child : niNode->mChildren)
        if (!child.empty())
            collectNifGeometryInfos(child.getPtr(), world, geometries);
}

int runFnvGeometryDump(const std::filesystem::path& meshPath, const std::filesystem::path& outPath)
{
    Nif::Reader::setLoadUnsupportedFiles(true);

    std::unique_ptr<Nif::NIFFile> meshFile = readNifFile(meshPath);
    std::vector<FnvGeometryDumpInfo> geometries;
    for (const Nif::Record* record : meshFile->mRoots)
        if (const Nif::NiAVObject* root = dynamic_cast<const Nif::NiAVObject*>(record))
            collectNifGeometryInfos(root, osg::Matrixf::identity(), geometries);

    std::ofstream out(outPath);
    if (!out)
        throw std::runtime_error("failed to open output path");

    out << std::setprecision(9);
    out << "{\n";
    out << "  \"mesh\": \"" << jsonEscape(Files::pathToUnicodeString(meshPath)) << "\",\n";
    out << "  \"geometryCount\": " << geometries.size() << ",\n";
    out << "  \"geometries\": [";

    bool firstGeometry = true;
    for (const FnvGeometryDumpInfo& info : geometries)
    {
        const Nif::NiGeometry* geometry = info.mGeometry;
        if (geometry->mData.empty())
            continue;

        const std::vector<osg::Vec3f>& vertices = geometry->mData->mVertices;
        osg::BoundingBox box;
        osg::BoundingBox worldBox;
        for (const osg::Vec3f& vertex : vertices)
        {
            box.expandBy(vertex);
            worldBox.expandBy(vertex * info.mWorldTransform);
        }

        if (!firstGeometry)
            out << ",";
        firstGeometry = false;

        out << "\n    {\"name\":\"" << jsonEscape(geometry->mName) << "\""
            << ",\"vertexCount\":" << vertices.size()
            << ",\"hasSkin\":" << (!geometry->mSkin.empty() ? "true" : "false")
            << ",\"extent\":" << formatVec3Json(fnvAuditExtent(box))
            << ",\"worldExtent\":" << formatVec3Json(fnvAuditExtent(worldBox))
            << ",\"localTransform\":" << formatMatrixJson(geometry->mTransform.toMatrix())
            << ",\"worldTransform\":" << formatMatrixJson(info.mWorldTransform)
            << ",\"vertices\":[";
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            if (i != 0)
                out << ",";
            out << formatVec3Json(vertices[i]);
        }
        out << "],\"worldVertices\":[";
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            if (i != 0)
                out << ",";
            out << formatVec3Json(vertices[i] * info.mWorldTransform);
        }
        out << "]";

        if (!geometry->mSkin.empty() && !geometry->mSkin->mData.empty())
        {
            const Nif::NiSkinData& skinData = *geometry->mSkin->mData.getPtr();
            const osg::Matrixf skinRoot = skinData.mTransform.toMatrix();
            osg::Matrixf invSkinRoot;
            const bool hasInvSkinRoot = invSkinRoot.invert(skinRoot);

            auto writeVertexArray = [&](std::string_view name, const std::vector<osg::Vec3f>& values) {
                out << ",\"" << name << "\":[";
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    if (i != 0)
                        out << ",";
                    out << formatVec3Json(values[i]);
                }
                out << "]";
            };

            auto writeMatrixSpace = [&](std::string_view name, const osg::Matrixf& matrix) {
                std::vector<osg::Vec3f> values;
                values.reserve(vertices.size());
                for (const osg::Vec3f& vertex : vertices)
                    values.push_back(vertex * matrix);
                writeVertexArray(name, values);
            };

            auto writeWeightedSpace = [&](std::string_view name, const std::vector<osg::Matrixf>& matrices) {
                std::vector<std::vector<std::pair<std::size_t, float>>> influencesByVertex(vertices.size());
                for (std::size_t boneIndex = 0; boneIndex < skinData.mBones.size() && boneIndex < matrices.size();
                     ++boneIndex)
                {
                    for (const auto& [vertexIndex, weight] : skinData.mBones[boneIndex].mWeights)
                        if (vertexIndex < vertices.size())
                            influencesByVertex[vertexIndex].push_back({ boneIndex, weight });
                }

                std::vector<osg::Vec3f> values;
                values.reserve(vertices.size());
                for (std::size_t vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex)
                {
                    osg::Vec3f value(0.f, 0.f, 0.f);
                    float weightSum = 0.f;
                    for (const auto& [boneIndex, weight] : influencesByVertex[vertexIndex])
                    {
                        value += (vertices[vertexIndex] * matrices[boneIndex]) * weight;
                        weightSum += weight;
                    }

                    if (influencesByVertex[vertexIndex].empty())
                        value = vertices[vertexIndex];
                    else if (weightSum > 0.0001f && std::abs(weightSum - 1.f) > 0.0001f)
                        value /= weightSum;
                    values.push_back(value);
                }

                writeVertexArray(name, values);
            };

            writeMatrixSpace("skinRootVertices", skinRoot);
            if (hasInvSkinRoot)
                writeMatrixSpace("invSkinRootVertices", invSkinRoot);

            std::vector<osg::Matrixf> authoredInvBindMatrices;
            std::vector<osg::Matrixf> authoredInvBindSkinRootMatrices;
            std::vector<osg::Matrixf> authoredBindMatrices;
            authoredInvBindMatrices.reserve(skinData.mBones.size());
            authoredInvBindSkinRootMatrices.reserve(skinData.mBones.size());
            authoredBindMatrices.reserve(skinData.mBones.size());
            for (const Nif::NiSkinData::BoneInfo& boneInfo : skinData.mBones)
            {
                const osg::Matrixf invBind = boneInfo.mTransform.toMatrix();
                authoredInvBindMatrices.push_back(invBind);
                authoredInvBindSkinRootMatrices.push_back(invBind * skinRoot);

                osg::Matrixf bind;
                if (bind.invert(invBind))
                    authoredBindMatrices.push_back(bind);
                else
                    authoredBindMatrices.push_back(osg::Matrixf());
            }
            writeWeightedSpace("authoredInvBindVertices", authoredInvBindMatrices);
            writeWeightedSpace("authoredInvBindSkinRootVertices", authoredInvBindSkinRootMatrices);
            writeWeightedSpace("authoredBindVertices", authoredBindMatrices);
        }

        out << "}";
    }

    out << "\n  ]\n";
    out << "}\n";
    return 0;
}

struct FnvSkinCandidateMetric
{
    std::string mName;
    float mMaxVertexDelta = 0.f;
    unsigned int mMaxVertex = 0;
    float mMaxBlendedVertexDelta = 0.f;
    unsigned int mMaxBlendedVertex = 0;
    osg::BoundingBox mSourceBox;
    osg::BoundingBox mSkinnedBox;
};

std::vector<osg::Matrixf> makeFNVWeightedBoneMatrices(const std::string& candidate,
    const Nif::NiSkinData& data, const Nif::NiSkinInstance& skin,
    const std::unordered_map<std::string, osg::Matrixf>& skeletonWorlds)
{
    std::vector<osg::Matrixf> matrices;
    matrices.reserve(skin.mBones.size());
    const osg::Matrixf skinTransform = data.mTransform.toMatrix();
    osg::Matrixf inverseSkinTransform;
    const bool hasInverseSkinTransform = inverseSkinTransform.invert(skinTransform);

    for (std::size_t i = 0; i < skin.mBones.size(); ++i)
    {
        const Nif::NiAVObject* bone = skin.mBones[i].getPtr();
        const std::string lowerName = bone != nullptr ? Misc::StringUtils::lowerCase(bone->mName) : std::string();
        const auto found = skeletonWorlds.find(lowerName);
        const osg::Matrixf bindWorld = found != skeletonWorlds.end() ? found->second : osg::Matrixf::identity();
        const osg::Matrixf skinBone = i < data.mBones.size() ? data.mBones[i].mTransform.toMatrix() : osg::Matrixf();

        osg::Matrixf inverseBindWorld;
        const bool hasInverseBindWorld = inverseBindWorld.invert(bindWorld);
        osg::Matrixf inverseSkinBone;
        const bool hasInverseSkinBone = inverseSkinBone.invert(skinBone);

        if (candidate == "engine")
            matrices.push_back(skinBone * bindWorld * skinTransform);
        else if (candidate == "engineNoSkinRoot")
            matrices.push_back(skinBone * bindWorld);
        else if (candidate == "skeletonDerived")
            matrices.push_back(hasInverseBindWorld ? inverseBindWorld * bindWorld : osg::Matrixf());
        else if (candidate == "skeletonDerivedSkinRoot")
            matrices.push_back(hasInverseBindWorld ? inverseBindWorld * bindWorld * skinTransform : osg::Matrixf());
        else if (candidate == "skinRootInverseThenEngine")
            matrices.push_back(hasInverseSkinTransform ? inverseSkinTransform * skinBone * bindWorld * skinTransform
                                                       : osg::Matrixf());
        else if (candidate == "inverseSkinBoneThenBind")
            matrices.push_back(hasInverseSkinBone ? inverseSkinBone * bindWorld : osg::Matrixf());
        else
            matrices.push_back(osg::Matrixf());
    }

    return matrices;
}

FnvSkinCandidateMetric auditFNVRestCandidate(const std::string& candidate, const Nif::NiGeometry& geometry,
    const Nif::NiSkinData& data, const Nif::NiSkinInstance& skin,
    const std::unordered_map<std::string, osg::Matrixf>& skeletonWorlds)
{
    FnvSkinCandidateMetric metric;
    metric.mName = candidate;

    const std::vector<osg::Matrixf> boneMatrices = makeFNVWeightedBoneMatrices(candidate, data, skin, skeletonWorlds);
    const std::vector<osg::Vec3f>& vertices = geometry.mData->mVertices;
    if (vertices.empty())
        return metric;

    for (const osg::Vec3f& vertex : vertices)
        metric.mSourceBox.expandBy(vertex);

    std::vector<std::vector<std::pair<std::size_t, float>>> influencesByVertex(vertices.size());
    for (std::size_t boneIndex = 0; boneIndex < data.mBones.size() && boneIndex < boneMatrices.size(); ++boneIndex)
    {
        const osg::Matrixf& boneMatrix = boneMatrices[boneIndex];
        for (const auto& [vertexIndex, weight] : data.mBones[boneIndex].mWeights)
        {
            if (vertexIndex >= vertices.size())
                continue;

            // This per-bone max is intentionally conservative. Full blended vertex accumulation is logged separately
            // by the renderer; the bind audit rejects any individual influence that cannot preserve the rest pose.
            const osg::Vec3f skinned = vertices[vertexIndex] * boneMatrix;
            const float delta = (skinned - vertices[vertexIndex]).length() * std::abs(weight);
            if (delta > metric.mMaxVertexDelta)
            {
                metric.mMaxVertexDelta = delta;
                metric.mMaxVertex = vertexIndex;
            }
            influencesByVertex[vertexIndex].push_back({ boneIndex, weight });
        }
    }

    for (std::size_t vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex)
    {
        osg::Vec3f skinned(0.f, 0.f, 0.f);
        float weightSum = 0.f;
        for (const auto& [boneIndex, weight] : influencesByVertex[vertexIndex])
        {
            skinned += (vertices[vertexIndex] * boneMatrices[boneIndex]) * weight;
            weightSum += weight;
        }

        if (influencesByVertex[vertexIndex].empty())
            skinned = vertices[vertexIndex];
        else if (weightSum > 0.0001f && std::abs(weightSum - 1.f) > 0.0001f)
            skinned /= weightSum;

        metric.mSkinnedBox.expandBy(skinned);
        const float delta = (skinned - vertices[vertexIndex]).length();
        if (delta > metric.mMaxBlendedVertexDelta)
        {
            metric.mMaxBlendedVertexDelta = delta;
            metric.mMaxBlendedVertex = static_cast<unsigned int>(vertexIndex);
        }
    }

    return metric;
}

int runFnvSkinBindAudit(const std::filesystem::path& skeletonPath, const std::filesystem::path& meshPath,
    const std::filesystem::path& outPath)
{
    Nif::Reader::setLoadUnsupportedFiles(true);

    std::unique_ptr<Nif::NIFFile> skeletonFile = readNifFile(skeletonPath);
    std::unordered_map<std::string, osg::Matrixf> skeletonWorlds;
    for (const Nif::Record* record : skeletonFile->mRoots)
        if (const Nif::NiAVObject* root = dynamic_cast<const Nif::NiAVObject*>(record))
            collectNifWorldMatrices(root, osg::Matrixf::identity(), skeletonWorlds);

    std::unique_ptr<Nif::NIFFile> meshFile = readNifFile(meshPath);
    std::vector<const Nif::NiGeometry*> geometries;
    for (const Nif::Record* record : meshFile->mRoots)
        if (const Nif::NiAVObject* root = dynamic_cast<const Nif::NiAVObject*>(record))
            collectSkinnedGeometries(root, geometries);

    const std::vector<std::string> candidates = { "engine", "engineNoSkinRoot", "skeletonDerived",
        "skeletonDerivedSkinRoot", "skinRootInverseThenEngine", "inverseSkinBoneThenBind" };

    std::ofstream out(outPath);
    if (!out)
        throw std::runtime_error("failed to open output path");

    out << std::setprecision(9);
    out << "{\n";
    out << "  \"skeleton\": \"" << jsonEscape(Files::pathToUnicodeString(skeletonPath)) << "\",\n";
    out << "  \"mesh\": \"" << jsonEscape(Files::pathToUnicodeString(meshPath)) << "\",\n";
    out << "  \"skeletonNodeCount\": " << skeletonWorlds.size() << ",\n";
    out << "  \"skinnedGeometryCount\": " << geometries.size() << ",\n";
    out << "  \"restPoseInvariant\": \"A valid bind-pose skinning formula should keep source vertices stationary at bind pose.\",\n";
    out << "  \"geometries\": [";
    bool firstGeometry = true;
    for (const Nif::NiGeometry* geometry : geometries)
    {
        const Nif::NiSkinInstance* skin = geometry->mSkin.getPtr();
        const Nif::NiSkinData* data = skin != nullptr ? skin->mData.getPtr() : nullptr;
        if (skin == nullptr || data == nullptr || geometry->mData.empty())
            continue;

        if (!firstGeometry)
            out << ",";
        firstGeometry = false;

        std::size_t missingBones = 0;
        float maxSkinBoneVsSkeletonInverseDelta = 0.f;
        for (std::size_t i = 0; i < skin->mBones.size() && i < data->mBones.size(); ++i)
        {
            const Nif::NiAVObject* bone = skin->mBones[i].getPtr();
            const std::string lowerName = bone != nullptr ? Misc::StringUtils::lowerCase(bone->mName) : std::string();
            const auto found = skeletonWorlds.find(lowerName);
            if (found == skeletonWorlds.end())
            {
                ++missingBones;
                continue;
            }

            osg::Matrixf inverseBindWorld;
            if (!inverseBindWorld.invert(found->second))
                continue;
            maxSkinBoneVsSkeletonInverseDelta = std::max(maxSkinBoneVsSkeletonInverseDelta,
                fnvMatrixMaxAbsDelta(data->mBones[i].mTransform.toMatrix(), inverseBindWorld));
        }

        out << "\n    {\"name\":\"" << jsonEscape(geometry->mName) << "\""
            << ",\"vertexCount\":" << geometry->mData->mVertices.size()
            << ",\"boneCount\":" << skin->mBones.size()
            << ",\"missingSkeletonBones\":" << missingBones
            << ",\"maxSkinBoneVsSkeletonInverseDelta\":" << maxSkinBoneVsSkeletonInverseDelta
            << ",\"skinRootTransform\":" << formatMatrixJson(data->mTransform.toMatrix())
            << ",\"candidates\":[";

        bool firstCandidate = true;
        for (const std::string& candidate : candidates)
        {
            const FnvSkinCandidateMetric metric
                = auditFNVRestCandidate(candidate, *geometry, *data, *skin, skeletonWorlds);
            const osg::Vec3f sourceExtent = fnvAuditExtent(metric.mSourceBox);
            const osg::Vec3f skinnedExtent = fnvAuditExtent(metric.mSkinnedBox);
            if (!firstCandidate)
                out << ",";
            firstCandidate = false;
            out << "{\"name\":\"" << candidate << "\""
                << ",\"maxWeightedVertexDelta\":" << metric.mMaxVertexDelta
                << ",\"maxVertex\":" << metric.mMaxVertex
                << ",\"maxBlendedVertexDelta\":" << metric.mMaxBlendedVertexDelta
                << ",\"maxBlendedVertex\":" << metric.mMaxBlendedVertex
                << ",\"sourceExtent\":" << formatVec3Json(sourceExtent)
                << ",\"skinnedExtent\":" << formatVec3Json(skinnedExtent)
                << ",\"verdict\":\"" << (metric.mMaxBlendedVertexDelta <= 0.01f ? "PASS" : "FAIL") << "\"}";
        }
        out << "]}";
    }
    out << "\n  ]\n";
    out << "}\n";

    return 0;
}

int runFnvTransformDump(
    const std::filesystem::path& skeletonPath, const std::filesystem::path& kfPath, const std::filesystem::path& outPath)
{
    Nif::Reader::setLoadUnsupportedFiles(true);

    std::unique_ptr<Nif::NIFFile> skeletonFile = readNifFile(skeletonPath);
    osg::ref_ptr<osg::Node> skeleton = NifOsg::Loader::load(*skeletonFile, nullptr, nullptr);
    if (skeleton == nullptr)
        throw std::runtime_error("failed to load skeleton scene graph");

    std::unique_ptr<Nif::NIFFile> kfFile = readNifFile(kfPath);
    SceneUtil::KeyframeHolder keyframes;
    NifOsg::Loader::loadKf(*kfFile, keyframes);

    const float sampleTime = 1.2f;
    std::shared_ptr<SceneUtil::ControllerSource> sampleTimeSource
        = std::make_shared<ConstantTransformDumpTimeSource>(sampleTime);
    for (const auto& [name, controller] : keyframes.mKeyframeControllers)
    {
        auto* mutableController = const_cast<SceneUtil::KeyframeController*>(controller.get());
        mutableController->setSource(sampleTimeSource);
    }

    std::vector<TransformDumpNode> nodes;
    std::unordered_map<osg::MatrixTransform*, osg::Matrixf> bindWorlds;
    TransformDumpVisitor visitor(nodes, bindWorlds);
    skeleton->accept(visitor);

    std::map<std::string, TransformDumpNode*> nodesByLower;
    for (TransformDumpNode& node : nodes)
        nodesByLower.emplace(node.mLowerName, &node);

    const std::vector<std::string> modes = { "bind", "rawKey", "bindThenKey", "keyThenBind", "splitKeyXZThenBind",
        "bindCoreRawLimbs", "bindCoreBindLowerRawUpper", "bindCoreBindLowerSplitUpper",
        "bindCoreRawLowerBindUpper", "rawCoreBindLowerRawUpper", "bindCoreBindLowerBindUpper",
        "bindCoreBindLowerBindArmsRawHands", "bindCoreBindLowerBindUpperRawForearmsHands",
        "bindCoreBindLowerRawUpperBindHands", "bindCoreBindLowerRawClavicleBindArms" };
    std::unordered_map<osg::MatrixTransform*, SceneUtil::KeyframeController::KfTransform> sampledKeyframes;
    std::unordered_map<osg::MatrixTransform*, std::map<std::string, osg::Matrixf>> recursiveWorlds;

    const auto findController = [&](const std::string& nodeName) {
        auto controllerIt = keyframes.mKeyframeControllers.find(nodeName);
        if (controllerIt != keyframes.mKeyframeControllers.end())
            return controllerIt;

        for (auto it = keyframes.mKeyframeControllers.begin(); it != keyframes.mKeyframeControllers.end(); ++it)
        {
            if (Misc::StringUtils::ciEqual(it->first, nodeName))
                return it;
        }
        return keyframes.mKeyframeControllers.end();
    };

    for (TransformDumpNode& node : nodes)
    {
        auto controllerIt = findController(node.mName);
        SceneUtil::KeyframeController::KfTransform keyTransform;
        if (controllerIt != keyframes.mKeyframeControllers.end())
        {
            auto* controller = const_cast<SceneUtil::KeyframeController*>(controllerIt->second.get());
            *controller->mTime = sampleTime;
            keyTransform = controller->getCurrentTransformation(nullptr);
        }
        sampledKeyframes.emplace(node.mTransform, keyTransform);

        const osg::Vec3f keyTranslation = keyTransform.mTranslation.value_or(node.mBindLocal.getTrans());
        const osg::Quat keyRotation = keyTransform.mRotation.value_or(node.mBindLocal.getRotate());
        const osg::Quat bindRotation = node.mBindLocal.getRotate();

        for (const std::string& mode : modes)
        {
            const osg::Quat modeRotation
                = composeFNVTransformDumpRotation(mode, node.mLowerName, keyRotation, bindRotation);
            const osg::Vec3f modeTranslation
                = mode == "bind" ? osg::Vec3f(node.mBindLocal.getTrans()) : keyTranslation;
            const osg::Matrixf local = makeLocalMatrixLike(node.mTransform, modeRotation, modeTranslation);
            osg::Matrixf parentWorld = osg::Matrixf::identity();
            if (node.mParentTransform != nullptr)
            {
                auto parentIt = recursiveWorlds.find(node.mParentTransform);
                if (parentIt != recursiveWorlds.end())
                {
                    auto modeIt = parentIt->second.find(mode);
                    if (modeIt != parentIt->second.end())
                        parentWorld = modeIt->second;
                }
            }
            recursiveWorlds[node.mTransform][mode] = local * parentWorld;
        }
    }

    std::ofstream out(outPath);
    if (!out)
        throw std::runtime_error("failed to open output path");

    out << std::setprecision(9);
    out << "{\n";
    out << "  \"skeleton\": \"" << jsonEscape(Files::pathToUnicodeString(skeletonPath)) << "\",\n";
    out << "  \"kf\": \"" << jsonEscape(Files::pathToUnicodeString(kfPath)) << "\",\n";
    out << "  \"sampleTime\": " << sampleTime << ",\n";
    out << "  \"controllerCount\": " << keyframes.mKeyframeControllers.size() << ",\n";
    out << "  \"textKeys\": [";
    bool firstTextKey = true;
    for (const auto& [time, text] : keyframes.mTextKeys)
    {
        if (!firstTextKey)
            out << ",";
        firstTextKey = false;
        out << "{\"time\":" << time << ",\"text\":\"" << jsonEscape(text) << "\"}";
    }
    out << "],\n";
    out << "  \"controllerNames\": [";
    bool firstControllerName = true;
    for (const auto& [name, controller] : keyframes.mKeyframeControllers)
    {
        (void)controller;
        if (!firstControllerName)
            out << ",";
        firstControllerName = false;
        out << "\"" << jsonEscape(name) << "\"";
    }
    out << "],\n";
    out << "  \"controllerBindings\": [";
    bool firstControllerBinding = true;
    for (const auto& [name, controller] : keyframes.mKeyframeControllers)
    {
        auto* mutableController = const_cast<SceneUtil::KeyframeController*>(controller.get());
        *mutableController->mTime = sampleTime;
        const SceneUtil::KeyframeController::KfTransform keyTransform
            = mutableController->getCurrentTransformation(nullptr);
        const std::string lowerName = Misc::StringUtils::lowerCase(name);
        if (!firstControllerBinding)
            out << ",";
        firstControllerBinding = false;
        out << "{\"name\":\"" << jsonEscape(name) << "\""
            << ",\"hasSkeletonNode\":" << (nodesByLower.find(lowerName) != nodesByLower.end() ? "true" : "false")
            << ",\"isHashHelper\":" << (Misc::StringUtils::ciStartsWith(name, "##") ? "true" : "false")
            << ",\"isWeaponTarget\":" << (Misc::StringUtils::ciEqual(name, "Weapon") ? "true" : "false")
            << ",\"hasKeyTranslation\":" << (keyTransform.mTranslation ? "true" : "false")
            << ",\"hasKeyRotation\":" << (keyTransform.mRotation ? "true" : "false")
            << ",\"keyTranslation\":"
            << formatVec3Json(keyTransform.mTranslation.value_or(osg::Vec3f()))
            << ",\"keyRotationQuat\":"
            << formatQuatJson(keyTransform.mRotation.value_or(osg::Quat()))
            << "}";
    }
    out << "],\n";
    out << "  \"controlledBlocks\": [";
    bool firstBlock = true;
    for (const auto& record : kfFile->mRecords)
    {
        if (!record || record->recType != Nif::RC_NiControllerSequence)
            continue;

        const auto& sequence = static_cast<const Nif::NiControllerSequence&>(*record);
        for (const Nif::ControlledBlock& block : sequence.mControlledBlocks)
        {
            const std::string targetName = resolveNifControlledBlockTargetName(sequence, block);
            const int interpolatorType = block.mInterpolator.empty() ? 0 : block.mInterpolator->recType;
            const std::string interpolatorName = block.mInterpolator.empty() ? "" : block.mInterpolator->recName;
            if (!firstBlock)
                out << ",";
            firstBlock = false;
            out << "{\"sequence\":\"" << jsonEscape(sequence.mName) << "\""
                << ",\"target\":\"" << jsonEscape(targetName) << "\""
                << ",\"targetName\":\"" << jsonEscape(block.mTargetName) << "\""
                << ",\"nodeName\":\"" << jsonEscape(block.mNodeName) << "\""
                << ",\"controllerId\":\"" << jsonEscape(block.mControllerId) << "\""
                << ",\"interpolatorId\":\"" << jsonEscape(block.mInterpolatorId) << "\""
                << ",\"interpolatorType\":" << interpolatorType
                << ",\"interpolatorName\":\"" << jsonEscape(interpolatorName) << "\""
                << ",\"loadedAsTransform\":" << (isLoadedFNVTransformInterpolator(interpolatorType) ? "true" : "false")
                << "}";
        }
    }
    out << "],\n";
    out << "  \"modePosture\": [";
    bool firstMode = true;
    for (const std::string& mode : modes)
    {
        const auto getModeOrigin = [&](const std::string& lowerName) {
            const auto nodeIt = nodesByLower.find(lowerName);
            if (nodeIt == nodesByLower.end())
                return osg::Vec3f();
            const auto worldIt = recursiveWorlds.find(nodeIt->second->mTransform);
            if (worldIt == recursiveWorlds.end())
                return osg::Vec3f();
            const auto modeIt = worldIt->second.find(mode);
            if (modeIt == worldIt->second.end())
                return osg::Vec3f();
            return osg::Vec3f(modeIt->second.getTrans());
        };
        const auto getModeRotation = [&](const std::string& lowerName) {
            const auto nodeIt = nodesByLower.find(lowerName);
            if (nodeIt == nodesByLower.end())
                return osg::Quat();

            SceneUtil::KeyframeController::KfTransform keyTransform;
            const auto sampledIt = sampledKeyframes.find(nodeIt->second->mTransform);
            if (sampledIt != sampledKeyframes.end())
                keyTransform = sampledIt->second;

            const osg::Quat keyRotation = keyTransform.mRotation.value_or(nodeIt->second->mBindLocal.getRotate());
            const osg::Quat bindRotation = nodeIt->second->mBindLocal.getRotate();
            return composeFNVTransformDumpRotation(mode, lowerName, keyRotation, bindRotation);
        };
        const auto rotationBindDelta = [&](const std::string& lowerName) {
            const auto nodeIt = nodesByLower.find(lowerName);
            if (nodeIt == nodesByLower.end())
                return 0.0;
            return quatDeltaDegrees(getModeRotation(lowerName), nodeIt->second->mBindLocal.getRotate());
        };
        const auto distance = [](const osg::Vec3f& left, const osg::Vec3f& right) {
            return (left - right).length();
        };

        const osg::Vec3f head = getModeOrigin("bip01 head");
        const osg::Vec3f pelvis = getModeOrigin("bip01 pelvis");
        const osg::Vec3f leftFoot = getModeOrigin("bip01 l foot");
        const osg::Vec3f rightFoot = getModeOrigin("bip01 r foot");
        const osg::Vec3f leftUpperArm = getModeOrigin("bip01 l upperarm");
        const osg::Vec3f rightUpperArm = getModeOrigin("bip01 r upperarm");
        const osg::Vec3f leftForearm = getModeOrigin("bip01 l forearm");
        const osg::Vec3f rightForearm = getModeOrigin("bip01 r forearm");
        const osg::Vec3f leftHand = getModeOrigin("bip01 l hand");
        const osg::Vec3f rightHand = getModeOrigin("bip01 r hand");
        const osg::Vec3f avgFoot = (leftFoot + rightFoot) * 0.5f;
        const osg::Vec2f torsoHorizontal(head.x() - pelvis.x(), head.y() - pelvis.y());
        const osg::Vec2f footSpread(leftFoot.x() - rightFoot.x(), leftFoot.y() - rightFoot.y());
        const double leftUpperArmBindDelta = rotationBindDelta("bip01 l upperarm");
        const double rightUpperArmBindDelta = rotationBindDelta("bip01 r upperarm");
        const double leftForearmBindDelta = rotationBindDelta("bip01 l forearm");
        const double rightForearmBindDelta = rotationBindDelta("bip01 r forearm");
        const double leftHandBindDelta = rotationBindDelta("bip01 l hand");
        const double rightHandBindDelta = rotationBindDelta("bip01 r hand");
        const double maxArmBindDelta = std::max({ leftUpperArmBindDelta, rightUpperArmBindDelta, leftForearmBindDelta,
            rightForearmBindDelta, leftHandBindDelta, rightHandBindDelta });
        if (!firstMode)
            out << ",";
        firstMode = false;
        out << "{\"mode\":\"" << jsonEscape(mode) << "\""
            << ",\"headMinusPelvisZ\":" << (head.z() - pelvis.z())
            << ",\"pelvisMinusAvgFeetZ\":" << (pelvis.z() - avgFoot.z())
            << ",\"torsoHorizontal\":" << torsoHorizontal.length()
            << ",\"footSpread\":" << footSpread.length()
            << ",\"leftUpperArmBindDeltaDeg\":" << leftUpperArmBindDelta
            << ",\"rightUpperArmBindDeltaDeg\":" << rightUpperArmBindDelta
            << ",\"leftForearmBindDeltaDeg\":" << leftForearmBindDelta
            << ",\"rightForearmBindDeltaDeg\":" << rightForearmBindDelta
            << ",\"leftHandBindDeltaDeg\":" << leftHandBindDelta
            << ",\"rightHandBindDeltaDeg\":" << rightHandBindDelta
            << ",\"maxArmBindDeltaDeg\":" << maxArmBindDelta
            << ",\"leftUpperToForearmDistance\":" << distance(leftUpperArm, leftForearm)
            << ",\"rightUpperToForearmDistance\":" << distance(rightUpperArm, rightForearm)
            << ",\"leftForearmToHandDistance\":" << distance(leftForearm, leftHand)
            << ",\"rightForearmToHandDistance\":" << distance(rightForearm, rightHand)
            << ",\"leftHandToHeadDistance\":" << distance(leftHand, head)
            << ",\"rightHandToHeadDistance\":" << distance(rightHand, head)
            << ",\"leftHandToPelvisDistance\":" << distance(leftHand, pelvis)
            << ",\"rightHandToPelvisDistance\":" << distance(rightHand, pelvis)
            << ",\"headOrigin\":" << formatVec3Json(head)
            << ",\"pelvisOrigin\":" << formatVec3Json(pelvis)
            << ",\"leftFootOrigin\":" << formatVec3Json(leftFoot)
            << ",\"rightFootOrigin\":" << formatVec3Json(rightFoot)
            << ",\"leftHandOrigin\":" << formatVec3Json(leftHand)
            << ",\"rightHandOrigin\":" << formatVec3Json(rightHand)
            << "}";
    }
    out << "],\n";
    out << "  \"bones\": [\n";

    bool firstBone = true;
    for (TransformDumpNode& node : nodes)
    {
        if (!isImportantFNVHumanBone(node.mLowerName))
            continue;

        auto controllerIt = findController(node.mName);
        bool hasController = controllerIt != keyframes.mKeyframeControllers.end();
        SceneUtil::KeyframeController::KfTransform keyTransform;
        const auto sampledIt = sampledKeyframes.find(node.mTransform);
        if (sampledIt != sampledKeyframes.end())
            keyTransform = sampledIt->second;

        osg::Vec3f keyTranslation = keyTransform.mTranslation.value_or(node.mBindLocal.getTrans());
        osg::Quat keyRotation = keyTransform.mRotation.value_or(node.mBindLocal.getRotate());
        osg::Quat bindRotation = node.mBindLocal.getRotate();
        osg::Matrixf rawLocal = makeLocalMatrixLike(node.mTransform, keyRotation, keyTranslation);
        osg::Matrixf bindThenKeyLocal = makeLocalMatrixLike(node.mTransform, bindRotation * keyRotation, keyTranslation);
        osg::Matrixf keyThenBindLocal = makeLocalMatrixLike(node.mTransform, keyRotation * bindRotation, keyTranslation);
        osg::Matrixf rawWorld = recursiveWorlds[node.mTransform]["rawKey"];
        osg::Matrixf bindThenKeyWorld = recursiveWorlds[node.mTransform]["bindThenKey"];
        osg::Matrixf keyThenBindWorld = recursiveWorlds[node.mTransform]["keyThenBind"];

        if (!firstBone)
            out << ",\n";
        firstBone = false;

        out << "    {\n";
        out << "      \"name\": \"" << jsonEscape(node.mName) << "\",\n";
        out << "      \"parent\": \"" << jsonEscape(node.mParentName) << "\",\n";
        out << "      \"hasController\": " << (hasController ? "true" : "false") << ",\n";
        out << "      \"hasKeyTranslation\": " << (keyTransform.mTranslation ? "true" : "false") << ",\n";
        out << "      \"hasKeyRotation\": " << (keyTransform.mRotation ? "true" : "false") << ",\n";
        out << "      \"bindLocalTranslation\": " << formatVec3Json(node.mBindLocal.getTrans()) << ",\n";
        out << "      \"bindWorldOrigin\": " << formatVec3Json(node.mBindWorld.getTrans()) << ",\n";
        out << "      \"bindLocalQuat\": " << formatQuatJson(bindRotation) << ",\n";
        out << "      \"keyTranslation\": " << formatVec3Json(keyTranslation) << ",\n";
        out << "      \"keyRotationQuat\": " << formatQuatJson(keyRotation) << ",\n";
        out << "      \"rawWorldOrigin\": " << formatVec3Json(rawWorld.getTrans()) << ",\n";
        out << "      \"bindThenKeyWorldOrigin\": " << formatVec3Json(bindThenKeyWorld.getTrans()) << ",\n";
        out << "      \"keyThenBindWorldOrigin\": " << formatVec3Json(keyThenBindWorld.getTrans()) << ",\n";
        out << "      \"modeWorldOrigins\": {";
        bool firstModeOrigin = true;
        for (const std::string& mode : modes)
        {
            if (!firstModeOrigin)
                out << ",";
            firstModeOrigin = false;
            out << "\"" << jsonEscape(mode) << "\":"
                << formatVec3Json(recursiveWorlds[node.mTransform][mode].getTrans());
        }
        out << "},\n";
        out << "      \"bindLocalMatrix\": " << formatMatrixJson(node.mBindLocal) << ",\n";
        out << "      \"rawLocalMatrix\": " << formatMatrixJson(rawLocal) << "\n";
        out << "    }";
    }

    out << "\n  ]\n";
    out << "}\n";

    return 0;
}

bool isBSA(const std::filesystem::path& path)
{
    return classifyFile(path).second == FileClass::Archive;
}

std::unique_ptr<VFS::Archive> makeArchive(const std::filesystem::path& path)
{
    if (isBSA(path))
        return VFS::makeBsaArchive(path, nullptr);
    if (std::filesystem::is_directory(path))
        return std::make_unique<VFS::FileSystemArchive>(path);
    return nullptr;
}

bool readFile(
    const std::filesystem::path& source, const std::filesystem::path& path, const VFS::Manager* vfs, bool quiet)
{
    const auto [fileType, fileClass] = classifyFile(path);
    if (fileClass != FileClass::NIF && fileClass != FileClass::Material)
        return false;

    const std::string pathStr = Files::pathToUnicodeString(path);
    if (!quiet)
    {
        std::cout << "Reading " << getFileTypeName(fileType) << " file '" << pathStr << "'";
        if (!source.empty())
            std::cout << " from '" << Files::pathToUnicodeString(isBSA(source) ? source.filename() : source) << "'";
        std::cout << std::endl;
    }
    const std::filesystem::path fullPath = !source.empty() ? source / path : path;
    try
    {
        switch (fileClass)
        {
            case FileClass::NIF:
            {
                Nif::NIFFile file(VFS::Path::Normalized(Files::pathToUnicodeString(fullPath)));
                Nif::Reader reader(file, nullptr);
                if (vfs != nullptr)
                    reader.parse(vfs->get(pathStr));
                else
                    reader.parse(Files::openConstrainedFileStream(fullPath));
                break;
            }
            case FileClass::Material:
            {
                if (vfs != nullptr)
                    Bgsm::parse(vfs->get(pathStr));
                else
                    Bgsm::parse(Files::openConstrainedFileStream(fullPath));
                break;
            }
            default:
                break;
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Failed to read '" << pathStr << "':" << std::endl << e.what() << std::endl;
    }
    return true;
}

/// Check all the nif files in a given VFS::Archive
/// \note Can not read a bsa file inside of a bsa file.
void readVFS(std::unique_ptr<VFS::Archive>&& archive, const std::filesystem::path& archivePath, bool quiet)
{
    if (archive == nullptr)
        return;

    if (!quiet)
        std::cout << "Reading data source '" << Files::pathToUnicodeString(archivePath) << "'" << std::endl;

    VFS::Manager vfs;
    vfs.addArchive(std::move(archive));
    vfs.buildIndex();

    for (const auto& name : vfs.getRecursiveDirectoryIterator())
    {
        readFile(archivePath, name.value(), &vfs, quiet);
    }

    if (!archivePath.empty() && !isBSA(archivePath))
    {
        const Files::Collections fileCollections({ archivePath });
        const Files::MultiDirCollection& bsaCol = fileCollections.getCollection(".bsa");
        const Files::MultiDirCollection& ba2Col = fileCollections.getCollection(".ba2");
        for (const Files::MultiDirCollection& collection : { bsaCol, ba2Col })
        {
            for (auto& file : collection)
            {
                try
                {
                    readVFS(VFS::makeBsaArchive(file.second, nullptr), file.second, quiet);
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Failed to read archive file '" << Files::pathToUnicodeString(file.second)
                              << "': " << e.what() << std::endl;
                }
            }
        }
    }
}

bool parseOptions(int argc, char** argv, Files::PathContainer& files, Files::PathContainer& archives,
    bool& writeDebugLog, bool& quiet)
{
    bpo::options_description desc(
        R"(Ensure that OpenMW can use the provided NIF, KF, BTO/BTR, RDT, PSA, BGEM/BGSM and BSA/BA2 files

Usages:
  niftest <nif files, kf files, bto/btr files, rdt files, psa files, bgem/bgsm files, BSA/BA2 files, or directories>
      Scan the file or directories for NIF errors.

Allowed options)");
    auto addOption = desc.add_options();
    addOption("help,h", "print help message.");
    addOption("write-debug-log,v", "write debug log for unsupported nif files");
    addOption("quiet,q", "do not log read archives/files");
    addOption("archives", bpo::value<Files::MaybeQuotedPathContainer>(), "path to archive files to provide files");
    addOption("input-file", bpo::value<Files::MaybeQuotedPathContainer>(), "input file");

    // Default option if none provided
    bpo::positional_options_description p;
    p.add("input-file", -1);

    bpo::variables_map variables;
    try
    {
        bpo::parsed_options validOpts = bpo::command_line_parser(argc, argv).options(desc).positional(p).run();
        bpo::store(validOpts, variables);
        bpo::notify(variables);
        if (variables.count("help"))
        {
            std::cout << desc << std::endl;
            return false;
        }
        writeDebugLog = variables.count("write-debug-log") > 0;
        quiet = variables.count("quiet") > 0;
        if (variables.count("input-file"))
        {
            files = asPathContainer(variables["input-file"].as<Files::MaybeQuotedPathContainer>());
            if (const auto it = variables.find("archives"); it != variables.end())
                archives = asPathContainer(it->second.as<Files::MaybeQuotedPathContainer>());
            return true;
        }
    }
    catch (std::exception& e)
    {
        std::cout << "Error parsing arguments: " << e.what() << "\n\n" << desc << std::endl;
        return false;
    }

    std::cout << "No input files or directories specified!" << std::endl;
    std::cout << desc << std::endl;
    return false;
}

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--fnv-geometry-dump")
    {
        try
        {
            return runFnvGeometryDump(argv[2], argv[3]);
        }
        catch (const std::exception& e)
        {
            std::cerr << "FNV geometry dump failed: " << e.what() << std::endl;
            return 2;
        }
    }

    if (argc == 5 && std::string_view(argv[1]) == "--fnv-skin-bind-audit")
    {
        try
        {
            return runFnvSkinBindAudit(argv[2], argv[3], argv[4]);
        }
        catch (const std::exception& e)
        {
            std::cerr << "FNV skin bind audit failed: " << e.what() << std::endl;
            return 2;
        }
    }

    if (argc == 5 && std::string_view(argv[1]) == "--fnv-transform-dump")
    {
        try
        {
            return runFnvTransformDump(argv[2], argv[3], argv[4]);
        }
        catch (const std::exception& e)
        {
            std::cerr << "FNV transform dump failed: " << e.what() << std::endl;
            return 2;
        }
    }

    Files::PathContainer files, sources;
    bool writeDebugLog = false;
    bool quiet = false;
    if (!parseOptions(argc, argv, files, sources, writeDebugLog, quiet))
        return 1;

    Nif::Reader::setLoadUnsupportedFiles(true);
    Nif::Reader::setWriteNifDebugLog(writeDebugLog);

    std::unique_ptr<VFS::Manager> vfs;
    if (!sources.empty())
    {
        vfs = std::make_unique<VFS::Manager>();
        for (const std::filesystem::path& path : sources)
        {
            const std::string pathStr = Files::pathToUnicodeString(path);
            if (!quiet)
                std::cout << "Adding data source '" << pathStr << "'" << std::endl;

            try
            {
                if (auto archive = makeArchive(path))
                    vfs->addArchive(std::move(archive));
                else
                    std::cerr << "Error: '" << pathStr << "' is not an archive or directory" << std::endl;
            }
            catch (std::exception& e)
            {
                std::cerr << "Failed to add data source '" << pathStr << "':  " << e.what() << std::endl;
            }
        }

        vfs->buildIndex();
    }

    for (const auto& path : files)
    {
        const std::string pathStr = Files::pathToUnicodeString(path);
        try
        {
            const bool isFile = readFile({}, path, vfs.get(), quiet);
            if (!isFile)
            {
                if (auto archive = makeArchive(path))
                {
                    readVFS(std::move(archive), path, quiet);
                }
                else
                {
                    std::cerr << "Error: '" << pathStr << "' is not a NIF file, material file, archive, or directory"
                              << std::endl;
                }
            }
        }
        catch (std::exception& e)
        {
            std::cerr << "Failed to read '" << pathStr << "':  " << e.what() << std::endl;
        }
    }
    return 0;
}
