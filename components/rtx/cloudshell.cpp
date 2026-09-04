#include "cloudshell.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

#include <osg/Geometry>
#include <osg/Matrixf>
#include <osg/NodeVisitor>
#include <osg/Transform>
#include <osg/Vec3f>

#include <components/debug/debuglog.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/vfs/manager.hpp>

#include "shaders/look.h"
#include "shaders/scene.h"

namespace Rtx
{
    namespace
    {
        /// Every vertex of the mesh, placed where the graph puts it, beside the sheet coordinate it
        /// carries.
        ///
        /// **Placed, because the vanilla cap is not where its vertices say.** Its `NiTriShape` sits
        /// fifteen units below its `NiNode`, which is a twentieth of the height everything else here
        /// is a ratio against — and whether the loader leaves that as a transform or folds it into
        /// the array is the scene manager's business rather than this reader's.
        class ShellReader : public osg::NodeVisitor
        {
        public:
            ShellReader()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Geometry& geometry) override
            {
                const auto* vertices = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                const auto* coords = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
                if (vertices == nullptr || coords == nullptr || vertices->size() != coords->size())
                    return;

                const osg::Matrixf placed = osg::computeLocalToWorld(getNodePath());
                mPlaced.reserve(mPlaced.size() + vertices->size());
                mCoords.reserve(mCoords.size() + coords->size());

                mAlphas.reserve(mAlphas.size() + vertices->size());

                for (std::size_t i = 0; i < vertices->size(); ++i)
                {
                    mPlaced.push_back(placed.preMult((*vertices)[i]));
                    mCoords.push_back((*coords)[i]);

                    // **`ModVertexAlphaVisitor::Clouds`, applied here rather than read.** It writes
                    // by vertex index and nothing in the file records what it wrote, so the only way
                    // to know what the engine fades a mesh by is to run its rule over the same mesh.
                    // A file it makes nonsense of is one this makes the same nonsense of, which is
                    // the whole point of copying the rule instead of guessing at its intent.
                    mAlphas.push_back(i >= 49 && i <= 64 ? 0.0f
                            : i >= 33 && i <= 48         ? Shaders::CLOUD_RING_ALPHA
                                                         : 1.0f);
                }
            }

            std::vector<osg::Vec3f> mPlaced;
            std::vector<osg::Vec2f> mCoords;

            /// What `ModVertexAlphaVisitor::Clouds` would paint each of them with.
            std::vector<float> mAlphas;
        };

        /// The sagitta a mesh's vertices describe, in the units they were modelled in.
        struct Sagitta
        {
            double mHeight = 0.0;
            double mCurvature = 0.0;
        };

        /// The one every vertex agrees on, or nothing where they described no layer at all.
        ///
        /// **Least squares over `z = h - k r²` rather than the apex and one ring.** A cap the artist
        /// closed by hand has no vertex exactly at the middle to read a height off, and any single
        /// ring taken as the curvature is one ring's rounding error. Over Morrowind's own cap the fit
        /// lands within 2.4% of where each of its four rings actually is, which is nearer than the
        /// rings are to a circle.
        std::optional<Sagitta> fitSurface(const std::vector<osg::Vec3f>& placed)
        {
            double count = 0.0;
            double squared = 0.0;
            double quartic = 0.0;
            double raised = 0.0;
            double weighted = 0.0;

            for (const osg::Vec3f& vertex : placed)
            {
                const double x = vertex.x();
                const double y = vertex.y();
                const double z = vertex.z();
                const double radius = x * x + y * y;

                count += 1.0;
                squared += radius;
                quartic += radius * radius;
                raised += z;
                weighted += radius * z;
            }

            // **A mesh whose vertices all stand at one radius says nothing about how a layer falls
            // away**, and asking anyway divides by the nothing they disagree about. Cauchy-Schwarz
            // puts this ratio in `[0, 1]`, so what it measures is the share of the spread in `r²`
            // that is real disagreement — and a thousandth of a radius, squared, is where rounding
            // stops being distinguishable from a ring.
            const double spread = count * quartic - squared * squared;
            if (!(spread > 1.0e-6 * count * quartic))
                return std::nullopt;

            const Sagitta fitted{
                .mHeight = (raised * quartic - squared * weighted) / spread,

                // A layer that rises as it recedes is not one. Nought is the flat plane, which the
                // deck handles as the case it is rather than as one to reject.
                .mCurvature = std::max((squared * raised - count * weighted) / spread, 0.0),
            };

            if (!(fitted.mHeight > 0.0))
                return std::nullopt;

            return fitted;
        }

