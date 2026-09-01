#ifndef OPENMW_COMPONENTS_BODYPARTS_SLOTS_H
#define OPENMW_COMPONENTS_BODYPARTS_SLOTS_H

#include <array>

#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbody.hpp>

namespace BodyParts
{
    /// Which part slot one kind of skin fills. A paired limb is one record and two slots.
    struct SkinSlot
    {
        ESM::BodyPart::MeshPart mPart;
        ESM::PartReferenceType mSlot;
    };

    /// What a naked person is made of, and so what a garment covers up.
    ///
    /// **Not the inverse of `ESM::getMeshPart`, though it looks like one.** That function answers
    /// for every slot that has a mesh part at all and throws for the rest; what a race's skin fills
    /// is a shorter list. The head and the hair are out because the NPC record names those rather
    /// than the race, which is the whole of how one Dunmer is told from another, and the skirt, the
    /// shield, the weapon and the pauldrons are out because no skin record fills them. Deriving the
    /// list would be restating those exclusions around a call that throws.
    constexpr std::array sSkin{
        SkinSlot{ ESM::BodyPart::MP_Neck, ESM::PRT_Neck },
        SkinSlot{ ESM::BodyPart::MP_Chest, ESM::PRT_Cuirass },
        SkinSlot{ ESM::BodyPart::MP_Groin, ESM::PRT_Groin },
        SkinSlot{ ESM::BodyPart::MP_Hand, ESM::PRT_RHand },
        SkinSlot{ ESM::BodyPart::MP_Hand, ESM::PRT_LHand },
        SkinSlot{ ESM::BodyPart::MP_Wrist, ESM::PRT_RWrist },
        SkinSlot{ ESM::BodyPart::MP_Wrist, ESM::PRT_LWrist },
        SkinSlot{ ESM::BodyPart::MP_Forearm, ESM::PRT_RForearm },
        SkinSlot{ ESM::BodyPart::MP_Forearm, ESM::PRT_LForearm },
        SkinSlot{ ESM::BodyPart::MP_Upperarm, ESM::PRT_RUpperarm },
        SkinSlot{ ESM::BodyPart::MP_Upperarm, ESM::PRT_LUpperarm },
        SkinSlot{ ESM::BodyPart::MP_Foot, ESM::PRT_RFoot },
        SkinSlot{ ESM::BodyPart::MP_Foot, ESM::PRT_LFoot },
        SkinSlot{ ESM::BodyPart::MP_Ankle, ESM::PRT_RAnkle },
        SkinSlot{ ESM::BodyPart::MP_Ankle, ESM::PRT_LAnkle },
        SkinSlot{ ESM::BodyPart::MP_Knee, ESM::PRT_RKnee },
        SkinSlot{ ESM::BodyPart::MP_Knee, ESM::PRT_LKnee },
        SkinSlot{ ESM::BodyPart::MP_Upperleg, ESM::PRT_RLeg },
        SkinSlot{ ESM::BodyPart::MP_Upperleg, ESM::PRT_LLeg },
        SkinSlot{ ESM::BodyPart::MP_Tail, ESM::PRT_Tail },
    };

    /// What a robe hides whether or not it draws anything there.
    ///
    /// **A garment covers more of a body than it replaces.** A robe to the ankles leaves no legs to
    /// see, and it carries no leg mesh — so the slots have to be claimed and left empty, or a pair
    /// of bare shins walks out from under it.
    constexpr std::array sUnderRobe{ ESM::PRT_Groin, ESM::PRT_Skirt, ESM::PRT_RLeg, ESM::PRT_LLeg, ESM::PRT_RUpperarm,
        ESM::PRT_LUpperarm, ESM::PRT_RKnee, ESM::PRT_LKnee, ESM::PRT_RForearm, ESM::PRT_LForearm, ESM::PRT_Cuirass };

    /// The same for a skirt, which reaches the knees and claims what is under them.
    constexpr std::array sUnderSkirt{ ESM::PRT_Groin, ESM::PRT_RLeg, ESM::PRT_LLeg };
}

#endif
