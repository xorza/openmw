#include "nightsky.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/Texture2D>
#include <osg/TriangleIndexFunctor>

#include <components/debug/debuglog.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

namespace Rtx
{
    namespace
    {
        /// What one drawable of the night mesh came to, before it is sorted into a field or a patch.
        struct Layer
        {
            const osg::Image* mImage = nullptr;

            /// The mean of the directions its vertices point, and the angle to the furthest of them.
            osg::Vec3f mDirection{ 0.0f, 0.0f, 1.0f };
            float mAngularRadius = 0.0f;

            /// How far its texture coordinates run, along each axis separately.
            ///
            /// **What tells a field from a patch is that a field repeats in *both* directions.** A
            /// sheet laid once across a piece of sky stays inside about one tile in at least one of
            /// them — Morrowind's constellations overshoot to one and a half across their long axis
            /// and half a tile across their short one — where the field runs four tiles by two.
            /// Measuring the two together, or against the origin, puts a constellation over the
            /// whole sky.
            osg::Vec2f mUvSpan;

            /// Texture units of sheet per radian of sky, taken over every edge and reduced to the
            /// median so one degenerate triangle cannot speak for the mesh.
            float mUvRate = 0.0f;

            /// The lowest elevation the engine still draws, in radians — see `NightSky::mHorizon`.
            float mKeptFrom = 0.0f;
        };

        /// How near the zenith a vertex stops having a bearing at all.
        constexpr float sPoleCosine = 0.9995f;

        /// Where a direction points, as the two angles an unwrap is written in.
        struct Bearing
        {
            float mAzimuth = 0.0f;
            float mElevation = 0.0f;

            /// Whether the azimuth means anything here. It does not at the pole, where every bearing
            /// meets, and an edge that ends there would divide by an angle nobody chose.
            bool mDefined = false;
        };

        /// How much sheet one radian is worth, along each edge of a drawable's triangles.
        ///
        /// **Against the two angles the unwrap is written in, and not against the arc between them**,
        /// which is the difference between measuring what the mesh does and measuring something
        /// else. A cylindrical unwrap advances its texture evenly in azimuth and in elevation; the
        /// *arc* the same step of azimuth covers shrinks as `cos(elevation)`, so dividing by it reads
        /// the rate as steeper the higher up the dome it is asked, and the answer comes out a fifth
        /// too large. What the shader consumes is the angles, so what this measures is the angles.
        ///
        /// **And along one triangle's edges rather than every pair of its vertices**, because the
        /// answer is a local one: two vertices on opposite sides of a dome are a wrap apart in
        /// azimuth and say nothing about a rate. `osg::TriangleIndexFunctor` walks whichever
        /// primitive set the file happened to use, which is what keeps this from caring.
        struct EdgeRates
        {
            const osg::Vec2Array* mCoords = nullptr;
            const std::vector<Bearing>* mBearings = nullptr;
            std::vector<float> mRates;

            void operator()(unsigned int a, unsigned int b, unsigned int c)
            {
                take(a, b);
                take(b, c);
                take(c, a);
            }

            void take(unsigned int i, unsigned int j)
            {
                if (i >= mBearings->size() || j >= mBearings->size())
                    return;

                const Bearing& from = (*mBearings)[i];
                const Bearing& to = (*mBearings)[j];
                if (!from.mDefined || !to.mDefined)
                    return;

                // Round the short way, so the seam a dome closes on is one step and not a whole turn.
                float turned = to.mAzimuth - from.mAzimuth;
                while (turned > osg::PIf)
                    turned -= 2.0f * osg::PIf;
                while (turned < -osg::PIf)
                    turned += 2.0f * osg::PIf;

                const float apart = osg::Vec2f(turned, to.mElevation - from.mElevation).length();
                const float across = ((*mCoords)[i] - (*mCoords)[j]).length();

                // A degenerate edge says nothing about a rate and would divide by nearly nothing.
                if (apart > 1.0e-3f && across > 1.0e-4f)
                    mRates.push_back(across / apart);
            }
        };

