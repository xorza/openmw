#include "tracedground.hpp"

#include <components/terrain/view.hpp>

namespace MWRender
{
    namespace
    {
        class NullView final : public Terrain::View
        {
        public:
            void reset() override {}
        };
    }

    TracedGround::TracedGround(
        osg::Group& sceneRoot, Terrain::Storage& storage, const unsigned int nodeMask, const ESM::RefId worldspace)
        : Terrain::World(&sceneRoot, &storage, nodeMask, worldspace)
    {
    }

    Terrain::View* TracedGround::createView()
    {
        return new NullView;
    }
}
