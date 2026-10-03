#pragma once

#include <cstdint>
#include <vector>

namespace wowvr
{
    // Rewrites a vertex shader so its fog is computed from the true distance to the
    // camera instead of the depth along the camera's forward axis.
    //
    // Every world shader the client ships (terrain, map objects, M2 models, liquids)
    // ends the same way:
    //
    //     mad  rF, rV.z, cK.x, cK.y      ; fog ramp on view-space DEPTH
    //     (max rF, rF, 0)
    //     pow  rG, rF, cK.z
    //     min  oFog, rG, 1
    //
    // where rV is the vertex's position in the game camera's view space. Depth is
    // the right quantity on a monitor, where nothing is far off-axis, but a headset
    // shows ~114 degrees per eye and turns with the head, so a planar fog leaves a
    // cone straight down the game camera's axis visibly foggier than terrain at the
    // same distance off to the side. Replacing rV.z with length(rV.xyz) makes fog a
    // pure function of distance: identical to the client's own straight ahead, and
    // no longer dependent on where anyone is looking.
    //
    // The rewrite inserts three instructions into a free temporary register just
    // before the fog ramp:
    //
    //     dp3 rT.x, rV, rV
    //     rsq rT.x, rT.x
    //     rcp rT.x, rT.x
    //
    // and points the ramp at rT.x. Nothing else in the shader - position, lighting,
    // shadow coordinates - is touched. Shader models 2.0 and 3.0 only.
    //
    // 'distanceScale' pushes the fog out (2 = fog starts and ends twice as far). Other
    // than 1 it adds 'mul rT.x, rT.x, cK.x' with cK = 1/scale defined INSIDE the shader
    // (def), in a register the shader never reads - below any relatively-addressed
    // array such as the bone palette. A runtime constant was not an option: the client
    // uploads all the way to c255, and skinned models index their bones that far.
    struct FogRewriteResult
    {
        bool rewritten = false;
        const char* reason = "";       // why not, when not
        uint32_t viewRegister = 0;     // rV
        uint32_t scratchRegister = 0;  // rT
    };

    FogRewriteResult RewriteFogToRadial(const uint32_t* tokens, std::vector<uint32_t>& out,
                                        float distanceScale = 1.0f);
}
