    struct OpenNvCanonicalBone
    {
        std::string mName;
        int mParent = -1;
        osg::Matrixf mLocal;
        osg::Matrixf mSkeleton;
    };

    class OpenNvSkeletonCandidateVisitor final : public osg::NodeVisitor
    {
    public:
        OpenNvSkeletonCandidateVisitor()
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
        {
            setTraversalMask(~0u);
            setNodeMaskOverride(~0u);
        }

        void apply(osg::Group& group) override
        {
            if (auto* skeleton = dynamic_cast<SceneUtil::Skeleton*>(&group))
            {
                NamedTransformCounter counter(skeleton);
                skeleton->accept(counter);
                if (counter.count() > mBestCount)
                {
                    mBest = skeleton;
                    mBestCount = counter.count();
                }
            }
            traverse(group);
        }

        const SceneUtil::Skeleton* best() const { return mBest; }

    private:
        class NamedTransformCounter final : public osg::NodeVisitor
        {
        public:
            explicit NamedTransformCounter(const SceneUtil::Skeleton* root)
                : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
                , mRoot(root)
            {
                setTraversalMask(~0u);
                setNodeMaskOverride(~0u);
            }

            void apply(osg::Group& group) override
            {
                if (&group != mRoot && dynamic_cast<SceneUtil::Skeleton*>(&group) != nullptr)
                    return;
                traverse(group);
            }

            void apply(osg::MatrixTransform& transform) override
            {
                if (!transform.getName().empty())
                    ++mCount;
                traverse(transform);
            }

            std::size_t count() const { return mCount; }

        private:
            const SceneUtil::Skeleton* mRoot;
            std::size_t mCount = 0;
        };

        const SceneUtil::Skeleton* mBest = nullptr;
        std::size_t mBestCount = 0;
    };

    class OpenNvCanonicalSkeletonVisitor final : public osg::NodeVisitor
    {
    public:
        OpenNvCanonicalSkeletonVisitor(const osg::NodePath& actorPath, const osg::Matrixd& worldToActor,
            const SceneUtil::Skeleton* selectedSkeleton)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            , mActorPath(actorPath)
            , mWorldToActor(worldToActor)
            , mSelectedSkeleton(selectedSkeleton)
        {
            setTraversalMask(~0u);
            setNodeMaskOverride(~0u);
        }

        void apply(osg::Group& group) override
        {
            const auto* candidate = dynamic_cast<SceneUtil::Skeleton*>(&group);
            if (candidate != nullptr && candidate != mSelectedSkeleton)
                return;
            const bool skeleton = candidate == mSelectedSkeleton;
            if (skeleton)
                ++mSkeletonDepth;
            traverse(group);
            if (skeleton)
                --mSkeletonDepth;
        }

        void apply(osg::MatrixTransform& transform) override
        {
            if (mSkeletonDepth == 0 || transform.getName().empty())
            {
                traverse(transform);
                return;
            }
            const osg::Matrixf skeletonMatrix(currentActorMatrix());
            const std::string key = Misc::StringUtils::lowerCase(transform.getName());
            std::size_t index = mBones.size();
            const auto found = mBoneByName.find(key);
            if (found != mBoneByName.end())
            {
                index = found->second;
                if (!matrixClose(mBones[index].mSkeleton, skeletonMatrix))
                    return;
            }
            else
            {
                OpenNvCanonicalBone bone;
                bone.mName = transform.getName();
                bone.mParent = mBoneStack.empty() ? -1 : static_cast<int>(mBoneStack.back());
                bone.mSkeleton = skeletonMatrix;
                bone.mLocal = skeletonMatrix;
                if (bone.mParent >= 0)
                {
                    osg::Matrixf parentInverse;
                    if (!parentInverse.invert(mBones[static_cast<std::size_t>(bone.mParent)].mSkeleton))
                        throw std::runtime_error("OpenNV canonical skeleton parent matrix is singular");
                    bone.mLocal = skeletonMatrix * parentInverse;
                }
                mBoneByName.emplace(key, index);
                mBones.emplace_back(std::move(bone));
            }
            mBoneStack.push_back(index);
            traverse(transform);
            mBoneStack.pop_back();
        }

        const std::vector<OpenNvCanonicalBone>& getBones() const { return mBones; }

    private:
        osg::Matrixd currentActorMatrix() const
        {
            osg::NodePath fullPath = mActorPath;
            if (!fullPath.empty())
                fullPath.pop_back();
            const osg::NodePath& localPath = getNodePath();
            fullPath.insert(fullPath.end(), localPath.begin(), localPath.end());
            return osg::computeLocalToWorld(fullPath) * mWorldToActor;
        }

        static bool matrixClose(const osg::Matrixf& left, const osg::Matrixf& right)
        {
            for (unsigned int row = 0; row < 4; ++row)
                for (unsigned int column = 0; column < 4; ++column)
                    if (std::abs(left(row, column) - right(row, column)) > 0.001f)
                        return false;
            return true;
        }

        osg::NodePath mActorPath;
        osg::Matrixd mWorldToActor;
        const SceneUtil::Skeleton* mSelectedSkeleton;
        unsigned int mSkeletonDepth = 0;
        std::vector<std::size_t> mBoneStack;
        std::unordered_map<std::string, std::size_t> mBoneByName;
        std::vector<OpenNvCanonicalBone> mBones;
    };

    // Lossless, engine-neutral actor skin payload. OBJ remains useful as a
    // diagnostic snapshot, but it cannot carry the bind mesh, bone palette or
    // vertex influences Godot needs for real animation. All scalar values are
    // little-endian; the companion decoder rejects non-little-endian hosts.
    class OpenNvActorSkeletonVisitor final : public osg::NodeVisitor
    {
    public:
        OpenNvActorSkeletonVisitor(std::ostream& output, const osg::Matrixd& worldToActor,
            const osg::NodePath& actorPath, const std::vector<OpenNvCanonicalBone>& canonicalBones)
            : osg::NodeVisitor(osg::NodeVisitor::TRAVERSE_ALL_CHILDREN)
            , mOutput(output)
            , mWorldToActor(worldToActor)
            , mActorPath(actorPath)
            , mCanonicalBones(canonicalBones)
        {
            setTraversalMask(~0u);
            setNodeMaskOverride(~0u);
            static_assert(std::endian::native == std::endian::little,
                "OpenNV actor skeleton export currently requires little-endian storage");
            mOutput.write("ONVSKEL2", 8);
            writeValue(std::uint32_t{ 2 });
            mSurfaceCountOffset = mOutput.tellp();
            writeValue(std::uint32_t{ 0 });
            writeValue(static_cast<std::uint32_t>(mCanonicalBones.size()));
            for (const OpenNvCanonicalBone& bone : mCanonicalBones)
            {
                writeString(bone.mName);
                writeValue(static_cast<std::int32_t>(bone.mParent));
                writeMatrix(bone.mLocal);
                writeMatrix(bone.mSkeleton);
            }
        }

        void apply(osg::Drawable& drawable) override
        {
            if (drawable.getNodeMask() == 0)
                return;
            for (const osg::Node* node : getNodePath())
                if (node != nullptr && node->getNodeMask() == 0)
                    return;
            auto* rig = dynamic_cast<SceneUtil::RigGeometry*>(&drawable);
            if (rig == nullptr)
            {
                if (osg::Geometry* geometry = drawable.asGeometry())
                    writeStaticSurface(drawable, *geometry);
                return;
            }
            osg::Geometry* source = rig->getSourceGeometry();
            const auto* vertices = source == nullptr ? nullptr
                : dynamic_cast<const osg::Vec3Array*>(source->getVertexArray());
            if (source == nullptr || vertices == nullptr || vertices->empty())
                return;

            std::vector<SceneUtil::RigGeometry::BoneInfo> bones;
            std::vector<SceneUtil::RigGeometry::BoneWeights> influences;
            std::vector<osg::Matrixf> localBoneMatrices;
            std::vector<osg::Matrixf> skeletonBoneMatrices;
            osg::Matrixf transform;
            osg::Matrixf skinToSkeleton;
            if (!rig->getSkinningDebugData(bones, influences, localBoneMatrices,
                    skeletonBoneMatrices, transform, skinToSkeleton))
                return;
            if (influences.size() != vertices->size() || bones.empty())
                return;
            std::vector<int> parentBoneIndices;
            if (!rig->getBoneParentIndices(parentBoneIndices) || parentBoneIndices.size() != bones.size())
                return;

            std::vector<std::uint32_t> indices;
            appendTriangleIndices(*source, indices);
            if (indices.empty())
                return;

            writeString(drawable.getName());
            writeString(std::string(rig->getRootBone()));
            writeString(texturePath(drawable, *source));
            writeValue(static_cast<std::uint32_t>(vertices->size()));
            writeValue(static_cast<std::uint32_t>(indices.size()));
            writeValue(static_cast<std::uint32_t>(bones.size()));
            writeMatrix(transform);
            writeMatrix(skinToSkeleton);

            const auto* normals = dynamic_cast<const osg::Vec3Array*>(source->getNormalArray());
            const auto* texcoords = dynamic_cast<const osg::Vec2Array*>(source->getTexCoordArray(0));
            for (std::size_t i = 0; i < vertices->size(); ++i)
            {
                const osg::Vec3f& vertex = (*vertices)[i];
                const osg::Vec3f normal
                    = normals != nullptr && i < normals->size() ? (*normals)[i] : osg::Vec3f(0.f, 0.f, 1.f);
                const osg::Vec2f uv
                    = texcoords != nullptr && i < texcoords->size() ? (*texcoords)[i] : osg::Vec2f();
                writeValue(vertex.x());
                writeValue(vertex.y());
                writeValue(vertex.z());
                writeValue(normal.x());
                writeValue(normal.y());
                writeValue(normal.z());
                writeValue(uv.x());
                writeValue(uv.y());
            }
            for (std::uint32_t index : indices)
                writeValue(index);

            for (std::size_t i = 0; i < bones.size(); ++i)
            {
                writeString(bones[i].mName);
                writeValue(static_cast<std::int32_t>(parentBoneIndices[i]));
                writeMatrix(bones[i].mInvBindMatrix);
                osg::Matrixf compressedLocal = i < skeletonBoneMatrices.size()
                    ? skeletonBoneMatrices[i] : osg::Matrixf();
                if (parentBoneIndices[i] >= 0)
                {
                    osg::Matrixf parentInverse;
                    if (!parentInverse.invert(skeletonBoneMatrices[static_cast<std::size_t>(parentBoneIndices[i])]))
                        throw std::runtime_error("OpenNV actor skeleton parent matrix is singular");
                    compressedLocal = skeletonBoneMatrices[i] * parentInverse;
                }
                writeMatrix(compressedLocal);
                writeMatrix(i < skeletonBoneMatrices.size() ? skeletonBoneMatrices[i] : osg::Matrixf());
            }
            for (const SceneUtil::RigGeometry::BoneWeights& vertexInfluences : influences)
            {
                const std::size_t count = std::min<std::size_t>(
                    vertexInfluences.size(), std::numeric_limits<std::uint16_t>::max());
                writeValue(static_cast<std::uint16_t>(count));
                for (std::size_t i = 0; i < count; ++i)
                {
                    if (vertexInfluences[i].first > std::numeric_limits<std::uint16_t>::max())
                        throw std::runtime_error("OpenNV actor skeleton bone index exceeds uint16 range");
                    writeValue(static_cast<std::uint16_t>(vertexInfluences[i].first));
                    writeValue(vertexInfluences[i].second);
                }
            }
            ++mSurfaceCount;
            mVertexCount += vertices->size();
            mTriangleCount += indices.size() / 3;
        }

        void finish()
        {
            const std::streampos end = mOutput.tellp();
            mOutput.seekp(mSurfaceCountOffset);
            writeValue(static_cast<std::uint32_t>(mSurfaceCount));
            mOutput.seekp(end);
        }

        std::size_t getSurfaceCount() const { return mSurfaceCount; }
        std::size_t getVertexCount() const { return mVertexCount; }
        std::size_t getTriangleCount() const { return mTriangleCount; }

    private:
        void writeStaticSurface(osg::Drawable& drawable, osg::Geometry& geometry)
        {
            const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (vertices == nullptr || vertices->empty())
                return;
            std::vector<std::uint32_t> indices;
            appendTriangleIndices(geometry, indices);
            if (indices.empty())
                return;
            std::string attachmentBone;
            osg::Matrixd geometryTransform = currentActorMatrix();
            for (auto node = getNodePath().rbegin(); node != getNodePath().rend(); ++node)
            {
                const auto* matrixTransform = dynamic_cast<const osg::MatrixTransform*>(*node);
                if (matrixTransform == nullptr || matrixTransform->getName().empty())
                    continue;
                const std::string key = Misc::StringUtils::lowerCase(matrixTransform->getName());
                const auto found = std::find_if(mCanonicalBones.begin(), mCanonicalBones.end(), [&](const auto& bone) {
                    return Misc::StringUtils::lowerCase(bone.mName) == key;
                });
                if (found == mCanonicalBones.end())
                    continue;
                osg::Matrixf boneInverse;
                if (!boneInverse.invert(found->mSkeleton))
                    throw std::runtime_error("OpenNV static attachment bone matrix is singular");
                geometryTransform *= osg::Matrixd(boneInverse);
                attachmentBone = found->mName;
                break;
            }
            writeString(drawable.getName());
            writeString(attachmentBone);
            writeString(texturePath(drawable, geometry));
            writeValue(static_cast<std::uint32_t>(vertices->size()));
            writeValue(static_cast<std::uint32_t>(indices.size()));
            writeValue(std::uint32_t{ 0 });
            osg::Matrixf identity;
            identity.makeIdentity();
            writeMatrix(identity);
            writeMatrix(identity);
            const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
            const auto* texcoords = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
            for (std::size_t i = 0; i < vertices->size(); ++i)
            {
                const osg::Vec3d vertex = osg::Vec3d((*vertices)[i]) * geometryTransform;
                osg::Vec3d normal = normals != nullptr && i < normals->size()
                    ? osg::Matrixd::transform3x3(osg::Vec3d((*normals)[i]), geometryTransform)
                    : osg::Vec3d(0.0, 0.0, 1.0);
                normal.normalize();
                const osg::Vec2f uv
                    = texcoords != nullptr && i < texcoords->size() ? (*texcoords)[i] : osg::Vec2f();
                writeValue(static_cast<float>(vertex.x()));
                writeValue(static_cast<float>(vertex.y()));
                writeValue(static_cast<float>(vertex.z()));
                writeValue(static_cast<float>(normal.x()));
                writeValue(static_cast<float>(normal.y()));
                writeValue(static_cast<float>(normal.z()));
                writeValue(uv.x());
                writeValue(uv.y());
            }
            for (std::uint32_t index : indices)
                writeValue(index);
            for (std::size_t i = 0; i < vertices->size(); ++i)
                writeValue(std::uint16_t{ 0 });
            ++mSurfaceCount;
            mVertexCount += vertices->size();
            mTriangleCount += indices.size() / 3;
        }

        osg::Matrixd currentActorMatrix() const
        {
            osg::NodePath fullPath = mActorPath;
            if (!fullPath.empty())
                fullPath.pop_back();
            const osg::NodePath& localPath = getNodePath();
            fullPath.insert(fullPath.end(), localPath.begin(), localPath.end());
            return osg::computeLocalToWorld(fullPath) * mWorldToActor;
        }

        template <class T>
        void writeValue(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            mOutput.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        void writeString(std::string_view value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("OpenNV actor skeleton string exceeds uint32 range");
            writeValue(static_cast<std::uint32_t>(value.size()));
            mOutput.write(value.data(), static_cast<std::streamsize>(value.size()));
        }

        void writeMatrix(const osg::Matrixf& matrix)
        {
            for (unsigned int row = 0; row < 4; ++row)
                for (unsigned int column = 0; column < 4; ++column)
                    writeValue(matrix(row, column));
        }

        static void appendTriangleIndices(const osg::Geometry& geometry, std::vector<std::uint32_t>& result)
        {
            const auto emit = [&](unsigned int a, unsigned int b, unsigned int c) {
                result.push_back(a);
                result.push_back(b);
                result.push_back(c);
            };
            for (unsigned int p = 0; p < geometry.getNumPrimitiveSets(); ++p)
            {
                const osg::PrimitiveSet* primitive = geometry.getPrimitiveSet(p);
                const unsigned int count = primitive == nullptr ? 0 : primitive->getNumIndices();
                if (primitive == nullptr || count < 3)
                    continue;
                if (primitive->getMode() == GL_TRIANGLES)
                {
                    for (unsigned int i = 0; i + 2 < count; i += 3)
                        emit(primitive->index(i), primitive->index(i + 1), primitive->index(i + 2));
                }
                else if (primitive->getMode() == GL_TRIANGLE_STRIP)
                {
                    for (unsigned int i = 0; i + 2 < count; ++i)
                    {
                        if ((i & 1u) == 0)
                            emit(primitive->index(i), primitive->index(i + 1), primitive->index(i + 2));
                        else
                            emit(primitive->index(i + 1), primitive->index(i), primitive->index(i + 2));
                    }
                }
            }
        }

        std::string texturePath(const osg::Drawable& drawable, const osg::Geometry& geometry) const
        {
            std::string result;
            const auto gather = [&](const osg::StateSet* state) {
                const auto* texture = state == nullptr ? nullptr
                    : dynamic_cast<const osg::Texture2D*>(
                        state->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
                const osg::Image* image = texture == nullptr ? nullptr : texture->getImage();
                if (image != nullptr && !image->getFileName().empty())
                    result = image->getFileName();
            };
            for (const osg::Node* node : getNodePath())
                if (node != nullptr)
                    gather(node->getStateSet());
            gather(geometry.getStateSet());
            gather(drawable.getStateSet());
            return result;
        }

        std::ostream& mOutput;
        osg::Matrixd mWorldToActor;
        osg::NodePath mActorPath;
        const std::vector<OpenNvCanonicalBone>& mCanonicalBones;
        std::streampos mSurfaceCountOffset;
        std::size_t mSurfaceCount = 0;
        std::size_t mVertexCount = 0;
        std::size_t mTriangleCount = 0;
    };
