#ifndef MWRENDER_CHARACTERPREVIEW_H
#define MWRENDER_CHARACTERPREVIEW_H

#include <memory>
#include <osg/ref_ptr>

#include <osg/PositionAttitudeTransform>

#include <components/esm3/loadnpc.hpp>

#include <components/resource/resourcesystem.hpp>

#include "../mwworld/ptr.hpp"

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Group;
}

namespace MWRender
{

    class NpcAnimation;
    class OffscreenView;
    class Renderer;

    class CharacterPreview
    {
    public:
        CharacterPreview(Renderer& renderer, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character,
            int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt);
        virtual ~CharacterPreview();

        int getTextureWidth() const;
        int getTextureHeight() const;

        void redraw();

        void rebuild();

        MyGUI::ITexture& getTexture();

    private:
        CharacterPreview(const CharacterPreview&);
        CharacterPreview& operator=(const CharacterPreview&);

    protected:
        virtual bool renderHeadOnly() { return false; }

        /// The subtree is not the subtree it was: equipment changed, or the body was rebuilt.
        void setBlendMode();

        virtual void onSetup();

        Resource::ResourceSystem* mResourceSystem;

        /// What the view draws, and the only thing about it the game owns. Anything hung off here
        /// runs during the view's own update, which is what lets the race preview find the head
        /// before the picture is taken.
        osg::ref_ptr<osg::Group> mScene;

        std::unique_ptr<OffscreenView> mView;

        osg::Vec3f mPosition;
        osg::Vec3f mLookAt;

        MWWorld::Ptr mCharacter;

        osg::ref_ptr<MWRender::NpcAnimation> mAnimation;
        osg::ref_ptr<osg::PositionAttitudeTransform> mNode;
        std::string mCurrentAnimGroup;

        int mSizeX;
        int mSizeY;
    };

    class InventoryPreview : public CharacterPreview
    {
    public:
        InventoryPreview(Renderer& renderer, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character);

        void updatePtr(const MWWorld::Ptr& ptr);

        void update(); // Render preview again, e.g. after changed equipment
        void setViewport(int sizeX, int sizeY);

        int getSlotSelected(int posX, int posY);

    protected:
        void onSetup() override;

    private:
        /// How much of the picture the window is currently showing. Zero until the window has been
        /// laid out, which is also when there is nothing to have clicked on.
        int mExtentX = 0;
        int mExtentY = 0;
    };

    class UpdateCameraCallback;

    class RaceSelectionPreview : public CharacterPreview
    {
        ESM::NPC mBase;
        MWWorld::LiveCellRef<ESM::NPC> mRef;

    protected:
        bool renderHeadOnly() override { return true; }
        void onSetup() override;

    public:
        RaceSelectionPreview(Renderer& renderer, Resource::ResourceSystem* resourceSystem);
        virtual ~RaceSelectionPreview();

        void setAngle(float angleRadians);

        const ESM::NPC& getPrototype() const { return mBase; }

        void setPrototype(const ESM::NPC& proto);

    private:
        osg::ref_ptr<UpdateCameraCallback> mUpdateCameraCallback;

        float mPitchRadians;
    };

}

#endif
