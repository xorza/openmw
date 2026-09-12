#include "templatewalk.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <osg/Drawable>
#include <osg/Sequence>
#include <osg/StateSet>
#include <osg/Transform>

#include "materialresolver.hpp"
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

    void TemplateWalk::read(const osg::Node& root, const osg::Node::NodeMask mask, PreparedModel& into)
    {
        mInto = &into;
        mHere = osg::Matrix();
        mShading.clear();
        setTraversalMask(mask);

        // OSG's visitor API is non-const throughout, and this walk writes nothing: the cast happens
        // once, here.
        const_cast<osg::Node&>(root).accept(*this);

        mInto = nullptr;
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
        // an `AutoTransform` included — so a billboard reads the same here as under the frame.
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
        const DrawableRead read = readDrawable(drawable, mKinds.of(drawable));
        if (read.mGeometry == nullptr)
            return;

        MeshReading reading;
        if (!mMeshes.read(read, reading))
            return;

        PreparedModel& into = *mInto;

        PreparedPart part;
        part.mDrawable = &drawable;
        part.mMaterial = MaterialResolver::read(mShading, mAlpha);
        part.mLocal = osg::Matrixf(mHere);
        part.mShape = reading.mShape;

        part.mVertices = appended(into.mPositions, reading.mArrays.mPositions);
        part.mNormals = appended(into.mNormals, reading.mArrays.mNormals);
        part.mTexCoords = appended(into.mTexCoords, reading.mArrays.mTexCoords);
        part.mColours = appended(into.mColours, reading.mArrays.mColours);
        part.mIndices = appended(into.mIndices, reading.mArrays.mIndices);

        into.mParts.push_back(std::move(part));
    }

    /// The frame the sequence stands on, and no step. The frame's walk runs a flipbook's clock
    /// and then walks the frame it settled on; a template's clock is nobody's to run, so what a
    /// distant fire shows is the frame its file was authored to open on, which is what the
    /// rasterizer's paging shows of it too.
    void TemplateWalk::descend(osg::Node& node)
    {
        descendInWorld(node, mKinds.of(node), *this, [](osg::Sequence&) {});
    }
}
