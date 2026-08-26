#include "npc.hpp"

#include <algorithm>
#include <array>
#include <optional>

#include <osg/Group>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/attach.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include "actor.hpp"
#include "world.hpp"

namespace RtxTool
{
    namespace
    {
        /// Which bone each of the twenty-seven part slots hangs on.
        ///
        /// Morrowind dresses a person as a paper doll: every garment names the slots it fills, and a
        /// slot is one bone with one thing on it. The table is the game's own. A hairstyle is the one
        /// entry whose filter differs from its bone — hair and skull hang on the same bone and would
        /// take each other's triangles otherwise.
        ///
        /// **The weapon slot is one bone here where the game's table has two.** A bow attaches to
        /// "Weapon Bone Left" there, because the right hand draws the string — and then the game
        /// looks that bone up and falls back to this one when the skeleton has not got it, which
        /// vanilla's has not: `xbase_anim.nif` carries "Weapon Bone", "Weapon", "Shield Bone" and
        /// "Shield", and no left-hand weapon bone at all. So on the content this fork renders the
        /// exception never fires, and a lookup that can only ever fail is not carried.
        constexpr std::array<std::string_view, ESM::PRT_Count> sBones{
            "Head",
            "Head", // PRT_Hair, filtered by "hair" instead
            "Neck",
            "Chest",
            "Groin",
            "Groin", // PRT_Skirt
            "Right Hand",
            "Left Hand",
            "Right Wrist",
            "Left Wrist",
            "Shield Bone",
            "Right Forearm",
            "Left Forearm",
            "Right Upper Arm",
            "Left Upper Arm",
            "Right Foot",
            "Left Foot",
            "Right Ankle",
            "Left Ankle",
            "Right Knee",
            "Left Knee",
            "Right Upper Leg",
            "Left Upper Leg",
            "Right Clavicle",
            "Left Clavicle",
            "Weapon Bone",
            "Tail",
        };

        bool isAmmunition(int type)
        {
            return type == ESM::Weapon::Arrow || type == ESM::Weapon::Bolt;
        }

        /// The idle a weapon is held in, and what stands in for it where nobody animated one.
        struct WeaponIdle
        {
            std::string_view mStance;
            std::string_view mFallback;
        };

        /// One entry per kind of weapon somebody can hold, in `ESM::Weapon::Type` order.
        ///
        /// **A weapon is drawn into a stance and not into a hand.** "Weapon Bone" is wherever the
        /// animation being played leaves it, and the plain `idle` is the one with nothing in that
        /// hand — so a sword hung there comes out lying across its owner rather than held beside
        /// them.
        ///
        /// **The fallback is the ordinary answer and not a rescue**, which is why it is a column
        /// here rather than a special case somewhere: vanilla's `xbase_anim.kf` carries `idle1h`,
        /// `idle2c`, `idle2w` and `idlecrossbow` and none of the other eight, so a short blade, an
        /// axe, a mace, a bow and a thrown weapon all reach it. The rule is the game's own —
        /// two-handed *melee* stands in the two-handed close idle and everything else in the
        /// one-handed one, so a bow, which is two-handed and ranged, goes with the swords.
        ///
        /// Ammunition is not here: an arrow is carried, not held.
        constexpr std::array<WeaponIdle, ESM::Weapon::MarksmanThrown + 1> sWeaponIdles{
            WeaponIdle{ "idle1s", "idle1h" }, // ShortBladeOneHand
            WeaponIdle{ "idle1h", "idle1h" }, // LongBladeOneHand
            WeaponIdle{ "idle2c", "idle2c" }, // LongBladeTwoHand
            WeaponIdle{ "idle1b", "idle1h" }, // BluntOneHand
            WeaponIdle{ "idle2b", "idle2c" }, // BluntTwoClose
            WeaponIdle{ "idle2w", "idle2c" }, // BluntTwoWide
            WeaponIdle{ "idle2w", "idle2c" }, // SpearTwoWide
            WeaponIdle{ "idle1b", "idle1h" }, // AxeOneHand
            WeaponIdle{ "idle2b", "idle2c" }, // AxeTwoHand
            WeaponIdle{ "idlebow", "idle1h" }, // MarksmanBow
            WeaponIdle{ "idlecrossbow", "idle1h" }, // MarksmanCrossbow
            WeaponIdle{ "idle1t", "idle1h" }, // MarksmanThrown
        };

