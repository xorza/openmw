#ifndef OPENMW_COMPONENTS_NIFOSG_LOADER
#define OPENMW_COMPONENTS_NIFOSG_LOADER

#include <components/nif/niffile.hpp>

#include <osg/ref_ptr>

namespace SceneUtil
{
    class KeyframeHolder;
}

namespace osg
{
    class Node;
}

namespace Resource
{
    class ImageManager;
    class BgsmFileManager;
}

namespace NifOsg
{
    /// The main class responsible for loading NIF files into an OSG-Scenegraph.
    /// @par This scene graph is self-contained and can be cloned using osg::clone if desired. Particle emitters
    ///      and programs hold a pointer to their ParticleSystem, which would need to be manually updated when cloning.
    class Loader
    {
    public:
        /// Create a scene graph for the given NIF. Auto-detects when skinning is used and wraps the graph in a Skeleton
        /// if so.
        static osg::ref_ptr<osg::Node> load(
            Nif::FileView file, Resource::ImageManager* imageManager, Resource::BgsmFileManager* materialManager);

        /// Load keyframe controllers from the given kf file.
        static void loadKf(Nif::FileView kf, SceneUtil::KeyframeHolder& target);

        /// What a host tells the loader before it reads any content.
        ///
        /// **One call, because these are one decision taken once for the process.** Several hosts
        /// stand up this loader — the game, `openmw-rtxtool` and the editor — and a setter apiece
        /// was four chances for one of them to be configured by nobody at all. `load` asserts that
        /// this call was made.
        struct Configuration
        {
            /// What a node the content hides carries, and so what every walk has to leave out.
            ///
            /// **Zero is an answer here and not an absence.** A node with no bits is skipped by
            /// every visitor rather than by the one that meant to skip it, so the `VisController`
            /// that would show it later never runs and it stays hidden for the life of the process.
            /// `Terrain::ObjectPaging` and `Rtx::SceneExtractor` both read it back, the first to
            /// decide what distant land may copy and the second to decide what a ray may reach.
            unsigned int mHiddenNodeMask = 0;

            /// What a `NiCollisionSwitch` with its collision off carries, so an intersection walk
            /// can leave it out. All ones keeps the node's ordinary mask, which is the answer for a
            /// host that tests for no intersections.
            unsigned int mIntersectionDisabledNodeMask = ~0u;

            /// Whether a material that asks for a soft edge is given one.
            bool mSoftEffects = false;

            /// Whether nodes marked "MRK" are built. Hidden in a game and shown in the editor.
            bool mShowMarkers = false;
        };

        /// Installs `configuration`, which every host does before it loads any content.
        static void configure(const Configuration& configuration);

        static bool getShowMarkers();
        static unsigned int getHiddenNodeMask();
        static unsigned int getIntersectionDisabledNodeMask();
        static bool getSoftEffectEnabled();

    private:
        static Configuration sConfiguration;
        static bool sConfigured;
    };

}

#endif
