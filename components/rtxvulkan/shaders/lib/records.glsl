#ifndef OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RECORDS_GLSL
#define OPENMW_COMPONENTS_RTXVULKAN_SHADERS_LIB_RECORDS_GLSL

// The records that cross the frame and belong to no walk: what a surface is in the upscaler's
// terms, what a water surface reflects, and a puff's case for a pixel's motion.
//
// **Their own file because the payload and the reprojection read them and nothing else of the
// walks that fill them.** Declared where they were filled, the miss shader compiled the whole of
// the water and the shading to hold a `VisibilityPayload`, and the reprojection pulled in the
// sprite walk and the water for two structs.

/// What a shading model made of a surface, in the terms a temporal upscaler demodulates by.
///
/// **Reported by whatever shaded the pixel rather than guessed after it.** Ray Reconstruction
/// separates a noisy pixel into a diffuse and a specular half using the albedos and the roughness it
/// is handed, so those three have to describe what this renderer actually did — and only the
/// function that did it knows. A constant roughness of one, a permanently zero specular albedo and
/// the *flat quad's* normal for water is a description of a renderer nobody wrote.
struct SurfaceResponse
{
    /// The normal the shading used, which for water is the wave's and not the plane's.
    vec3 mNormal;

    /// What the diffuse half is multiplied by, and nothing else: the surface's own albedo, with
    /// none of what the path took off it between here and the eye.
    vec3 mDiffuse;

    /// What the specular half is multiplied by — the surface's reflectance at this angle.
    vec3 mSpecular;

    /// Nought for a mirror and one for Lambert.
    float mRoughness;
};

/// A pixel with no surface behind it: the sky, or a ray that reached nothing.
SurfaceResponse noResponse()
{
    return SurfaceResponse(vec3(0.0), vec3(0.0), vec3(0.0), 1.0);
}

/// What a water surface reflects, which is not where the water is.
struct WaterMirror
{
    /// Where the reflected surface stands, in world units.
    vec3 mAt;

    /// The direction the reflection left along, for the case where it found no surface at all.
    vec3 mAlong;

    /// Which instance row it came off, so the frame can ask where that used to be.
    uint mInstance;

    /// False where the reflection reached the sky, which is a reflection with no distance to it and
    /// not a reflection of nothing: `mAlong` is the whole of the answer there.
    bool mFound;
};

/// One puff's case for owning a pixel's motion vector.
struct PuffClaim
{
    /// Where the puff stands relative to the eye: the ray's direction times how far along it the
    /// puff was seen. What `puffMotionOf` carries to the previous eye.
    vec3 mToward;

    /// How far it travelled since the last frame, in world units.
    vec3 mMoved;

    /// How strong the case is, in whatever its kind is judged by — the share it hid, or the light
    /// it added. Nought for a claim nothing filled.
    float mWeight;
};

#endif