        /// The hardest single blow a weapon can land, over all three attacks.
        ///
        /// The maximum of each range and not its mean, which is the comparison the game makes: what
        /// it asks of a weapon is what it is capable of, not what it usually does.
        int hardestBlow(const ESM::Weapon& weapon)
        {
            return std::max({ weapon.mData.mChop[1], weapon.mData.mSlash[1], weapon.mData.mThrust[1] });
        }

        /// Whether the inventory holds anything this weapon could fire.
        ///
        /// **The game's own refusal**: a bow with no arrows and a crossbow with no bolts stay in the
        /// pack, so an archer out of ammunition is somebody standing empty-handed rather than
        /// somebody miming.
        bool hasAmmunitionFor(const World& world, const ESM::NPC& npc, int type)
        {
            const int wanted = type == ESM::Weapon::MarksmanBow ? ESM::Weapon::Arrow
                : type == ESM::Weapon::MarksmanCrossbow         ? ESM::Weapon::Bolt
                                                                : ESM::Weapon::None;
            if (wanted == ESM::Weapon::None)
                return true;

            for (const ESM::ContItem& carried : npc.mInventory.mList)
                if (const ESM::Weapon* ammunition = world.findRecord<ESM::Weapon>(carried.mItem))
                    if (ammunition->mData.mType == wanted)
                        return true;

            return false;
        }

        /// What somebody would have drawn out of their own inventory, or null where they are unarmed.
        ///
        /// **The game's comparison with its outer loop taken off.** `InventoryStore::autoEquipWeapon`
        /// first finds the weapon skill the wearer is best at and only then takes the hardest-hitting
        /// weapon of that class; a skill is an autocalculated stat the harness has no route to, so
        /// the damage comparison stands on its own — the same substitution `dress` makes for armour.
        /// It decides one armed person in nine: 1,513 of Morrowind's 3,041 NPC records carry a
        /// weapon and 169 of those carry two.
        const ESM::Weapon* drawnWeapon(const World& world, const ESM::NPC& npc)
        {
            const ESM::Weapon* best = nullptr;
            int hardest = -1;

            for (const ESM::ContItem& carried : npc.mInventory.mList)
            {
                const ESM::Weapon* weapon = world.findRecord<ESM::Weapon>(carried.mItem);
                if (weapon == nullptr || isAmmunition(weapon->mData.mType) || weapon->mModel.empty())
                    continue;

                if (!hasAmmunitionFor(world, npc, weapon->mData.mType))
                    continue;

                if (const int blow = hardestBlow(*weapon); blow > hardest)
                {
                    hardest = blow;
                    best = weapon;
                }
            }

            return best;
        }

        /// Which part slot one kind of skin fills. A paired limb is one record and two slots.
        struct SkinSlot
        {
            ESM::BodyPart::MeshPart mPart;
            ESM::PartReferenceType mSlot;
        };

        /// What a naked person is made of, and so what a garment covers up.
        ///
        /// The head and the hair are not here: those two are named by the NPC record itself rather
        /// than chosen by race, which is the whole of how one Dunmer is told from another.
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

        /// A slot of the wardrobe, as distinct from a slot of the body: one garment goes in each.
        enum Wearing
        {
            WornRobe,
            WornSkirt,
            WornHelmet,
            WornCuirass,
            WornGreaves,
            WornLeftPauldron,
            WornRightPauldron,
            WornBoots,
            WornLeftGauntlet,
            WornRightGauntlet,
            WornShirt,
            WornPants,
            WornShield,

            WornCount
        };

