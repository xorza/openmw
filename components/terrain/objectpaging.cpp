#include "objectpaging.hpp"

#include <limits>
#include <unordered_map>
#include <vector>

#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/Sequence>
#include <osg/Switch>
#include <osgAnimation/BasicAnimationManager>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystemUpdater>
#include <osgUtil/IncrementalCompileOperation>

#include <components/esm/util.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/nifosg/autotransform.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/util.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

namespace Terrain
{
    osg::ref_ptr<osg::Node> ObjectPaging::getChunk(float size, const osg::Vec2f& center, unsigned char /*lod*/,
        unsigned int lodFlags, bool activeGrid, const osg::Vec3f& viewPoint, bool compile)
    {
        if (activeGrid && !mActiveGrid)
            return nullptr;

        const ChunkId id = std::make_tuple(center, size, activeGrid);

        if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
            return static_cast<osg::Node*>(obj.get());

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());
        return node;
    }

    namespace
    {
        class CanOptimizeCallback : public SceneUtil::Optimizer::IsOperationPermissibleForObjectCallback
        {
        public:
            bool isOperationPermissibleForObjectImplementation(
                const SceneUtil::Optimizer* optimizer, const osg::Drawable* node, unsigned int option) const override
            {
                return true;
            }
            bool isOperationPermissibleForObjectImplementation(
                const SceneUtil::Optimizer* optimizer, const osg::Node* node, unsigned int option) const override
            {
                return (node->getDataVariance() != osg::Object::DYNAMIC);
            }
        };

        using LODRange = osg::LOD::MinMaxPair;

        LODRange intersection(const LODRange& left, const LODRange& right)
        {
            return { std::max(left.first, right.first), std::min(left.second, right.second) };
        }

        bool empty(const LODRange& r)
        {
            return r.first >= r.second;
        }

        LODRange operator/(const LODRange& r, float div)
        {
            return { r.first / div, r.second / div };
        }

        class CopyOp : public osg::CopyOp
        {
        public:
            bool mActiveGrid = false;
            LODRange mDistances = { 0.f, 0.f };
            osg::Node::NodeMask mCopyMask = ~0u;

            CopyOp(bool activeGrid, osg::Node::NodeMask copyMask)
                : mActiveGrid(activeGrid)
                , mCopyMask(copyMask)
            {
            }

            void copy(const osg::Node* toCopy, osg::Group* attachTo)
            {
                const osg::Group* groupToCopy = toCopy->asGroup();
                if (toCopy->getStateSet() || toCopy->asTransform() || !groupToCopy)
                    attachTo->addChild(operator()(toCopy));
                else
                {
                    for (unsigned int i = 0; i < groupToCopy->getNumChildren(); ++i)
                        attachTo->addChild(operator()(groupToCopy->getChild(i)));
                }
            }

            osg::Node* operator()(const osg::Node* node) const override
            {
                if (!(node->getNodeMask() & mCopyMask))
                    return nullptr;

                if (const osg::Drawable* d = node->asDrawable())
                    return operator()(d);

                if (dynamic_cast<const osgParticle::ParticleProcessor*>(node))
                    return nullptr;
                if (dynamic_cast<const osgParticle::ParticleSystemUpdater*>(node))
                    return nullptr;

                if (const osg::Switch* sw = node->asSwitch())
                {
                    osg::Group* n = new osg::Group;
                    for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                        if (sw->getValue(i))
                            n->addChild(operator()(sw->getChild(i)));
                    n->setDataVariance(osg::Object::STATIC);
                    return n;
                }
                if (const osg::LOD* lod = dynamic_cast<const osg::LOD*>(node))
                {
                    std::vector<std::pair<osg::ref_ptr<osg::Node>, LODRange>> children;
                    for (unsigned int i = 0; i < lod->getNumChildren(); ++i)
                        if (const auto r = intersection(lod->getRangeList()[i], mDistances); !empty(r))
                            children.emplace_back(operator()(lod->getChild(i)), lod->getRangeList()[i]);
                    if (children.empty())
                        return nullptr;

                    if (children.size() == 1)
                        return children.front().first.release();
                    else
                    {
                        osg::LOD* n = new osg::LOD;
                        for (const auto& [child, range] : children)
                            n->addChild(child, range.first, range.second);
                        n->setRangeMode(lod->getRangeMode());
                        n->setCenterMode(lod->getCenterMode());
                        n->setCenter(lod->getCenter());
                        n->setRadius(lod->getRadius());
                        n->setDataVariance(osg::Object::STATIC);
                        return n;
                    }
                }
                if (const osg::Sequence* sq = dynamic_cast<const osg::Sequence*>(node))
                {
                    osg::Group* n = new osg::Group;
                    n->addChild(operator()(sq->getChild(sq->getValue() != -1 ? sq->getValue() : 0)));
                    n->setDataVariance(osg::Object::STATIC);
                    return n;
                }

                if (!mActiveGrid)
                {
                    if (const auto* autoTransform = dynamic_cast<const NifOsg::AutoTransform*>(node))
                    {
                        osg::MatrixTransform* n = new osg::MatrixTransform();
                        n->setMatrix(autoTransform->computeMatrix(nullptr));

                        for (unsigned int i = 0; i < autoTransform->getNumChildren(); ++i)
                            if (osg::Node* clonedChild = operator()(autoTransform->getChild(i)))
                                n->addChild(clonedChild);

                        n->setDataVariance(osg::Object::STATIC);

                        handleCallbacks(node, n);

                        return n;
                    }
                }

                osg::Node* cloned = static_cast<osg::Node*>(node->clone(*this));
                if (!mActiveGrid)
                    cloned->setDataVariance(osg::Object::STATIC);
                cloned->setUserDataContainer(nullptr);
                cloned->setName("");

                handleCallbacks(node, cloned);

                return cloned;
            }

            void handleCallbacks(const osg::Node* node, osg::Node* cloned) const
            {
                for (const osg::Callback* callback = node->getCullCallback(); callback != nullptr;
                     callback = callback->getNestedCallback())
                {
                    if (node->getCullCallback()->getNestedCallback())
                    {
                        osg::Callback* clonedCallback = osg::clone(callback, osg::CopyOp::SHALLOW_COPY);
                        clonedCallback->setNestedCallback(nullptr);
                        cloned->addCullCallback(clonedCallback);
                    }
                    else
                        cloned->addCullCallback(const_cast<osg::Callback*>(callback));
                }
            }
            osg::Drawable* operator()(const osg::Drawable* drawable) const override
            {
                if (!(drawable->getNodeMask() & mCopyMask))
                    return nullptr;

                if (dynamic_cast<const osgParticle::ParticleSystem*>(drawable))
                    return nullptr;

                if (dynamic_cast<const SceneUtil::OsgaRigGeometry*>(drawable))
                    return nullptr;
                if (const SceneUtil::RigGeometry* rig = dynamic_cast<const SceneUtil::RigGeometry*>(drawable))
                    return operator()(rig->getSourceGeometry());
                if (const SceneUtil::MorphGeometry* morph = dynamic_cast<const SceneUtil::MorphGeometry*>(drawable))
                    return operator()(morph->getSourceGeometry());

                if (getCopyFlags() & DEEP_COPY_DRAWABLES)
                {
                    osg::Drawable* d = static_cast<osg::Drawable*>(drawable->clone(*this));
                    d->setDataVariance(osg::Object::STATIC);
                    d->setUserDataContainer(nullptr);
                    d->setName("");
                    return d;
                }
                else
                    return const_cast<osg::Drawable*>(drawable);
            }
            osg::Callback* operator()(const osg::Callback* callback) const override { return nullptr; }
        };

        class RefnumSet : public osg::Object
        {
        public:
            RefnumSet() {}
            RefnumSet(const RefnumSet& copy, const osg::CopyOp&)
                : mRefnums(copy.mRefnums)
            {
            }
            META_Object(Terrain, RefnumSet)
            std::vector<ESM::RefNum> mRefnums;
        };

        class AnalyzeVisitor : public osg::NodeVisitor
        {
        public:
            AnalyzeVisitor(osg::Node::NodeMask analyzeMask)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mCurrentStateSet(nullptr)
            {
                setTraversalMask(analyzeMask);
            }

            typedef std::unordered_map<osg::StateSet*, unsigned int> StateSetCounter;
            struct Result
            {
                StateSetCounter mStateSetCounter;
                unsigned int mNumVerts = 0;
            };

            void apply(osg::Node& node) override
            {
                if (node.getStateSet())
                    mCurrentStateSet = node.getStateSet();

                if (osg::Switch* sw = node.asSwitch())
                {
                    for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                        if (sw->getValue(i))
                            traverse(*sw->getChild(i));
                    return;
                }
                if (osg::LOD* lod = dynamic_cast<osg::LOD*>(&node))
                {
                    for (unsigned int i = 0; i < lod->getNumChildren(); ++i)
                        if (const auto r = intersection(lod->getRangeList()[i], mDistances); !empty(r))
                            traverse(*lod->getChild(i));
                    return;
                }
                if (osg::Sequence* sq = dynamic_cast<osg::Sequence*>(&node))
                {
                    traverse(*sq->getChild(sq->getValue() != -1 ? sq->getValue() : 0));
                    return;
                }

                traverse(node);
            }
            void apply(osg::Geometry& geom) override
            {
                if (osg::Array* array = geom.getVertexArray())
                    mResult.mNumVerts += array->getNumElements();

                ++mResult.mStateSetCounter[mCurrentStateSet];
                ++mGlobalStateSetCounter[mCurrentStateSet];
            }
            Result retrieveResult()
            {
                Result result = mResult;
                mResult = Result();
                mCurrentStateSet = nullptr;
                return result;
            }
            void addInstance(const Result& result)
            {
                for (auto pair : result.mStateSetCounter)
                    mGlobalStateSetCounter[pair.first] += pair.second;
            }
            float getMergeBenefit(const Result& result)
            {
                if (result.mStateSetCounter.empty())
                    return 1;
                float mergeBenefit = 0;
                for (auto pair : result.mStateSetCounter)
                {
                    mergeBenefit += mGlobalStateSetCounter[pair.first];
                }
                mergeBenefit /= result.mStateSetCounter.size();
                return mergeBenefit;
            }

            Result mResult;
            osg::StateSet* mCurrentStateSet;
            StateSetCounter mGlobalStateSetCounter;
            LODRange mDistances = { 0.f, 0.f };
        };

        class DebugVisitor : public osg::NodeVisitor
        {
        public:
            DebugVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }
            void apply(osg::Drawable& node) override
            {
                osg::ref_ptr<SceneUtil::Material> m(new SceneUtil::Material);
                osg::Vec4f color(
                    Misc::Rng::rollProbability(), Misc::Rng::rollProbability(), Misc::Rng::rollProbability(), 0.f);
                color.normalize();
                m->setDiffuse(osg::Vec4f(0.1f, 0.1f, 0.1f, 1.f));
                m->setAmbient(osg::Vec4f(0.1f, 0.1f, 0.1f, 1.f));
                m->setEmission(osg::Vec4f(color));
                m->setVertexColorMode(SceneUtil::VertexColorModes::None);
                osg::ref_ptr<osg::StateSet> stateset = node.getStateSet()
                    ? osg::clone(node.getStateSet(), osg::CopyOp::SHALLOW_COPY)
                    : new osg::StateSet;
                stateset->setAttribute(m);
                m->updateStateSet(stateset);
                node.setStateSet(stateset);
            }
        };

        class AddRefnumMarkerVisitor : public osg::NodeVisitor
        {
        public:
            AddRefnumMarkerVisitor(ESM::RefNum refnum)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mRefnum(refnum)
            {
            }
            ESM::RefNum mRefnum;
            void apply(osg::Geometry& node) override
            {
                osg::ref_ptr<RefnumMarker> marker(new RefnumMarker);
                marker->mRefnum = mRefnum;
                if (osg::Array* array = node.getVertexArray())
                    marker->mNumVertices = array->getNumElements();
                node.getOrCreateUserDataContainer()->addUserObject(marker);
            }
        };
    }

    ObjectPaging::ObjectPaging(Resource::SceneManager* sceneManager, const ObjectStorage& storage,
        ESM::RefId worldspace, unsigned int nodeMask, bool pageActiveGrid)
        : GenericResourceManager<ChunkId>(nullptr, Settings::cells().mCacheExpiryDelay)
        , QuadTreeWorld::ChunkManager(worldspace)
        , mSceneManager(sceneManager)
        , mStorage(&storage)
        , mNodeMask(nodeMask)
        , mActiveGrid(pageActiveGrid)
        , mDebugBatches(Settings::terrain().mDebugChunks)
        , mMergeFactor(Settings::terrain().mObjectPagingMergeFactor)
        , mMinSize(Settings::terrain().mObjectPagingMinSize)
        , mMinSizeMergeFactor(Settings::terrain().mObjectPagingMinSizeMergeFactor)
        , mMinSizeCostMultiplier(Settings::terrain().mObjectPagingMinSizeCostMultiplier)
        , mRefTrackerLocked(false)
    {
    }

    osg::ref_ptr<osg::Node> ObjectPaging::createChunk(float size, const osg::Vec2f& center, bool activeGrid,
        const osg::Vec3f& viewPoint, bool compile, unsigned char lod)
    {
        const osg::Vec2i startCell(static_cast<int>(std::floor(center.x() - size / 2.f)),
            static_cast<int>(std::floor(center.y() - size / 2.f)));
        std::map<ESM::RefNum, PagedCellRef> refs;
        mStorage->collectReferences(size, startCell, mWorldspace, refs);

        if (activeGrid && !refs.empty())
        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            const std::set<ESM::RefNum>& blacklist = getRefTracker().mBlacklist;
            if (blacklist.size() < refs.size())
            {
                for (ESM::RefNum ref : blacklist)
                    refs.erase(ref);
            }
            else
            {
                std::erase_if(refs, [&](const auto& ref) { return blacklist.contains(ref.first); });
            }
        }

        const osg::Vec2f minBound = (center - osg::Vec2f(size / 2.f, size / 2.f));
        const osg::Vec2f maxBound = (center + osg::Vec2f(size / 2.f, size / 2.f));
        const osg::Vec2i floorMinBound(
            static_cast<int>(std::floor(minBound.x())), static_cast<int>(std::floor(minBound.y())));
        const osg::Vec2i ceilMaxBound(
            static_cast<int>(std::ceil(maxBound.x())), static_cast<int>(std::ceil(maxBound.y())));
        struct InstanceList
        {
            osg::ref_ptr<const osg::Node> mTemplate;
            std::vector<const PagedCellRef*> mInstances;
            AnalyzeVisitor::Result mAnalyzeResult;
            bool mNeedCompile = false;
        };

        // **In the order the references named them, and not in the order the allocator handed them
        // out.** These were a `std::map` keyed on the template's `ref_ptr`, which orders by address:
        // which geometries ended up adjacent in a merged drawable followed where the templates
        // happened to be in memory, and so did how many degenerate triangles the merge left behind.
        // The chunk was the same chunk either way, but the count of its triangles was a property of
        // the binary rather than of the content — one unused `#include` elsewhere moved Balmora by
        // two — and that is the number the ray tracer's harness prints as a control.
        //
        // `refs` is a `std::map<ESM::RefNum, ...>`, so first-encounter order is already the content's
        // own. The map below is the dedup index and nothing else.
        std::vector<InstanceList> nodes;
        std::unordered_map<const osg::Node*, std::size_t> byTemplate;
        const osg::ref_ptr<RefnumSet> refnumSet = activeGrid ? new RefnumSet : nullptr;

        // **What the loader hid, and asked from the loader.** A NIF loader marks two kinds of node
        // as not-to-be-drawn — collision shapes, and anything a `VisController` may hide at runtime
        // — with a mask its owner chose. This paging runs no such controller, so both are simply
        // left out of the copy, and asking the loader is what keeps a second world from choosing a
        // different bit and copying a town's collision meshes into its distant hills.
        const auto copyMask = ~NifOsg::Loader::getHiddenNodeMask();

        const int cellSize = getCellSize(mWorldspace);
        const float smallestDistanceToChunk = (size > 1 / 8.f) ? (size * cellSize) : 0.f;
        const float higherDistanceToChunk
            = activeGrid ? ((size < 1) ? 5 : 3) * cellSize * size + 1 : smallestDistanceToChunk + 1;
        const LODRange lodDistances = activeGrid ? LODRange{ 0.f, std::numeric_limits<float>::max() }
                                                 : LODRange{ smallestDistanceToChunk, higherDistanceToChunk };

        AnalyzeVisitor analyzeVisitor(copyMask);
        const float minSize = mMinSizeMergeFactor ? mMinSize * mMinSizeMergeFactor : mMinSize;
        for (const auto& [refNum, ref] : refs)
        {
            if (size < 1.f)
            {
                const osg::Vec3f cellPos = ref.mPosition / static_cast<float>(cellSize);
                if ((minBound.x() > floorMinBound.x() && cellPos.x() < minBound.x())
                    || (minBound.y() > floorMinBound.y() && cellPos.y() < minBound.y())
                    || (maxBound.x() < ceilMaxBound.x() && cellPos.x() >= maxBound.x())
                    || (maxBound.y() < ceilMaxBound.y() && cellPos.y() >= maxBound.y()))
                    continue;
            }

            const float dSqr = (viewPoint - ref.mPosition).length2();
            if (!activeGrid)
            {
                std::lock_guard<std::mutex> lock(mSizeCacheMutex);
                SizeCache::iterator found = mSizeCache.find(refNum);
                if (found != mSizeCache.end() && found->second < dSqr * minSize * minSize)
                    continue;
            }

            if (Misc::ResourceHelpers::isHiddenMarker(ref.mRefId))
                continue;

            const int type = ref.mType;
            VFS::Path::Normalized model = mStorage->getModel(type, ref.mRefId);
            if (model.empty())
                continue;
            model = Misc::ResourceHelpers::correctMeshPath(model);

            if (activeGrid && type != ESM::REC_STAT && type != ESM::REC_STAT4)
            {
                model = Misc::ResourceHelpers::correctActorModelPath(model, mSceneManager->getVFS());
                constexpr VFS::Path::ExtensionView nif("nif");
                if (model.extension() == nif)
                {
                    VFS::Path::Normalized kfname = model;
                    constexpr VFS::Path::ExtensionView kf("kf");
                    kfname.changeExtension(kf);
                    if (mSceneManager->getVFS()->exists(kfname))
                        continue;
                }
            }

            if (!activeGrid)
            {
                std::lock_guard<std::mutex> lock(mLODNameCacheMutex);
                LODNameCacheKey key{ model, lod };
                LODNameCache::const_iterator found = mLODNameCache.lower_bound(key);
                if (found != mLODNameCache.end() && found->first == key)
                    model = found->second;
                else
                    model = mLODNameCache
                                .emplace_hint(found, std::move(key),
                                    Misc::ResourceHelpers::getLODMeshName(mStorage->getEsmVersion(refNum.mContentFile),
                                        model, *mSceneManager->getVFS(), lod))
                                ->second;
            }

            osg::ref_ptr<const osg::Node> cnode = mSceneManager->getTemplate(model, false);

            if (activeGrid)
            {
                if (cnode->getNumChildrenRequiringUpdateTraversal() > 0
                    || SceneUtil::hasUserDescription(cnode, Constants::NightDayLabel)
                    || SceneUtil::hasUserDescription(cnode, Constants::HerbalismLabel)
                    || (cnode->getName() == "Collada visual scene group"
                        && dynamic_cast<const osgAnimation::BasicAnimationManager*>(cnode->getUpdateCallback())))
                    continue;
                else
                    refnumSet->mRefnums.push_back(refNum);
            }

            {
                std::lock_guard<std::mutex> lock(mRefTrackerMutex);
                if (getRefTracker().mDisabled.count(refNum))
                    continue;
            }

            const float radius2 = cnode->getBound().radius2() * ref.mScale * ref.mScale;
            if (radius2 < dSqr * minSize * minSize && !activeGrid)
            {
                std::lock_guard<std::mutex> lock(mSizeCacheMutex);
                mSizeCache[refNum] = radius2;
                continue;
            }

            const osg::Node* const nodePtr = cnode.get();
            const auto emplaced = byTemplate.emplace(nodePtr, nodes.size());
            if (emplaced.second)
            {
                // Moved rather than copied, because `mNeedCompile` below counts the references to
                // this template and a second one held here would be one too many.
                nodes.push_back(InstanceList{ .mTemplate = std::move(cnode) });

                analyzeVisitor.mDistances = lodDistances / ref.mScale;
                // const-trickery required because there is no const version of NodeVisitor
                const_cast<osg::Node*>(nodePtr)->accept(analyzeVisitor);
                nodes.back().mAnalyzeResult = analyzeVisitor.retrieveResult();
                nodes.back().mNeedCompile = compile && nodePtr->referenceCount() <= 2;
            }
            else
                analyzeVisitor.addInstance(nodes[emplaced.first->second].mAnalyzeResult);
            nodes[emplaced.first->second].mInstances.push_back(&ref);
        }

        const osg::Vec3f worldCenter
            = osg::Vec3f(center.x(), center.y(), 0) * static_cast<float>(getCellSize(mWorldspace));
        osg::ref_ptr<osg::Group> group = new osg::Group;
        osg::ref_ptr<osg::Group> mergeGroup = new osg::Group;
        osg::ref_ptr<Resource::TemplateMultiRef> templateRefs = new Resource::TemplateMultiRef;
        osgUtil::StateToCompile stateToCompile(0, nullptr);
        CopyOp copyop(activeGrid, copyMask);
        for (const InstanceList& entry : nodes)
        {
            const osg::Node* cnode = entry.mTemplate.get();

            const AnalyzeVisitor::Result& analyzeResult = entry.mAnalyzeResult;

            const float mergeCost = analyzeResult.mNumVerts * size;
            const float mergeBenefit = analyzeVisitor.getMergeBenefit(analyzeResult) * mMergeFactor;
            const bool merge = mergeBenefit > mergeCost;

            const float factor2
                = mergeBenefit > 0 ? std::min(1.f, mergeCost * mMinSizeCostMultiplier / mergeBenefit) : 1;
            const float minSizeMergeFactor2 = (1 - factor2) * mMinSizeMergeFactor + factor2;
            const float minSizeMerged = minSizeMergeFactor2 > 0 ? mMinSize * minSizeMergeFactor2 : mMinSize;

            unsigned int numinstances = 0;
            for (const PagedCellRef* refPtr : entry.mInstances)
            {
                const PagedCellRef& ref = *refPtr;

                if (!activeGrid && minSizeMerged != minSize
                    && cnode->getBound().radius2() * ref.mScale * ref.mScale
                        < (viewPoint - ref.mPosition).length2() * minSizeMerged * minSizeMerged)
                    continue;

                const osg::Vec3f nodePos = ref.mPosition - worldCenter;
                const osg::Quat nodeAttitude = osg::Quat(ref.mRotation.z(), osg::Vec3f(0, 0, -1))
                    * osg::Quat(ref.mRotation.y(), osg::Vec3f(0, -1, 0))
                    * osg::Quat(ref.mRotation.x(), osg::Vec3f(-1, 0, 0));
                const osg::Vec3f nodeScale(ref.mScale, ref.mScale, ref.mScale);

                osg::ref_ptr<osg::Group> trans;
                if (merge)
                {
                    // Optimizer currently supports only MatrixTransforms.
                    osg::Matrixf matrix;
                    matrix.preMultTranslate(nodePos);
                    matrix.preMultRotate(nodeAttitude);
                    matrix.preMultScale(nodeScale);
                    trans = new osg::MatrixTransform(matrix);
                    trans->setDataVariance(osg::Object::STATIC);
                }
                else
                {
                    trans = new SceneUtil::PositionAttitudeTransform;
                    SceneUtil::PositionAttitudeTransform* pat
                        = static_cast<SceneUtil::PositionAttitudeTransform*>(trans.get());
                    pat->setPosition(nodePos);
                    pat->setScale(nodeScale);
                    pat->setAttitude(nodeAttitude);
                }

                // DO NOT COPY AND PASTE THIS CODE. Cloning osg::Geometry without also cloning its contained Arrays is
                // generally unsafe. In this specific case the operation is safe under the following two assumptions:
                // - When Arrays are removed or replaced in the cloned geometry, the original Arrays in their place must
                // outlive the cloned geometry regardless. (ensured by TemplateMultiRef)
                // - Arrays that we add or replace in the cloned geometry must be explicitely forbidden from reusing
                // BufferObjects of the original geometry. (ensured by needvbo() in optimizer.cpp)
                copyop.setCopyFlags(merge ? osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES
                                          : osg::CopyOp::DEEP_COPY_NODES);
                copyop.mDistances = lodDistances / ref.mScale;
                copyop.copy(cnode, trans);

                if (activeGrid)
                {
                    if (merge)
                    {
                        AddRefnumMarkerVisitor visitor(ref.mRefNum);
                        trans->accept(visitor);
                    }
                    else
                    {
                        osg::ref_ptr<RefnumMarker> marker = new RefnumMarker;
                        marker->mRefnum = ref.mRefNum;
                        trans->getOrCreateUserDataContainer()->addUserObject(marker);
                    }
                }

                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);
                ++numinstances;
            }
            if (numinstances > 0)
            {
                // add a ref to the original template to help verify the safety of shallow cloning operations
                // in addition, we hint to the cache that it's still being used and should be kept in cache
                templateRefs->addRef(cnode);

                if (entry.mNeedCompile)
                {
                    int mode = osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES;
                    if (!merge)
                        mode |= osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                    stateToCompile._mode = mode;
                    const_cast<osg::Node*>(cnode)->accept(stateToCompile);
                }
            }
        }

        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;

        if (mergeGroup->getNumChildren())
        {
            SceneUtil::Optimizer optimizer;
            if (size > 1 / 8.f)
            {
                optimizer.setViewPoint(relativeViewPoint);
                optimizer.setMergeAlphaBlending(true);
            }
            optimizer.setIsOperationPermissibleForObjectCallback(new CanOptimizeCallback);
            const unsigned int options = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
                | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;

            optimizer.optimize(mergeGroup, options);

            group->addChild(mergeGroup);

            if (mDebugBatches)
            {
                DebugVisitor dv;
                mergeGroup->accept(dv);
            }
            if (compile)
            {
                stateToCompile._mode = osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                mergeGroup->accept(stateToCompile);
            }
        }

        osgUtil::IncrementalCompileOperation* const ico = mSceneManager->getIncrementalCompileOperation();
        if (!stateToCompile.empty() && ico)
        {
            auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);
            compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
            ico->add(compileSet, false);
        }

        group->getBound();
        group->setNodeMask(mNodeMask);
        osg::UserDataContainer* udc = group->getOrCreateUserDataContainer();
        if (activeGrid)
        {
            std::sort(refnumSet->mRefnums.begin(), refnumSet->mRefnums.end());
            refnumSet->mRefnums.erase(
                std::unique(refnumSet->mRefnums.begin(), refnumSet->mRefnums.end()), refnumSet->mRefnums.end());
            udc->addUserObject(refnumSet);
            group->addCullCallback(new SceneUtil::LightListCallback);
        }
        udc->addUserObject(templateRefs);

        return group;
    }

    unsigned int ObjectPaging::getNodeMask()
    {
        return mNodeMask;
    }

    namespace
    {
        osg::Vec2f clampToCell(const osg::Vec3f& cellPos, const osg::Vec2i& cell)
        {
            return osg::Vec2f(std::clamp(cellPos.x(), static_cast<float>(cell.x()), cell.x() + 1.f),
                std::clamp(cellPos.y(), static_cast<float>(cell.y()), cell.y() + 1.f));
        }

        class CollectIntersecting
        {
        public:
            explicit CollectIntersecting(
                bool activeGridOnly, const osg::Vec3f& position, const osg::Vec2i& cell, ESM::RefId worldspace)
                : mActiveGridOnly(activeGridOnly)
                , mPosition(clampToCell(position / static_cast<float>(getCellSize(worldspace)), cell))
            {
            }

            void operator()(const ChunkId& id, osg::Object* /*obj*/)
            {
                if (mActiveGridOnly && !std::get<2>(id))
                    return;
                if (intersects(id))
                    mCollected.push_back(id);
            }

            const std::vector<ChunkId>& getCollected() const { return mCollected; }

        private:
            bool intersects(ChunkId id) const
            {
                const osg::Vec2f center = std::get<0>(id);
                const float halfSize = std::get<1>(id) / 2;
                return mPosition.x() >= center.x() - halfSize && mPosition.y() >= center.y() - halfSize
                    && mPosition.x() <= center.x() + halfSize && mPosition.y() <= center.y() + halfSize;
            }

            bool mActiveGridOnly;
            osg::Vec2f mPosition;
            std::vector<ChunkId> mCollected;
        };
    }

    bool ObjectPaging::enableObject(
        int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell, bool enabled)
    {
        if (!pagedType(type, false))
            return false;

        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            if (enabled && !getWritableRefTracker().mDisabled.erase(refnum))
                return false;
            if (!enabled && !getWritableRefTracker().mDisabled.insert(refnum).second)
                return false;
            if (mRefTrackerLocked)
                return false;
        }

        CollectIntersecting ccf(false, pos, cell, mWorldspace);
        mCache->call(ccf);
        if (ccf.getCollected().empty())
            return false;
        for (const ChunkId& chunk : ccf.getCollected())
            mCache->removeFromObjectCache(chunk);
        return true;
    }

    bool ObjectPaging::blacklistObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell)
    {
        if (!pagedType(type, false))
            return false;

        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            if (!getWritableRefTracker().mBlacklist.insert(refnum).second)
                return false;
            if (mRefTrackerLocked)
                return false;
        }

        CollectIntersecting ccf(true, pos, cell, mWorldspace);
        mCache->call(ccf);
        if (ccf.getCollected().empty())
            return false;
        for (const ChunkId& chunk : ccf.getCollected())
            mCache->removeFromObjectCache(chunk);
        return true;
    }

    void ObjectPaging::clear()
    {
        std::lock_guard<std::mutex> lock(mRefTrackerMutex);
        mRefTrackerNew.mDisabled.clear();
        mRefTrackerNew.mBlacklist.clear();
        mRefTrackerLocked = true;
    }

    bool ObjectPaging::unlockCache()
    {
        if (!mRefTrackerLocked)
            return false;
        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            mRefTrackerLocked = false;
            if (mRefTracker == mRefTrackerNew)
                return false;
            else
                mRefTracker = mRefTrackerNew;
        }
        mCache->clear();
        return true;
    }

    namespace
    {
        struct GetRefnumsFunctor
        {
            GetRefnumsFunctor(std::vector<ESM::RefNum>& output)
                : mOutput(output)
            {
            }
            void operator()(ChunkId chunkId, osg::Object* obj)
            {
                if (!std::get<2>(chunkId))
                    return;
                const osg::Vec2f& center = std::get<0>(chunkId);
                const bool activeGrid = (center.x() > mActiveGrid.x() || center.y() > mActiveGrid.y()
                    || center.x() < mActiveGrid.z() || center.y() < mActiveGrid.w());
                if (!activeGrid)
                    return;

                osg::UserDataContainer* udc = obj->getUserDataContainer();
                if (udc && udc->getNumUserObjects())
                {
                    RefnumSet* refnums = dynamic_cast<RefnumSet*>(udc->getUserObject(0));
                    if (!refnums)
                        return;
                    mOutput.insert(mOutput.end(), refnums->mRefnums.begin(), refnums->mRefnums.end());
                }
            }
            osg::Vec4i mActiveGrid;
            std::vector<ESM::RefNum>& mOutput;
        };
    }

    void ObjectPaging::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)
    {
        GetRefnumsFunctor grf(out);
        grf.mActiveGrid = activeGrid;
        mCache->call(grf);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void ObjectPaging::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Object Chunk", frameNumber, mCache->getStats(), *stats);
    }

}
