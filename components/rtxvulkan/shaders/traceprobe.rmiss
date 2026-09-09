#version 460

#extension GL_EXT_ray_tracing : require

// The one record `traceprobe.rgen` names, and nothing more.
//
// **What this exists to prove is that it exists.** A hit object records the index of a record and
// the execute runs it; an index into a table with no such record does not fault and does not fail
// validation — it loses the device. The probe executes it so that a table laid out wrongly is a
// test rather than a rendering mystery, and this is the record it names.

layout(location = 0) rayPayloadInEXT uint answered;

void main()
{
    answered = 0u;
}
