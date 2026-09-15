#include "characterpreview.hpp"

#include <algorithm>
#include <cmath>

#include <osg/Group>
#include <osg/Matrixf>
#include <osg/PositionAttitudeTransform>

#include <components/debug/debuglog.hpp>
#include <components/fallback/fallback.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/offscreenframing.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/inventorystore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "npcanimation.hpp"
#include "offscreenview.hpp"
#include "renderer.hpp"
#include "vismask.hpp"

namespace MWRender
{

    CharacterPreview::CharacterPreview(Renderer& renderer, Resource::ResourceSystem* resourceSystem,
        const MWWorld::Ptr& character, int sizeX, int sizeY, const osg::Vec3f& position, const osg::Vec3f& lookAt)
        : mResourceSystem(resourceSystem)
        , mScene(new osg::Group)
        , mPosition(position)
        , mLookAt(lookAt)
        , mCharacter(character)
        , mAnimation(nullptr)
        , mSizeX(sizeX)
        , mSizeY(sizeY)
    {
        mNode = new osg::PositionAttitudeTransform;
        mScene->addChild(mNode);

        OffscreenViewSpec spec{ *mScene };
        spec.mWidth = sizeX;
        spec.mHeight = sizeY;
        spec.mMask = ~Mask_UpdateVisitor;
        spec.mFraming.mProjection = SceneUtil::Perspective{ .mFieldOfView = 12.3f };
        spec.mFraming.mNear = 4.f;
        spec.mFraming.mFar = 10000.f;
        // Transparent: the figure is composited over the window behind it
        spec.mClearColour = osg::Vec4f(0.f, 0.f, 0.f, 0.f);

        float diffuseR = Fallback::Map::getFloat("Inventory_DirectionalDiffuseR");
        float diffuseG = Fallback::Map::getFloat("Inventory_DirectionalDiffuseG");
        float diffuseB = Fallback::Map::getFloat("Inventory_DirectionalDiffuseB");
        float ambientR = Fallback::Map::getFloat("Inventory_DirectionalAmbientR");
        float ambientG = Fallback::Map::getFloat("Inventory_DirectionalAmbientG");
        float ambientB = Fallback::Map::getFloat("Inventory_DirectionalAmbientB");
        float azimuth = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationX"));
        float altitude = osg::DegreesToRadians(Fallback::Map::getFloat("Inventory_DirectionalRotationY"));
        float positionX = -std::cos(azimuth) * std::sin(altitude);
        float positionY = std::sin(azimuth) * std::sin(altitude);
        float positionZ = std::cos(altitude);
        spec.mSun.mDirection = osg::Vec3f(positionX, positionY, positionZ);
        spec.mSun.mDiffuse = osg::Vec4f(diffuseR, diffuseG, diffuseB, 1);
        spec.mSun.mAmbient = osg::Vec4f(ambientR, ambientG, ambientB, 1);

        mView = renderer.createSubjectView(spec);

        mCharacter.mCell = nullptr;
    }

    CharacterPreview::~CharacterPreview() = default;

    int CharacterPreview::getTextureWidth() const
    {
        return mSizeX;
    }

    int CharacterPreview::getTextureHeight() const
    {
        return mSizeY;
    }

    void CharacterPreview::setBlendMode()
    {
        mView->sceneChanged();
    }

    void CharacterPreview::onSetup()
    {
        setBlendMode();
    }

    MyGUI::ITexture& CharacterPreview::getTexture()
    {
        return mView->getTexture();
    }

    void CharacterPreview::rebuild()
    {
        mAnimation = nullptr;

        mAnimation = new NpcAnimation(mCharacter, mNode, mResourceSystem, true,
            (renderHeadOnly() ? NpcAnimation::VM_HeadOnly : NpcAnimation::VM_Normal));

        onSetup();

        redraw();
    }

    void CharacterPreview::redraw()
    {
        mView->redraw();
    }

    // --------------------------------------------------------------------------------------------------

