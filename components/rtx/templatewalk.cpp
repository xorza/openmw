#include "templatewalk.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <osg/Drawable>
#include <osg/Matrixf>
#include <osg/Sequence>
#include <osg/StateSet>
#include <osg/Transform>

#include "materialresolver.hpp"
#include "meshtable.hpp"
#include "prepared.hpp"
#include "runs.hpp"
#include "worlddescent.hpp"

namespace Rtx
{
    namespace
    {
        /// Appends `values` to `into` and answers the run they landed in. One statement, because a
        /// run that named where it started and a count taken from another array is exactly what
        /// `Rtx::Run` exists to stop.
        template <class T>
        Run appended(std::vector<T>& into, std::span<const T> values)
        {
            const Run run{ .mOffset = static_cast<std::uint32_t>(into.size()),
                .mCount = static_cast<std::uint32_t>(values.size()) };
            into.insert(into.end(), values.begin(), values.end());

            return run;
        }
    }

    TemplateWalk::TemplateWalk()
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
    {
    }

    Result<void, std::string> TemplateWalk::read(
        const osg::Node& root, const osg::Node::NodeMask mask, PreparedModel& into)
    {
        mInto = &into;
        mHere = osg::Matrix();
        mShading.clear();
        mRefused.clear();
        setTraversalMask(mask);

        // OSG's visitor API is non-const throughout, and this walk writes nothing: the cast happens
        // once, here.
        const_cast<osg::Node&>(root).accept(*this);

        mInto = nullptr;

        if (!mRefused.empty())
            return Err{ mRefused };

        return {};
    }

    void TemplateWalk::pushShading(const osg::StateSet& stateSet)
    {
        const float above = mShading.empty() ? 1.0f : mShading.back().mFade;
        mShading.push_back(Shading{ .mStateSet = &stateSet, .mFade = fadeThrough(stateSet, above) });
    }

    void TemplateWalk::apply(osg::Node& node)
    {
        const std::size_t held = mShading.size();

        if (const osg::StateSet* own = node.getStateSet())
            pushShading(*own);

        descend(node);

        mShading.resize(held);
    }

    void TemplateWalk::apply(osg::Transform& node)
    {
        // The visitor goes with it, as the frame's walk hands itself over. A visitor that is
        // not a cull visitor takes the branch a null one would in every transform this tree has —
        // an `AutoTransform` included, which the frame's walk turns toward its eye and this one
        // leaves at its base rotation: no vanilla static carries a billboard, so the ring has no
        // eye to turn one to.
        const osg::Matrix above = mHere;
        node.computeLocalToWorldMatrix(mHere, this);

        apply(static_cast<osg::Node&>(node));

        mHere = above;
    }

    void TemplateWalk::apply(osg::Drawable& drawable)
    {
        const std::size_t held = mShading.size();

        if (const osg::StateSet* own = drawable.getStateSet())
            pushShading(*own);

        take(drawable);

        mShading.resize(held);
    }

    void TemplateWalk::take(const osg::Drawable& drawable)
    {
        // A model is refused whole, so what follows the first refusal is not read.
        if (!mRefused.empty())
            return;

        const DrawableRead read = readDrawable(drawable, mKinds.of(drawable));
        if (read.mGeometry == nullptr)
            return;

        MeshReading reading;
        const Result<bool, std::string> readMesh = mMeshes.read(read, reading);
        if (!readMesh.isOk())
        {
            mRefused = readMesh.error();
            return;
        }
        if (!readMesh.value())
            return;

        // Here on the reader's thread, where the model is refused whole, and not at the adoption,
        // which is inside the frame's walk.
        if (const Result<void, std::string> fits = MeshTable::checkFits(reading.mArrays); !fits.isOk())
        {
            mRefused = fits.error();
            return;
        }

        PreparedModel& into = *mInto;

        PreparedPart part;
        part.mDrawable = &drawable;
        part.mMaterial = MaterialResolver::read(mShading, mAlpha, mMeans);
        part.mLocal = osg::Matrixf(mHere);
        part.mShape = reading.mShape;

        part.mVertices = appended(into.mPositions, reading.mArrays.mPositions);
        part.mNormals = appended(into.mNormals, reading.mArrays.mNormals);
        part.mTexCoords = appended(into.mTexCoords, reading.mArrays.mTexCoords);
        part.mSecondTexCoords = appended(into.mSecondTexCoords, reading.mArrays.mSecondTexCoords);
        part.mUnitStreams = reading.mArrays.mUnitStreams;
        part.mColours = appended(into.mColours, reading.mArrays.mColours);
        part.mTangents = appended(into.mTangents, reading.mArrays.mTangents);
        part.mIndices = appended(into.mIndices, reading.mArrays.mIndices);

        into.mParts.push_back(std::move(part));
    }

    /// The frame the sequence stands on, and no step. The frame's walk runs a flipbook's clock
    /// and then walks the frame it settled on; a template's clock is nobody's to run, so what a
    /// distant fire shows is the frame its file was authored to open on, which is what the
    /// rasterizer's paging shows of it too.
    void TemplateWalk::descend(osg::Node& node)
    {
        descendInWorld(
            node, mKinds.of(node), *this, [](osg::Sequence&) {}, [](unsigned int) {});
    }
}