        /// How firmly what goes in each wardrobe slot holds its ground, in the order it is put on.
        ///
        /// **The order is the layering, and it runs outward in**: a robe goes on over everything and
        /// is decided first, a shirt is under all of it and is decided last. The two numbers that are
        /// not zero are the game's own, and the comment beside them there admits they are a count of
        /// slots the garment reserves rather than anything derived.
        constexpr std::array<int, WornCount> sBasePriority{ 11, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

        /// What a robe hides whether or not it draws anything there.
        ///
        /// **A garment covers more of a body than it replaces.** A robe to the ankles leaves no legs
        /// to see, and it carries no leg mesh — so the slots have to be claimed and left empty, or a
        /// pair of bare shins walks out from under it.
        constexpr std::array sUnderRobe{ ESM::PRT_Groin, ESM::PRT_Skirt, ESM::PRT_RLeg, ESM::PRT_LLeg,
            ESM::PRT_RUpperarm, ESM::PRT_LUpperarm, ESM::PRT_RKnee, ESM::PRT_LKnee, ESM::PRT_RForearm,
            ESM::PRT_LForearm, ESM::PRT_Cuirass };

        constexpr std::array sUnderSkirt{ ESM::PRT_Groin, ESM::PRT_RLeg, ESM::PRT_LLeg };

        std::optional<Wearing> wornAsClothing(int type)
        {
            switch (type)
            {
                case ESM::Clothing::Shirt:
                    return WornShirt;
                case ESM::Clothing::Pants:
                    return WornPants;
                case ESM::Clothing::Shoes:
                    return WornBoots;
                case ESM::Clothing::Robe:
                    return WornRobe;
                case ESM::Clothing::Skirt:
                    return WornSkirt;
                case ESM::Clothing::LGlove:
                    return WornLeftGauntlet;
                case ESM::Clothing::RGlove:
                    return WornRightGauntlet;
                default:
                    // A belt, a ring and an amulet are worn and not seen: none of them names a part.
                    return std::nullopt;
            }
        }

        std::optional<Wearing> wornAsArmour(int type)
        {
            switch (type)
            {
                case ESM::Armor::Helmet:
                    return WornHelmet;
                case ESM::Armor::Cuirass:
                    return WornCuirass;
                case ESM::Armor::Greaves:
                    return WornGreaves;
                case ESM::Armor::LPauldron:
                    return WornLeftPauldron;
                case ESM::Armor::RPauldron:
                    return WornRightPauldron;
                case ESM::Armor::Boots:
                    return WornBoots;
                case ESM::Armor::LGauntlet:
                case ESM::Armor::LBracer:
                    return WornLeftGauntlet;
                case ESM::Armor::RGauntlet:
                case ESM::Armor::RBracer:
                    return WornRightGauntlet;
                case ESM::Armor::Shield:
                    return WornShield;
                default:
                    return std::nullopt;
            }
        }

        /// One garment somebody has on, as everything that decides the picture.
        struct Garment
        {
            const ESM::PartReferenceList* mParts = nullptr;

            /// Armour beats clothing outright, whatever either is worth.
            bool mArmour = false;

            /// `ESM::Armor::Type`, so two pieces of armour in one slot can be ordered by their kind.
            int mKind = 0;

            /// The armour rating, or the price of a garment. What two of a kind are compared by.
            int mWorth = 0;
        };

        /// Whether a beast may put this on: no boots and no closed helm, by what the garment covers.
        ///
        /// **Asked of the part references rather than of a list of item names**, exactly as the game
        /// asks it: a Khajiit's foot is not a boot's shape and their skull is not a helm's, so any
        /// garment claiming those slots is one they cannot wear.
        bool fitsABeast(const ESM::PartReferenceList& parts)
        {
            for (const ESM::PartReference& part : parts.mParts)
                if (part.mPart == ESM::PRT_Head || part.mPart == ESM::PRT_LFoot || part.mPart == ESM::PRT_RFoot)
                    return false;

            return true;
        }

        /// Whether `candidate` displaces what is already in the slot, by the game's own order.
        bool outranks(const Garment& candidate, const Garment& worn)
        {
            if (worn.mParts == nullptr)
                return true;

            if (candidate.mArmour != worn.mArmour)
                return candidate.mArmour;

            // Two pieces of armour of different kinds in one slot — a gauntlet against a bracer —
            // are ordered by the kind alone, and the lower number wins.
            if (candidate.mArmour && candidate.mKind != worn.mKind)
                return candidate.mKind < worn.mKind;

            return candidate.mWorth > worn.mWorth;
        }

        /// What everybody in `npc`'s inventory list actually has on.
        ///
        /// **The record's inventory is the outfit.** Nothing here simulates a person and nothing needs
        /// to: 2991 of Morrowind's 3049 NPC records carry something wearable, and the game equips
        /// exactly that list the first time it builds their inventory.
        ///
        /// Only 284 of them carry two things for one slot, which is the whole of what the game's
        /// `autoEquip` has to decide, and its rule is reproduced here with one substitution. It
        /// compares two pieces of armour of the same kind by a rating scaled by the wearer's skill in
        /// that armour's weight class; those are autocalculated stats the harness has no route to, so
        /// the record's own rating stands in. Where the two disagree somebody wears the other cuirass.
        std::array<Garment, WornCount> dress(const World& world, const ESM::NPC& npc, bool beast)
        {
            std::array<Garment, WornCount> wearing{};

            for (const ESM::ContItem& carried : npc.mInventory.mList)
            {
                std::optional<Wearing> where;
                Garment garment;

                if (const ESM::Clothing* clothes = world.findRecord<ESM::Clothing>(carried.mItem))
                {
                    where = wornAsClothing(clothes->mData.mType);
                    garment = Garment{ .mParts = &clothes->mParts,
                        .mArmour = false,
                        .mKind = clothes->mData.mType,
                        .mWorth = clothes->mData.mValue };
                }
                else if (const ESM::Armor* armour = world.findRecord<ESM::Armor>(carried.mItem))
                {
                    where = wornAsArmour(armour->mData.mType);
                    garment = Garment{ .mParts = &armour->mParts,
                        .mArmour = true,
                        .mKind = armour->mData.mType,
                        .mWorth = armour->mData.mArmor };
                }

                if (!where.has_value() || garment.mParts->mParts.empty())
                    continue;

                if (beast && !fitsABeast(*garment.mParts))
                    continue;

                if (outranks(garment, wearing[*where]))
                    wearing[*where] = garment;
            }

            return wearing;
        }

        bool isFemalePart(const ESM::BodyPart& part)
        {
            return (part.mData.mFlags & ESM::BodyPart::BPF_Female) != 0;
        }

        /// Whether a body part is skin this race can wear in the third person.
        bool wearable(const ESM::BodyPart& part)
        {
            return (part.mData.mFlags & ESM::BodyPart::BPF_NotPlayable) == 0
                && part.mData.mType == ESM::BodyPart::MT_Skin && !ESM::isFirstPersonBodyPart(part);
        }

        /// The skin `race` has for `want`, preferring `female` and falling back to the other sex.
        ///
        /// **The fallback is the game's and not a convenience.** Morrowind ships races whose female
        /// body is only partly authored, and a person missing a forearm because nobody drew a female
        /// one is a hole in the middle of them.
        const ESM::BodyPart* findSkin(
            const World& world, const ESM::RefId& race, ESM::BodyPart::MeshPart want, bool female)
        {
            const ESM::BodyPart* fallback = nullptr;

            for (const ESM::BodyPart& part : world.getRecords<ESM::BodyPart>())
            {
                if (!wearable(part) || part.mRace != race || part.mData.mPart != want)
                    continue;

                if (isFemalePart(part) == female)
                    return &part;

                if (fallback == nullptr)
                    fallback = &part;
            }

            return fallback;
        }

        /// Whether `race` walks on the beast skeleton.
        ///
        /// **Asked of the body parts rather than of the race record**, which the harness does not
        /// load: the two beast races are exactly the two with a tail to skin, and deriving it from
        /// the content beats a list of two names that a content file could extend.
        bool isBeast(const World& world, const ESM::RefId& race)
        {
            for (const ESM::BodyPart& part : world.getRecords<ESM::BodyPart>())
                if (wearable(part) && part.mRace == race && part.mData.mPart == ESM::BodyPart::MP_Tail)
                    return true;

            return false;
        }

        /// Which shared skeleton this person's parts hang on.
        ///
        /// Read off the same settings the game reads, so a content file that replaces the base
        /// animation replaces it here too. One beast skeleton serves both sexes, which is Morrowind's
        /// own arrangement and not a simplification.
        VFS::Path::Normalized skeletonFor(const World& world, bool female, bool beast)
        {
            const VFS::Path::Normalized& named = beast ? Settings::models().mBaseanimkna.get()
                : female                               ? Settings::models().mBaseanimfemale.get()
                                                       : Settings::models().mBaseanim.get();

            return Misc::ResourceHelpers::correctActorModelPath(named, world.getResourceSystem().getVFS());
        }

        /// Every part slot of one body, with what has claimed it and how firmly.
        ///
        /// **Decided in full before anything is hung.** The game attaches each part as it goes and
        /// detaches whatever it displaces, because it is dressing somebody already standing there;
        /// nobody here is standing yet, so one comparison settles the whole outfit and the graph is
        /// built once from the answer.
        class Outfit
        {
        public:
            /// Puts `model` in `slot` unless something stronger has it.
            ///
            /// An empty model reserves the slot instead of filling it, which is how a hem covers a
            /// knee it draws nothing for — and how a garment naming a body part nobody shipped comes
            /// out as a gap rather than as bare skin under a coat.
            void claim(ESM::PartReferenceType slot, int priority, VFS::Path::NormalizedView model)
            {
                if (priority <= mPriority[slot])
                    return;

                mPriority[slot] = priority;
                mModel[slot] = model;
            }

            void reserve(ESM::PartReferenceType slot, int priority) { claim(slot, priority, VFS::Path::Normalized()); }

            /// Whether nothing but skin has a claim on `slot`, which is what skin itself tests.
            bool bare(ESM::PartReferenceType slot) const { return mPriority[slot] < 1; }

            int getPriority(ESM::PartReferenceType slot) const { return mPriority[slot]; }

            const VFS::Path::Normalized& getModel(ESM::PartReferenceType slot) const { return mModel[slot]; }

        private:
            std::array<int, ESM::PRT_Count> mPriority{};
            std::array<VFS::Path::Normalized, ESM::PRT_Count> mModel;
        };

        /// The mesh a garment names for one slot, or empty where it names none for this sex.
        ///
        /// A garment lists a female and a male part per slot and may leave either out, so a woman in
        /// something only drawn for men wears the man's version rather than a hole.
        VFS::Path::Normalized garmentPiece(const World& world, const ESM::PartReference& part, bool female)
        {
            const ESM::BodyPart* found = nullptr;
            if (female && !part.mFemale.empty())
                found = world.findRecord<ESM::BodyPart>(part.mFemale);

            if (found == nullptr && !part.mMale.empty())
                found = world.findRecord<ESM::BodyPart>(part.mMale);

            if (found == nullptr)
                return VFS::Path::Normalized();

            return Misc::ResourceHelpers::correctMeshPath(found->mModel.getNormalized());
        }

        /// Hangs one mesh on `bone`, taking the piece of it that bone is for.
        void hang(World& world, osg::Group& skeleton, const SceneUtil::NodeMap& bones, VFS::Path::NormalizedView model,
            std::string_view bone, std::string_view filter)
        {
            const auto found = bones.find(std::string(bone));
            if (found == bones.end())
                return;

            Resource::SceneManager& scene = world.getSceneManager();

            try
            {
                SceneUtil::attach(scene.getTemplate(model), &skeleton, filter, found->second.get(), &scene);
            }
            catch (const std::exception& failed)
            {
                Log(Debug::Warning) << "Cannot hang " << model << " on " << bone << ": " << failed.what();
            }
        }
    }

