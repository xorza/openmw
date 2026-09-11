#include "cellholds.hpp"

#include <utility>

namespace Rtx
{
    void CellHolds::holdTexture(const PreparedTexture& texture)
    {
        const osg::Image* const image = texture.mImage.get();
        HeldTexture& held = mTextures.findOrInsert(
            image, [&] { return HeldTexture{ .mImage = image, .mTexture = &texture, .mHolders = 0 }; });

        ++held.mHolders;
    }

    void CellHolds::dropTexture(const PreparedTexture& texture)
    {
        const osg::Image* const image = texture.mImage.get();
        HeldTexture& held = mTextures.at(image);
        if (--held.mHolders == 0)
            mTextures.erase(image);
    }

    const PreparedTexture* CellHolds::find(const osg::Image& image) const
    {
        const HeldTexture* const held = mTextures.find(&image);
        return held != nullptr ? held->mTexture : nullptr;
    }

    CellHolds::HeldModel& CellHolds::know(PreparedModel& model)
    {
        bool made = false;
        HeldModel& known = mModels.findOrInsert(&model, [&] {
            made = true;

            HeldModel taking = mSpareModels.take();
            taking.mModel = &model;
            taking.mParts.clear();
            taking.mHeld = 0;
            taking.mHanded = 0;
            return taking;
        });

        // The images the model names, for `find`: counted per model that names them, so only the
        // first to know it counts.
        if (made)
            for (const PreparedTexture* texture : model.mTextures)
                holdTexture(*texture);

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
            // **The material before the mesh**, as the walk resolves them: a mesh records the
            // material it arrives wearing.
            const Index material = into.adoptMaterial(part.mMaterial);
            const Index mesh = into.adoptMesh(*part.mDrawable, model.readingOf(part), material);

            held.mParts.push_back(AdoptedPart{
                .mMesh = mesh,
                .mMaterial = material,
                .mDrawable = part.mDrawable.get(),
                .mKey = part.mMaterial.mKey,
            });
        }
    }

    void CellHolds::release(PreparedModel& model, const bool wasHeld)
    {
        HeldModel& known = mModels.at(&model);
        if (wasHeld)
            --known.mHeld;
        else
            --known.mHanded;

        if (known.mHeld > 0 || known.mHanded > 0)
            return;

        for (const PreparedTexture* texture : model.mTextures)
            dropTexture(*texture);

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
            mReleasing.insert(mReleasing.end(), held.mParts.begin(), held.mParts.end());

        mModels.clear();
        mTextures.clear();
    }
}