        /// Reads every drawable of a loaded sky mesh.
        class LayerReader : public osg::NodeVisitor
        {
        public:
            LayerReader()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geometry& geometry) override
            {
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                const auto* coords = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
                if (vertices == nullptr || coords == nullptr || vertices->size() != coords->size() || vertices->empty())
                    return;

                Layer layer;
                layer.mImage = imageOf(geometry);
                if (layer.mImage == nullptr)
                    return;

                std::vector<osg::Vec3f> directions;
                directions.reserve(vertices->size());
                for (const osg::Vec3f& vertex : *vertices)
                {
                    osg::Vec3f towards = vertex;
                    if (towards.length2() <= 0.0f)
                        return;

                    towards.normalize();
                    directions.push_back(towards);
                    layer.mDirection += towards;
                }

                if (layer.mDirection.length2() <= 0.0f)
                    return;

                layer.mDirection.normalize();
                for (const osg::Vec3f& towards : directions)
                    layer.mAngularRadius = std::max(
                        layer.mAngularRadius, std::acos(std::clamp(towards * layer.mDirection, -1.0f, 1.0f)));

                // Seeded from the first coordinate rather than from nothing, and kept per axis: a
                // sheet whose coordinates all sit above one would otherwise measure its distance
                // from the origin, and one that runs far along a single axis is still a sheet laid
                // once.
                osg::Vec2f lowest = (*coords)[0];
                osg::Vec2f highest = (*coords)[0];
                for (const osg::Vec2f& coord : *coords)
                {
                    lowest.x() = std::min(lowest.x(), coord.x());
                    lowest.y() = std::min(lowest.y(), coord.y());
                    highest.x() = std::max(highest.x(), coord.x());
                    highest.y() = std::max(highest.y(), coord.y());
                }

                layer.mUvSpan = highest - lowest;

                // **The rate the unwrap runs at, along the mesh's own edges.** A span over an extent
                // would be thrown by the seam vertices a dome carries, which hold a duplicated
                // coordinate at the same place in the sky; and any two vertices at all would be
                // worse, because texture per radian is a *local* quantity — a pair across the dome
                // has a coordinate distance that is not its angle times anything. An edge is the
                // shortest baseline the mesh offers, which is the one the rate is defined on.
                std::vector<Bearing> bearings;
                bearings.reserve(directions.size());
                for (const osg::Vec3f& towards : directions)
                    bearings.push_back(Bearing{ .mAzimuth = std::atan2(towards.y(), towards.x()),
                        .mElevation = std::asin(std::clamp(towards.z(), -1.0f, 1.0f)),
                        .mDefined = std::abs(towards.z()) < sPoleCosine });

                osg::TriangleIndexFunctor<EdgeRates> edges;
                edges.mCoords = coords;
                edges.mBearings = &bearings;
                geometry.accept(edges);

                if (!edges.mRates.empty())
                {
                    // The median, because a dome closed with a cap the modeller unwrapped by hand has
                    // a handful of edges that agree with nothing; a mean would carry them.
                    const std::size_t middle = edges.mRates.size() / 2;
                    std::nth_element(edges.mRates.begin(), edges.mRates.begin() + middle, edges.mRates.end());
                    layer.mUvRate = edges.mRates[middle];
                }

                layer.mKeptFrom = keptFrom(geometry, directions);
                mLayers.push_back(layer);
            }

            std::vector<Layer> mLayers;