        /// How much sheet a unit of ground is worth, signed by which way up the sheet is laid, or
        /// nothing where the coordinates described no map.
        ///
        /// **The whole map is fitted and then reduced to one signed rate.** What a sheet of tiling
        /// cloud is laid at is the only part of an unwrap anyone can see, and how far it is turned is
        /// not, since a turned tiling is the same tiling. So the two-by-two the vertices describe is
        /// taken for its determinant: the area one unit of ground covers, and the sign that says
        /// whether the sheet was laid face up.
        std::optional<double> fitSheet(const std::vector<osg::Vec3f>& placed, const std::vector<osg::Vec2f>& coords)
        {
            double middleX = 0.0;
            double middleY = 0.0;
            double middleU = 0.0;
            double middleV = 0.0;
            for (std::size_t i = 0; i < placed.size(); ++i)
            {
                middleX += double(placed[i].x());
                middleY += double(placed[i].y());
                middleU += double(coords[i].x());
                middleV += double(coords[i].y());
            }

            const double taken = double(placed.size());
            middleX /= taken;
            middleY /= taken;
            middleU /= taken;
            middleV /= taken;

            double xx = 0.0;
            double xy = 0.0;
            double yy = 0.0;
            double xu = 0.0;
            double yu = 0.0;
            double xv = 0.0;
            double yv = 0.0;

            for (std::size_t i = 0; i < placed.size(); ++i)
            {
                const double x = double(placed[i].x()) - middleX;
                const double y = double(placed[i].y()) - middleY;
                const double u = double(coords[i].x()) - middleU;
                const double v = double(coords[i].y()) - middleV;

                xx += x * x;
                xy += x * y;
                yy += y * y;
                xu += x * u;
                yu += y * u;
                xv += x * v;
                yv += y * v;
            }

            // The same conditioning test the surface makes, on the ground the sheet is laid over:
            // vertices along one line say how far the sheet runs across it and nothing about the
            // other axis.
            const double spread = xx * yy - xy * xy;
            if (!(spread > 1.0e-6 * xx * yy))
                return std::nullopt;

            const double toU = (yy * xu - xy * yu) / spread;
            const double alongU = (xx * yu - xy * xu) / spread;
            const double toV = (yy * xv - xy * yv) / spread;
            const double alongV = (xx * yv - xy * xv) / spread;

            const double area = toU * alongV - alongU * toV;
            const double rate = std::sqrt(std::abs(area));
            if (!(rate > 0.0))
                return std::nullopt;

            return area < 0.0 ? -rate : rate;
        }
    }

    namespace
    {
        /// The three radii the engine's fade turns on, in the units the vertices were modelled in.
        ///
        /// **Reduced from the alpha it paints rather than from the ring count.** What matters is
        /// where each alpha level stops, and a level's outer edge is the furthest vertex carrying
        /// it — so a mesh whose rings are not circles, or whose rows the rule splits differently,
        /// still comes out as the fade the rasterizer would draw.
        osg::Vec3f fadeRadii(const std::vector<osg::Vec3f>& placed, const std::vector<float>& alphas)
        {
            osg::Vec3f reach;
            for (std::size_t i = 0; i < placed.size(); ++i)
            {
                const float radius = std::hypot(placed[i].x(), placed[i].y());
                const std::size_t band = alphas[i] >= 1.0f ? 0u : alphas[i] > 0.0f ? 1u : 2u;

                reach[band] = std::max(reach[band], radius);
            }

            // A band nobody painted reaches no further than the one inside it, which collapses its
            // stretch of the ramp to nothing rather than running it backwards.
            reach[1] = std::max(reach[1], reach[0]);
            reach[2] = std::max(reach[2], reach[1]);

            return reach;
        }
    }

    CloudShell readCloudShell(osg::Node& mesh)
    {
        ShellReader read;
        mesh.accept(read);

        // Three vertices is the least a surface can be fitted to, and it is also what keeps
        // `fitSheet`'s means from dividing by an empty mesh.
        if (read.mPlaced.size() < 3)
            return CloudShell{};

        const std::optional<Sagitta> surface = fitSurface(read.mPlaced);
        if (!surface.has_value())
            return CloudShell{};

        const std::optional<double> rate = fitSheet(read.mPlaced, read.mCoords);
        if (!rate.has_value())
            return CloudShell{};

        return CloudShell{
            .mTiles = osg::Vec2f(float(surface->mHeight * std::abs(*rate)), float(surface->mHeight * *rate)),
            .mCurvature = float(surface->mCurvature * surface->mHeight),
            .mRings = fadeRadii(read.mPlaced, read.mAlphas) * float(std::abs(*rate)),
        };
    }

    CloudShell readCloudShell(Resource::SceneManager& scenes, VFS::Path::NormalizedView mesh)
    {
        if (!scenes.getVFS()->exists(mesh))
        {
            Log(Debug::Warning) << "no cloud mesh at \"" << mesh << "\"; drawing no deck";
            return CloudShell{};
        }

        return readCloudShell(const_cast<osg::Node&>(*scenes.getTemplate(mesh, false)));
    }
}
