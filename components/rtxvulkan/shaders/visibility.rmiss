#version 460

#extension GL_EXT_ray_tracing : require

// The sky's record in the shader binding table, and nothing more.
//
// **A record the sort reads and never a shader that runs.** Stage 1 traces every ray as an inline
// query and executes nothing off a hit object, so this is never invoked — but the index a hit object
// records has to name a record that exists. It named none, and the device was lost rather than
// faulted: `.notes/rtx/ser-plan.md` §7 anticipated the question and the hardware answered it before
// the layers did.
//
// Stage 2 is what puts the sky's shading here.

layout(location = 0) rayPayloadInEXT uint answered;

void main()
{
    answered = 0u;
}