        private:
            /// The sheet on a drawable's first texture unit, wherever it is bound.
            static const osg::Image* imageOf(const osg::Geometry& geometry)
            {
                for (const osg::StateSet* state : { geometry.getStateSet(), stateOfParents(geometry) })
                {
                    if (state == nullptr)
                        continue;

                    const auto* texture = dynamic_cast<const osg::Texture2D*>(
                        state->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
                    if (texture != nullptr && texture->getImage() != nullptr
                        && !texture->getImage()->getFileName().empty())
                        return texture->getImage();
                }

                return nullptr;
            }

            static const osg::StateSet* stateOfParents(const osg::Geometry& geometry)
            {
                for (const osg::Node* parent : geometry.getParents())
                    if (parent->getStateSet() != nullptr)
                        return parent->getStateSet();

                return nullptr;
            }

            /// **`MWRender::ModVertexAlphaVisitor::Stars`'s rule, read rather than reimplemented
            /// twice**: the engine draws a vertex of the star dome only where its authored colour is
            /// exactly white, and its bottom ring alone is not — so what it keeps begins at the ring
            /// above the horizon. Nothing authored means the whole of it is kept.
            static float keptFrom(const osg::Geometry& geometry, const std::vector<osg::Vec3f>& directions)
            {
                const auto* colours = dynamic_cast<const osg::Vec4Array*>(geometry.getColorArray());
                if (colours == nullptr || colours->size() != directions.size())
                    return 0.0f;

                float lowest = 0.5f * osg::PIf;
                for (std::size_t i = 0; i < directions.size(); ++i)
                    if ((*colours)[i].x() == 1.0f)
                        lowest = std::min(lowest, std::asin(std::clamp(directions[i].z(), -1.0f, 1.0f)));

                return std::max(lowest, 0.0f);
            }
        };

        /// How far a sheet has to run in **both** directions before it counts as tiled.
        ///
        /// Morrowind's leaves a factor of two either side of this: its widest patch repeats half a
        /// tile across its short axis and its field two whole ones.
        constexpr float sTiledSpan = 1.5f;
    }

    NightSky readNightSky(SceneDesc& scene, Resource::SceneManager& scenes)
    {
        NightSky sky;

        const VFS::Path::Normalized mesh = scenes.getVFS()->exists(Settings::models().mSkynight02.get())
            ? Settings::models().mSkynight02.get()
            : Settings::models().mSkynight01.get();

        if (!scenes.getVFS()->exists(mesh))
        {
            Log(Debug::Warning) << "no night sky mesh at \"" << mesh << "\"; drawing none";
            return sky;
        }

        LayerReader read;
        const_cast<osg::Node&>(*scenes.getTemplate(mesh, false)).accept(read);

        std::size_t next = 0;
        for (const Layer& layer : read.mLayers)
        {
            const Index slot = scene.addTexture(VFS::Path::Normalized(layer.mImage->getFileName()));
            scene.holdTexture(slot);

            if (std::min(layer.mUvSpan.x(), layer.mUvSpan.y()) > sTiledSpan)
            {
                // **The first that looks like one and no more.** A file with two would otherwise
                // leave the earlier one's hold taken and nothing holding it, which is a slot the
                // sweep can never reclaim.
                if (sky.mField != sNoIndex)
                {
                    scene.dropTexture(slot);
                    continue;
                }

                sky.mField = slot;
                sky.mTile = layer.mUvRate > 0.0f ? 1.0f / layer.mUvRate : 0.0f;
                sky.mHorizon = layer.mKeptFrom;
                continue;
            }

            if (next >= sky.mPatches.size())
            {
                scene.dropTexture(slot);
                continue;
            }

            sky.mPatches[next++] = NightSky::Patch{
                .mTexture = slot,
                .mDirection = layer.mDirection,
                .mAngularRadius = layer.mAngularRadius,
            };
        }

        return sky;
    }

    void dropNightSky(SceneDesc& scene, const NightSky& sky)
    {
        if (sky.mField != sNoIndex)
            scene.dropTexture(sky.mField);

        for (const NightSky::Patch& patch : sky.mPatches)
            if (patch.mTexture != sNoIndex)
                scene.dropTexture(patch.mTexture);
    }
}
