#include <metal_stdlib>
#include <metal_raytracing>

using namespace metal;
using namespace raytracing;

#include "camera.h"
#include "colour.h"
#include "scene.h"
#include "visibility.h"

// Primary visibility. One ray per pixel against the instance structure, shaded by the albedo of
// whatever it hit — the same question `visibility.comp` answers, in the language of the other
// backend. The structures both read are the same file.

/// The tables a hit reads, as one argument rather than fifteen.
///
/// Metal has no program-scope resources, so a kernel's inputs arrive as arguments and anything that
/// reads them takes this. The Vulkan shader is being brought to the same shape for the same reason.
struct Scene
{
    device const GpuInstance* mInstances;
    device const GpuMaterial* mMaterials;
};

kernel void visibility(instance_acceleration_structure scene [[buffer(0)]],
    constant VisibilityConstants& camera [[buffer(1)]], constant Scene& tables [[buffer(2)]],
    device atomic_uint* hits [[buffer(3)]], texture2d<float, access::write> target [[texture(0)]],
    uint2 pixel [[thread_position_in_grid]])
{
    if (pixel.x >= camera.mCamera.mWidth || pixel.y >= camera.mCamera.mHeight)
        return;

    // **The Vulkan shader's own `rayAt`, not a second derivation of it.** Pixel centres, the jitter
    // and both projections all live in `camera.h`, which is written so that both shading languages
    // can compile it — a ray that differed from the other backend's by half a pixel would be two
    // renderers rather than one seen twice.
    const Ray generated = rayAt(camera.mCamera, float2(pixel));
    const float3 direction = float3(generated.mDirection);

    ray probe;
    probe.origin = float3(camera.mOrigin) + float3(generated.mOffset);
    probe.direction = direction;
    probe.min_distance = 0.0f;
    probe.max_distance = camera.mFar;

    intersection_query<instancing, triangle_data> query;
    query.reset(probe, scene, MASK_SOLID | MASK_WATER);

    // Every instance this backend builds is opaque so far, so traversal never stops to ask and this
    // runs to completion in one step. The candidate loop the cutouts need arrives with them.
    while (query.next())
    {
    }

    const bool hit = query.get_committed_intersection_type() == intersection_type::triangle;

    float3 colour;
    if (hit)
    {
        // The instance's index is its position in the array the structure was built from, which is
        // the order `makeInstanceRecords` produced and so the row the tables are keyed by.
        const GpuInstance instance = tables.mInstances[query.get_committed_instance_id()];
        const GpuMaterial material = tables.mMaterials[instance.mMaterial];

        // Untextured surfaces are mid-grey rather than black, so a missing texture reads as missing
        // rather than as shadow. Textured ones arrive with the bindless array.
        colour = float3(0.5f) * float4(material.mDiffuseColour).xyz;

        atomic_fetch_add_explicit(hits, 1u, memory_order_relaxed);
    }
    else
        // **The sun is not in it.** The Vulkan shader answers a miss with `skyRadiance`, which
        // draws the disc on top of this and is what puts a sun in a reflection and a glitter path
        // on water; that arrives with the cone width it is widened by.
        colour = skyGradient(camera.mSkyHorizon, camera.mSkyZenith, direction);

    target.write(float4(encodeSrgb(colour), 1.0f), pixel);
}
