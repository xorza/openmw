#include "held.hpp"

#include <cassert>

namespace Rtx
{
    CellHolds::HeldModel& CellHolds::know(PreparedModel& model)
    {
        HeldModel& known = mModels.findOrInsert(&model, [&] {
            HeldModel taking = mSpareModels.take();
            taking.mModel = &model;
            taking.mParts.clear();
            taking.mNamed = 0;
            return taking;
        });

        return known;
    }

    CellHolds::HeldModel& CellHolds::knownOf(const PreparedModel& model)
    {
        return mModels.at(&model);
    }

    void CellHolds::adoptParts(HeldModel& held, SceneAdopter& into)
    {
        const PreparedModel& model = *held.mModel;
        held.mParts.reserve(model.mParts.size());

        for (const PreparedPart& part : model.mParts)
        {
            const Index material = into.adoptMaterial(part.mMaterial);
            const Index mesh = into.adoptMesh(*part.mDrawable, model.readingOf(part));

            held.mParts.push_back(AdoptedPart{
                .mMesh = mesh,
                .mMaterial = material,
                .mDrawable = part.mDrawable.get(),
                .mKey = part.mMaterial.mKey,
            });
        }
    }

    void CellHolds::release(PreparedModel& model)
    {
        HeldModel& known = mModels.at(&model);
        assert(known.mNamed > 0 && "a model released by more cells than named it");
        if (--known.mNamed > 0)
            return;

        mReleasing.insert(mReleasing.end(), known.mParts.begin(), known.mParts.end());
        mSpareModels.give(mModels.take(&model));
    }

    void CellHolds::releaseParts(SceneAdopter& into)
    {
        for (const AdoptedPart& part : mReleasing)
        {
            into.releaseMesh(*part.mDrawable);
            into.releaseMaterial(part.mKey);
        }

        mReleasing.clear();
    }

    void CellHolds::forget()
    {
        // The holds on the parts outlive the models they were adopted from, which die with the
        // reader: `AdoptedPart` says why it keeps what the release needs.
        for (HeldModel& held : mModels)
        {
            mReleasing.insert(mReleasing.end(), held.mParts.begin(), held.mParts.end());

            // The row's room is kept for the next world's models, as `release` keeps a row's.
            held.mParts.clear();
            held.mModel = nullptr;
            held.mNamed = 0;
            mSpareModels.give(std::move(held));
        }

        mModels.clear();
    }
}
