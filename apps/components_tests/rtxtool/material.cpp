#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Material>
#include <osg/NodeVisitor>
#include <osg/Texture2D>

#include <gtest/gtest.h>

#include <boost/program_options/variables_map.hpp>

#include <components/esm3/loadcell.hpp>
#include <components/files/configurationmanager.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/surface/material.hpp>

#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

#include <apps/rtxtool/cellscene.hpp>
#include <apps/rtxtool/world.hpp>

#include "installation.hpp"

namespace RtxTool
{
    namespace
    {
        namespace bpo = boost::program_options;

        /// An exterior with a town in it and the densest interior the shipped content has: between
        /// them, foliage, water, terrain, architecture, clutter, lit rooms and actors.
        constexpr std::string_view sExterior = "-2,-9";
        constexpr std::string_view sInterior = "Seyda Neen, Census and Excise Office";

        /// What one drawable's state sets came to, resolved the way anything reading a surface does.
        struct Resolved
        {
            const Surface::Material* mDescription = nullptr;
            const osg::StateSet* mTextured = nullptr;
            const osg::Material* mColours = nullptr;
            bool mBlended = false;
            const osg::AlphaFunc* mTested = nullptr;
        };

        /// Walks every drawable and checks the description against the pipeline state beside it.
        ///
        /// **The invariant, stated once.** `NifOsg` and `Terrain` author a `Surface::Material` for
        /// every surface they build, and they build it from the same records the OpenGL state comes
        /// from. So the two must agree — and where they do not, one of the two paths has drifted,
        /// which is exactly what could not be noticed while the description *was* the state.
        struct Audit : osg::NodeVisitor
        {
            std::vector<const osg::StateSet*> mChain;

            std::size_t mSurfaces = 0;
            std::size_t mUndescribed = 0;
            std::size_t mWrongTexture = 0;
            std::size_t mWrongAlpha = 0;
            std::size_t mWrongColour = 0;

            /// The first thing that disagreed, so a failure names something rather than counting.
            std::string mFirstComplaint;

            Audit()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
                setNodeMaskOverride(~0u);
            }

            void apply(osg::Node& node) override
            {
                const bool pushed = push(node.getStateSet());
                traverse(node);
                if (pushed)
                    mChain.pop_back();
            }

            void apply(osg::Drawable& drawable) override
            {
                const bool pushed = push(drawable.getStateSet());
                check(drawable);
                if (pushed)
                    mChain.pop_back();
            }

        private:
            bool push(const osg::StateSet* stateSet)
            {
                if (stateSet == nullptr)
                    return false;

                mChain.push_back(stateSet);
                return true;
            }

            Resolved resolve() const
            {
                Resolved out;
                for (auto it = mChain.rbegin(); it != mChain.rend(); ++it)
                {
                    const osg::StateSet& state = **it;

                    if (out.mDescription == nullptr)
                        out.mDescription = Surface::getMaterial(state);

                    if (out.mTextured == nullptr && !state.getTextureAttributeList().empty())
                        out.mTextured = &state;

                    if (out.mColours == nullptr)
                        out.mColours
                            = dynamic_cast<const osg::Material*>(state.getAttribute(osg::StateAttribute::MATERIAL));

                    if (out.mTested == nullptr)
                        out.mTested
                            = dynamic_cast<const osg::AlphaFunc*>(state.getAttribute(osg::StateAttribute::ALPHAFUNC));

                    if (!out.mBlended)
                        out.mBlended = (state.getMode(GL_BLEND) & osg::StateAttribute::ON) != 0
                            && state.getAttribute(osg::StateAttribute::BLENDFUNC) != nullptr;
                }
                return out;
            }

            void complain(const osg::Drawable& drawable, std::size_t& counter, const std::string& what)
            {
                ++counter;
                if (mFirstComplaint.empty())
                    mFirstComplaint = std::string(drawable.className()) + " \"" + drawable.getName() + "\": " + what;
            }

            void check(const osg::Drawable& drawable)
            {
                if (drawable.asGeometry() == nullptr)
                    return;

                // **A chunk of ground is not one surface.** `Terrain` gives it a pass per ground
                // texture and describes each, because what a layer is made of and how much of it
                // there is are different questions. The chain above the drawable says nothing.
                if (const auto* terrain = dynamic_cast<const Terrain::TerrainDrawable*>(&drawable))
                {
                    checkTerrain(drawable, *terrain);
                    return;
                }

                ++mSurfaces;

                const Resolved resolved = resolve();
                if (resolved.mDescription == nullptr)
                {
                    complain(drawable, mUndescribed, "nothing described this surface");
                    return;
                }

                checkTextures(drawable, resolved);
                checkAlpha(drawable, resolved);
                checkColours(drawable, resolved);
            }

            void checkTerrain(const osg::Drawable& drawable, const Terrain::TerrainDrawable& terrain)
            {
                for (const osg::ref_ptr<osg::StateSet>& pass : terrain.getPasses())
                {
                    ++mSurfaces;

                    const Surface::Material* described = Surface::getMaterial(*pass);
                    if (described == nullptr)
                    {
                        complain(drawable, mUndescribed, "a terrain layer described nothing");
                        continue;
                    }

                    // Unit zero is the layer's ground texture. Unit one is its blend map, which is
                    // the chunk's business rather than the surface's and is deliberately not here.
                    const osg::StateAttribute* bound = pass->getTextureAttribute(0, osg::StateAttribute::TEXTURE);
                    const osg::Texture* texture = bound != nullptr ? bound->asTexture() : nullptr;
                    if (texture != nullptr
                        && described->getTexture(Surface::TextureRole::Diffuse) != texture->getImage(0))
                        complain(drawable, mWrongTexture, "a terrain layer's ground texture disagrees");
                }
            }