    InventoryPreview::InventoryPreview(
        Renderer& renderer, Resource::ResourceSystem* resourceSystem, const MWWorld::Ptr& character)
        : CharacterPreview(renderer, resourceSystem, character, 512, 1024, osg::Vec3f(0, 700, 71), osg::Vec3f(0, 0, 71))
    {
    }

    void InventoryPreview::setViewport(int sizeX, int sizeY)
    {
        mExtentX = std::clamp(sizeX, 0, mSizeX);
        mExtentY = std::clamp(sizeY, 0, mSizeY);

        mView->setExtent(mExtentX, mExtentY);

        mView->redraw();
    }

    void InventoryPreview::update()
    {
        if (!mAnimation.get())
            return;

        mAnimation->showWeapons(true);
        mAnimation->updateParts();

        MWWorld::InventoryStore& inv = mCharacter.getClass().getInventoryStore(mCharacter);
        MWWorld::ContainerStoreIterator iter = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
        std::string groupname = "inventoryhandtohand";
        bool showCarriedLeft = true;
        if (iter != inv.end())
        {
            groupname = "inventoryweapononehand";
            if (iter->getType() == ESM::Weapon::sRecordId)
            {
                MWWorld::LiveCellRef<ESM::Weapon>* ref = iter->get<ESM::Weapon>();
                const ESM::RefId type = ref->mBase->mData.mType;
                const ESM::WeaponType* weaponInfo = MWMechanics::getWeaponType(type);
                showCarriedLeft = !(weaponInfo->mFlags & ESM::WeaponType::TwoHanded);

                std::string inventoryGroup = weaponInfo->mLongGroup;
                inventoryGroup = "inventory" + inventoryGroup;

                // We still should use one-handed animation as fallback
                if (mAnimation->hasAnimation(inventoryGroup))
                    groupname = std::move(inventoryGroup);
                else
                {
                    static const std::string oneHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::WeaponType::LongBladeOneHand)->mLongGroup;
                    static const std::string twoHandFallback
                        = "inventory" + MWMechanics::getWeaponType(ESM::WeaponType::LongBladeTwoHand)->mLongGroup;

                    // For real two-handed melee weapons use 2h swords animations as fallback, otherwise use the 1h ones
                    if (weaponInfo->mFlags & ESM::WeaponType::TwoHanded
                        && weaponInfo->mWeaponClass == ESM::WeaponType::Melee)
                        groupname = twoHandFallback;
                    else
                        groupname = oneHandFallback;
                }
            }
        }

        mAnimation->showCarriedLeft(showCarriedLeft);

        mCurrentAnimGroup = std::move(groupname);
        mAnimation->play(mCurrentAnimGroup, 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", 0.0f, 0);

        MWWorld::ConstContainerStoreIterator torch = inv.getSlot(MWWorld::InventoryStore::Slot_CarriedLeft);
        if (torch != inv.end() && torch->getType() == ESM::Light::sRecordId && showCarriedLeft)
        {
            if (!mAnimation->getInfo("torch"))
                mAnimation->play("torch", 2, BlendMask::BlendMask_LeftArm, false, 1.0f, "start", "stop", 0.0f,
                    std::numeric_limits<uint32_t>::max(), true);
        }
        else if (mAnimation->getInfo("torch"))
            mAnimation->disable("torch");

        mAnimation->runAnimation(0.0f);

        setBlendMode();

        redraw();
    }

    int InventoryPreview::getSlotSelected(int posX, int posY)
    {
        if (mExtentX <= 0 || mExtentY <= 0)
            return -1;

        const float projX = (posX / static_cast<float>(mExtentX)) * 2 - 1;
        const float projY = (posY / static_cast<float>(mExtentY)) * 2 - 1;

        osg::NodePath hit;
        if (!mView->pick(projX, projY, hit))
            return -1;

        return mAnimation->getSlot(hit);
    }

    void InventoryPreview::updatePtr(const MWWorld::Ptr& ptr)
    {
        mCharacter = MWWorld::Ptr(ptr.getBase(), nullptr);
    }

