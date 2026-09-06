#include <array>

#include <gtest/gtest.h>

#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/ref_ptr>
#include <osgParticle/ParticleSystem>

#include <components/nifosg/matrixtransform.hpp>
#include <components/rtx/nodelibrary.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/skeleton.hpp>
// `terraindrawable.hpp` holds `osg::ref_ptr`s to composite-map types it only forward-declares, so it
// does not compile on its own. This is what completes them.
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>

namespace Rtx
{
    namespace
    {
        /// One node and what the gate has to say about it.
        struct Named
        {
            osg::ref_ptr<osg::Node> mNode;
            Library mLibrary;
            const char* mName;
        };

        /// A node of every library the enumeration names, and two apiece for two of them.
        ///
        /// **Two `osg` classes and two `SceneUtil` ones**, because the memo is keyed on the address
        /// of a library name rather than on a class: what has to hold is that two classes of one
        /// library agree. `osg` is nearly every node in a cell and no cast down the walk is gated on
        /// it, so it shares `Other` with every library the walk has never met.
        std::array<Named, 7> everyLibrary()
        {
            return {
                Named{ new osg::Group, Library::Other, "osg" },
                Named{ new osg::MatrixTransform, Library::Other, "osg" },
                Named{ new SceneUtil::LightSource, Library::SceneUtil, "SceneUtil" },
                Named{ new SceneUtil::Skeleton, Library::SceneUtil, "SceneUtil" },
                Named{ new osgParticle::ParticleSystem, Library::OsgParticle, "osgParticle" },
                Named{ new NifOsg::MatrixTransform, Library::NifOsg, "NifOsg" },
                Named{ new Terrain::TerrainDrawable, Library::Terrain, "Terrain" },
            };
        }

        /// The gate answers what the library name says, and the second ask agrees with the first.
        ///
        /// **The name is asserted beside the answer**, because the enumeration is only worth
        /// anything if it says what `libraryName()` says: a test comparing the gate against itself
        /// would pass whatever either of them was.
        TEST(RtxNodeLibraryTest, everyGatedLibraryIsAnsweredAndTheSecondAskAgrees)
        {
            const NodeLibrary gate;

            for (const Named& named : everyLibrary())
            {
                EXPECT_STREQ(named.mNode->libraryName(), named.mName);
                EXPECT_EQ(gate.of(*named.mNode), named.mLibrary) << "at " << named.mName;

                // The first ask worked the answer out and the second reads it back, which is the
                // whole of what the memo does and the one place it could hand out the wrong entry.
                EXPECT_EQ(gate.of(*named.mNode), named.mLibrary) << "the second ask at " << named.mName;
            }
        }

        /// A gate holding every library still tells them apart.
        ///
        /// **Asked round and round rather than one at a time**, so the scan runs with every entry
        /// live: a gate that answered from the first entry it kept, or that walked one slot too far,
        /// passes the test above and fails this one.
        TEST(RtxNodeLibraryTest, aGateHoldingEveryLibraryStillTellsThemApart)
        {
            const NodeLibrary gate;
            const std::array<Named, 7> named = everyLibrary();

            for (int round = 0; round < 3; ++round)
                for (const Named& one : named)
                    EXPECT_EQ(gate.of(*one.mNode), one.mLibrary) << "round " << round << " at " << one.mName;
        }
    }
}