            /// Every unit whose name is a role holds the texture the description gives that role.
            ///
            /// The nearest textured state set and not all of them: a `NiTexturingProperty` that
            /// overrides another replaces it whole, and the description is cleared with the units.
            void checkTextures(const osg::Drawable& drawable, const Resolved& resolved)
            {
                if (resolved.mTextured == nullptr)
                    return;

                const osg::StateSet& state = *resolved.mTextured;
                for (unsigned int unit = 0; unit < state.getTextureAttributeList().size(); ++unit)
                {
                    const osg::StateAttribute* named
                        = state.getTextureAttribute(unit, SceneUtil::TextureType::AttributeType);
                    if (named == nullptr)
                        continue;

                    const std::optional<Surface::TextureRole> role = Surface::textureRoleNamed(named->getName());
                    if (!role.has_value())
                        continue;

                    const osg::StateAttribute* bound = state.getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                    if (bound == nullptr)
                        continue;

                    const osg::Texture* texture = bound->asTexture();
                    if (texture == nullptr)
                        continue;

                    if (resolved.mDescription->getTexture(*role) != texture->getImage(0))
                        complain(drawable, mWrongTexture,
                            "unit " + std::to_string(unit) + " is bound as " + named->getName()
                                + " and the description says otherwise");
                }
            }

            void checkAlpha(const osg::Drawable& drawable, const Resolved& resolved)
            {
                const Surface::AlphaMode expected = resolved.mBlended ? Surface::AlphaMode::Blend
                    : resolved.mTested != nullptr && resolved.mTested->getReferenceValue() > 0.0f
                    ? Surface::AlphaMode::Cutout
                    : Surface::AlphaMode::Opaque;

                if (resolved.mDescription->mAlphaMode != expected)
                    complain(drawable, mWrongAlpha, "the alpha mode disagrees with the pipeline state");
                else if (resolved.mTested != nullptr
                    && resolved.mDescription->mAlphaRef != resolved.mTested->getReferenceValue())
                    complain(drawable, mWrongAlpha, "the alpha reference disagrees with the alpha function");
            }

            void checkColours(const osg::Drawable& drawable, const Resolved& resolved)
            {
                if (resolved.mColours == nullptr)
                    return;

                const osg::Material& colours = *resolved.mColours;
                const osg::Vec4f emission = colours.getEmission(osg::Material::FRONT);
                const osg::Vec4f ambient = colours.getAmbient(osg::Material::FRONT);

                if (resolved.mDescription->mDiffuseColour != colours.getDiffuse(osg::Material::FRONT))
                    complain(drawable, mWrongColour, "the diffuse colour disagrees with the osg::Material");
                else if (resolved.mDescription->mEmissiveColour != osg::Vec3f(emission.x(), emission.y(), emission.z()))
                    complain(drawable, mWrongColour, "the emissive colour disagrees with the osg::Material");
                else if (resolved.mDescription->mAmbientColour != osg::Vec3f(ambient.x(), ambient.y(), ambient.z()))
                    complain(drawable, mWrongColour, "the ambient colour disagrees with the osg::Material");
            }
        };

        Audit auditCell(World& world, std::string_view name)
        {
            const ESM::Cell* cell = world.findCell(std::string(name));
            EXPECT_NE(cell, nullptr) << name;

            osg::ref_ptr<osg::Group> root = new osg::Group;
            LoadedCells loaded;

            // A scene of its own, thrown away with the call: this audits what the graph holds, and
            // `readRegion` needs somewhere to put the water quad no graph can carry.
            Rtx::SceneDesc placed;
            Rtx::SceneExtractor extractor(placed);
            readRegion(world, *cell, *root, placed, extractor, loaded, /*liveProps=*/false);

            Audit audit;
            root->accept(audit);
            return audit;
        }

        /// Every surface the shipped content produces describes itself, and says the same thing the
        /// OpenGL state beside it says.
        ///
        /// **This is what makes deleting the other half safe.** While both are authored the two can
        /// be compared over real content; the day the OpenGL renderer builds its state sets *from*
        /// the description, this is the test that says nothing changed.
        TEST(RtxSurfaceMaterialTest, everySurfaceInACellIsDescribedAndAgreesWithItsState)
        {
            Files::ConfigurationManager config;
            bpo::variables_map variables;
            const std::unique_ptr<World> world = openWorld(config, variables);
            if (world == nullptr)
                GTEST_SKIP() << "no Morrowind installation configured";

            for (const std::string_view name : { sExterior, sInterior })
            {
                const Audit audit = auditCell(*world, name);

                EXPECT_GT(audit.mSurfaces, 100u) << name << " should be full of things";
                EXPECT_EQ(audit.mUndescribed, 0u) << name << ": " << audit.mFirstComplaint;
                EXPECT_EQ(audit.mWrongTexture, 0u) << name << ": " << audit.mFirstComplaint;
                EXPECT_EQ(audit.mWrongAlpha, 0u) << name << ": " << audit.mFirstComplaint;
                EXPECT_EQ(audit.mWrongColour, 0u) << name << ": " << audit.mFirstComplaint;
            }
        }
    }
}