    void InventoryPreview::onSetup()
    {
        CharacterPreview::onSetup();
        osg::Vec3f scale(1.f, 1.f, 1.f);
        mCharacter.getClass().adjustScale(mCharacter, scale, true);

        mNode->setScale(scale);

        mView->setView(osg::Matrixf::lookAt(mPosition * scale.z(), mLookAt * scale.z(), osg::Vec3f(0, 0, 1)));
    }

    // --------------------------------------------------------------------------------------------------

    RaceSelectionPreview::RaceSelectionPreview(Renderer& renderer, Resource::ResourceSystem* resourceSystem)
        : CharacterPreview(
            renderer, resourceSystem, MWMechanics::getPlayer(), 512, 512, osg::Vec3f(0, 125, 8), osg::Vec3f(0, 0, 8))
        , mBase(*mCharacter.get<ESM::NPC>()->mBase)
        , mRef(ESM::makeBlankCellRef(), &mBase)
        , mPitchRadians(osg::DegreesToRadians(6.f))
    {
        mCharacter = MWWorld::Ptr(&mRef, nullptr);
    }

    RaceSelectionPreview::~RaceSelectionPreview() {}

    void RaceSelectionPreview::setAngle(float angleRadians)
    {
        mNode->setAttitude(osg::Quat(mPitchRadians, osg::Vec3(1, 0, 0)) * osg::Quat(angleRadians, osg::Vec3(0, 0, 1)));
        redraw();
    }

    void RaceSelectionPreview::setPrototype(const ESM::NPC& proto)
    {
        mBase = proto;
        mBase.mId = ESM::RefId::stringRefId("Player");
        rebuild();
    }

    // On the subtree, because the head is only posed once the keyframe controllers under it have run, and the
    // view's own update is the only traversal that runs them
    class UpdateCameraCallback : public SceneUtil::NodeCallback<UpdateCameraCallback>
    {
    public:
        UpdateCameraCallback(OffscreenView& view, osg::ref_ptr<const osg::Node> nodeToFollow,
            const osg::Vec3& posOffset, const osg::Vec3& lookAtOffset)
            : mView(view)
            , mNodeToFollow(std::move(nodeToFollow))
            , mPosOffset(posOffset)
            , mLookAtOffset(lookAtOffset)
        {
        }

        void operator()(osg::Node* node, osg::NodeVisitor* nv)
        {
            // Update keyframe controllers in the scene graph first...
            traverse(node, nv);

            // Now update camera utilizing the updated head position
            osg::NodePathList nodepaths = mNodeToFollow->getParentalNodePaths();
            if (nodepaths.empty())
                return;
            osg::Matrix worldMat = osg::computeLocalToWorld(nodepaths[0]);
            osg::Vec3 headOffset = worldMat.getTrans();

            mView.setView(
                osg::Matrixf::lookAt(headOffset + mPosOffset, headOffset + mLookAtOffset, osg::Vec3(0, 0, 1)));
        }

    private:
        OffscreenView& mView;
        osg::ref_ptr<const osg::Node> mNodeToFollow;
        osg::Vec3 mPosOffset;
        osg::Vec3 mLookAtOffset;
    };

    void RaceSelectionPreview::onSetup()
    {
        CharacterPreview::onSetup();
        mAnimation->play("idle", 1, BlendMask::BlendMask_All, false, 1.0f, "start", "stop", 0.0f, 0);
        mAnimation->runAnimation(0.f);

        // attach camera to follow the head node
        if (mUpdateCameraCallback)
            mScene->removeUpdateCallback(mUpdateCameraCallback);

        const osg::Node* head = mAnimation->getNode("Bip01 Head");
        if (head)
        {
            mUpdateCameraCallback = new UpdateCameraCallback(*mView, head, mPosition, mLookAt);
            mScene->addUpdateCallback(mUpdateCameraCallback);
        }
        else
            Log(Debug::Error) << "Error: Bip01 Head node not found";
    }

}