    const ESM::NPC* findNpc(const World& world, std::string_view id)
    {
        return world.findRecord<ESM::NPC>(ESM::RefId::stringRefId(id));
    }

    ActorModel buildNpc(World& world, const ESM::NPC& npc, const bool dressed)
    {
        const bool female = !npc.isMale();
        const bool beast = isBeast(world, npc.mRace);

        ActorModel built;
        built.mSkeleton = skeletonFor(world, female, beast);

        // An instance rather than a template, because parts are about to be hung on this one's bones
        // and the template is shared with everyone else of this sex.
        osg::ref_ptr<osg::Node> created = world.getSceneManager().getInstance(built.mSkeleton);
        built.mRoot = dynamic_cast<SceneUtil::Skeleton*>(created.get());
        if (built.mRoot == nullptr)
        {
            osg::ref_ptr<SceneUtil::Skeleton> wrapper = new SceneUtil::Skeleton;
            wrapper->addChild(created);
            built.mRoot = wrapper;
        }

        // **The skeleton is bones and nothing else.** Morrowind's base animation files ship with
        // placeholder geometry in them — the female one most visibly — and a person built without
        // stripping it wears a magenta torso and a grey chevron nobody drew. The game does the same
        // under the name `baseonly`, and it is why an NPC's skeleton cannot simply be loaded the way
        // a creature's is.
        SceneUtil::CleanObjectRootVisitor bare;
        built.mRoot->accept(bare);
        bare.remove();

        // **Outward in.** Every garment is offered its slots in the order it goes on, and the first
        // to reach one keeps it — so a cuirass takes the chest the shirt would have had, and a robe
        // over both takes the lot.
        const std::array<Garment, WornCount> wearing
            = dressed ? dress(world, npc, beast) : std::array<Garment, WornCount>{};
        Outfit outfit;

        for (int worn = 0; worn < WornCount; ++worn)
        {
            const Garment& garment = wearing[worn];
            if (garment.mParts == nullptr)
                continue;

            const int priority = ((sBasePriority[worn] + 1) << 1) + (garment.mArmour ? 1 : 0);

            for (const ESM::PartReference& part : garment.mParts->mParts)
            {
                if (part.mPart >= ESM::PRT_Count)
                    continue;

                outfit.claim(
                    static_cast<ESM::PartReferenceType>(part.mPart), priority, garmentPiece(world, part, female));
            }

            if (worn == WornRobe)
                for (const ESM::PartReferenceType slot : sUnderRobe)
                    outfit.reserve(slot, priority);
            else if (worn == WornSkirt)
                for (const ESM::PartReferenceType slot : sUnderSkirt)
                    outfit.reserve(slot, priority);
        }

        // Their own face, and their own hair unless a helmet took the skull.
        if (const ESM::BodyPart* head = world.findRecord<ESM::BodyPart>(npc.mHead))
            outfit.claim(ESM::PRT_Head, 1, Misc::ResourceHelpers::correctMeshPath(head->mModel.getNormalized()));

        if (outfit.getPriority(ESM::PRT_Head) <= 1)
            if (const ESM::BodyPart* hair = world.findRecord<ESM::BodyPart>(npc.mHair))
                outfit.claim(ESM::PRT_Hair, 1, Misc::ResourceHelpers::correctMeshPath(hair->mModel.getNormalized()));

        // And skin wherever nothing was put on, which under a full suit of armour is nowhere.
        for (const SkinSlot& slot : sSkin)
            if (outfit.bare(slot.mSlot))
                if (const ESM::BodyPart* skin = findSkin(world, npc.mRace, slot.mPart, female))
                    outfit.claim(slot.mSlot, 1, Misc::ResourceHelpers::correctMeshPath(skin->mModel.getNormalized()));

        // **Held rather than worn, so it comes off the record and not off a part list.** A shield is
        // a body part — all sixty-five of the shipped ones name one — and goes through the wardrobe
        // above with everything else; a weapon is the item's own model hung on a bone, and nothing
        // in the paper doll speaks for it.
        //
        // **Drawn, which the game would not do.** Morrowind holsters an undrawn weapon out of sight
        // and only `showWeapons` puts it in a hand; a harness that hid it would be hiding the thing
        // it exists to look at. `--clothes=false` is what takes it off along with everything else.
        const ESM::Weapon* weapon = dressed ? drawnWeapon(world, npc) : nullptr;
        if (weapon != nullptr)
        {
            outfit.claim(ESM::PRT_Weapon, 1, Misc::ResourceHelpers::correctMeshPath(weapon->mModel.getNormalized()));
            const WeaponIdle& stance = sWeaponIdles[static_cast<std::size_t>(weapon->mData.mType)];
            built.mIdle = stance.mStance;
            built.mIdleFallback = stance.mFallback;
        }

        SceneUtil::NodeMap bones;
        SceneUtil::NodeMapVisitor collect(bones);
        built.mRoot->accept(collect);

        for (int slot = 0; slot < ESM::PRT_Count; ++slot)
        {
            const auto part = static_cast<ESM::PartReferenceType>(slot);
            if (outfit.getModel(part).empty())
                continue;

            // Hair is the one slot whose filter is not its bone: it shares the skull's, and the two
            // would take each other's triangles.
            const std::string_view filter = part == ESM::PRT_Hair ? std::string_view("hair") : sBones[slot];
            hang(world, *built.mRoot, bones, outfit.getModel(part), sBones[slot], filter);
        }

        return built;
    }
}
