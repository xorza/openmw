#version 460

#extension GL_EXT_ray_tracing : require

// The record for ground that kept its layer stack, and nothing more. `visibility.rmiss` says
// why a record that is never invoked has to exist at all.
//
// **Its own file rather than one shader named three times**, because what the sort reads first is
// the shader the record names: three records of one shader may carry one identifier, and then the
// kind of shading ahead would sort nothing. Distinct code is what makes them distinct keys.
//
// Stage 2 is what puts this kind's `resolve` and `shadeSurface` here.

layout(location = 0) rayPayloadInEXT uint answered;

void main()
{
    answered = 2u;
}
