#include "nightsky.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/Texture2D>
#include <osg/TriangleIndexFunctor>

#include <components/debug/debuglog.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/vfs/manager.hpp>

#include "scenedesc.hpp"
#include "shaders/look.h"
#include "shaders/scene.h"
#include "texels.hpp"

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

            /// How far its texture coordinates run, along each axis separately, because what tells
            /// a field from a patch is that a field repeats in *both* directions: a constellation
            /// overshoots to one and a half tiles on its long axis, where the field runs four by two.
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

        /// How much sheet one radian is worth, along each edge of a drawable's triangles — against
        /// the two angles the unwrap is written in and not the arc between them, which shrinks as
        /// `cos(elevation)` and reads a fifth too large. Along a triangle's edges, because the rate
        /// is local and two vertices across a dome are a wrap apart.
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

                // The rate the unwrap runs at, along the mesh's own edges: a span over an extent
                // would be thrown by a dome's seam vertices, and an edge is the shortest baseline
                // the mesh offers.
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

    NightSky readNightSky(SceneDesc& scene, Resource::SceneManager& scenes, VFS::Path::NormalizedView mesh,
        VFS::Path::NormalizedView fallback)
    {
        NightSky sky;

        const VFS::Path::NormalizedView chosen = scenes.getVFS()->exists(mesh) ? mesh : fallback;

        if (!scenes.getVFS()->exists(chosen))
        {
            Log(Debug::Warning) << "no night sky mesh at \"" << chosen << "\"; drawing none";
            return sky;
        }

        LayerReader read;
        const_cast<osg::Node&>(*scenes.getTemplate(chosen, false)).accept(read);

        std::size_t next = 0;
        for (const Layer& layer : read.mLayers)
        {
            const Index slot = scene.textures().add(VFS::Path::Normalized(layer.mImage->getFileName()));
            scene.textures().hold(slot);

            if (std::min(layer.mUvSpan.x(), layer.mUvSpan.y()) > sTiledSpan)
            {
                // **The first that looks like one and no more.** A file with two would otherwise
                // leave the earlier one's hold taken and nothing holding it, which is a slot the
                // sweep can never reclaim.
                if (sky.mField != sNoIndex)
                {
                    scene.textures().drop(slot);
                    continue;
                }

                sky.mField = slot;
                sky.mTile = layer.mUvRate > 0.0f ? 1.0f / layer.mUvRate : 0.0f;
                sky.mHorizon = layer.mKeptFrom;

                // The field is laid over the whole dome, so its own mean is what it adds to the
                // sky's — `STAR_RADIANCE` is the scale the shader draws it at.
                sky.mGlow += meanTexel(*layer.mImage).mColour * Shaders::STAR_RADIANCE;
                continue;
            }

            if (next >= sky.mPatches.size())
            {
                scene.textures().drop(slot);
                continue;
            }

            sky.mPatches[next++] = NightSky::Patch{
                .mTexture = slot,
                .mDirection = layer.mDirection,
                .mAngularRadius = layer.mAngularRadius,
            };

            // A cap of half-angle `t` is `1 - cos(t)` of a hemisphere, which is the share of the
            // sky's mean this patch speaks for. Overlaps are counted twice, which is the sky's mean
            // to first order and spent out of the weather's own ambient either way.
            sky.mGlow += meanTexel(*layer.mImage).mColour
                * (Shaders::NEBULA_RADIANCE * (1.0f - std::cos(layer.mAngularRadius)));
        }

        return sky;
    }
}
