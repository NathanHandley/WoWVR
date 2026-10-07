#include "proxy/device_hooks.h"

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/image_dump.h"
#include "core/log.h"
#include "core/paths.h"
#include "diag/frame_report.h"
#include "game/camera_probe.h"
#include "game/cull_frustum.h"
#include "game/sound_listener.h"
#include "game/billboard_facing.h"
#include "game/view_distance.h"
#include "game/portrait_fix.h"
#include "ui/help_card.h"
#include "game/ui_canvas.h"
#include "game/comfort_vignette.h"
#include "game/world_pointer.h"
#include "game/field_watch.h"
#include "game/game_camera.h"
#include "present/d3d12_present.h"
#include "present/gl_interop.h"
#include "present/presenter.h"
#include "proxy/d3d9_slots.h"
#include "proxy/vtable_hook.h"
#include "render/eye_targets.h"
#include "stereo/fog_rewrite.h"
#include "stereo/projection_patch.h"
#include "stereo/stereo_targets.h"
#include "ui/ui_panel.h"
#include "vr/vr_session.h"

#include <windows.h>

#include <d3d9.h>

#include <cstring>
#include <iterator>
#include <vector>

namespace wowvr
{
    namespace
    {
        typedef HRESULT (WINAPI *CreateDeviceFn)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                 D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
        typedef HRESULT (WINAPI *PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*,
                                            HWND, const RGNDATA*);
        typedef HRESULT (WINAPI *ResetFn)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
        typedef HRESULT (WINAPI *BeginSceneFn)(IDirect3DDevice9*);
        typedef HRESULT (WINAPI *SetTransformFn)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
        typedef HRESULT (WINAPI *SetVertexShaderConstantFFn)(IDirect3DDevice9*, UINT, const float*, UINT);
        typedef HRESULT (WINAPI *CreateVertexShaderFn)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
        typedef HRESULT (WINAPI *SetVertexShaderFn)(IDirect3DDevice9*, IDirect3DVertexShader9*);
        typedef HRESULT (WINAPI *SetFVFFn)(IDirect3DDevice9*, DWORD);
        typedef HRESULT (WINAPI *SetVertexDeclarationFn)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
        typedef HRESULT (WINAPI *SetRenderTargetFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
        typedef HRESULT (WINAPI *SetDepthStencilSurfaceFn)(IDirect3DDevice9*, IDirect3DSurface9*);
        typedef HRESULT (WINAPI *SetViewportFn)(IDirect3DDevice9*, const D3DVIEWPORT9*);
        typedef HRESULT (WINAPI *SetScissorRectFn)(IDirect3DDevice9*, const RECT*);
        typedef HRESULT (WINAPI *ClearFn)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
        typedef HRESULT (WINAPI *SetRenderStateFn)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
        typedef HRESULT (WINAPI *DrawPrimitiveFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
        typedef HRESULT (WINAPI *DrawIndexedPrimitiveFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
        typedef HRESULT (WINAPI *DrawPrimitiveUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
        typedef HRESULT (WINAPI *DrawIndexedPrimitiveUPFn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                           const void*, D3DFORMAT, const void*, UINT);

        CreateDeviceFn g_originalCreateDevice = nullptr;
        PresentFn g_originalPresent = nullptr;
        ResetFn g_originalReset = nullptr;
        BeginSceneFn g_originalBeginScene = nullptr;
        SetTransformFn g_originalSetTransform = nullptr;
        SetVertexShaderConstantFFn g_originalSetVertexShaderConstantF = nullptr;
        CreateVertexShaderFn g_originalCreateVertexShader = nullptr;
        SetVertexShaderFn g_originalSetVertexShader = nullptr;
        SetFVFFn g_originalSetFVF = nullptr;
        SetVertexDeclarationFn g_originalSetVertexDeclaration = nullptr;
        SetRenderTargetFn g_originalSetRenderTarget = nullptr;
        SetDepthStencilSurfaceFn g_originalSetDepthStencilSurface = nullptr;
        SetViewportFn g_originalSetViewport = nullptr;
        SetScissorRectFn g_originalSetScissorRect = nullptr;
        typedef HRESULT (WINAPI *GetRenderTargetFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**);
        typedef HRESULT (WINAPI *GetDepthStencilSurfaceFn)(IDirect3DDevice9*, IDirect3DSurface9**);
        GetRenderTargetFn g_originalGetRenderTarget = nullptr;
        GetDepthStencilSurfaceFn g_originalGetDepthStencilSurface = nullptr;
        ClearFn g_originalClear = nullptr;
        SetRenderStateFn g_originalSetRenderState = nullptr;

        // The device is PUREDEVICE, so GetRenderState is unavailable and anything we
        // want to know about device state has to be tracked as it is set.
        bool g_depthTestEnabled = true;
        bool g_depthWriteEnabled = true;
        bool g_alphaBlendEnabled = false;

        // The interface is drawn into a target of its own that starts fully
        // transparent, and that changes what its blend states mean.
        //
        // On the real back buffer there is no alpha channel to speak of and every draw
        // lands on top of an opaque image. Give the same draws a transparent target and
        // the alpha channel suddenly matters, because it is what later decides how much
        // of the world the panel hides. Two things then go wrong. Ordinary blended draws
        // compute alpha with the same SRCALPHA factor as colour, so coverage comes out
        // squared instead of accumulated. Worse, an additive draw - which is what a
        // highlight or a glow is - writes alpha where it should write none at all: on a
        // back buffer it only ever adds light, but here it manufactures coverage, and
        // that coverage then punches a hole in the world at composite time. A glow whose
        // colour is added but whose alpha says "I cover this" reads as a black patch
        // that fades in and out with the animation.
        //
        // The fix is to keep colour and alpha on separate blend factors while the panel
        // is the target, so the colour channel accumulates premultiplied as it always
        // did and the alpha channel records only genuine coverage.
        DWORD g_srcBlend = D3DBLEND_ONE;
        DWORD g_destBlend = D3DBLEND_ZERO;
        bool g_uiTargetBound = false;
        bool g_uiAlphaOverrideActive = false;
        unsigned long long g_additiveUiDraws = 0;

        // Defined with the other render state handling, further down.
        void ApplyUiAlphaBlend(IDirect3DDevice9* device);
        void EndUiAlphaOverride(IDirect3DDevice9* device);
        DWORD g_depthFunction = D3DCMP_LESSEQUAL;
        DrawPrimitiveFn g_originalDrawPrimitive = nullptr;
        DrawIndexedPrimitiveFn g_originalDrawIndexedPrimitive = nullptr;
        DrawPrimitiveUPFn g_originalDrawPrimitiveUP = nullptr;
        DrawIndexedPrimitiveUPFn g_originalDrawIndexedPrimitiveUP = nullptr;

        // Defined below, once the stereo state it needs is in scope.
        bool StereoActive();
        void ProbeForCombinedTransforms();

        // A genuine world-view residual is a rigid transform, possibly uniformly scaled:
        // its three basis rows are mutually perpendicular and the same length. A window
        // straddling two entries of a 4x3 bone palette satisfies "affine" by accident
        // but never satisfies this, which is what separates terrain's c4 from the bone
        // array at c32 - patching the latter collapsed models into black shards.
        bool IsPlausibleWorldView(const Mat4& m)
        {
            float lengths[3];
            for (int row = 0; row < 3; ++row)
            {
                lengths[row] = sqrtf(m.m[row][0] * m.m[row][0]
                                   + m.m[row][1] * m.m[row][1]
                                   + m.m[row][2] * m.m[row][2]);
                if (!(lengths[row] > 1.0e-4f) || !(lengths[row] < 1.0e4f))
                {
                    return false;
                }
            }

            // Uniform scale across the three axes.
            const float smallest = fminf(lengths[0], fminf(lengths[1], lengths[2]));
            const float largest = fmaxf(lengths[0], fmaxf(lengths[1], lengths[2]));
            if (largest > smallest * 1.05f)
            {
                return false;
            }

            // Mutually perpendicular.
            for (int a = 0; a < 3; ++a)
            {
                for (int b = a + 1; b < 3; ++b)
                {
                    const float dot = m.m[a][0] * m.m[b][0]
                                    + m.m[a][1] * m.m[b][1]
                                    + m.m[a][2] * m.m[b][2];
                    if (fabsf(dot) > 0.02f * lengths[a] * lengths[b])
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        bool IsNearlyIdentity(const Mat4& m)
        {
            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    const float expected = (row == column) ? 1.0f : 0.0f;
                    if (fabsf(m.m[row][column] - expected) > 1.0e-3f)
                    {
                        return false;
                    }
                }
            }
            return true;
        }


        IDirect3DDevice9* g_device = nullptr;
        EyeTargets g_eyeTargets[EyeCount];
        Presenter g_presenter;

        // Which zero-copy route frames take to the compositor, if any. Decided every
        // time the resources are built, so a driver refusing either route just lands
        // us back on the system-memory copy.
        //
        // g_d3d12Active:     the client runs on D3D9On12 and eye textures are lent
        //                    to the compositor as D3D12 resources. The route that
        //                    actually works on this client.
        // g_zeroCopyActive:  D3D9 share handle opened by the D3D11 presenter.
        //                    Needs a D3D9Ex device, so on a native-driver device it
        //                    is only a future-proofing attempt that fails fast.
        // g_glInteropActive: WGL_NV_DX_interop alias submitted as a GL texture.
        //                    Fails on plain D3D9 allocations for the same reason
        //                    share handles do (not shareable), kept for the day the
        //                    device changes character.
        bool g_d3d12Active = false;
        bool g_zeroCopyActive = false;
        bool g_glInteropActive = false;
        D3D12Present g_d3d12Present;
        GlInterop g_glInterop;
        IDirect3DQuery9* g_frameSyncQuery = nullptr;
        bool g_frameSyncQueryFailed = false;

        // Pool census bookkeeping; see the census block further down for why.
        bool g_creatingProxyResource = false;
        enum PoolCensusKind { CensusTexture = 0, CensusVertexBuffer, CensusIndexBuffer, CensusKinds };
        unsigned long long g_poolCounts[CensusKinds][4] = {};

        void CountPool(int kind, D3DPOOL pool)
        {
            if (!g_creatingProxyResource && pool >= D3DPOOL_DEFAULT && pool <= D3DPOOL_SCRATCH)
            {
                ++g_poolCounts[kind][pool];
            }
        }

        bool g_vrInitAttempted = false;
        bool g_resourcesReady = false;
        bool g_resourceFailureLogged = false;
        bool g_dumpNextFrame = false;
        unsigned long long g_frameCount = 0;

        // Back buffer geometry, and whether the currently bound target is it. The
        // shadow map and the glow passes reuse the camera's constant register, so the
        // projection patch needs to know which pass it is looking at.
        uint32_t g_backBufferWidth = 0;
        D3DFORMAT g_backBufferFormat = D3DFMT_UNKNOWN;
        uint32_t g_backBufferHeight = 0;
        bool g_renderingToBackBuffer = true;

        // --- stereo state -------------------------------------------------
        StereoTargets g_stereo;
        IDirect3DSurface9* g_realBackBuffer = nullptr;

        // The two eye projections for the current frame, in the layout the game
        // uploaded, plus where to put them back.
        // Every constant register found to hold the scene camera this frame, with the
        // matrix each eye needs put back into it before that eye's draw.
        struct PatchedBlock
        {
            UINT startRegister = 0;
            float source[16] = {};   // the game's own matrix, kept so the eye versions
                                     // can be rebuilt against a fresh head pose
            bool combined = false;   // holds a world-view-projection, not a projection
            float left[16] = {};
            float right[16] = {};
        };
        // A shadow copy of the vertex shader constant file, holding what the *game*
        // uploaded rather than what we forwarded. Needed because the camera can be read
        // from registers we never see identified at the head of an upload, and the
        // device cannot be queried: it was created PUREDEVICE.
        static constexpr int kShadowRegisters = 256;
        float g_shadowConstants[kShadowRegisters][4] = {};

        // The scene projection exactly as the game supplied it, used to recognise every
        // other register holding the same matrix.
        float g_sceneProjectionRaw[16] = {};
        bool g_haveSceneProjectionRaw = false;

        // The register the scene camera was actually discovered in, by shape, at the head
        // of an upload. Other registers only ever join the table by matching its contents
        // exactly, and those are general-purpose - c0 in particular is the water's
        // combined transform and holds model data for other shaders. The primary must
        // always be restored; the secondaries must not be restored over whatever else
        // now lives there.
        UINT g_primaryCameraRegister = 0xFFFFFFFFu;

        static constexpr int kMaxPatchedBlocks = 8;
        PatchedBlock g_patchedBlocks[kMaxPatchedBlocks];
        int g_patchedBlockCount = 0;
        bool g_haveEyeProjections = false;

        // Registers found to carry a combined world-view-projection. Terrain uses one,
        // and it is re-uploaded for every chunk, so these are re-derived on each upload
        // rather than cached like a plain projection.
        static constexpr int kMaxCombinedRegisters = 8;
        UINT g_combinedRegisters[kMaxCombinedRegisters] = {};
        int g_combinedRegisterCount = 0;
        UINT g_combinedSeen[16] = {};
        int g_combinedSeenCount = 0;
        unsigned long long g_lastProbeFrame = 0;
        int g_rejectionsLogged = 0;

        void RememberPatchedBlock(UINT startRegister, const float* source,
                                  const float* left, const float* right,
                                  bool combined = false)
        {
            PatchedBlock* block = nullptr;
            for (int i = 0; i < g_patchedBlockCount; ++i)
            {
                if (g_patchedBlocks[i].startRegister == startRegister)
                {
                    block = &g_patchedBlocks[i];
                    break;
                }
            }

            if (block == nullptr)
            {
                if (g_patchedBlockCount >= kMaxPatchedBlocks)
                {
                    return;
                }
                block = &g_patchedBlocks[g_patchedBlockCount++];
                block->startRegister = startRegister;
            }

            if (Report().IsActive() || Cfg().logWorldDrawTo > 0)
            {
                WOWVR_INFO("  camera register c%u registered as %s (table now %d)",
                           startRegister, combined ? "COMBINED" : "plain",
                           g_patchedBlockCount);
            }

            block->combined = combined;
            memcpy(block->source, source, sizeof(block->source));
            memcpy(block->left, left, sizeof(block->left));
            memcpy(block->right, right, sizeof(block->right));
            g_haveEyeProjections = true;
        }

        // Rebuilds every known camera register against the current head pose. Needed
        // because WoW draws terrain *before* re-uploading its camera each frame: those
        // draws would otherwise inherit the previous frame's left-eye matrix, identical
        // in both eyes and a frame stale, which reads as terrain welded to the head.
        bool MatricesMatch(const float* a, const float* b)
        {
            for (int i = 0; i < 16; ++i)
            {
                const float scale = (fabsf(b[i]) > 1.0f) ? fabsf(b[i]) : 1.0f;
                if (fabsf(a[i] - b[i]) > 1.0e-5f * scale)
                {
                    return false;
                }
            }
            return true;
        }

        // Finds every register currently holding the scene camera, by exact comparison
        // against the matrix the game supplied - not by guessing at matrix shape, which
        // matched thousands of unrelated constants per frame when it was tried.
        void FindAllCameraRegisters()
        {
            if (!g_haveSceneProjectionRaw)
            {
                return;
            }

            int found = 0;
            for (int reg = 0; reg + 4 <= kShadowRegisters; ++reg)
            {
                if (!MatricesMatch(&g_shadowConstants[reg][0], g_sceneProjectionRaw))
                {
                    continue;
                }
                ++found;

                float left[16];
                float right[16];
                if (Projection().TryPatch(&g_shadowConstants[reg][0], left, right))
                {
                    RememberPatchedBlock(static_cast<UINT>(reg), &g_shadowConstants[reg][0],
                                         left, right);
                }
            }

            if (Report().IsActive())
            {
                WOWVR_INFO("  end-of-frame camera sweep: %d register(s) hold the scene "
                           "matrix, table now %d", found, g_patchedBlockCount);
            }
        }

        // Diagnostic: is any register holding a combined world-view-projection built on
        // the camera we know? If M = WV * P then M * inverse(P) is affine, which is a
        // property no unrelated constant satisfies by accident.
        void ProbeForCombinedTransforms()
        {
            static int probeRuns = 0;
            if (++probeRuns <= 6)
            {
                WOWVR_INFO("  combined probe run %d at frame %llu (scene matrix known: %s)",
                           probeRuns, g_frameCount, g_haveSceneProjectionRaw ? "yes" : "no");
            }

            if (!g_haveSceneProjectionRaw)
            {
                return;
            }

            Mat4 sceneRaw;
            memcpy(&sceneRaw.m[0][0], g_sceneProjectionRaw, sizeof(sceneRaw.m));

            // The upload may be transposed; try the matrix both ways round.
            Mat4 candidates[2] = { sceneRaw, Mat4Transpose(sceneRaw) };

            for (int variant = 0; variant < 2; ++variant)
            {
                Mat4 inverse;
                if (!Mat4Inverse(candidates[variant], inverse))
                {
                    continue;
                }

                int hits = 0;
                for (int reg = 0; reg + 4 <= kShadowRegisters && hits < 6; ++reg)
                {
                    Mat4 m;
                    memcpy(&m.m[0][0], &g_shadowConstants[reg][0], sizeof(m.m));

                    const Mat4 residual = Mat4Multiply(m, inverse);
                    const Mat4 residualT = Mat4Multiply(Mat4Transpose(m), inverse);

                    // A register holding the camera itself passes trivially: P * P⁻¹ is
                    // the identity, which is affine. Only a residual carrying a real
                    // world-view component counts.
                    // Registers from c31 upward are the bone palette: the frame report
                    // shows uploads from c31 in multiples of three, i.e. 4x3 matrices.
                    // A four-register window there straddles two bones and satisfies the
                    // affine test by coincidence - that is what c32 was, and patching it
                    // collapsed models into black shards. Terrain's own transform sits
                    // well below that.
                    const bool inBonePalette = reg >= 28;

                    const bool direct = !inBonePalette && Mat4IsAffine(residual, 1.0e-3f)
                        && !IsNearlyIdentity(residual);
                    const bool flipped = !inBonePalette && Mat4IsAffine(residualT, 1.0e-3f)
                        && !IsNearlyIdentity(residualT);

                    // Anything affine and non-identity is reported with its measured
                    // properties, so the acceptance thresholds can be set from data
                    // instead of guessed at.
                    if (!direct && !flipped)
                    {
                        const Mat4& r = Mat4IsAffine(residual, 1.0e-3f) ? residual : residualT;
                        if ((Mat4IsAffine(residual, 1.0e-3f) || Mat4IsAffine(residualT, 1.0e-3f))
                            && !IsNearlyIdentity(r) && g_rejectionsLogged < 8)
                        {
                            ++g_rejectionsLogged;
                            float len[3];
                            for (int row = 0; row < 3; ++row)
                            {
                                len[row] = sqrtf(r.m[row][0]*r.m[row][0]
                                               + r.m[row][1]*r.m[row][1]
                                               + r.m[row][2]*r.m[row][2]);
                            }
                            const float d01 = r.m[0][0]*r.m[1][0] + r.m[0][1]*r.m[1][1] + r.m[0][2]*r.m[1][2];
                            WOWVR_INFO("  c%-3d affine residual rejected: row lengths %.4f %.4f %.4f, "
                                       "dot01 %.5f", reg, len[0], len[1], len[2], d01);
                        }
                    }

                    if (direct || flipped)
                    {
                        ++hits;

                        bool known = false;
                        for (int k = 0; k < g_combinedRegisterCount; ++k)
                        {
                            if (g_combinedRegisters[k] == static_cast<UINT>(reg))
                            {
                                known = true;
                                break;
                            }
                        }
                        // Accepted on sight. Requiring a second sighting was tried and
                        // simply prevented discovery: the probe mostly runs before the
                        // world is loaded, so terrain's register is rarely seen twice
                        // before it is needed. The register-range exclusion above is
                        // what keeps the bone palette out.
                        if (!known && g_combinedRegisterCount < kMaxCombinedRegisters)
                        {
                            g_combinedRegisters[g_combinedRegisterCount++] = static_cast<UINT>(reg);
                            WOWVR_INFO("  c%-3d carries a combined world-view-projection; "
                                       "rebuilding it per eye", reg);
                        }
                    }
                }
                if (hits > 0)
                {
                    WOWVR_INFO("  combined-transform probe, P variant %d: %d register(s)",
                               variant, hits);
                }
            }
        }

        void RefreshPatchedBlocks()
        {
            for (int i = 0; i < g_patchedBlockCount; ++i)
            {
                PatchedBlock& block = g_patchedBlocks[i];
                if (block.combined)
                {
                    Projection().TryPatchCombined(block.source, block.left, block.right);
                }
                else
                {
                    Projection().TryPatch(block.source, block.left, block.right);
                }
            }
        }

        // The same thing again for the fixed-function pipeline. Not every world draw
        // uses a vertex shader: a couple per frame go through SetTransform instead, and
        // patching only the shader constant leaves those rendering with the game's own
        // narrow camera while everything around them uses the headset's - which is what
        // put a large black shape through the middle of the view.
        float g_eyeFixedProjection[EyeCount][16] = {};
        bool g_haveFixedEyeProjections = false;

        // The game sets the fixed-function projection only occasionally, not every
        // frame, so the per-eye versions cannot be rebuilt from the SetTransform call
        // alone - they would be missing on most frames, leaving the sky to draw with a
        // stale matrix at the wrong place and the wrong depth. The original is kept and
        // the eye matrices are rebuilt from it whenever the head moves.
        float g_currentFixedProjection[16] = {};
        bool g_haveFixedProjectionSource = false;

        // Colour writes off means the game is drawing something it intends to be
        // invisible - an occlusion-query box, most often. Such a draw becoming visible
        // is a state-preservation bug, not a transform bug.
        DWORD g_colorWriteMask = 0x0F;
        bool g_alphaTestEnabled = false;

        // Whether the projection currently set is orthographic. Screen-space geometry
        // cannot follow the head no matter what is done to the eye transform.
        bool g_fixedProjectionIsOrtho = false;

        IDirect3DVertexShader9* g_currentVertexShader = nullptr;
        IDirect3DPixelShader9* g_currentPixelShader = nullptr;

        // Set while the frame is being rendered into the side-by-side target. Guards
        // against duplicating draws that belong to the shadow or post passes.
        bool g_stereoRedirected = false;
        bool g_duplicatingDraw = false;
        bool g_userWasPresent = false;

        // The frame is world-then-UI. Counting back buffer *binds* to find the split
        // does not work: the game binds it once before the shadow pass without drawing
        // anything, so the world is the third bind, not the second. Counting binds
        // that actually receive draws is stable, because the empty ones drop out.
        int g_backBufferDrawPeriod = 0;
        bool g_wasDrawingToBackBuffer = false;
        bool g_uiPassStarted = false;

        // Whether the stereo target is bound right now, and whether the world was
        // rendered into it at all this frame. These differ once the UI pass moves the
        // render target away, and the frame still needs capturing.
        bool g_stereoHasContent = false;

        UiPanel g_uiPanel;
        bool g_uiRendered = false;

        // Whether the last frame actually put the interface on the panel. World pointing
        // maps the mouse through the panel, so it must stand down whenever the
        // interface is anywhere else - a flat frame, a loading screen.
        bool g_panelCompositedLastFrame = false;

        // The interface's readback for the SteamVR overlay (Config::panelOverlay): same
        // size as the interface texture, pipelined like the eyes.
        EyeTargets g_uiTarget;
        bool g_dumpMirrorNext = false;

        // Where the terrain shader's fog ramp reaches full fog, from its fog constants
        // (c12: factor = (depth * x + y) ^ z, so zero at depth = -y / x). Diagnostic, to
        // see whether the client moves its fog with the draw distance.
        float g_terrainFogEnd = 0.0f;

        // Radial fog bookkeeping, for the periodic log.
        unsigned long long g_fogShadersRewritten = 0;
        unsigned long long g_fogShadersLeft = 0;
        unsigned long long g_fogRewriteRefused = 0;
        const char* g_fogLastSkipReason = "none";
        unsigned long long g_panelDrawn = 0;
        unsigned long long g_panelSkipped = 0;
        const char* g_lastSkipReason = "none";

        // Draw routing counters, summed over the reporting interval.
        unsigned long long g_drawsToStereo = 0;
        unsigned long long g_drawsToUi = 0;
        unsigned long long g_drawsOffscreen = 0;

        // Diagnostic: which offscreen targets those draws went to, by surface.
        struct OffscreenTarget
        {
            IDirect3DSurface9* surface;
            uint32_t width;
            uint32_t height;
            uint32_t format;
            unsigned long long draws;
            unsigned long long binds;
        };
        const int kOffscreenTargets = 12;
        OffscreenTarget g_offscreenTargets[kOffscreenTargets] = {};
        int g_offscreenTargetCount = 0;
        int g_currentOffscreenTarget = -1;
        unsigned long long g_drawsStrayBackBuffer = 0;
        int g_lastPeriodCount = 0;
        unsigned long long g_skySliceDraws = 0;

        // Upload sizes, to test whether a low sun really does push the constant blocks
        // past the bounds of the scratch buffers.
        UINT g_maxVector4Count = 0;
        UINT g_maxStartPlusCount = 0;
        unsigned long long g_oversizeUploads = 0;
        unsigned long long g_beyondShadowMirror = 0;

        // True while the client is rendering into its shadow map. The pass is recognised
        // by the render target being the big square offscreen surface it clears twice a
        // frame for its two cascades.
        bool g_renderingShadowMap = false;
        uint32_t g_shadowMapSize = 0;
        unsigned long long g_shadowCascadesWidened = 0;
        IDirect3DSurface9* g_shadowSurface = nullptr;
        bool g_dumpShadowMapNext = false;

        // Numbered eye-buffer captures, so a sweep of the horizon keeps every frame.
        bool g_dumpNumbered = false;
        int g_dumpSeriesIndex = 0;
        int g_shadowMapDumpIndex = 0;
        unsigned long long g_shadowMapDraws = 0;
        unsigned long long g_shadowMapPrims = 0;
        IDirect3DVertexShader9* g_shadowCasterShaders[16] = {};
        int g_shadowCasterShaderCount = 0;

        // The light's orthographic matrix, captured while the shadow map is rendered.
        // The world pass has to sample through the same transform, so finding it there
        // is a matter of matching against this.
        float g_lightOrtho[16] = {};
        bool g_haveLightOrtho = false;
        unsigned long long g_lightOrthoSeenInWorld = 0;
        unsigned long long g_shadowCascadeRowsScaled = 0;
        unsigned long long g_depthSubstituted = 0;
        int g_shadowSamplerLogged = 0;
        unsigned long long g_eyeStateRestored = 0;
        unsigned long long g_renderTargetQueriesRedirected = 0;
        unsigned long long g_postProcessDrawsSkipped = 0;
        int g_skippedDrawsLogged = 0;
        IDirect3DSurface9* g_gameDepthSurface = nullptr;
        bool g_currentTargetIsStereo = false;
        unsigned long long g_setRenderTargetFailures = 0;
        unsigned long long g_depthSubstitutionDeclined = 0;

        // Which registers the upload path actually rewrites, split by how they were
        // identified. A register being rewritten that has no business being a camera is
        // the whole question here.
        unsigned long long g_substShape[kShadowRegisters] = {};
        unsigned long long g_substExact[kShadowRegisters] = {};
        unsigned long long g_substCombined[kShadowRegisters] = {};

        // The game uses the hardware cursor, which never reaches the render target, so
        // in the headset there is nothing to aim with. This lifts the real bitmap out
        // of Windows and draws it onto the panel, so the pointer changes shape with
        // context - gauntlet, cast, loot, interact - exactly as it does on the desktop.
        HWND g_gameWindow = nullptr;

        struct CursorArt
        {
            HCURSOR source = nullptr;
            IDirect3DTexture9* texture = nullptr;   // null means "this shape would not build"
            int width = 0;
            int height = 0;
            int hotspotX = 0;
            int hotspotY = 0;
        };

        // A small fixed cache. The game cycles through a handful of shapes and each one
        // costs several GDI round trips to build, which is not something to be doing in
        // the middle of a frame every time the pointer crosses a button.
        //
        // The client creates a NEW cursor handle every time its pointer art changes -
        // 32 distinct handles in about thirty seconds of play - and destroys the old
        // ones, so a handle value can come back later meaning a different picture. A
        // cache keyed on handles therefore cannot be a lookup table. It is a small ring
        // instead: art is rebuilt whenever the handle differs from the one drawn last,
        // and reused only while it stays the same. (The old fixed table of 32 filled up
        // and then served "whatever was drawn last" forever, which after one hidden
        // frame - mouselook - was nothing: the pointer vanished for the session.)
        const int kCursorCacheSize = 4;
        CursorArt g_cursorCache[kCursorCacheSize];
        int g_cursorCacheCount = 0;
        int g_cursorCacheNext = 0;
        HCURSOR g_lastLookupShape = nullptr;
        int g_lastLookupSlot = -1;
        unsigned long long g_cursorArtBuilds = 0;
        const CursorArt* g_activeCursor = nullptr;
        unsigned long long g_cursorDrawn = 0;
        float g_lastCursorU = -1.0f;
        float g_lastCursorV = -1.0f;

        // Telling the game's own pointer art apart from Windows' standard cursors.
        //
        // GetCursorInfo reports a single global cursor, and that is only ever whatever
        // the last window to handle WM_SETCURSOR asked for. Alt-tab away and it becomes
        // some other application's; come back, and the game only puts its own back when
        // Windows next sends it WM_SETCURSOR - which does not reliably happen if the
        // pointer has not moved. On the desktop nobody notices, because the first mouse
        // movement corrects it. In the headset we draw from that same stale state, so
        // it just stays wrong.
        //
        // Asking the client instead does not work: it calls SetCursor(NULL) and applies
        // its real art by some other route, so there is nothing useful to intercept.
        // What does work is that the shared system cursors have the same handle in every
        // process, and the client never uses one inside its own window. So a standard
        // handle appearing while the game holds focus means the global state is stale
        // rather than that the pointer genuinely changed, and the last art the game did
        // own is a far better answer than the arrow Windows happens to be showing.
        HCURSOR g_systemCursors[16] = {};
        int g_systemCursorCount = 0;
        HCURSOR g_lastGameCursor = nullptr;
        unsigned long long g_staleCursorSubstitutions = 0;

        // The client's own "cursor visible" switch: CGxDevice +0x2950, written only by
        // its cursor-visibility method (0x00683640 via 0x0068E750) and read by its
        // WM_SETCURSOR handler at 0x006A056B. The device is the global at 0x00C5DF88.
        //
        // This is the authority on whether the pointer should show. Windows' own
        // answer (GetCursorInfo's CURSOR_SHOWING) also goes false when "Hide pointer
        // while typing" is on and a key is pressed - i.e. constantly while moving with
        // WASD - and stays false until the mouse moves, which is what made the pointer
        // vanish from the panel mid-play. The client never hid it; Windows did.
        const uintptr_t kGxDevicePtrRva = 0x00C5DF88u - 0x00400000u;
        const uintptr_t kGxCursorVisible = 0x2950u;
        unsigned long long g_vanishSubstitutions = 0;
        int g_lastGameCursorFlag = -1;

        // 1 shown, 0 hidden, -1 unreadable (no device yet, or not this client).
        int ReadGameCursorVisible()
        {
            __try
            {
                const uintptr_t image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                const uintptr_t device = *reinterpret_cast<const uintptr_t*>(image + kGxDevicePtrRva);
                if (device == 0)
                {
                    return -1;
                }
                const uint32_t value = *reinterpret_cast<const uint32_t*>(device + kGxCursorVisible);
                if (value > 1u)
                {
                    return -1;   // not a flag: wrong object, so do not trust it
                }
                return static_cast<int>(value);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        // Whether the pointer is currently confined to the game window, so ClipCursor
        // is only called when the answer actually changes.
        bool g_cursorConfined = false;
        RECT g_confinedTo = {};
        bool g_calibrateKeyDown = false;

        bool GameHasFocus()
        {
            return g_gameWindow != nullptr && GetForegroundWindow() == g_gameWindow;
        }

        // Injected input has to be spread across frames, not sent in a burst.
        //
        // A drag sent as fifteen moves inside one Present call does nothing at all: the
        // client's main thread is blocked in that very call, so it never sees the pointer
        // in any intermediate position and only picks up the end state after the button
        // has already come back up. Wheel clicks survive the same treatment because they
        // are discrete events that queue, which is exactly why the wheel appeared to work
        // while the drag looked like a dead camera. The calibration's own nudges were
        // right all along - one move per frame - and this is the same thing for the
        // command-driven diagnostics.
        int g_injectDragFrames = 0;
        bool g_injectDragging = false;
        int g_injectDragDx = 8;
        int g_injectDragDy = 0;
        int g_injectWheelClicks = 0;
        int g_injectWheelDelta = -120;

        // A head turn stood in for, so the orbit correction can be measured on the desktop.
        float g_fakeHeadYaw = 0.0f;
        // "headsweep <amplitude rad> <period s>": the fake yaw swung as a sine every frame,
        // because a real head is never still and the command file is only read every 15
        // frames - too coarse to stand in for one. "headsweep 0" stops it.
        float g_sweepAmplitude = 0.0f;
        float g_sweepPeriod = 2.0f;
        LARGE_INTEGER g_sweepStart = {};
        bool g_compensateOrbit = true;
        bool g_aimEnabled = true;
        bool g_pitchTrace = false;

        // Runtime switch for the head-driven cull frustum, so the two arms of an A/B can
        // be compared inside one session rather than across two launches. Kept separate
        // from the config flag rather than folded into it: a bool ORed with a setting made
        // "on" a silent no-op the last time culling was being tested, and both arms of
        // that A/B were the same state.
        bool g_cullRotateEnabled = true;

        // The runtime half of Cfg().cullTerrainAllAround, so the terrain cone can be
        // A/B'd from the command file without a rebuild, same as the rotation itself.
        bool g_cullTerrainOpen = true;

        // Command-only diagnostic: whether the CWorld terrain-culling bit is being held
        // clear (the "cullterrainbit" toggle). Never persisted to config on purpose.
        bool g_terrainCullBitCleared = false;

        // Command-only experiment: whether the master corner array at 0x00CDB108 is
        // being stomped (the "cullworldplanes" toggle - kept as the repro that found it).
        bool g_worldPlanesOpen = false;

        // The runtime half of Cfg().cullRotateMasterCorners, so the master rotation can
        // be A/B'd from the command file without a rebuild.
        bool g_cullMasterRotate = true;

        // One-shot latch for Cfg().cullMasterShadowFeeds: the displacement patches are
        // applied once when the frustum hook is up, then owned by the cullfeed
        // commands. Re-applying the config mask every frame would fight the bisection
        // levers the mask exists to persist.
        bool g_masterFeedsApplied = false;

        // The runtime half of Cfg().wmoGroupsAlwaysVisible (the "cullwmoall" toggle).
        bool g_wmoGroupsAllVisible = true;
        // Pushed once rather than every frame so that a command can change the threshold
        // mid-session and not be overwritten by the config on the very next frame.
        bool g_cullSpanConfigured = false;
        bool g_directChainEverWorked = false;

        // The same runtime switch for the head-driven sound listener, and for the same
        // reason: the difference between ears on the head and ears on the camera is a
        // thing you judge by listening to it twice in a row, not across two launches.
        bool g_headListenerEnabled = true;

        // The INI's ListenerFollowsHeadPosition is the STARTING state of a switch a
        // command can move afterwards, so it is pushed once rather than every frame -
        // re-asserting a setting each frame would silently undo "sndmove 0" a few
        // milliseconds after it was typed.
        bool g_headListenerConfigured = false;

        // Stepped pitch aiming. The camera is a culling device, not the view - the
        // view takes its pitch from the head matrix regardless of what the camera
        // holds. So the camera only needs to be WITHIN the widened frustum's vertical
        // margin of the head, not equal to it, and re-aiming it every frame just
        // churns the client's camera update against the compensation - the
        // 'correction loop' a nod exposes. Held at a 30-degree band centre instead,
        // and moved only when the head pitch leaves the band by a margin: ordinary
        // nodding then writes nothing at all, and the client's camera sits perfectly
        // still while the head does the moving.
        bool g_pitchStepEnabled = true;
        float g_pitchAimHeld = 0.0f;

        float SteppedAimPitch(float headPitch)
        {
            if (!g_pitchStepEnabled)
            {
                return headPitch;
            }

            const float kBand = 0.5236f;   // 30 degrees between band centres
            const float kSlack = 0.105f;   // 6 degrees past the edge before re-aiming

            if (fabsf(headPitch - g_pitchAimHeld) > kBand * 0.5f + kSlack)
            {
                const float previous = g_pitchAimHeld;
                g_pitchAimHeld = kBand * floorf(headPitch / kBand + 0.5f);
                WOWVR_INFO("Pitch band re-aim: %+.4f -> %+.4f (head %+.4f).",
                           previous, g_pitchAimHeld, headPitch);
            }
            return g_pitchAimHeld;
        }
        // Vertical aiming, on now that the pitch field is read out of the client's own code
        // rather than guessed at. Aspect ratio buys width and cannot buy height, so moving
        // the client's 58.9-degree vertical band with the head is the only way there is
        // geometry above and below where you look.
        bool g_aimPitch = true;
        bool g_signatureSweepDone = false;
        bool g_cameraDumped = false;

        // Defeats the client's camera collision by holding the live distance at its
        // setting. Off permanently: measured being overwritten in the same frame.
        bool g_pinOrbitRadius = false;

        // Switches off the client's camera collision outright, in its own code. On by
        // default: with collision in play a head pitch moves the third-person camera by
        // fourteen yards, and nothing downstream can put that back.
        // Runtime switch, ANDed with the config one at the point of use - reading Cfg() here
        // would run before the ini has been loaded.
        bool g_disableCameraCollision = true;

        // 0 leaves the client's own value alone.
        float g_cullFovOverride = 0.0f;
        float g_cullAspectOverride = 0.0f;
        int g_inWorldFrames = 0;
        int g_calibrateAfterFrames = 0;
        int g_locateAttempts = 0;

        // Puts the pointer in the middle of the game window before any injected drag or
        // wheel click.
        //
        // Injected mouse input goes wherever the pointer happens to be, and on a wide
        // desktop the client's window occupies only the middle of it - so a pointer left
        // near the left edge is outside the game entirely and every drag and wheel click
        // lands on whatever is behind it. That failure is silent and looks exactly like
        // the camera refusing to move, which cost two runs of chasing a stale address
        // that was never stale.
        void CentreCursorOnGame()
        {
            if (g_gameWindow == nullptr)
            {
                return;
            }

            RECT rect = {};
            if (!GetClientRect(g_gameWindow, &rect))
            {
                return;
            }

            POINT centre;
            centre.x = (rect.right - rect.left) / 2;
            centre.y = (rect.bottom - rect.top) / 2;
            if (ClientToScreen(g_gameWindow, &centre))
            {
                SetCursorPos(centre.x, centre.y);
            }
        }

        // One step of whatever injection is outstanding. Called once per frame.
        void ServiceInjectedInput()
        {
            if (g_injectWheelClicks > 0)
            {
                mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                            static_cast<DWORD>(g_injectWheelDelta), 0);
                --g_injectWheelClicks;
                return;
            }

            if (g_injectDragFrames <= 0)
            {
                return;
            }

            if (!g_injectDragging)
            {
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                g_injectDragging = true;
            }

            mouse_event(MOUSEEVENTF_MOVE, g_injectDragDx, g_injectDragDy, 0, 0);

            if (--g_injectDragFrames <= 0)
            {
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                g_injectDragging = false;
            }
        }

        // Pre-transformed vertices carry absolute screen coordinates and ignore the
        // view and projection entirely. Duplicating such a draw per eye does not move
        // it, so it lands in the same place both times and is merely clipped
        // differently - which is what a hard edge at the game's own width looks like.
        UINT g_lastPrimitiveCount = 0;
        bool g_projectionPatchedThisFrame = false;
        bool g_previousFrameHadCamera = true;
        bool g_probeCombinedPending = false;
        bool g_stateBlockFailureLogged = false;
        bool g_sequenceDumpArmed = false;
        bool g_armSequenceNextFrame = false;
        int g_worldDrawIndex = 0;
        int g_sequenceDumpsWritten = 0;

        bool g_vertexFormatIsPreTransformed = false;
        unsigned long long g_preTransformedWorldDraws = 0;

        // Scissor state. The scissor rectangle is in the game's own screen space, so
        // inside the wider side-by-side target it clips everything to the game's
        // 1920x1080 corner no matter what viewport a draw is given.
        D3DVIEWPORT9 g_currentViewport = {};

        // The depth range the game itself asked for, kept apart from the eye viewport
        // we impose. BeginEye overwrites g_currentViewport, so it cannot double as the
        // record of what the game wanted.
        float g_depthRanges[16][2] = {};
        int g_depthRangeCount = 0;
        // The viewport the game last asked for, kept separate from g_currentViewport
        // because BeginEye overwrites that one with the eye rect.
        D3DVIEWPORT9 g_gameViewport = {};
        float g_gameViewportMinZ = 0.0f;
        float g_gameViewportMaxZ = 1.0f;
        RECT g_gameScissor = {};
        bool g_haveGameScissor = false;
        bool g_scissorTestEnabled = false;
        unsigned long long g_scissoredWorldDraws = 0;


        // Frame cost accounting. The copy-based presenter moves a whole eye buffer
        // through system memory every frame, and the only way to know whether that is
        // affordable at the headset's refresh rate is to measure it.
        struct FrameTimers
        {
            double captureMs = 0.0;
            double uploadMs = 0.0;
            double submitMs = 0.0;
            double waitMs = 0.0;
            unsigned samples = 0;
        };

        FrameTimers g_timers;

        // Game-thread frame pacing, Present to Present, with where a slow frame went.
        struct FramePacing
        {
            LARGE_INTEGER last = {};
            double maxIntervalMs = 0.0;
            double sumIntervalMs = 0.0;
            unsigned over10 = 0;
            unsigned over20 = 0;
            unsigned over50 = 0;
            unsigned samples = 0;
            double maxPresentMs = 0.0;      // the real Present (DXVK / driver)
            double maxWorkerWaitMs = 0.0;   // waiting for the presenter worker's Submit
            double maxPoseWaitMs = 0.0;     // WaitGetPoses
            double sumPresentMs = 0.0;
            double sumWorkerWaitMs = 0.0;
        };
        FramePacing g_pacing;

        // Lifebars in the world (Nameplates3D): the strip's layout and the texture it is
        // cut into. See DrawWorldPlates.
        IDirect3DTexture9* g_plateTexture = nullptr;
        IDirect3DSurface9* g_plateSurface = nullptr;
        uint32_t g_plateTextureWidth = 0;
        uint32_t g_plateTextureHeight = 0;
        RECT g_plateStripRect = {};
        bool g_plateStripActive = false;
        float g_plateCellU = 0.0f;
        float g_plateCellV = 0.0f;
        float g_plateStripTopV = 0.0f;
        float g_plateStripHeightV = 0.0f;
        unsigned long long g_platesDrawn = 0;
        // How far down its cell a lifebar's anchor is put. The client hangs the lifebar
        // below its anchor (measured: name and bar span about the next 45 px at 1.5x),
        // so near the top leaves room under it for a cast bar.
        const float kPlateAnchorV = 0.15f;

        // The adaptive half-rate governor's inputs and state. 'Work' is a frame's
        // Present-to-Present interval minus the time spent idle in WaitGetPoses, i.e.
        // how long the frame would take if the compositor never held it back. That
        // stays meaningful while throttled, which is what lets it decide to go back.
        struct HalfRateGovernor
        {
            double lastIdleMs = 0.0;      // WaitGetPoses idle of the previous frame
            unsigned window = 0;
            unsigned late = 0;            // frames whose work missed the full-rate budget
            unsigned nearlyLate = 0;      // ... or came within 10% of it
            double lastSwitchSeconds = 0.0;
            unsigned long long switches = 0;
        };
        HalfRateGovernor g_halfRate;
        volatile LONGLONG g_presenterLastPoseWaitUs = 0;

        LARGE_INTEGER g_qpcFrequency = {};

        // The HMD pose this frame and last frame were rendered with. The pipelined
        // readback uploads last frame's pixels, and the submit stamps them with
        // last frame's pose so the compositor reprojects by the true delta.
        HeadPoseStamp g_renderPoseCurrent;
        HeadPoseStamp g_renderPosePrevious;

        double ElapsedMs(const LARGE_INTEGER& from, const LARGE_INTEGER& to)
        {
            if (g_qpcFrequency.QuadPart == 0)
            {
                QueryPerformanceFrequency(&g_qpcFrequency);
            }
            return 1000.0 * static_cast<double>(to.QuadPart - from.QuadPart)
                 / static_cast<double>(g_qpcFrequency.QuadPart);
        }

        LARGE_INTEGER Now()
        {
            LARGE_INTEGER value;
            QueryPerformanceCounter(&value);
            return value;
        }

        // The presenter thread: takes the locked previous-frame staging pointers
        // and runs the D3D11 upload + compositor submit off the game thread. The
        // game thread owns every D3D9 call and WaitGetPoses; the worker owns the
        // D3D11 context and Submit while a job is in flight. The done event is
        // manual-reset and doubles as the idle gate - the game thread waits on it
        // before unlocking the staging surfaces the worker was reading and before
        // any device teardown touches the presenter's textures.
        struct PresenterJob
        {
            const void* pixels[EyeCount];
            uint32_t pitch[EyeCount];
            bool stereo;
            HeadPoseStamp pose;

            // The interface overlay: pixels to upload and show, or hide it.
            const void* uiPixels;
            uint32_t uiPitch;
            uint32_t uiWidth;
            uint32_t uiHeight;
            float overlayPose[3][4];
            float overlayWidth;
            float overlayCurvature;
            bool overlayPremultiplied;
            bool hideOverlay;
        };

        PresenterJob g_presenterJob = {};
        HANDLE g_presenterJobEvent = nullptr;    // auto-reset: a job is ready
        HANDLE g_presenterDoneEvent = nullptr;   // manual-reset: worker is idle
        // Manual-reset: the job's frame is through the compositor - eyes submitted,
        // PostPresentHandoff, and the WaitGetPoses for the next frame, all on the
        // worker and in that order - so fresh poses are ready for the game thread.
        // The interface overlay upload that follows does not involve the
        // compositor's frame protocol and overlaps the game's next frame.
        HANDLE g_presenterSubmittedEvent = nullptr;
        // Set by SubmitFrame when it handed this frame to the worker, so the
        // worker - not the game thread - makes this frame's WaitGetPoses call.
        bool g_presenterJobPostedThisFrame = false;
        volatile LONGLONG g_presenterPoseWaitUs = 0;
        HANDLE g_presenterThreadHandle = nullptr;
        bool g_presenterThreadOn = true;         // runtime half of Cfg().presenterThread
        bool g_presenterThreadConfigApplied = false;
        bool g_presenterThreadBroken = false;

        // Worker-side frame cost, accumulated in integer microseconds because the
        // game thread reads and resets them from ReportFrameTimings.
        volatile LONGLONG g_presenterUploadUs = 0;
        volatile LONGLONG g_presenterSubmitUs = 0;
        volatile LONGLONG g_presenterOverlayUs = 0;
        volatile LONG g_presenterSamples = 0;

        // Uploads and shows the interface overlay, or hides it. On whichever thread owns
        // the presenter's D3D11 context for this frame.
        void ApplyInterfaceOverlay(const PresenterJob& job)
        {
            if (job.uiPixels != nullptr)
            {
                if (g_presenter.UploadOverlay(job.uiPixels, job.uiPitch, job.uiWidth, job.uiHeight))
                {
                    Vr().ShowInterfaceOverlay(g_presenter.OverlayTexture(), job.overlayPose,
                                              job.overlayWidth, job.overlayCurvature,
                                              job.overlayPremultiplied);
                }
            }
            else if (job.hideOverlay)
            {
                Vr().HideInterfaceOverlay();
            }
        }

        void RunPresenterJob(const PresenterJob& job)
        {
            const LARGE_INTEGER uploadStart = Now();
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                if (!job.stereo && eye == EyeRight)
                {
                    break;
                }
                if (job.pixels[eye] != nullptr)
                {
                    g_presenter.Upload(eye, job.pixels[eye], job.pitch[eye]);
                }
            }

            const LARGE_INTEGER submitStart = Now();
            void* leftTexture = g_presenter.EyeTexture(EyeLeft);
            Vr().SubmitEye(EyeLeft, leftTexture, &job.pose);
            Vr().SubmitEye(EyeRight,
                           job.stereo ? g_presenter.EyeTexture(EyeRight) : leftTexture,
                           &job.pose);
            Vr().PostSubmit();
            const LARGE_INTEGER submitEnd = Now();

            // WaitGetPoses here, straight after this thread's own Submit. Every call in
            // the compositor's frame protocol stays on one thread in its required
            // order: never concurrent (the compositor is not thread-safe), never a
            // WaitGetPoses ahead of the Submit it follows, and never skipped after
            // one - skipping it while a submit was slow is what once left every
            // later Submit blocking for half a second.
            Vr().WaitForFrame();
            const LONGLONG poseWaitUs = static_cast<LONGLONG>(ElapsedMs(submitEnd, Now()) * 1000.0);
            InterlockedExchangeAdd64(&g_presenterPoseWaitUs, poseWaitUs);
            InterlockedExchange64(&g_presenterLastPoseWaitUs, poseWaitUs);
            const LARGE_INTEGER posesReady = Now();
            SetEvent(g_presenterSubmittedEvent);
            ApplyInterfaceOverlay(job);
            InterlockedExchangeAdd64(&g_presenterOverlayUs,
                                     static_cast<LONGLONG>(ElapsedMs(posesReady, Now()) * 1000.0));

            InterlockedExchangeAdd64(&g_presenterUploadUs,
                                     static_cast<LONGLONG>(ElapsedMs(uploadStart, submitStart) * 1000.0));
            InterlockedExchangeAdd64(&g_presenterSubmitUs,
                                     static_cast<LONGLONG>(ElapsedMs(submitStart, submitEnd) * 1000.0));
            InterlockedIncrement(&g_presenterSamples);
        }

        DWORD WINAPI PresenterThreadProc(LPVOID)
        {
            for (;;)
            {
                WaitForSingleObject(g_presenterJobEvent, INFINITE);
                RunPresenterJob(g_presenterJob);
                SetEvent(g_presenterDoneEvent);
            }
        }

        // Lazily brings the worker up. Never torn down: the thread parks in an
        // event wait and dies with the process, which is the only shutdown that
        // cannot deadlock a DllMain.
        bool EnsurePresenterThread()
        {
            if (g_presenterThreadHandle != nullptr)
            {
                return true;
            }
            if (g_presenterThreadBroken)
            {
                return false;
            }

            g_presenterJobEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_presenterDoneEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            g_presenterSubmittedEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            if (g_presenterJobEvent != nullptr && g_presenterDoneEvent != nullptr
                && g_presenterSubmittedEvent != nullptr)
            {
                g_presenterThreadHandle =
                    CreateThread(nullptr, 0, PresenterThreadProc, nullptr, 0, nullptr);
            }
            if (g_presenterThreadHandle == nullptr)
            {
                WOWVR_WARN("Presenter thread unavailable (%lu); upload and submit stay "
                           "on the game thread.", GetLastError());
                g_presenterThreadBroken = true;
                return false;
            }

            WOWVR_INFO("Presenter thread up; D3D11 upload and compositor submit run "
                       "off the game thread.");
            return true;
        }

        // Blocks until the worker has finished the outstanding job, then releases
        // the staging locks it was reading from. Cheap in the steady state: the
        // worker had a whole game frame to do ~1.3 ms of work.
        void WaitPresenterIdleAndUnlock()
        {
            if (g_presenterThreadHandle == nullptr)
            {
                return;
            }
            WaitForSingleObject(g_presenterDoneEvent, INFINITE);
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                g_eyeTargets[eye].Unlock();
            }
            g_uiTarget.Unlock();
        }

        // The per-frame version, which must never be able to freeze the game.
        //
        // Measured 2026-10-03: the worker sat inside IVRCompositor::Submit - in
        // vrclient, waiting in the NVIDIA D3D11 driver - and never came back, and the
        // unbounded wait above then hung the game thread with it: the whole client
        // locked up a few seconds into the world. Nothing in the frame can make a
        // stuck compositor submit return, but the game does not have to wait for it.
        // So: wait a bounded time, and if the worker is still busy, skip this frame's
        // VR output entirely (its staging slot is still the worker's, so nothing may
        // capture into it) and keep polling on later frames; the moment the submit
        // returns, the locks are released and output resumes.
        bool g_presenterStalled = false;
        unsigned long long g_presenterStalledFrames = 0;
        const DWORD kPresenterStallTimeoutMs = 500;

        // Waits (bounded) for the worker to finish its job, keeping the stall state
        // and its log lines. countSkip: this call decides whether the frame's VR
        // output is skipped, so it is the one that counts skipped frames.
        bool TryWaitPresenterDone(bool countSkip, bool submittedOnly = false)
        {
            if (g_presenterThreadHandle == nullptr)
            {
                return true;
            }

            const DWORD timeout = g_presenterStalled ? 0 : kPresenterStallTimeoutMs;
            const HANDLE gate = submittedOnly ? g_presenterSubmittedEvent : g_presenterDoneEvent;
            if (WaitForSingleObject(gate, timeout) != WAIT_OBJECT_0)
            {
                if (!g_presenterStalled)
                {
                    g_presenterStalled = true;
                    WOWVR_ERROR("Presenter worker has been inside the compositor submit for "
                                "over %lu ms; pausing headset output so the game keeps "
                                "running. It resumes by itself if the submit returns.",
                                kPresenterStallTimeoutMs);
                }
                if (countSkip)
                {
                    ++g_presenterStalledFrames;
                }
                return false;
            }

            if (g_presenterStalled)
            {
                g_presenterStalled = false;
                WOWVR_WARN("Presenter worker came back after %llu skipped frames; headset "
                           "output resumed.", g_presenterStalledFrames);
                g_presenterStalledFrames = 0;
            }
            return true;
        }

        bool TryWaitPresenterIdleAndUnlock()
        {
            if (!TryWaitPresenterDone(true))
            {
                return false;
            }
            if (g_presenterThreadHandle == nullptr)
            {
                return true;
            }
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                g_eyeTargets[eye].Unlock();
            }
            g_uiTarget.Unlock();
            return true;
        }

        // Decided per frame from the frame's work; applied by ApplyHalfRateDecision at
        // the one point in the frame where the compositor is free.
        int g_halfRateWanted = -1;   // -1 no change, 0 full rate, 1 half rate

        void UpdateHalfRateGovernor(double workMs)
        {
            if (!Cfg().adaptiveHalfRate || !Vr().IsActive() || !(workMs > 0.0))
            {
                if (Vr().HalfRate()) { g_halfRateWanted = 0; }
                return;
            }
            const double hz = Vr().DisplayFrequency() > 1.0f ? Vr().DisplayFrequency() : 90.0;
            const double budget = 1000.0 / hz;
            ++g_halfRate.window;
            if (workMs > budget) { ++g_halfRate.late; }
            if (workMs > budget * 0.9) { ++g_halfRate.nearlyLate; }

            const double now = static_cast<double>(Now().QuadPart)
                               / static_cast<double>(g_qpcFrequency.QuadPart);
            const bool settled = now - g_halfRate.lastSwitchSeconds > 3.0;
            if (!Vr().HalfRate())
            {
                // A second's worth of frames, a third of them late: the cadence is
                // already uneven enough to feel.
                if (g_halfRate.window >= static_cast<unsigned>(hz))
                {
                    if (settled && g_halfRate.late * 3 >= g_halfRate.window)
                    {
                        g_halfRateWanted = 1;
                    }
                    g_halfRate.window = g_halfRate.late = g_halfRate.nearlyLate = 0;
                }
            }
            else
            {
                // Back to full rate only after three seconds (at half rate) in which
                // almost no frame even came close to the full-rate budget.
                if (g_halfRate.window >= static_cast<unsigned>(hz * 1.5))
                {
                    if (settled && g_halfRate.nearlyLate * 50 <= g_halfRate.window)
                    {
                        g_halfRateWanted = 0;
                    }
                    g_halfRate.window = g_halfRate.late = g_halfRate.nearlyLate = 0;
                }
            }
        }

        void ApplyHalfRateDecision()
        {
            if (g_halfRateWanted < 0)
            {
                return;
            }
            const bool on = g_halfRateWanted == 1;
            g_halfRateWanted = -1;
            if (on == Vr().HalfRate())
            {
                return;
            }
            Vr().SetHalfRate(on);
            g_halfRate.lastSwitchSeconds = static_cast<double>(Now().QuadPart)
                                           / static_cast<double>(g_qpcFrequency.QuadPart);
            g_halfRate.window = g_halfRate.late = g_halfRate.nearlyLate = 0;
            ++g_halfRate.switches;
            WOWVR_INFO("Frame rate: %s (headset %.0f Hz, worst frame interval this report window %.2f ms).",
                       on ? "the game cannot keep up - running at half rate with SteamVR "
                            "filling in every other frame"
                          : "the game keeps up again - back to full rate",
                       Vr().DisplayFrequency(), g_pacing.maxIntervalMs);
        }

        void ReportFrameTimings()
        {
            if (g_timers.samples == 0)
            {
                return;
            }

            const double inverse = 1.0 / static_cast<double>(g_timers.samples);
            WOWVR_INFO("Frame cost over %u frames: capture %.2f ms, upload %.2f ms, "
                       "submit %.2f ms, compositor wait %.2f ms (%ux%u per eye)",
                       g_timers.samples,
                       g_timers.captureMs * inverse,
                       g_timers.uploadMs * inverse,
                       g_timers.submitMs * inverse,
                       g_timers.waitMs * inverse,
                       Vr().RenderWidth(), Vr().RenderHeight());

            g_timers = FrameTimers();


            if (g_pacing.samples > 0)
            {
                const double n = static_cast<double>(g_pacing.samples);
                WOWVR_INFO("Frame pacing over %u frames: interval avg %.2f max %.2f ms, %u over "
                           "10 ms, %u over 20 ms, %u over 50 ms; Present avg %.2f max %.2f ms; "
                           "waiting for fresh poses avg %.2f max %.2f ms (worker "
                           "WaitGetPoses %.2f ms avg).",
                           g_pacing.samples, g_pacing.sumIntervalMs / n, g_pacing.maxIntervalMs,
                           g_pacing.over10, g_pacing.over20, g_pacing.over50,
                           g_pacing.sumPresentMs / n, g_pacing.maxPresentMs,
                           g_pacing.sumWorkerWaitMs / n, g_pacing.maxWorkerWaitMs,
                           static_cast<double>(InterlockedExchange64(&g_presenterPoseWaitUs, 0))
                               / 1000.0 / n);
                const LARGE_INTEGER keepLast = g_pacing.last;
                g_pacing = FramePacing();
                g_pacing.last = keepLast;
            }
            // Only while the worker is idle: the compositor is not thread-safe.
            if (!g_presenterStalled)
            {
                Vr().LogCompositorStats();
            }
            Vr().RefreshDisplayFrequency();
            Vignette().LogStatus();
            WOWVR_INFO("Lifebars in the world: strip %s (%ld rows of cells %.0fx%.0f px), %llu "
                       "lifebar cards drawn since the last report.",
                       g_plateStripActive ? "on" : "off",
                       g_plateStripActive ? static_cast<long>(g_plateStripHeightV / (g_plateCellV > 0 ? g_plateCellV : 1)) : 0L,
                       g_plateCellU * static_cast<float>(g_uiPanel.Width()),
                       g_plateCellV * static_cast<float>(g_uiPanel.Height()), g_platesDrawn);
            g_platesDrawn = 0;

            WOWVR_INFO("Frame rate mode: %s, %llu switches so far.",
                       Vr().HalfRate() ? "half rate" : "full rate", g_halfRate.switches);

            const LONG presenterSamples = g_presenterSamples;
            if (presenterSamples > 0)
            {
                WOWVR_INFO("Presenter thread cost over %ld frames: upload %.2f ms, "
                           "submit %.2f ms, interface overlay %.2f ms (after the submit).",
                           presenterSamples,
                           static_cast<double>(g_presenterUploadUs) / 1000.0 / presenterSamples,
                           static_cast<double>(g_presenterSubmitUs) / 1000.0 / presenterSamples,
                           static_cast<double>(g_presenterOverlayUs) / 1000.0 / presenterSamples);
                InterlockedExchange64(&g_presenterUploadUs, 0);
                InterlockedExchange64(&g_presenterSubmitUs, 0);
                InterlockedExchange64(&g_presenterOverlayUs, 0);
                InterlockedExchange(&g_presenterSamples, 0);
            }

            WOWVR_INFO("Pool census (default/managed/sysmem/scratch): textures %llu/%llu/%llu/%llu, "
                       "vertex buffers %llu/%llu/%llu/%llu, index buffers %llu/%llu/%llu/%llu",
                       g_poolCounts[CensusTexture][0], g_poolCounts[CensusTexture][1],
                       g_poolCounts[CensusTexture][2], g_poolCounts[CensusTexture][3],
                       g_poolCounts[CensusVertexBuffer][0], g_poolCounts[CensusVertexBuffer][1],
                       g_poolCounts[CensusVertexBuffer][2], g_poolCounts[CensusVertexBuffer][3],
                       g_poolCounts[CensusIndexBuffer][0], g_poolCounts[CensusIndexBuffer][1],
                       g_poolCounts[CensusIndexBuffer][2], g_poolCounts[CensusIndexBuffer][3]);
            Projection().LogLastDecision();
            WOWVR_INFO("UI panel: composited %llu frames, skipped %llu (last reason: %s)",
                       g_panelDrawn, g_panelSkipped, g_lastSkipReason);
            Pointer().LogStatus();
            Billboards().LogStatus();
            WOWVR_INFO("Draw distance: client %.1f yards, drawn to %.1f (x%.2f); terrain fog "
                       "fully closes at %.1f yards of view depth.", DrawRange().ClientDistance(),
                       DrawRange().DrawDistance(), Cfg().viewDistanceScale, g_terrainFogEnd);
            WOWVR_INFO("Radial fog: %llu world shaders rewritten, %llu left as shipped (last "
                       "reason: %s), %llu refused by the runtime.", g_fogShadersRewritten,
                       g_fogShadersLeft, g_fogLastSkipReason, g_fogRewriteRefused);
            WOWVR_INFO("Draw routing: stereo %llu, ui %llu, offscreen %llu, "
                       "STRAY-to-backbuffer %llu; periods last frame %d; "
                       "sky-slice eye draws %llu",
                       g_drawsToStereo, g_drawsToUi, g_drawsOffscreen,
                       g_drawsStrayBackBuffer, g_lastPeriodCount, g_skySliceDraws);
            // Alongside the draw counts on purpose: the claim this whole approach rests on
            // is that pointing the culling volume somewhere else does not change how much
            // is in it, and these two numbers together are what settles that.
            CullView().Report();
            HeadListener().Report();
            {
                char line[512];
                int used = 0;
                for (UINT reg = 0; reg < kShadowRegisters; ++reg)
                {
                    if ((g_substShape[reg] | g_substExact[reg] | g_substCombined[reg]) == 0)
                    {
                        continue;
                    }
                    if (used < static_cast<int>(sizeof(line)) - 48)
                    {
                        used += sprintf_s(line + used, sizeof(line) - used,
                                          "c%u[s%llu e%llu k%llu] ", reg,
                                          g_substShape[reg], g_substExact[reg],
                                          g_substCombined[reg]);
                    }
                }
                line[used] = 0;
                WOWVR_INFO("Registers rewritten (s=byShape e=byExactMatch k=combined): %s",
                           used ? line : "(none)");
            }

            WOWVR_INFO("SetRenderTarget failures: %llu", g_setRenderTargetFailures);

            WOWVR_INFO("Post-process draws skipped: %llu", g_postProcessDrawsSkipped);

            WOWVR_INFO("Eye state restored after %llu duplicated draws; render-target "
                       "queries answered with the client's own surface %llu times",
                       g_eyeStateRestored, g_renderTargetQueriesRedirected);

            WOWVR_INFO("Depth surface substituted %llu times, declined %llu times "
                       "(declined = the client's own offscreen passes, including its shadow map)",
                       g_depthSubstituted, g_depthSubstitutionDeclined);

            WOWVR_INFO("Shadow cascade transforms scaled %llu times",
                       g_shadowCascadeRowsScaled);

            WOWVR_INFO("Light matrix seen in the world pass %llu times (have it: %d)",
                       g_lightOrthoSeenInWorld, g_haveLightOrtho ? 1 : 0);

            WOWVR_INFO("Shadow map: %llu caster draws, %llu primitives written into it",
                       g_shadowMapDraws, g_shadowMapPrims);

            WOWVR_INFO("Shadow map: %ux%u, %llu cascades widened by %.2fx",
                       g_shadowMapSize, g_shadowMapSize, g_shadowCascadesWidened,
                       Cfg().shadowCoverageScale);

            WOWVR_INFO("Constant uploads: largest %u regs, highest register touched %u "
                       "(shadow mirror holds %u); %llu uploads over the 1024 patch bound, "
                       "%llu reaching past the mirror",
                       g_maxVector4Count, g_maxStartPlusCount,
                       static_cast<unsigned>(kShadowRegisters),
                       g_oversizeUploads, g_beyondShadowMirror);

            WOWVR_INFO("Cursor: window %p, %d shape(s) cached (%llu built so far), current %s, drawn %llu, "
                       "last position %.3f, %.3f",
                       static_cast<void*>(g_gameWindow), g_cursorCacheCount,
                       g_cursorArtBuilds, g_activeCursor != nullptr ? "shown" : "hidden",
                       g_cursorDrawn, g_lastCursorU, g_lastCursorV);
            WOWVR_INFO("UI panel compositing: %s, %llu additive interface draws given a "
                       "no-coverage alpha blend",
                       Cfg().premultipliedUi ? "premultiplied" : "straight alpha",
                       g_additiveUiDraws);
            WOWVR_INFO("Cursor: %llu stale-pointer substitutions after focus changes, "
                       "last game art %p, focus %s, confined %s; game says %s, %llu frames "
                       "drawn while Windows had it hidden",
                       g_staleCursorSubstitutions, static_cast<void*>(g_lastGameCursor),
                       GameHasFocus() ? "held" : "elsewhere",
                       g_cursorConfined ? "yes" : "no",
                       g_lastGameCursorFlag == 1 ? "shown"
                           : (g_lastGameCursorFlag == 0 ? "hidden" : "unknown"),
                       g_vanishSubstitutions);
            WOWVR_INFO("World draws using pre-transformed screen coordinates: %llu",
                       g_preTransformedWorldDraws);
            WOWVR_INFO("World draws with the scissor test on: %llu (game scissor %ld,%ld - %ld,%ld)",
                       g_scissoredWorldDraws, g_gameScissor.left, g_gameScissor.top,
                       g_gameScissor.right, g_gameScissor.bottom);
            g_preTransformedWorldDraws = 0;
            g_scissoredWorldDraws = 0;
            g_panelDrawn = 0;
            g_panelSkipped = 0;
            g_drawsToStereo = 0;
            g_drawsToUi = 0;
            g_drawsOffscreen = 0;
            for (int i = 0; i < g_offscreenTargetCount; ++i)
            {
                const OffscreenTarget& t = g_offscreenTargets[i];
                if (t.draws > 0 || t.binds > 0)
                {
                    WOWVR_INFO("  offscreen target %p %ux%u fmt=%u: %llu draws, %llu binds",
                               static_cast<void*>(t.surface), t.width, t.height, t.format,
                               t.draws, t.binds);
                }
                g_offscreenTargets[i].draws = 0;
                g_offscreenTargets[i].binds = 0;
            }
            g_drawsStrayBackBuffer = 0;
        }

        void ReleasePlateTexture();

        void ReleaseFrameResources()
        {
            // The presenter thread may still be uploading into textures about to
            // die; everything below assumes exclusive ownership.
            WaitPresenterIdleAndUnlock();

            // The presenter's adopted textures and the GL registrations alias the
            // D3D9 surfaces being destroyed just below, so they go first. The GL
            // context and interop device survive; only the per-texture state dies.
            g_presenter.ReleaseAdoptedTextures();
            g_glInterop.UnregisterEyeTextures();
            g_zeroCopyActive = false;
            g_glInteropActive = false;
            g_d3d12Active = false;

            for (int eye = 0; eye < EyeCount; ++eye)
            {
                g_eyeTargets[eye].Destroy();
            }
            g_uiTarget.Destroy();
            g_stereo.Destroy();
            g_uiPanel.Destroy();
            ReleasePlateTexture();

            if (g_frameSyncQuery != nullptr)
            {
                g_frameSyncQuery->Release();
                g_frameSyncQuery = nullptr;
            }
            g_frameSyncQueryFailed = false;

            if (g_realBackBuffer != nullptr)
            {
                g_realBackBuffer->Release();
                g_realBackBuffer = nullptr;
            }

            g_stereoRedirected = false;
            g_haveEyeProjections = false;
            g_resourcesReady = false;
        }

        bool CreateEyeTargets(IDirect3DDevice9* device, uint32_t width, uint32_t height,
                              EyeTargets::Kind kind)
        {
            bool ready = true;
            g_creatingProxyResource = true;
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                ready = g_eyeTargets[eye].Create(device, width, height, kind) && ready;
                g_eyeTargets[eye].SetPipelined(Cfg().pipelinedReadback);
            }
            g_creatingProxyResource = false;
            return ready;
        }

        // The GL half of the zero-copy path: interop device plus one registration
        // per eye texture. Any failure tears the attempt down and reports false.
        bool AttachGlInterop(IDirect3DDevice9* device)
        {
            if (!g_glInterop.Init(device))
            {
                return false;
            }

            for (int eye = 0; eye < EyeCount; ++eye)
            {
                if (!g_glInterop.RegisterEyeTexture(eye, g_eyeTargets[eye].RenderTargetTexture()))
                {
                    g_glInterop.UnregisterEyeTextures();
                    return false;
                }
            }
            return true;
        }

        // Brings up OpenVR and the per-eye resources the first time the game presents.
        // Deliberately lazy: DllMain is far too early to be starting SteamVR, and the
        // device does not exist until CreateDevice has returned.
        void EnsureVrResources(IDirect3DDevice9* device)
        {
            if (!Cfg().vrEnabled || g_resourcesReady)
            {
                return;
            }

            if (!g_vrInitAttempted)
            {
                g_vrInitAttempted = true;

                if (Cfg().flatDebug)
                {
                    WOWVR_INFO("FlatDebug is set; skipping the OpenVR connection.");
                    return;
                }

                WOWVR_INFO("Connecting to OpenVR. This may take a moment if SteamVR is not running yet.");
                Vr().Init();
            }

            if (!Vr().IsActive())
            {
                return;
            }

            const uint32_t width = Vr().RenderWidth();
            const uint32_t height = Vr().RenderHeight();

            // Route choice, best first. On this client only the 9on12 route can go
            // zero-copy: the native driver refuses share handles (D3DERR_INVALIDCALL)
            // and refuses to register plain allocations with the GL interop
            // (ERROR_OPEN_FAILED), both for the same underlying reason - a plain
            // D3D9 allocation is simply not shareable. The other attempts stay
            // because they are cheap and become viable if the device ever changes
            // character (an Ex device, a different driver).
            bool targetsReady = false;
            bool d3d12Route = false;
            bool sharedRoute = false;
            bool glRoute = false;

            const bool presenterReady = g_presenter.IsReady()
                ? (g_presenter.Width() == width && g_presenter.Height() == height)
                : g_presenter.Init(Vr().PreferredAdapterIndex(), width, height);
            bool copyReady = presenterReady;

            if (Cfg().zeroCopyPresenter && g_d3d12Present.Init(device))
            {
                targetsReady = CreateEyeTargets(device, width, height, EyeTargets::KindTexture);
                d3d12Route = targetsReady;
            }

            // The shared-handle hand-off: the D3D11 side opens the very surfaces
            // the game's device rescales into. Rebuilt on every resource build
            // because the D3D9 side does not survive a device reset.
            if (!d3d12Route && Cfg().zeroCopyPresenter && presenterReady)
            {
                targetsReady = CreateEyeTargets(device, width, height,
                                                EyeTargets::KindSharedHandle);
                sharedRoute = targetsReady
                    && g_presenter.AdoptSharedTextures(g_eyeTargets[EyeLeft].SharedHandle(),
                                                       g_eyeTargets[EyeRight].SharedHandle());
            }

            // The GL hand-off: alias the eye textures and submit them as GL textures.
            if (!d3d12Route && !sharedRoute && Cfg().zeroCopyPresenter)
            {
                targetsReady = CreateEyeTargets(device, width, height, EyeTargets::KindTexture);
                glRoute = targetsReady && AttachGlInterop(device);
            }

            if (!d3d12Route && !sharedRoute && !glRoute)
            {
                if (Cfg().zeroCopyPresenter)
                {
                    WOWVR_WARN("No zero-copy route available; using the copy presenter instead.");
                }
                targetsReady = CreateEyeTargets(device, width, height, EyeTargets::KindCopy);
                if (copyReady && !g_presenter.HasEyeTextures())
                {
                    copyReady = g_presenter.CreateEyeTextures();
                }
                targetsReady = targetsReady && copyReady;
            }

            g_d3d12Active = d3d12Route;
            g_zeroCopyActive = sharedRoute;
            g_glInteropActive = glRoute;
            if (g_d3d12Active)
            {
                WOWVR_INFO("Zero-copy active through D3D9On12; frames stay on the GPU.");
            }
            else if (g_zeroCopyActive || g_glInteropActive)
            {
                WOWVR_INFO("Zero-copy active through %s; frames stay on the GPU.",
                           g_zeroCopyActive ? "a shared surface" : "GL interop");
            }

            // The side-by-side target the game will be redirected into, plus a
            // reference to the real back buffer so the redirect can recognise it.
            bool stereoReady = true;
            if (Cfg().stereo)
            {
                g_stereo.SetBackBufferFormat(static_cast<uint32_t>(g_backBufferFormat));
                stereoReady = g_stereo.Create(device, width, height);
                stereoReady = g_uiPanel.Create(device, g_backBufferWidth, g_backBufferHeight)
                              && stereoReady;
                if (g_uiPanel.IsReady()
                    && g_uiTarget.Create(device, g_uiPanel.Width(), g_uiPanel.Height(),
                                         EyeTargets::KindCopy))
                {
                    g_uiTarget.SetPipelined(Cfg().pipelinedReadback);
                }

                if (g_realBackBuffer == nullptr)
                {
                    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_realBackBuffer);
                }
                stereoReady = stereoReady && (g_realBackBuffer != nullptr);

                if (!stereoReady)
                {
                    WOWVR_ERROR("Stereo resources unavailable; falling back to a single view.");
                }
            }

            g_resourcesReady = targetsReady && presenterReady && stereoReady;

            if (!g_resourcesReady && !g_resourceFailureLogged)
            {
                WOWVR_ERROR("Could not create the VR frame resources; staying flat.");
                g_resourceFailureLogged = true;
            }
        }

        struct PanelVertex
        {
            float x;
            float y;
            float z;
            float u;
            float v;
        };

        // Lifebars in the world (Nameplates3D). The client draws each one into a cell of
        // a strip along the bottom of the interface image (game/world_pointer); at the
        // end of the interface pass that strip is copied here and cleared from the
        // interface, and each cell is drawn over its unit in both eyes.

        // DEBUG ("platedump"): the interface image and the lifebar strip, as BMPs.
        bool g_plateDumpNext = false;

        void DumpSurfaceBmp(IDirect3DDevice9* device, IDirect3DSurface9* surface, const wchar_t* name)
        {
            D3DSURFACE_DESC desc = {};
            if (surface == nullptr || FAILED(surface->GetDesc(&desc)))
            {
                return;
            }
            IDirect3DSurface9* copy = nullptr;
            IDirect3DSurface9* staging = nullptr;
            HRESULT hr = device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
                                                    D3DMULTISAMPLE_NONE, 0, FALSE, &copy, nullptr);
            if (SUCCEEDED(hr)) { hr = device->StretchRect(surface, nullptr, copy, nullptr, D3DTEXF_NONE); }
            if (SUCCEEDED(hr))
            {
                hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_A8R8G8B8,
                                                         D3DPOOL_SYSTEMMEM, &staging, nullptr);
            }
            if (SUCCEEDED(hr)) { hr = device->GetRenderTargetData(copy, staging); }
            D3DLOCKED_RECT locked = {};
            if (SUCCEEDED(hr) && SUCCEEDED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                SaveBgraBmp(ModuleFile(name).c_str(), locked.pBits, desc.Width, desc.Height,
                            static_cast<uint32_t>(locked.Pitch));
                staging->UnlockRect();
                WOWVR_INFO("platedump: %ls written (%ux%u).", name, desc.Width, desc.Height);
            }
            else
            {
                WOWVR_WARN("platedump: %ls failed (0x%08lx).", name, hr);
            }
            if (staging != nullptr) { staging->Release(); }
            if (copy != nullptr) { copy->Release(); }
        }

        void ReleasePlateTexture()
        {
            if (g_plateSurface != nullptr) { g_plateSurface->Release(); g_plateSurface = nullptr; }
            if (g_plateTexture != nullptr) { g_plateTexture->Release(); g_plateTexture = nullptr; }
            g_plateTextureWidth = 0;
            g_plateTextureHeight = 0;
        }

        // The strip's layout for this frame, from the canvas: it lives in the band of
        // extra canvas below the client's interface, so nothing anyone placed is lost.
        // Why 3D lifebars are or are not in use, logged whenever the answer changes, so a
        // machine where they fall back to the interface says why.
        const char* g_plateStripReason = "";

        void SetPlateStripReason(const char* reason)
        {
            if (strcmp(reason, g_plateStripReason) != 0)
            {
                g_plateStripReason = reason;
                WOWVR_INFO("Lifebars in the world: %s", reason);
            }
        }

        void UpdatePlateStrip()
        {
            g_plateStripActive = false;
            const float canvas = Canvas().PanelScale();
            const uint32_t width = g_uiPanel.Width();
            const uint32_t height = g_uiPanel.Height();
            if (!Cfg().nameplates3d)
            {
                Pointer().SetPlateStrip(false, 0.0f, 0.0f, 0.0f, 0, 0, 0.0f);
                SetPlateStripReason("off in WoWVR.ini (Nameplates3D=0).");
                return;
            }
            if (!(canvas > 1.05f) || width == 0 || height == 0)
            {
                Pointer().SetPlateStrip(false, 0.0f, 0.0f, 0.0f, 0, 0, 0.0f);
                SetPlateStripReason("waiting - the larger interface canvas is not in force (not in "
                                    "the world yet, or it could not be applied).");
                return;
            }
            // A lifebar's size in pixels follows the game's screen height (the interface is
            // laid out on a 768-unit-tall screen) and shrinks with WorldFrame by the canvas
            // factor. The cell sizes were measured at a 1395-pixel-tall screen; at any other
            // resolution they scale the same way, or a 1080p screen would find no room at all.
            const float pixelScale = static_cast<float>(height) / 1395.0f / canvas;
            const float cellPixelsX = floorf(330.0f * pixelScale);
            float cellPixelsY = floorf(150.0f * pixelScale);
            const float bandPixels = static_cast<float>(height) * (1.0f - 1.0f / canvas) * 0.5f;
            int rows = static_cast<int>((bandPixels - 4.0f) / cellPixelsY);
            if (rows < 1 && bandPixels - 4.0f >= cellPixelsY * 0.7f)
            {
                // Not quite a full cell of room: a shorter cell still holds the name and the
                // bar (they take the top half of it), so use what there is.
                rows = 1;
                cellPixelsY = floorf(bandPixels - 4.0f);
            }
            if (rows > 3) { rows = 3; }
            const int columns = static_cast<int>(static_cast<float>(width) / cellPixelsX);
            if (rows < 1 || columns < 1)
            {
                Pointer().SetPlateStrip(false, 0.0f, 0.0f, 0.0f, 0, 0, 0.0f);
                SetPlateStripReason("not enough room below the interface at this resolution and "
                                    "CanvasScale - raise CanvasScale. Lifebars stay on the interface.");
                return;
            }
            const float stripPixels = cellPixelsY * static_cast<float>(rows);
            g_plateStripRect.left = 0;
            g_plateStripRect.right = static_cast<LONG>(width);
            g_plateStripRect.bottom = static_cast<LONG>(height);
            g_plateStripRect.top = static_cast<LONG>(static_cast<float>(height) - stripPixels);
            g_plateCellU = cellPixelsX / static_cast<float>(width);
            g_plateCellV = cellPixelsY / static_cast<float>(height);
            g_plateStripTopV = static_cast<float>(g_plateStripRect.top) / static_cast<float>(height);
            g_plateStripHeightV = stripPixels / static_cast<float>(height);
            Pointer().SetPlateStrip(true, g_plateStripTopV, g_plateCellU, g_plateCellV, columns, rows,
                                    kPlateAnchorV);
            g_plateStripActive = Pointer().PlateStripOn();
            SetPlateStripReason(g_plateStripActive
                                    ? "in use (lifebars drawn over units in 3D)."
                                    : "waiting for world pointing to be active.");
        }

        // End of the interface pass: the strip, out of the interface and into its own
        // texture. Cleared even when no lifebar was placed, so the band is never shown.
        bool CapturePlateStrip(IDirect3DDevice9* device)
        {
            if (!g_plateStripActive || !g_uiPanel.IsReady())
            {
                return false;
            }
            const uint32_t width = static_cast<uint32_t>(g_plateStripRect.right - g_plateStripRect.left);
            const uint32_t height = static_cast<uint32_t>(g_plateStripRect.bottom - g_plateStripRect.top);
            if (g_plateTexture == nullptr || g_plateTextureWidth != width || g_plateTextureHeight != height)
            {
                ReleasePlateTexture();
                if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                                 D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_plateTexture,
                                                 nullptr))
                    || FAILED(g_plateTexture->GetSurfaceLevel(0, &g_plateSurface)))
                {
                    ReleasePlateTexture();
                    return false;
                }
                g_plateTextureWidth = width;
                g_plateTextureHeight = height;
            }
            IDirect3DSurface9* panel = g_uiPanel.Surface();
            if (g_plateDumpNext)
            {
                DumpSurfaceBmp(device, panel, L"WoWVR_platedump_interface.bmp");
            }
            const bool copied = Pointer().PlateCount() > 0
                && SUCCEEDED(device->StretchRect(panel, &g_plateStripRect, g_plateSurface, nullptr,
                                                 D3DTEXF_NONE));
            device->ColorFill(panel, &g_plateStripRect, D3DCOLOR_ARGB(0, 0, 0, 0));
            if (g_plateDumpNext)
            {
                g_plateDumpNext = false;
                DumpSurfaceBmp(device, g_plateSurface, L"WoWVR_platedump_strip.bmp");
                WOWVR_INFO("platedump: %d lifebars this frame; first slot u %.3f v %.3f, body "
                           "(%.2f %.2f %.2f) %.1f m.", Pointer().PlateCount(),
                           Pointer().PlateCount() > 0 ? Pointer().Plate(0).slotU : -1.0f,
                           Pointer().PlateCount() > 0 ? Pointer().Plate(0).slotV : -1.0f,
                           Pointer().PlateCount() > 0 ? Pointer().Plate(0).body.x : 0.0f,
                           Pointer().PlateCount() > 0 ? Pointer().Plate(0).body.y : 0.0f,
                           Pointer().PlateCount() > 0 ? Pointer().Plate(0).body.z : 0.0f,
                           Pointer().PlateCount() > 0 ? Pointer().Plate(0).distanceMetres : 0.0f);
            }
            return copied;
        }

        // The comfort vignette: a black ring with soft inner edge, laid over each eye on
        // its own optical axis. Built per size as a texture in tangent space, so its
        // inner radius is a true angle from the eye's centre of view.
        IDirect3DTexture9* g_vignetteTexture = nullptr;
        IDirect3DDevice9* g_vignetteDevice = nullptr;
        int g_vignetteTextureSize = -1;
        const float kVignetteReach = 3.0f;   // the quad spans tangents -3..3 (~143 deg)

        IDirect3DTexture9* VignetteTexture(IDirect3DDevice9* device, int size)
        {
            if (g_vignetteTexture != nullptr && g_vignetteDevice == device && g_vignetteTextureSize == size)
            {
                return g_vignetteTexture;
            }
            if (g_vignetteTexture != nullptr && g_vignetteDevice == device)
            {
                g_vignetteTexture->Release();
            }
            g_vignetteTexture = nullptr;
            g_vignetteDevice = device;
            g_vignetteTextureSize = size;

            // Where the darkening starts, in degrees from the centre of view, and how
            // wide its soft edge is. Larger sizes start closer in. Steps of 10 degrees:
            // Small 38 (was 48, too subtle), Medium 28 (the old Large), Large 18.
            const float innerDegrees = size <= 0 ? 38.0f : (size >= 2 ? 18.0f : 28.0f);
            const float featherDegrees = 16.0f;
            const int texels = 256;
            if (FAILED(device->CreateTexture(texels, texels, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                             &g_vignetteTexture, nullptr)))
            {
                g_vignetteTexture = nullptr;
                return nullptr;
            }
            D3DLOCKED_RECT locked = {};
            if (FAILED(g_vignetteTexture->LockRect(0, &locked, nullptr, 0)))
            {
                g_vignetteTexture->Release();
                g_vignetteTexture = nullptr;
                return nullptr;
            }
            for (int y = 0; y < texels; ++y)
            {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits)
                                                             + static_cast<size_t>(y) * locked.Pitch);
                for (int x = 0; x < texels; ++x)
                {
                    const float tx = ((x + 0.5f) / texels * 2.0f - 1.0f) * kVignetteReach;
                    const float ty = ((y + 0.5f) / texels * 2.0f - 1.0f) * kVignetteReach;
                    const float degrees = atanf(sqrtf(tx * tx + ty * ty)) * 57.2957795f;
                    float t = (degrees - innerDegrees) / featherDegrees;
                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                    t = t * t * (3.0f - 2.0f * t);   // smoothstep
                    row[x] = static_cast<uint32_t>(t * 255.0f + 0.5f) << 24;   // black
                }
            }
            g_vignetteTexture->UnlockRect(0);
            return g_vignetteTexture;
        }

        void DrawVignette(IDirect3DDevice9* device)
        {
            const float amount = Vignette().Amount();
            if (!(amount > 0.003f))
            {
                return;
            }
            IDirect3DTexture9* texture = VignetteTexture(device, Cfg().vignetteSize);
            if (texture == nullptr)
            {
                return;
            }
            const float r = kVignetteReach;
            const PanelVertex quad[6] = {
                { -r,  r, 1.0f, 0.0f, 0.0f }, { r,  r, 1.0f, 1.0f, 0.0f }, { -r, -r, 1.0f, 0.0f, 1.0f },
                {  r,  r, 1.0f, 1.0f, 0.0f }, { r, -r, 1.0f, 1.0f, 1.0f }, { -r, -r, 1.0f, 0.0f, 1.0f },
            };
            g_originalSetRenderTarget(device, 0, g_stereo.Color());
            g_originalSetDepthStencilSurface(device, nullptr);
            g_originalSetVertexShader(device, nullptr);
            device->SetPixelShader(nullptr);
            g_originalSetFVF(device, D3DFVF_XYZ | D3DFVF_TEX1);
            device->SetTexture(0, texture);
            g_originalSetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
            g_originalSetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            g_originalSetRenderState(device, D3DRS_LIGHTING, FALSE);
            g_originalSetRenderState(device, D3DRS_FOGENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_STENCILENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_originalSetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_originalSetRenderState(device, D3DRS_COLORWRITEENABLE, 0x0F);
            g_originalSetRenderState(device, D3DRS_TEXTUREFACTOR,
                                     static_cast<DWORD>(amount * 255.0f + 0.5f) << 24);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            // In the eye's own space: it moves with the head, as a vignette should.
            const Mat4 identity = Mat4Identity();
            g_originalSetTransform(device, D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&identity));
            g_originalSetTransform(device, D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&identity));
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                D3DVIEWPORT9 viewport = {};
                viewport.X = (eye == EyeLeft) ? 0 : g_stereo.EyeWidth();
                viewport.Width = g_stereo.EyeWidth();
                viewport.Height = g_stereo.EyeHeight();
                viewport.MaxZ = 1.0f;
                g_originalSetViewport(device, &viewport);
                float tanLeft = 0.0f, tanRight = 0.0f, tanTop = 0.0f, tanBottom = 0.0f;
                Vr().EyeTangents(eye, tanLeft, tanRight, tanTop, tanBottom);
                const Mat4 projection = Mat4PerspectiveTangents(tanLeft, tanRight, tanTop, tanBottom,
                                                                0.05f, 10.0f);
                g_originalSetTransform(device, D3DTS_PROJECTION,
                                       reinterpret_cast<const D3DMATRIX*>(&projection));
                g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2, quad, sizeof(PanelVertex));
            }
        }

        // Each captured lifebar as a small upright card over its unit, facing the head,
        // in both eyes. The same angular size it had on the interface, so it reads the
        // same at any distance; the depth is the unit's own. No depth test - the client's
        // lifebars show through walls too. Furthest first.
        void DrawWorldPlates(IDirect3DDevice9* device)
        {
            const int count = Pointer().PlateCount();
            if (count <= 0 || g_plateTexture == nullptr)
            {
                return;
            }
            int order[64];
            int n = 0;
            for (int i = 0; i < count && n < 64; ++i) { order[n++] = i; }
            for (int i = 1; i < n; ++i)
            {
                const int key = order[i];
                int j = i - 1;
                while (j >= 0 && Pointer().Plate(order[j]).distanceMetres
                                     < Pointer().Plate(key).distanceMetres)
                {
                    order[j + 1] = order[j];
                    --j;
                }
                order[j + 1] = key;
            }

            const PanelShape& shape = g_uiPanel.Shape();
            const Vec3 head = Projection().HeadOffsetMetres();
            const float cellWidthPixels = g_plateCellU * static_cast<float>(g_uiPanel.Width());
            const float cellHeightPixels = g_plateCellV * static_cast<float>(g_uiPanel.Height());

            static PanelVertex quads[64 * 6];
            int vertices = 0;
            for (int k = 0; k < n; ++k)
            {
                const WorldPlate& plate = Pointer().Plate(order[k]);
                Vec3 toHead = { head.x - plate.body.x, 0.0f, head.z - plate.body.z };
                const float flat = sqrtf(toHead.x * toHead.x + toHead.z * toHead.z);
                if (!(flat > 1.0e-3f))
                {
                    continue;
                }
                toHead.x /= flat;
                toHead.z /= flat;
                const Vec3 right = { -toHead.z, 0.0f, toHead.x };
                // Metres per texel at the unit's distance, for the panel's angular size.
                const float perPixel = shape.metresPerPixel * plate.distanceMetres
                                     / (shape.radius > 0.1f ? shape.radius : 0.1f);
                const float halfW = 0.5f * cellWidthPixels * perPixel;
                // The anchor row of the cell sits on the unit's point; the rest of the
                // cell hangs below it, as the lifebar hangs below its anchor.
                const float above = kPlateAnchorV * cellHeightPixels * perPixel;
                const float below = (1.0f - kPlateAnchorV) * cellHeightPixels * perPixel;
                const float cellTopV = plate.slotV - kPlateAnchorV * g_plateCellV;
                const float u0 = plate.slotU - 0.5f * g_plateCellU;
                const float u1 = plate.slotU + 0.5f * g_plateCellU;
                const float v0 = (cellTopV - g_plateStripTopV) / g_plateStripHeightV;
                const float v1 = (cellTopV + g_plateCellV - g_plateStripTopV) / g_plateStripHeightV;
                const Vec3 c = plate.body;
                const PanelVertex tl = { c.x - right.x * halfW, c.y + above, c.z - right.z * halfW, u0, v0 };
                const PanelVertex tr = { c.x + right.x * halfW, c.y + above, c.z + right.z * halfW, u1, v0 };
                const PanelVertex bl = { c.x - right.x * halfW, c.y - below, c.z - right.z * halfW, u0, v1 };
                const PanelVertex br = { c.x + right.x * halfW, c.y - below, c.z + right.z * halfW, u1, v1 };
                quads[vertices++] = tl; quads[vertices++] = tr; quads[vertices++] = bl;
                quads[vertices++] = tr; quads[vertices++] = br; quads[vertices++] = bl;
            }
            if (vertices == 0)
            {
                return;
            }

            g_originalSetRenderTarget(device, 0, g_stereo.Color());
            g_originalSetDepthStencilSurface(device, nullptr);
            g_originalSetVertexShader(device, nullptr);
            device->SetPixelShader(nullptr);
            g_originalSetFVF(device, D3DFVF_XYZ | D3DFVF_TEX1);
            device->SetTexture(0, g_plateTexture);
            g_originalSetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
            g_originalSetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            g_originalSetRenderState(device, D3DRS_LIGHTING, FALSE);
            g_originalSetRenderState(device, D3DRS_FOGENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_STENCILENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_SRCBLEND,
                                     Cfg().premultipliedUi ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
            g_originalSetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_originalSetRenderState(device, D3DRS_COLORWRITEENABLE, 0x0F);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            const Mat4 identity = Mat4Identity();
            g_originalSetTransform(device, D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&identity));

            for (int eye = 0; eye < EyeCount; ++eye)
            {
                D3DVIEWPORT9 viewport = {};
                viewport.X = (eye == EyeLeft) ? 0 : g_stereo.EyeWidth();
                viewport.Width = g_stereo.EyeWidth();
                viewport.Height = g_stereo.EyeHeight();
                viewport.MaxZ = 1.0f;
                g_originalSetViewport(device, &viewport);

                float tanLeft = 0.0f, tanRight = 0.0f, tanTop = 0.0f, tanBottom = 0.0f;
                Vr().EyeTangents(eye, tanLeft, tanRight, tanTop, tanBottom);
                const Mat4 projection = Mat4PerspectiveTangents(tanLeft, tanRight, tanTop, tanBottom,
                                                                0.05f, 2000.0f);
                g_originalSetTransform(device, D3DTS_PROJECTION,
                                       reinterpret_cast<const D3DMATRIX*>(&projection));
                const Vec3 openVrOffset = Mat4TranslationOf(Vr().EyeToHead(eye));
                const Vec3 eyeOffset = { openVrOffset.x, openVrOffset.y, -openVrOffset.z };
                const Mat4 world = UiPanel::BodyToEye(Projection().HeadRotation(),
                                                      Projection().HeadOffsetMetres(), eyeOffset);
                g_originalSetTransform(device, D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&world));
                g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLELIST, static_cast<UINT>(vertices / 3),
                                          quads, sizeof(PanelVertex));
            }
            g_platesDrawn += static_cast<unsigned long long>(vertices / 6);
        }

        // Upright slices the curved interface sheet is drawn with.
        const int kPanelSlices = 96;

        // Draws the interface texture as a curved sheet around the viewer, once per eye,
        // straight into the stereo target. Because it is real geometry at a real
        // distance the compositor stops treating it as a flat sheet at infinity, which
        // is what made it swim about when the head moved.
        // Every cursor Windows shares between processes. Loading them is cheap and they
        // are stable for the life of the session.
        void CacheSystemCursors()
        {
            static const wchar_t* const kStandard[] = {
                IDC_ARROW, IDC_IBEAM, IDC_WAIT, IDC_CROSS, IDC_UPARROW,
                IDC_SIZENWSE, IDC_SIZENESW, IDC_SIZEWE, IDC_SIZENS, IDC_SIZEALL,
                IDC_NO, IDC_HAND, IDC_APPSTARTING, IDC_HELP,
            };

            g_systemCursorCount = 0;
            for (int i = 0; i < static_cast<int>(std::size(kStandard)); ++i)
            {
                HCURSOR cursor = LoadCursorW(nullptr, kStandard[i]);
                if (cursor != nullptr
                    && g_systemCursorCount < static_cast<int>(std::size(g_systemCursors)))
                {
                    g_systemCursors[g_systemCursorCount] = cursor;
                    ++g_systemCursorCount;
                }
            }
            WOWVR_INFO("Cached %d standard Windows cursors, used to spot a stale pointer "
                       "after a focus change.", g_systemCursorCount);
        }

        bool IsSystemCursor(HCURSOR cursor)
        {
            for (int i = 0; i < g_systemCursorCount; ++i)
            {
                if (g_systemCursors[i] == cursor)
                {
                    return true;
                }
            }
            return false;
        }

        // Keeps the pointer inside the game window while the game holds focus. Wearing
        // the headset there is no way to see it wander off, and once it has left, the
        // panel stops drawing it and clicks land on whatever else is out there.
        //
        // The clip is dropped the moment focus goes elsewhere, so alt-tab still works
        // and a crash cannot leave the desktop with a trapped pointer.
        void UpdateCursorConfinement()
        {
            const bool wanted = Cfg().confineCursor && GameHasFocus();
            if (!wanted)
            {
                if (g_cursorConfined)
                {
                    ClipCursor(nullptr);
                    g_cursorConfined = false;
                }
                return;
            }

            RECT client = {};
            POINT topLeft = {};
            if (!GetClientRect(g_gameWindow, &client)
                || client.right <= client.left || client.bottom <= client.top)
            {
                return;
            }
            if (!ClientToScreen(g_gameWindow, &topLeft))
            {
                return;
            }

            RECT screen = {};
            screen.left = topLeft.x;
            screen.top = topLeft.y;
            screen.right = topLeft.x + (client.right - client.left);
            screen.bottom = topLeft.y + (client.bottom - client.top);

            // Only when it differs from the clip Windows actually holds. The window can
            // move, and Windows itself drops a clip on some events (the secure desktop,
            // Ctrl+Alt+Del, another program's ClipCursor), which a cached flag alone would
            // never notice.
            RECT current = {};
            if (g_cursorConfined && GetClipCursor(&current)
                && current.left == screen.left && current.top == screen.top
                && current.right == screen.right && current.bottom == screen.bottom)
            {
                return;
            }

            if (ClipCursor(&screen))
            {
                g_cursorConfined = true;
                g_confinedTo = screen;
            }
        }

        // Copies a GDI bitmap into a 32-bit top-down BGRA buffer, whatever depth it was
        // stored at. A negative height is what asks GDI for top-down rows.
        bool ReadBitmapPixels(HDC dc, HBITMAP bitmap, int width, int height,
                              std::vector<uint32_t>& pixels)
        {
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
            return GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info,
                             DIB_RGB_COLORS) != 0;
        }

        // Flattens the two source bitmaps into straight (not premultiplied) BGRA, which
        // is what the panel's SRCALPHA/INVSRCALPHA blend expects.
        void ComposeCursorPixels(const std::vector<uint32_t>* colour,
                                 const std::vector<uint32_t>* mask,
                                 int width, int height, std::vector<uint32_t>& out)
        {
            // A 32-bit cursor carries its own alpha; an older colour cursor leaves that
            // byte at zero and expects the AND mask to decide instead. Telling the two
            // apart by inspection is more reliable than trusting the bit depth.
            bool colourHasAlpha = false;
            if (colour != nullptr)
            {
                for (size_t i = 0; i < colour->size() && !colourHasAlpha; ++i)
                {
                    colourHasAlpha = ((*colour)[i] & 0xFF000000u) != 0;
                }
            }

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const size_t index = static_cast<size_t>(y) * width + x;

                    if (colour != nullptr)
                    {
                        const uint32_t texel = (*colour)[index];
                        if (colourHasAlpha)
                        {
                            out[index] = texel;
                            continue;
                        }
                        // A set AND-mask bit means "leave the screen as it is".
                        const bool transparent =
                            mask != nullptr && ((*mask)[index] & 0x00FFFFFFu) != 0;
                        out[index] = transparent ? 0x00000000u : (texel | 0xFF000000u);
                        continue;
                    }

                    if (mask == nullptr)
                    {
                        continue;
                    }

                    // Monochrome: the AND mask sits directly above the XOR mask in one
                    // double-height bitmap.
                    const uint32_t andBit = (*mask)[index] & 0x00FFFFFFu;
                    const uint32_t xorBit =
                        (*mask)[static_cast<size_t>(y + height) * width + x] & 0x00FFFFFFu;
                    if (andBit != 0)
                    {
                        // Either transparent, or an inversion of whatever is behind it.
                        // A texture cannot invert, so that case is drawn white.
                        out[index] = (xorBit != 0) ? 0xFFFFFFFFu : 0x00000000u;
                    }
                    else
                    {
                        out[index] = (xorBit != 0) ? 0xFFFFFFFFu : 0xFF000000u;
                    }
                }
            }
        }

        bool UploadCursorTexture(IDirect3DDevice9* device, const std::vector<uint32_t>& pixels,
                                 int width, int height, CursorArt& art)
        {
            // Managed pool so it survives a device reset without needing to be rebuilt.
            IDirect3DTexture9* texture = nullptr;
            g_creatingProxyResource = true;
            const HRESULT createHr = device->CreateTexture(
                static_cast<UINT>(width), static_cast<UINT>(height),
                1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr);
            g_creatingProxyResource = false;
            if (FAILED(createHr) || texture == nullptr)
            {
                return false;
            }

            D3DLOCKED_RECT locked = {};
            if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
            {
                texture->Release();
                return false;
            }

            for (int y = 0; y < height; ++y)
            {
                memcpy(static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch,
                       pixels.data() + static_cast<size_t>(y) * width,
                       static_cast<size_t>(width) * sizeof(uint32_t));
            }
            texture->UnlockRect(0);

            art.texture = texture;
            return true;
        }

        // Turns a Windows cursor handle into something drawable. Handles both forms the
        // API hands back: a colour bitmap plus mask, and the older monochrome cursor
        // where the shape lives entirely in a double-height AND/XOR mask.
        bool BuildCursorArt(IDirect3DDevice9* device, HCURSOR cursor, CursorArt& art)
        {
            ICONINFO info = {};
            if (!GetIconInfo(cursor, &info))
            {
                return false;
            }

            BITMAP maskInfo = {};
            BITMAP colourInfo = {};
            const bool haveMask = info.hbmMask != nullptr
                                  && GetObjectW(info.hbmMask, sizeof(maskInfo), &maskInfo) != 0;
            const bool haveColour = info.hbmColor != nullptr
                                    && GetObjectW(info.hbmColor, sizeof(colourInfo), &colourInfo) != 0;

            bool built = false;
            int width = 0;
            int height = 0;
            if (haveMask || haveColour)
            {
                width = haveColour ? colourInfo.bmWidth : maskInfo.bmWidth;
                // Only half of a monochrome mask is the visible shape.
                height = haveColour ? colourInfo.bmHeight : maskInfo.bmHeight / 2;
            }

            if (width > 0 && height > 0 && width <= 256 && height <= 256)
            {
                HDC dc = CreateCompatibleDC(nullptr);
                if (dc != nullptr)
                {
                    std::vector<uint32_t> colour;
                    std::vector<uint32_t> mask;
                    const bool colourRead =
                        haveColour && ReadBitmapPixels(dc, info.hbmColor, width, height, colour);
                    const bool maskRead =
                        haveMask && ReadBitmapPixels(dc, info.hbmMask, maskInfo.bmWidth,
                                                     maskInfo.bmHeight, mask);

                    if (colourRead || maskRead)
                    {
                        std::vector<uint32_t> pixels(
                            static_cast<size_t>(width) * static_cast<size_t>(height), 0);
                        ComposeCursorPixels(colourRead ? &colour : nullptr,
                                            maskRead ? &mask : nullptr, width, height, pixels);
                        built = UploadCursorTexture(device, pixels, width, height, art);
                    }
                    DeleteDC(dc);
                }
            }

            if (built)
            {
                art.source = cursor;
                art.width = width;
                art.height = height;
                art.hotspotX = static_cast<int>(info.xHotspot);
                art.hotspotY = static_cast<int>(info.yHotspot);
            }

            // GetIconInfo hands over copies, and they are ours to release.
            if (info.hbmMask != nullptr) { DeleteObject(info.hbmMask); }
            if (info.hbmColor != nullptr) { DeleteObject(info.hbmColor); }
            return built;
        }

        // The art for whatever cursor Windows is showing right now, built the first time
        // each shape appears. Null when the cursor is hidden - which is what the game
        // does during mouse-look, and drawing nothing there is correct, not a failure.
        const CursorArt* CurrentCursorArt(IDirect3DDevice9* device)
        {
            CURSORINFO cursorInfo = {};
            cursorInfo.cbSize = sizeof(cursorInfo);
            const bool haveGlobal = GetCursorInfo(&cursorInfo) != FALSE;

            // A hidden pointer stays hidden when the GAME hid it - mouselook, a drag of
            // the camera - and that must not be second-guessed. When the game says it is
            // showing, Windows hiding it (hide-while-typing) or reporting no shape is not
            // the game's doing, and the last art the game set is drawn instead.
            const int gameFlag = ReadGameCursorVisible();
            g_lastGameCursorFlag = gameFlag;
            const bool windowsShowing = haveGlobal && (cursorInfo.flags & CURSOR_SHOWING) != 0;
            HCURSOR shape = nullptr;
            if (windowsShowing)
            {
                shape = cursorInfo.hCursor;
            }
            if (gameFlag == 0)
            {
                shape = nullptr;
            }
            else if (gameFlag == 1 && shape == nullptr && g_lastGameCursor != nullptr
                     && GameHasFocus())
            {
                shape = g_lastGameCursor;
                ++g_vanishSubstitutions;
            }

            if (shape != nullptr && GameHasFocus())
            {
                if (!IsSystemCursor(shape))
                {
                    // The game's own art. Remember it: this is what to fall back on if
                    // Windows leaves a standard cursor behind after a focus change.
                    g_lastGameCursor = shape;
                }
                else if (g_lastGameCursor != nullptr)
                {
                    shape = g_lastGameCursor;
                    ++g_staleCursorSubstitutions;
                }
            }

            if (Cfg().logCursorDecisions)
            {
                static bool lastFocus = false;
                static HCURSOR lastGlobal = reinterpret_cast<HCURSOR>(1);
                static HCURSOR lastShape = reinterpret_cast<HCURSOR>(1);
                const bool focus = GameHasFocus();
                if (focus != lastFocus || cursorInfo.hCursor != lastGlobal || shape != lastShape)
                {
                    lastFocus = focus;
                    lastGlobal = cursorInfo.hCursor;
                    lastShape = shape;
                    WOWVR_INFO("Cursor decision: focus=%s global=%p (%s) drawing=%p "
                               "lastGameArt=%p flags=0x%lx gameFlag=%d substitutions=%llu",
                               focus ? "yes" : "no", static_cast<void*>(cursorInfo.hCursor),
                               IsSystemCursor(cursorInfo.hCursor) ? "standard" : "game art",
                               static_cast<void*>(shape),
                               static_cast<void*>(g_lastGameCursor),
                               cursorInfo.flags, gameFlag, g_staleCursorSubstitutions);
                }
            }

            if (shape == nullptr)
            {
                return nullptr;
            }

            // Same handle as last time: the art already built for it, or nothing if it
            // would not build (remembered, so a broken shape is not retried per frame).
            if (shape == g_lastLookupShape && g_lastLookupSlot >= 0)
            {
                const CursorArt& known = g_cursorCache[g_lastLookupSlot];
                return (known.texture != nullptr) ? &known : nullptr;
            }

            // A different handle: build afresh into the oldest slot.
            const int slot = g_cursorCacheNext;
            g_cursorCacheNext = (g_cursorCacheNext + 1) % kCursorCacheSize;
            if (g_cursorCacheCount < kCursorCacheSize)
            {
                ++g_cursorCacheCount;
            }
            if (g_cursorCache[slot].texture != nullptr)
            {
                g_cursorCache[slot].texture->Release();
            }
            g_cursorCache[slot] = CursorArt();

            CursorArt art;
            const bool built = BuildCursorArt(device, shape, art);
            if (!built)
            {
                art.source = shape;
                art.texture = nullptr;
            }
            g_cursorCache[slot] = art;
            g_lastLookupShape = shape;
            g_lastLookupSlot = slot;
            ++g_cursorArtBuilds;

            if (!built)
            {
                WOWVR_WARN("Could not read cursor shape %p; it will not be drawn.", shape);
                return nullptr;
            }
            return &g_cursorCache[slot];
        }

        void ReleaseCursorTextures()
        {
            for (int i = 0; i < g_cursorCacheCount; ++i)
            {
                if (g_cursorCache[i].texture != nullptr)
                {
                    g_cursorCache[i].texture->Release();
                }
                g_cursorCache[i] = CursorArt();
            }
            g_cursorCacheCount = 0;
            g_cursorCacheNext = 0;
            g_lastLookupShape = nullptr;
            g_lastLookupSlot = -1;
            g_activeCursor = nullptr;
        }

        // Where the pointer sits inside the game window, 0..1, along with the size of
        // that window so the art can be scaled to match. False when the pointer is
        // outside the window or the window is not known yet.
        bool CursorPanelPosition(float& u, float& v, float& clientWidth, float& clientHeight)
        {
            if (g_gameWindow == nullptr)
            {
                return false;
            }

            POINT point = {};
            RECT client = {};
            if (!GetCursorPos(&point) || !ScreenToClient(g_gameWindow, &point)
                || !GetClientRect(g_gameWindow, &client)
                || client.right <= client.left || client.bottom <= client.top)
            {
                return false;
            }

            clientWidth = static_cast<float>(client.right - client.left);
            clientHeight = static_cast<float>(client.bottom - client.top);
            u = static_cast<float>(point.x) / clientWidth;
            v = static_cast<float>(point.y) / clientHeight;
            return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
        }

        // Reads the client's shadow map back to a BMP. If the cascade box shows as
        // uniformly shadowed in the world, the first thing to establish is whether the
        // map has any caster depths in it at all.
        void DumpShadowMap(IDirect3DDevice9* device)
        {
            g_dumpShadowMapNext = false;
            if (g_shadowSurface == nullptr)
            {
                return;
            }

            D3DSURFACE_DESC desc = {};
            if (FAILED(g_shadowSurface->GetDesc(&desc)))
            {
                return;
            }

            IDirect3DSurface9* staging = nullptr;
            if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                           D3DPOOL_SYSTEMMEM, &staging, nullptr))
                || staging == nullptr)
            {
                WOWVR_WARN("Could not create a staging surface for the shadow map.");
                return;
            }

            const HRESULT hr = device->GetRenderTargetData(g_shadowSurface, staging);
            if (SUCCEEDED(hr))
            {
                D3DLOCKED_RECT locked = {};
                if (SUCCEEDED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
                {
                    wchar_t name[64];
                    swprintf_s(name, L"WoWVR_shadowmap_%02d.bmp", g_shadowMapDumpIndex++);
                    SaveBgraBmp(ModuleFile(name).c_str(), locked.pBits,
                                desc.Width, desc.Height, static_cast<uint32_t>(locked.Pitch));
                    staging->UnlockRect();
                    WOWVR_INFO("Shadow map %ux%u fmt=%d written to %S",
                               desc.Width, desc.Height, static_cast<int>(desc.Format), name);
                }
            }
            else
            {
                WOWVR_WARN("GetRenderTargetData on the shadow map failed (0x%08lx).", hr);
            }

            staging->Release();
        }

        // The interface goes out as a SteamVR overlay when it is asked for and the frame
        // is on the copy presenter (whose D3D11 device the overlay texture lives on).
        bool OverlayModeActive()
        {
            return Cfg().panelOverlay && Vr().IsActive() && !Vr().InterfaceOverlayFailed()
                && g_uiTarget.IsReady() && g_uiPanel.IsReady() && g_presenter.IsReady()
                && !g_zeroCopyActive && !g_glInteropActive && !g_d3d12Active;
        }

        // The overlay's pose in tracking space: centred on the middle of the panel's arc,
        // +Z back toward the cylinder's axis (the viewer), +Y up, +X to the viewer's
        // right - the same placement the panel geometry and the pointing math use.
        void BuildInterfaceOverlayPose(float out[3][4])
        {
            const PanelShape& shape = g_uiPanel.Shape();
            const Vec3 outward = { sinf(shape.yaw), 0.0f, cosf(shape.yaw) };
            const Vec3 middle = { shape.centre.x + outward.x * shape.radius, shape.centre.y,
                                  shape.centre.z + outward.z * shape.radius };
            const Vec3 position = Projection().BodyToStage(middle, true);
            const Vec3 z = Projection().BodyToStage({ -outward.x, 0.0f, -outward.z }, false);
            const Vec3 y = Projection().BodyToStage({ 0.0f, 1.0f, 0.0f }, false);
            const Vec3 x = { y.y * z.z - y.z * z.y, y.z * z.x - y.x * z.z, y.x * z.y - y.y * z.x };
            const Vec3 axes[3] = { x, y, z };
            for (int column = 0; column < 3; ++column)
            {
                out[0][column] = axes[column].x;
                out[1][column] = axes[column].y;
                out[2][column] = axes[column].z;
            }
            out[0][3] = position.x;
            out[1][3] = position.y;
            out[2][3] = position.z;
        }

        struct ScreenVertex
        {
            float x;
            float y;
            float z;
            float rhw;
            float u;
            float v;
        };

        // Draws a texture as a screen-space rectangle on whatever target is bound.
        void DrawScreenRect(IDirect3DDevice9* device, IDirect3DBaseTexture9* texture, float left,
                            float top, float right, float bottom, DWORD alphaFactor = 0xFF)
        {
            // Half-pixel offset: D3D9 maps texel centres to pixel corners.
            left -= 0.5f;
            top -= 0.5f;
            right -= 0.5f;
            bottom -= 0.5f;
            const ScreenVertex quad[4] = {
                { left,  top,    0.0f, 1.0f, 0.0f, 0.0f },
                { right, top,    0.0f, 1.0f, 1.0f, 0.0f },
                { left,  bottom, 0.0f, 1.0f, 0.0f, 1.0f },
                { right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
            };
            g_originalSetVertexShader(device, nullptr);
            device->SetPixelShader(nullptr);
            g_originalSetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1);
            device->SetTexture(0, texture);
            g_originalSetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
            g_originalSetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            g_originalSetRenderState(device, D3DRS_LIGHTING, FALSE);
            g_originalSetRenderState(device, D3DRS_FOGENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_STENCILENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_COLORWRITEENABLE, 0x0F);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            if (alphaFactor >= 0xFF)
            {
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            }
            else
            {
                // Fading: the texture's alpha scaled by a constant.
                g_originalSetRenderState(device, D3DRS_TEXTUREFACTOR, alphaFactor << 24);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
            }
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
        }

        // Overlay mode: the pointer is drawn into the interface texture itself, so the
        // overlay shows it. Straight-alpha art over a premultiplied target: colour by
        // source alpha, and alpha accumulated as premultiplied coverage.
        void DrawCursorIntoInterface(IDirect3DDevice9* device)
        {
            float u = 0.0f;
            float v = 0.0f;
            float clientWidth = 0.0f;
            float clientHeight = 0.0f;
            const bool havePointer = CursorPanelPosition(u, v, clientWidth, clientHeight);
            g_lastCursorU = havePointer ? u : -1.0f;
            g_lastCursorV = havePointer ? v : -1.0f;
            if (g_activeCursor == nullptr || !havePointer || !(clientWidth > 0.0f))
            {
                return;
            }

            const float width = static_cast<float>(g_uiPanel.Width());
            const float height = static_cast<float>(g_uiPanel.Height());
            g_originalSetRenderTarget(device, 0, g_uiPanel.Surface());
            g_originalSetDepthStencilSurface(device, nullptr);
            D3DVIEWPORT9 viewport = {};
            viewport.Width = g_uiPanel.Width();
            viewport.Height = g_uiPanel.Height();
            viewport.MaxZ = 1.0f;
            g_originalSetViewport(device, &viewport);

            g_originalSetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_originalSetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_originalSetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
            g_originalSetRenderState(device, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);

            // Texture pixels per window pixel, times the size knob.
            const float scale = (width / clientWidth) * Cfg().cursorScale / Canvas().PanelScale();
            const float left = u * width - g_activeCursor->hotspotX * scale;
            const float top = v * height - g_activeCursor->hotspotY * scale;
            DrawScreenRect(device, g_activeCursor->texture, left, top,
                           left + g_activeCursor->width * scale,
                           top + g_activeCursor->height * scale);
            ++g_cursorDrawn;
        }

        // The Ctrl+Alt+F1 command list, drawn into the interface image (centred, above the
        // game's interface and below the pointer) so it appears wherever the interface
        // does. Leaves the interface surface bound.
        // The launch hint's clock: started the first time the interface is actually
        // composited for the headset, so it is not spent while SteamVR is still coming up.
        DWORD g_launchHintStart = 0;
        bool g_launchHintStarted = false;

        // How visible the launch hint is right now: 0 gone, 255 fully shown, fading out
        // over its last second.
        DWORD LaunchHintAlpha()
        {
            const float seconds = Cfg().launchHintSeconds;
            if (!(seconds > 0.0f) || !g_launchHintStarted || Help().Visible())
            {
                return 0;
            }
            const float elapsed = static_cast<float>(GetTickCount() - g_launchHintStart) / 1000.0f;
            if (elapsed >= seconds)
            {
                return 0;
            }
            const float left = seconds - elapsed;
            const float fade = (left < 1.0f) ? left : 1.0f;
            return static_cast<DWORD>(fade * 255.0f);
        }

        // Draws a text card into the interface image at (left, top), or centred when
        // left is negative. Leaves the interface surface bound.
        void DrawCardIntoInterface(IDirect3DDevice9* device, TextCard& textCard, float left,
                                   float top, DWORD alpha)
        {
            IDirect3DTexture9* card = textCard.Texture(device);
            if (card == nullptr || alpha == 0)
            {
                return;
            }

            const float width = static_cast<float>(g_uiPanel.Width());
            const float height = static_cast<float>(g_uiPanel.Height());
            g_originalSetRenderTarget(device, 0, g_uiPanel.Surface());
            g_originalSetDepthStencilSurface(device, nullptr);
            D3DVIEWPORT9 viewport = {};
            viewport.Width = g_uiPanel.Width();
            viewport.Height = g_uiPanel.Height();
            viewport.MaxZ = 1.0f;
            g_originalSetViewport(device, &viewport);

            // Straight-alpha art over the premultiplied interface, as for the pointer.
            g_originalSetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_originalSetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_originalSetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
            g_originalSetRenderState(device, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);

            const float cardWidth = static_cast<float>(textCard.Width()) / Canvas().PanelScale();
            const float cardHeight = static_cast<float>(textCard.Height()) / Canvas().PanelScale();
            if (left < 0.0f)
            {
                left = floorf((width - cardWidth) * 0.5f);
                top = floorf((height - cardHeight) * 0.4f);
            }
            DrawScreenRect(device, card, left, top, left + cardWidth, top + cardHeight, alpha);
        }

        // The Ctrl+Alt+F1 list (centred) and the launch hint (top-left corner), whichever
        // is showing. True if anything was drawn.
        bool DrawHelpIntoInterface(IDirect3DDevice9* device)
        {
            if (!g_launchHintStarted && Vr().IsActive())
            {
                g_launchHintStarted = true;
                g_launchHintStart = GetTickCount();
            }
            bool drew = false;
            if (Help().Visible())
            {
                DrawCardIntoInterface(device, Help(), -1.0f, -1.0f, 0xFF);
                drew = true;
            }
            const DWORD hint = LaunchHintAlpha();
            if (hint > 0)
            {
                DrawCardIntoInterface(device, LaunchHintCard(), 24.0f, 24.0f, hint);
                drew = true;
            }
            return drew;
        }

        // Overlay mode: the eye image no longer contains the interface, so the desktop
        // mirror lays it over the copied eye flat, at window size.
        void DrawInterfaceOnMirror(IDirect3DDevice9* device)
        {
            IDirect3DStateBlock9* saved = nullptr;
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)))
            {
                saved = nullptr;
            }
            g_originalSetRenderTarget(device, 0, g_realBackBuffer);
            g_originalSetDepthStencilSurface(device, nullptr);
            D3DVIEWPORT9 viewport = {};
            viewport.Width = g_backBufferWidth;
            viewport.Height = g_backBufferHeight;
            viewport.MaxZ = 1.0f;
            g_originalSetViewport(device, &viewport);
            g_originalSetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_SRCBLEND,
                                     Cfg().premultipliedUi ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
            g_originalSetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            // On a stretched canvas (game/ui_canvas.h) the interface is drawn smaller in
            // the middle of its image; magnified about the centre here, the desktop shows
            // it at its usual size, the extra canvas running off the window's edges.
            const float canvas = Canvas().PanelScale();
            const float w = static_cast<float>(g_backBufferWidth);
            const float h = static_cast<float>(g_backBufferHeight);
            const float grow = (canvas - 1.0f) * 0.5f;
            DrawScreenRect(device, g_uiPanel.Texture(), -w * grow, -h * grow,
                           w * (1.0f + grow), h * (1.0f + grow));
            if (saved != nullptr)
            {
                saved->Apply();
                saved->Release();
            }
        }

        void CompositeUiPanel(IDirect3DDevice9* device)
        {
            // The interface pass is over either way, so the alpha override has to come
            // off before anything else is drawn - including this composite.
            EndUiAlphaOverride(device);
            g_uiTargetBound = false;

            g_panelCompositedLastFrame = false;
            if (!g_uiRendered || !g_uiPanel.IsReady() || !g_stereoHasContent)
            {
                ++g_panelSkipped;
                g_lastSkipReason = !g_uiPanel.IsReady() ? "panel target missing"
                                 : !g_stereoHasContent ? "no stereo content"
                                 : "UI pass never started";
                return;
            }
            ++g_panelDrawn;
            g_panelCompositedLastFrame = true;

            // Back onto the stereo target; the interface pass moved it away.
            g_originalSetRenderTarget(device, 0, g_stereo.Color());
            g_originalSetDepthStencilSurface(device, nullptr);

            // The panel is drawn with the fixed-function pipeline, which means changing
            // texture stage states, sampler states, lighting, fog and culling. The game
            // does not reset all of that before its own fixed-function draws, so
            // without putting it back the distant terrain renders untextured and flat.
            IDirect3DStateBlock9* savedState = nullptr;
            if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &savedState)))
            {
                savedState = nullptr;
                if (!g_stateBlockFailureLogged)
                {
                    WOWVR_WARN("Could not capture device state before compositing the UI "
                               "panel; the game's own fixed-function draws may be affected.");
                    g_stateBlockFailureLogged = true;
                }
            }

            UpdateCursorConfinement();
            g_activeCursor = CurrentCursorArt(device);

            // The lifebars' strip, out of the interface before anything reads it.
            CapturePlateStrip(device);

            // As a SteamVR overlay the panel is not drawn into the eyes at all; only the
            // pointer has to be put into the interface image the overlay shows.
            if (OverlayModeActive())
            {
                // Into the eyes (the overlay goes on top of them in the compositor).
                DrawWorldPlates(device);
                DrawVignette(device);
                DrawHelpIntoInterface(device);
                DrawCursorIntoInterface(device);
                if (savedState != nullptr)
                {
                    savedState->Apply();
                    savedState->Release();
                }
                g_originalSetRenderTarget(device, 0, g_realBackBuffer);
                return;
            }

            // Lifebars first, then the vignette, so the interface sheet lies over both.
            DrawWorldPlates(device);
            DrawVignette(device);
            g_originalSetRenderTarget(device, 0, g_stereo.Color());
            g_originalSetDepthStencilSurface(device, nullptr);

            // Into the interface image before the sheet samples it; then back onto the
            // stereo target the sheet is drawn into.
            if (DrawHelpIntoInterface(device))
            {
                g_originalSetRenderTarget(device, 0, g_stereo.Color());
                g_originalSetDepthStencilSurface(device, nullptr);
            }

            // The curved sheet as one triangle strip of upright slices. Enough slices
            // that the chord sag is far below a pixel: at the default 1.6 m radius and
            // ~70-90 degrees of arc, 96 slices sag well under a tenth of a millimetre.
            const PanelShape& shape = g_uiPanel.Shape();
            static PanelVertex sheet[(kPanelSlices + 1) * 2];
            for (int i = 0; i <= kPanelSlices; ++i)
            {
                const float u = static_cast<float>(i) / static_cast<float>(kPanelSlices);
                const Vec3 top = shape.LocalPoint(u, 0.0f);
                const Vec3 bottom = shape.LocalPoint(u, 1.0f);
                sheet[i * 2] = { top.x, top.y, top.z, u, 0.0f };
                sheet[i * 2 + 1] = { bottom.x, bottom.y, bottom.z, u, 1.0f };
            }

            // Fixed function is enough for a textured quad and avoids having to ship
            // shaders that match whatever the client is using.
            g_originalSetVertexShader(device, nullptr);
            device->SetPixelShader(nullptr);
            g_originalSetFVF(device, D3DFVF_XYZ | D3DFVF_TEX1);
            device->SetTexture(0, g_uiPanel.Texture());

            g_originalSetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
            g_originalSetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
            g_originalSetRenderState(device, D3DRS_LIGHTING, FALSE);
            g_originalSetRenderState(device, D3DRS_FOGENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
            // The panel's colour is already scaled by its own alpha - that is what
            // blending onto a transparent target produces - so multiplying by alpha a
            // second time here would darken everything translucent towards black.
            // Premultiplied compositing adds the colour as it stands and uses alpha
            // only to decide how much of the world shows through.
            g_originalSetRenderState(device, D3DRS_SRCBLEND,
                                     Cfg().premultipliedUi ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
            g_originalSetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_originalSetRenderState(device, D3DRS_COLORWRITEENABLE, 0x0F);

            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

            const Mat4 identity = Mat4Identity();
            g_originalSetTransform(device, D3DTS_VIEW,
                                   reinterpret_cast<const D3DMATRIX*>(&identity));

            for (int eye = 0; eye < EyeCount; ++eye)
            {
                D3DVIEWPORT9 viewport = {};
                viewport.X = (eye == EyeLeft) ? 0 : g_stereo.EyeWidth();
                viewport.Y = 0;
                viewport.Width = g_stereo.EyeWidth();
                viewport.Height = g_stereo.EyeHeight();
                viewport.MinZ = 0.0f;
                viewport.MaxZ = 1.0f;
                g_originalSetViewport(device, &viewport);

                float tanLeft = 0.0f;
                float tanRight = 0.0f;
                float tanTop = 0.0f;
                float tanBottom = 0.0f;
                Vr().EyeTangents(eye, tanLeft, tanRight, tanTop, tanBottom);

                // Metres, not game units: the panel is sized in real-world terms so it
                // stays put regardless of how the world is scaled.
                const Mat4 projection = Mat4PerspectiveTangents(tanLeft, tanRight, tanTop, tanBottom,
                                                                0.05f, 100.0f);
                g_originalSetTransform(device, D3DTS_PROJECTION,
                                       reinterpret_cast<const D3DMATRIX*>(&projection));

                const Vec3 openVrOffset = Mat4TranslationOf(Vr().EyeToHead(eye));
                Vec3 eyeOffset;
                eyeOffset.x = openVrOffset.x;
                eyeOffset.y = openVrOffset.y;
                eyeOffset.z = -openVrOffset.z;   // right-handed to left-handed

                const Mat4 world = g_uiPanel.PanelToEye(Projection().HeadRotation(),
                                                        Projection().HeadOffsetMetres(),
                                                        eyeOffset);
                g_originalSetTransform(device, D3DTS_WORLD,
                                       reinterpret_cast<const D3DMATRIX*>(&world));

                g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, kPanelSlices * 2, sheet,
                                          sizeof(PanelVertex));

                // The pointer, on the same panel and in the same world transform, so it
                // sits on the interface rather than floating in front of it.
                float u = 0.0f;
                float v = 0.0f;
                float clientWidth = 0.0f;
                float clientHeight = 0.0f;
                const bool havePointer = CursorPanelPosition(u, v, clientWidth, clientHeight);
                g_lastCursorU = havePointer ? u : -1.0f;
                g_lastCursorV = havePointer ? v : -1.0f;
                if (g_activeCursor != nullptr && havePointer && clientWidth > 0.0f)
                {
                    ++g_cursorDrawn;

                    // Metres per game pixel, so the pointer covers the same fraction of
                    // the interface here as it does on the desktop. Scale is on a knob
                    // because a 32-pixel cursor at true size is small through a headset.
                    const float scale = Cfg().cursorScale / Canvas().PanelScale();
                    const float pixel = shape.metresPerPixel
                                      * (static_cast<float>(g_uiPanel.Width()) / clientWidth)
                                      * scale;

                    // A small flat card laid against the curve where the pointer is,
                    // shifted by the hotspot so the part of the image that does the
                    // pointing lands on the pixel being pointed at rather than the
                    // image's top-left corner. Panel Y runs up, image Y runs down.
                    const Vec3 at = shape.LocalPoint(u, v);
                    const Vec3 across = shape.LocalTangentU(u);
                    const float left = -g_activeCursor->hotspotX * pixel;
                    const float right = (g_activeCursor->width - g_activeCursor->hotspotX) * pixel;
                    const float top = g_activeCursor->hotspotY * pixel;
                    const float bottom = -(g_activeCursor->height - g_activeCursor->hotspotY) * pixel;

                    const PanelVertex pointer[4] = {
                        { at.x + across.x * left,  at.y + top,    at.z + across.z * left,  0.0f, 0.0f },
                        { at.x + across.x * right, at.y + top,    at.z + across.z * right, 1.0f, 0.0f },
                        { at.x + across.x * left,  at.y + bottom, at.z + across.z * left,  0.0f, 1.0f },
                        { at.x + across.x * right, at.y + bottom, at.z + across.z * right, 1.0f, 1.0f },
                    };

                    // Built straight rather than premultiplied, so it needs the
                    // ordinary blend even when the panel itself is composited
                    // premultiplied.
                    device->SetTexture(0, g_activeCursor->texture);
                    g_originalSetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                    g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, pointer,
                                              sizeof(PanelVertex));
                    g_originalSetRenderState(device, D3DRS_SRCBLEND,
                                             Cfg().premultipliedUi ? D3DBLEND_ONE
                                                                   : D3DBLEND_SRCALPHA);
                    device->SetTexture(0, g_uiPanel.Texture());
                }
            }

            if (savedState != nullptr)
            {
                savedState->Apply();
                savedState->Release();
            }

            // Release the stereo surface before anyone tries to read it back: D3D9
            // refuses StretchRect and GetRenderTargetData on a target that is still
            // bound, and the capture that follows does both.
            g_originalSetRenderTarget(device, 0, g_realBackBuffer);
        }

        // Writes the whole side-by-side target, unscaled and uncropped, to a BMP.
        //
        // The per-eye dumps are taken after a StretchRect out of one half, which hides
        // anything to do with how the two halves relate: mis-sized viewports, regions
        // that were never cleared, content drawn once instead of twice. Those are
        // exactly the bugs left, so this dumps the target itself.
        int g_stereoDumpIndex = 0;

        // A stamp fixed at first use, so a session's shots never overwrite the previous
        // session's. Without it every run restarted at 00 and quietly clobbered whatever
        // was there.
        const wchar_t* SessionStamp()
        {
            static wchar_t stamp[24] = {};
            if (stamp[0] == 0)
            {
                SYSTEMTIME now = {};
                GetLocalTime(&now);
                swprintf_s(stamp, L"%02d%02d-%02d%02d",
                           now.wMonth, now.wDay, now.wHour, now.wMinute);
            }
            return stamp;
        }

        // One line per shot, so the series can be read back without guessing which files
        // belong to which run.
        void AppendShotManifest(const wchar_t* file, float yaw, float pitch,
                                float x, float y, float z)
        {
            wchar_t manifestName[64];
            swprintf_s(manifestName, L"WoWVR_shots_%s.txt", SessionStamp());

            FILE* manifest = nullptr;
            if (_wfopen_s(&manifest, ModuleFile(manifestName).c_str(), L"a, ccs=UTF-8") != 0
                || manifest == nullptr)
            {
                return;
            }

            SYSTEMTIME now = {};
            GetLocalTime(&now);
            fwprintf(manifest, L"%02d  %s  %02d:%02d:%02d  yaw %+7.2f  pitch %+7.2f  "
                               L"pos (%+.3f, %+.3f, %+.3f)\n",
                     g_stereoDumpIndex, file, now.wHour, now.wMinute, now.wSecond,
                     yaw, pitch, x, y, z);
            fclose(manifest);
        }

        void DumpStereoTarget(IDirect3DDevice9* device)
        {
            if (!g_stereo.IsReady())
            {
                return;
            }

            const uint32_t width = g_stereo.EyeWidth() * 2;
            const uint32_t height = g_stereo.EyeHeight();

            IDirect3DSurface9* staging = nullptr;
            HRESULT hr = device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
                                                             D3DPOOL_SYSTEMMEM, &staging, nullptr);
            if (FAILED(hr) || staging == nullptr)
            {
                WOWVR_WARN("Could not create a staging surface for the stereo dump (0x%08lx).", hr);
                return;
            }

            hr = device->GetRenderTargetData(g_stereo.Color(), staging);
            if (SUCCEEDED(hr))
            {
                D3DLOCKED_RECT locked = {};
                if (SUCCEEDED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
                {
                    // Stamped with the session so a series never overwrites an earlier
                    // run's, plus the fixed name for tooling that just wants the latest.
                    wchar_t numbered[64];
                    swprintf_s(numbered, L"WoWVR_shot_%s_%02d.bmp",
                               SessionStamp(), g_stereoDumpIndex);
                    SaveBgraBmp(ModuleFile(L"WoWVR_stereo.bmp").c_str(), locked.pBits,
                                width, height, static_cast<uint32_t>(locked.Pitch));
                    SaveBgraBmp(ModuleFile(numbered).c_str(), locked.pBits,
                                width, height, static_cast<uint32_t>(locked.Pitch));
                    staging->UnlockRect();

                    // The pose the shot was taken at. Without this a series of captures
                    // cannot be told apart from a series where tracking had frozen -
                    // which looks exactly like "everything is welded to the head".
                    const Mat4& head = Vr().HeadToStage();
                    const float yaw = atan2f(-head.m[2][0], head.m[2][2]) * 57.2957795f;
                    const float pitch = asinf(head.m[2][1] > 1.0f ? 1.0f
                                              : (head.m[2][1] < -1.0f ? -1.0f : head.m[2][1]))
                                        * 57.2957795f;
                    WOWVR_INFO("stereo dump %02d: head yaw %.2f deg, pitch %.2f deg, "
                               "pos (%.3f, %.3f, %.3f), poseValid=%d, userPresent=%d",
                               g_stereoDumpIndex, yaw, pitch,
                               head.m[3][0], head.m[3][1], head.m[3][2],
                               Vr().HasHeadPose() ? 1 : 0, Vr().UserIsPresent() ? 1 : 0);
                    WOWVR_INFO("  saved %S", numbered);
                    AppendShotManifest(numbered, yaw, pitch,
                                       head.m[3][0], head.m[3][1], head.m[3][2]);
                    ++g_stereoDumpIndex;
                }
            }
            else
            {
                WOWVR_WARN("GetRenderTargetData for the stereo dump failed (0x%08lx).", hr);
            }

            staging->Release();
        }

        // Snapshots the stereo target mid-frame. The target has to be unbound first:
        // D3D9 will not read back a surface that is currently the render target. Slow
        // and only ever used while hunting an artefact.
        void DumpWorldProgress(IDirect3DDevice9* device, int worldDrawIndex)
        {
            if (!g_stereo.IsReady() || g_realBackBuffer == nullptr)
            {
                return;
            }

            g_originalSetRenderTarget(device, 0, g_realBackBuffer);
            g_originalSetDepthStencilSurface(device, nullptr);

            const uint32_t width = g_stereo.EyeWidth() * 2;
            const uint32_t height = g_stereo.EyeHeight();

            IDirect3DSurface9* staging = nullptr;
            if (SUCCEEDED(device->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8,
                                                              D3DPOOL_SYSTEMMEM, &staging, nullptr))
                && staging != nullptr)
            {
                if (SUCCEEDED(device->GetRenderTargetData(g_stereo.Color(), staging)))
                {
                    D3DLOCKED_RECT locked = {};
                    if (SUCCEEDED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
                    {
                        wchar_t name[64];
                        swprintf_s(name, L"WoWVR_seq_%04d.bmp", worldDrawIndex);
                        SaveBgraBmp(ModuleFile(name).c_str(), locked.pBits,
                                    width, height, static_cast<uint32_t>(locked.Pitch));
                        staging->UnlockRect();
                    }
                }
                staging->Release();
            }

            g_originalSetRenderTarget(device, 0, g_stereo.Color());
            g_originalSetDepthStencilSurface(device, g_stereo.Depth());
        }

        // The zero-copy path hands the compositor surfaces the game's device has only
        // just written with StretchRect. Legacy shared surfaces carry no fence of
        // their own, so those writes must be provably retired before Submit or the
        // headset can sample a half-blitted frame.
        void FlushGameGpuQueue(IDirect3DDevice9* device)
        {
            if (g_frameSyncQueryFailed)
            {
                return;
            }

            if (g_frameSyncQuery == nullptr)
            {
                const HRESULT hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &g_frameSyncQuery);
                if (FAILED(hr) || g_frameSyncQuery == nullptr)
                {
                    WOWVR_WARN("Event query unavailable (0x%08lx); zero-copy frames submit "
                               "unfenced.", hr);
                    g_frameSyncQueryFailed = true;
                    return;
                }
            }

            if (FAILED(g_frameSyncQuery->Issue(D3DISSUE_END)))
            {
                return;
            }

            while (g_frameSyncQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE)
            {
                YieldProcessor();
            }
        }

        void SubmitFrame(IDirect3DDevice9* device)
        {
            if (!g_resourcesReady || !Vr().IsActive())
            {
                return;
            }

            const bool stereo = Cfg().stereo && g_stereo.IsReady() && Vr().IsActive() && g_stereoHasContent;

            const bool dumpThisFrame = g_dumpNextFrame
                || (Cfg().dumpFrameNumber > 0
                    && g_frameCount == static_cast<unsigned long long>(Cfg().dumpFrameNumber));
            g_dumpNextFrame = false;

            // The pose this frame was rendered with, and the one before it, so a
            // pipelined submit can stamp its frame-old pixels with the pose that
            // actually produced them.
            g_renderPosePrevious = g_renderPoseCurrent;
            g_renderPoseCurrent = Vr().CurrentPoseStamp();

            const LARGE_INTEGER captureStart = Now();

            const bool zeroCopy = g_zeroCopyActive || g_glInteropActive || g_d3d12Active;

            // The worker may still be reading last frame's locked staging surfaces
            // and owning the D3D11 context; both must settle before this frame
            // captures into the slot it was reading or touches the presenter
            // inline. Steady state this wait is ~0 - the worker had a whole game
            // frame for ~1.3 ms of work - and it books under 'capture'. Dump
            // frames run the classic inline path so a shot stays frame-exact.
            if (!g_presenterThreadConfigApplied)
            {
                g_presenterThreadConfigApplied = true;
                g_presenterThreadOn = Cfg().presenterThread;
            }
            if (!TryWaitPresenterIdleAndUnlock())
            {
                return;
            }
            const bool threaded = g_presenterThreadOn && !zeroCopy && !dumpThisFrame
                && EnsurePresenterThread();

            for (int eye = 0; eye < EyeCount; ++eye)
            {
                if (stereo)
                {
                    RECT half;
                    half.left = (eye == EyeLeft) ? 0 : static_cast<LONG>(g_stereo.EyeWidth());
                    half.top = 0;
                    half.right = half.left + static_cast<LONG>(g_stereo.EyeWidth());
                    half.bottom = static_cast<LONG>(g_stereo.EyeHeight());

                    if (!g_eyeTargets[eye].CaptureRegion(device, g_stereo.Color(), &half))
                    {
                        return;
                    }
                }
                else if (eye == EyeLeft)
                {
                    if (!g_eyeTargets[EyeLeft].CaptureBackBuffer(device))
                    {
                        return;
                    }
                }

            }

            // The interface for the overlay: captured from this frame's interface texture
            // (pointer included) and read back alongside the eyes.
            const bool overlayFrame = !zeroCopy && OverlayModeActive() && g_panelCompositedLastFrame
                && g_uiTarget.CaptureRegion(device, g_uiPanel.Surface(), nullptr);

            const LARGE_INTEGER uploadStart = Now();
            double lockMs = 0.0;

            bool uiLocked = false;
            if (overlayFrame)
            {
                const LARGE_INTEGER uiLockStart = Now();
                uiLocked = g_uiTarget.Lock(dumpThisFrame);
                lockMs += ElapsedMs(uiLockStart, Now());
            }
            PresenterJob overlayJob = {};
            if (uiLocked)
            {
                overlayJob.uiPixels = g_uiTarget.LockedPixels();
                overlayJob.uiPitch = g_uiTarget.LockedPitch();
                overlayJob.uiWidth = g_uiTarget.Width();
                overlayJob.uiHeight = g_uiTarget.Height();
                BuildInterfaceOverlayPose(overlayJob.overlayPose);
                overlayJob.overlayWidth = g_uiPanel.Shape().radius * g_uiPanel.Shape().arc;
                overlayJob.overlayCurvature = g_uiPanel.Shape().arc / 6.28318530718f;
                overlayJob.overlayPremultiplied = Cfg().premultipliedUi;
            }
            // Hidden whenever the interface is not going out as an overlay this frame
            // and is not merely between captures: switched off, or a path without one.
            overlayJob.hideOverlay = !uiLocked && !(OverlayModeActive() && overlayFrame);

            if (g_zeroCopyActive)
            {
                // The frame is already in the surfaces the compositor will read;
                // the only work left is making sure the blits have retired. (The GL
                // route needs no fence of its own - wglDXLockObjectsNV synchronises
                // against the pending D3D9 work when the submit block locks.)
                FlushGameGpuQueue(device);
            }

            for (int eye = 0; eye < EyeCount; ++eye)
            {
                if (!stereo && eye == EyeRight)
                {
                    break;
                }

                if (zeroCopy)
                {
                    // Readback exists only to feed the occasional BMP dump.
                    if (!dumpThisFrame || !g_eyeTargets[eye].ReadBack(device))
                    {
                        continue;
                    }
                }

                // The lock is where the readback's real cost lands -
                // GetRenderTargetData is asynchronous and LockRect waits for the
                // GPU - so its time is booked under 'capture', leaving 'upload' as
                // the pure CPU-to-D3D11 copy. Pipelined it takes last frame's
                // surface and returns at once; dump frames force this frame's so
                // the BMP shows what the log line says it shows.
                const LARGE_INTEGER lockStart = Now();
                const bool locked = g_eyeTargets[eye].Lock(zeroCopy || dumpThisFrame);
                lockMs += ElapsedMs(lockStart, Now());
                if (!locked)
                {
                    continue;
                }

                if (!zeroCopy && !threaded)
                {
                    g_presenter.Upload(eye, g_eyeTargets[eye].LockedPixels(),
                                       g_eyeTargets[eye].LockedPitch());
                }

                if (dumpThisFrame && eye == EyeLeft)
                {
                    DumpStereoTarget(device);
                }

                if (dumpThisFrame)
                {
                    SaveBgraBmp(ModuleFile(eye == EyeLeft ? L"WoWVR_left.bmp" : L"WoWVR_right.bmp").c_str(),
                                g_eyeTargets[eye].LockedPixels(),
                                g_eyeTargets[eye].Width(), g_eyeTargets[eye].Height(),
                                g_eyeTargets[eye].LockedPitch());

                    // A numbered copy as well, so pressing the key repeatedly builds a
                    // series instead of overwriting the same two files. That is what
                    // sweeping a circle to record how a loaded scene should look needs,
                    // and the fixed names have to stay because the command-file `shot`
                    // and every driving script read them.
                    if (g_dumpNumbered)
                    {
                        wchar_t name[64];
                        swprintf_s(name, L"WoWVR_shot_%03d_%s.bmp", g_dumpSeriesIndex,
                                   eye == EyeLeft ? L"left" : L"right");
                        SaveBgraBmp(ModuleFile(name).c_str(),
                                    g_eyeTargets[eye].LockedPixels(),
                                    g_eyeTargets[eye].Width(), g_eyeTargets[eye].Height(),
                                    g_eyeTargets[eye].LockedPitch());
                        if (eye == EyeRight || !stereo)
                        {
                            WOWVR_INFO("Shot %03d written (head yaw %.1f deg, pitch %.1f, "
                                       "roll %.1f).",
                                       g_dumpSeriesIndex,
                                       Projection().HeadYaw() * 57.2957795f,
                                       Projection().HeadPitch() * 57.2957795f,
                                       Projection().HeadRoll() * 57.2957795f);
                            ++g_dumpSeriesIndex;
                            g_dumpNumbered = false;
                        }
                    }
                }

                // Threaded, the lock stays held: the worker reads these pixels
                // for the rest of the frame, and the release happens at the top
                // of the next SubmitFrame once the worker is provably idle.
                if (!threaded)
                {
                    g_eyeTargets[eye].Unlock();
                }
            }

            const LARGE_INTEGER submitStart = Now();
            // Without stereo both eyes get the left image, which is flat but at least
            // consistent; with stereo each eye gets its own render.
            if (g_d3d12Active)
            {
                // Unwrap fences the StretchRects, the compositor copies on our
                // queue, and the fence handed back guards the return.
                if (g_d3d12Present.SubmitEyes(g_eyeTargets[EyeLeft].RenderTargetTexture(),
                                              g_eyeTargets[EyeRight].RenderTargetTexture(),
                                              stereo))
                {
                    Vr().PostSubmit();
                }
            }
            else if (g_glInteropActive)
            {
                // The lock synchronises against the StretchRects and grants the
                // compositor's GL view access for the span of the submit.
                if (g_glInterop.LockEyes())
                {
                    const uint32_t left = g_glInterop.GlTextureName(EyeLeft);
                    Vr().SubmitEyeGl(EyeLeft, left);
                    Vr().SubmitEyeGl(EyeRight, stereo ? g_glInterop.GlTextureName(EyeRight)
                                                      : left);
                    Vr().PostSubmit();
                    g_glInterop.UnlockEyes();
                }
            }
            else
            {
                // Copy path: the uploaded pixels may be a frame old (pipelined
                // readback), so stamp the submission with the pose they were
                // rendered at and let the compositor reproject the difference.
                // The adopted-texture zero-copy route holds this frame's pixels
                // and keeps the classic unstamped submit.
                const HeadPoseStamp* renderPose = nullptr;
                if (!zeroCopy)
                {
                    renderPose = g_eyeTargets[EyeLeft].LockedIsCurrentFrame()
                                     ? &g_renderPoseCurrent
                                     : &g_renderPosePrevious;
                }

                if (threaded)
                {
                    // Hand the frame to the worker: locked staging pointers, the
                    // pose that produced them, and the upload + submit + handoff
                    // happen off the game thread. WaitGetPoses stays here.
                    g_presenterJob.pixels[EyeLeft] = g_eyeTargets[EyeLeft].LockedPixels();
                    g_presenterJob.pitch[EyeLeft] = g_eyeTargets[EyeLeft].LockedPitch();
                    g_presenterJob.pixels[EyeRight] =
                        stereo ? g_eyeTargets[EyeRight].LockedPixels() : nullptr;
                    g_presenterJob.pitch[EyeRight] =
                        stereo ? g_eyeTargets[EyeRight].LockedPitch() : 0;
                    g_presenterJob.stereo = stereo;
                    g_presenterJob.pose =
                        (renderPose != nullptr) ? *renderPose : HeadPoseStamp{};
                    g_presenterJob.uiPixels = overlayJob.uiPixels;
                    g_presenterJob.uiPitch = overlayJob.uiPitch;
                    g_presenterJob.uiWidth = overlayJob.uiWidth;
                    g_presenterJob.uiHeight = overlayJob.uiHeight;
                    memcpy(g_presenterJob.overlayPose, overlayJob.overlayPose,
                           sizeof(overlayJob.overlayPose));
                    g_presenterJob.overlayWidth = overlayJob.overlayWidth;
                    g_presenterJob.overlayCurvature = overlayJob.overlayCurvature;
                    g_presenterJob.overlayPremultiplied = overlayJob.overlayPremultiplied;
                    g_presenterJob.hideOverlay = overlayJob.hideOverlay;
                    ResetEvent(g_presenterDoneEvent);
                    ResetEvent(g_presenterSubmittedEvent);
                    g_presenterJobPostedThisFrame = true;
                    SetEvent(g_presenterJobEvent);
                }
                else
                {
                    void* leftTexture = g_presenter.EyeTexture(EyeLeft);
                    Vr().SubmitEye(EyeLeft, leftTexture, renderPose);
                    Vr().SubmitEye(EyeRight,
                                   stereo ? g_presenter.EyeTexture(EyeRight) : leftTexture,
                                   renderPose);
                    Vr().PostSubmit();
                    ApplyInterfaceOverlay(overlayJob);
                    g_uiTarget.Unlock();
                    if (dumpThisFrame)
                    {
                        g_presenter.DumpCompositorMirror(ModuleFile(L"WoWVR_compositor.bmp").c_str());
                    }
                }
            }
            if ((g_d3d12Active || g_glInteropActive) && overlayJob.hideOverlay)
            {
                Vr().HideInterfaceOverlay();
            }
            const LARGE_INTEGER submitEnd = Now();

            g_timers.captureMs += ElapsedMs(captureStart, uploadStart) + lockMs;
            g_timers.uploadMs += ElapsedMs(uploadStart, submitStart) - lockMs;
            g_timers.submitMs += ElapsedMs(submitStart, submitEnd);
            ++g_timers.samples;
        }

        // Puts the device back on its real back buffer so that Present, and anything
        // the game does before the next frame starts, behave normally. Also mirrors
        // the left eye to the desktop window so the game is still watchable on screen.
        // Diagnostic: what the desktop mirror actually put in the back buffer.
        void DumpBackBuffer(IDirect3DDevice9* device)
        {
            D3DSURFACE_DESC desc = {};
            if (g_realBackBuffer == nullptr || FAILED(g_realBackBuffer->GetDesc(&desc)))
            {
                return;
            }
            IDirect3DSurface9* staging = nullptr;
            if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                           D3DPOOL_SYSTEMMEM, &staging, nullptr)))
            {
                return;
            }
            const HRESULT hr = device->GetRenderTargetData(g_realBackBuffer, staging);
            D3DLOCKED_RECT locked = {};
            if (SUCCEEDED(hr) && SUCCEEDED(staging->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            {
                SaveBgraBmp(ModuleFile(L"WoWVR_mirror.bmp").c_str(), locked.pBits, desc.Width,
                            desc.Height, static_cast<uint32_t>(locked.Pitch));
                staging->UnlockRect();
                WOWVR_INFO("Back buffer %ux%u written to WoWVR_mirror.bmp.", desc.Width,
                           desc.Height);
            }
            else
            {
                WOWVR_WARN("Back buffer readback failed (0x%08lx).", hr);
            }
            staging->Release();
        }

        void FinishStereoFrame(IDirect3DDevice9* device)
        {
            if (g_realBackBuffer == nullptr)
            {
                return;
            }

            // Mirrored whenever this frame drew into the stereo target - NOT only while
            // the redirect is still in force. The interface pass ends the redirect every
            // frame (it moves the target to the panel), so gating the mirror on it meant
            // the copy never ran and the window kept showing a stale early frame.
            if (Cfg().desktopMirror && g_stereoHasContent && g_stereo.IsReady())
            {
                RECT half;
                half.left = 0;
                half.top = 0;
                half.right = static_cast<LONG>(g_stereo.EyeWidth());
                half.bottom = static_cast<LONG>(g_stereo.EyeHeight());

                // The eye is tall (1480x1644 on the Index) and the window is wide, so
                // stretching one into the other squashes everything. Crop the eye to
                // the window's shape instead, centred on the eye's optical centre - the
                // point straight ahead of the eye, which an Index's canted, asymmetric
                // frustum puts well off the middle of the image.
                const float eyeW = static_cast<float>(g_stereo.EyeWidth());
                const float eyeH = static_cast<float>(g_stereo.EyeHeight());
                if (Cfg().mirrorCrop && g_backBufferWidth > 0 && g_backBufferHeight > 0
                    && eyeW > 0.0f && eyeH > 0.0f)
                {
                    const float windowAspect = static_cast<float>(g_backBufferWidth)
                                             / static_cast<float>(g_backBufferHeight);
                    float cropW = eyeW;
                    float cropH = eyeW / windowAspect;
                    if (cropH > eyeH)
                    {
                        cropH = eyeH;
                        cropW = eyeH * windowAspect;
                    }

                    float tanLeft = -1.0f;
                    float tanRight = 1.0f;
                    float tanTop = -1.0f;
                    float tanBottom = 1.0f;
                    Vr().EyeTangents(EyeLeft, tanLeft, tanRight, tanTop, tanBottom);
                    const float centreX = (tanRight > tanLeft)
                        ? eyeW * (-tanLeft / (tanRight - tanLeft)) : eyeW * 0.5f;
                    const float centreY = (tanBottom > tanTop)
                        ? eyeH * (-tanTop / (tanBottom - tanTop)) : eyeH * 0.5f;

                    float left = centreX - cropW * 0.5f;
                    float top = centreY - cropH * 0.5f;
                    if (left < 0.0f) { left = 0.0f; }
                    if (top < 0.0f) { top = 0.0f; }
                    if (left + cropW > eyeW) { left = eyeW - cropW; }
                    if (top + cropH > eyeH) { top = eyeH - cropH; }

                    half.left = static_cast<LONG>(left);
                    half.top = static_cast<LONG>(top);
                    half.right = half.left + static_cast<LONG>(cropW);
                    half.bottom = half.top + static_cast<LONG>(cropH);
                }

                const HRESULT mirrorHr = device->StretchRect(g_stereo.Color(), &half,
                                                             g_realBackBuffer, nullptr,
                                                             D3DTEXF_LINEAR);
                static bool mirrorRectLogged = false;
                if (!mirrorRectLogged)
                {
                    mirrorRectLogged = true;
                    WOWVR_INFO("Desktop mirror: eye rect %ld,%ld - %ld,%ld into the %ux%u back "
                               "buffer (StretchRect 0x%08lx).", half.left, half.top, half.right,
                               half.bottom, g_backBufferWidth, g_backBufferHeight, mirrorHr);
                }
                static bool mirrorFailureLogged = false;
                if (FAILED(mirrorHr) && !mirrorFailureLogged)
                {
                    mirrorFailureLogged = true;
                    WOWVR_WARN("Desktop mirror: StretchRect failed (0x%08lx); the window will "
                               "not show the headset view.", mirrorHr);
                }
            }

            if (Cfg().desktopMirror && g_stereoHasContent && g_stereo.IsReady()
                && OverlayModeActive() && g_panelCompositedLastFrame)
            {
                DrawInterfaceOnMirror(device);
            }

            if (g_dumpMirrorNext)
            {
                g_dumpMirrorNext = false;
                DumpBackBuffer(device);
            }

            if (!g_stereoRedirected)
            {
                return;
            }

            // Bypasses our own hook deliberately, otherwise this would be redirected
            // straight back into the stereo target.
            g_originalSetRenderTarget(device, 0, g_realBackBuffer);
            g_originalSetDepthStencilSurface(device, nullptr);

            g_stereoRedirected = false;
        }

        void PollHotkeys()
        {
            if (HotkeyPressed(Hotkey::FrameReport))
            {
                // Armed for the frame that is about to start, so the report covers a
                // whole frame rather than whatever is left of the current one.
                Report().RequestFrame(g_frameCount + 1);
                g_probeCombinedPending = true;
                WOWVR_INFO("F9: frame report armed for frame %llu.", g_frameCount + 1);
            }

            if (HotkeyPressed(Hotkey::FrameDump))
            {
                g_dumpNextFrame = true;
                // Armed separately: SubmitFrame consumes g_dumpNextFrame before the
                // per-frame reset runs, so keying off it there never fires.
                g_armSequenceNextFrame = (Cfg().dumpEveryNWorldDraws > 0);
                g_dumpShadowMapNext = true;
                g_dumpNumbered = true;
                WOWVR_INFO("Ctrl+Alt+F10: eye buffers will be written on the next frame%s, "
                           "plus numbered copies WoWVR_shot_%03d_left/right.bmp.",
                           g_armSequenceNextFrame ? ", with a mid-frame draw sequence" : "",
                           g_dumpSeriesIndex);
            }

            if (HotkeyPressed(Hotkey::Recenter))
            {
                // The body frame is about to be re-measured from the current pose, so
                // "straight ahead at the origin" is where the panel belongs in it.
                Projection().Recenter();
                g_uiPanel.RecenterAt(0.0f, Vec3());
                WOWVR_INFO("F11: recentre requested.");
            }

            if (HotkeyPressed(Hotkey::RecenterUi))
            {
                // Only the interface moves. Negated yaw: PanelToBody composes
                // Mat4RotationY(yaw) the same way the old body-lock did, and that faces the
                // way the head does only for the NEGATIVE of HeadYaw(). Yaw alone, so the
                // panel turns to the face but never tilts; centred at eye height because
                // its centre is the head's current position in the body frame.
                g_uiPanel.RecenterAt(-Projection().HeadYaw(), Projection().HeadOffsetMetres());
                WOWVR_INFO("F8: interface panel placed in front of the head (yaw %.1f deg, "
                           "eye height %+.2f m).", -Projection().HeadYaw() * 57.29578f,
                           Projection().HeadOffsetMetres().y);
            }

            if (HotkeyPressed(Hotkey::Vignette))
            {
                SetVignette(!Cfg().vignette);
                if (Cfg().vignette)
                {
                    Vignette().Preview(1.0f);
                }
                WOWVR_INFO("Ctrl+Alt+V: comfort vignette %s (saved).", Cfg().vignette ? "on" : "off");
            }
            if (HotkeyPressed(Hotkey::VignetteSize))
            {
                SetVignetteSize((Cfg().vignetteSize + 1) % 3);
                // A size change is shown even if the vignette is off, so the choice can be
                // seen; it does not switch the vignette on.
                Vignette().Preview(1.0f);
                WOWVR_INFO("Ctrl+Alt+N: comfort vignette size %ls (saved).",
                           VignetteSizeName(Cfg().vignetteSize));
            }

            if (HotkeyPressed(Hotkey::Help))
            {
                Help().Toggle();
                WOWVR_INFO("Ctrl+Alt+F1: command list %s.", Help().Visible() ? "shown" : "hidden");
            }

            // Pushing the interface out or pulling it in. Its angular size is set by
            // PixelsPerDegree, not by the distance, so it looks the same size at any
            // depth - only how far your eyes have to converge on it changes.
            const bool farther = HotkeyRepeated(Hotkey::PanelFarther);
            const bool nearer = HotkeyRepeated(Hotkey::PanelNearer);
            if (farther != nearer)
            {
                const float distance =
                    SetPanelDistance(Cfg().panelDistance + (farther ? 0.1f : -0.1f));
                g_uiPanel.RefreshGeometry();
                WOWVR_INFO("Ctrl+Alt+%s: interface panel distance %.2f m (saved).",
                           farther ? "PageUp" : "PageDown", distance);
            }

            if (HotkeyPressed(Hotkey::ReloadConfig))
            {
                LoadConfig();
                LogSetLevel(Cfg().logLevel);
                Projection().InvalidateCaches();
                WOWVR_INFO("F12: WoWVR.ini reloaded.");
            }
        }

        // The pitch to take back out of the view: the camera's MEASURED pitch, never
        // the intent that was written into it.
        //
        // While the free-look pitch field is being written the client discards its own
        // pitch entirely (the mouse cannot pitch the view), so every degree the camera
        // actually holds is either our aim or motion the client added on its own - the
        // follow logic that re-levels the camera while the character moves is the one
        // that showed: intent-based compensation left its easing visible, and the
        // world slowly tilted down as long as the character was walking. The head owns
        // all visible pitch in this design, so whatever the camera really holds is
        // exactly what must be removed. Measured also covers the client's write
        // smoothing, where the camera has not yet reached the intent.
        //
        // Yaw stays on intent: the yaw field is an offset on top of a heading the
        // mouse and character legitimately own and which must remain visible.
        float CompensationPitch()
        {
            float facingYaw = 0.0f;
            float facingPitch = 0.0f;
            if (GameCam().CameraFacing(facingYaw, facingPitch))
            {
                return facingPitch;
            }
            return GameCam().AppliedPitch();
        }

        // Re-measures the camera compensation from the camera as the client is USING
        // it for the frame being built.
        //
        // A measurement in the Present hook is a frame old by the time it is used:
        // the camera is written at the end of frame N, the client moves it during its
        // update, and frame N+1 is rendered against wherever it actually ended up.
        // While the camera is still - the common case - stale equals fresh and nothing
        // shows. While it is in motion the error is (camera movement per frame), which
        // during a third-person zoom is the radius change every single frame: the
        // world bounced against the head at exactly the zoom rate. And BeginScene is
        // not safe either: whether the client updates its camera before or after it is
        // an assumption, and a nodding head - a pitch target moving every frame -
        // twitched the world at exactly the per-frame pitch delta whenever that
        // assumption missed. The one moment that needs no assumption is the first
        // camera-derived constant upload of the frame: those constants are computed
        // FROM the camera, so by then it is necessarily final. That is where this is
        // called from, once per frame, via EnsureFreshCameraCompensation.
        void RefreshCameraCompensation()
        {
            if (!g_aimEnabled || !Cfg().aimCameraAtHead || !Vr().IsActive())
            {
                return;
            }

            // Re-resolved rather than trusted: the camera object dies across every
            // loading screen, and BeginScene is called during them too.
            if (!GameCam().Update())
            {
                return;
            }

            Projection().SetGameCameraPitch(CompensationPitch());

            Projection().SetGameCameraOrbitRadius(
                g_compensateOrbit ? GameCam().OrbitRadius() : 0.0f);

            Vec3 orbitShift;
            if (g_compensateOrbit
                && GameCam().OrbitDisplacementView(GameCam().AppliedYaw(), orbitShift))
            {
                Projection().SetOrbitDisplacement(orbitShift);
            }
            else
            {
                Projection().SetOrbitDisplacement(Vec3());
            }
        }

        // Once per frame, at the first moment the values are actually needed.
        bool g_compensationFresh = false;

        void EnsureFreshCameraCompensation()
        {
            if (g_compensationFresh)
            {
                return;
            }
            g_compensationFresh = true;
            RefreshCameraCompensation();

            // The persisted camera blocks are rebuilt here too, and only here: they
            // feed the early terrain draws of this very frame, so they must use the
            // same fresh measurement as everything re-uploaded later, or the two
            // halves of the world disagree by exactly the camera's per-frame motion.
            FindAllCameraRegisters();
            RefreshPatchedBlocks();
        }

        HRESULT WINAPI HookedBeginScene(IDirect3DDevice9* device)
        {
            // Deliberately does not refresh the compensation: whether the client's
            // camera update has run yet at BeginScene is an assumption, and the
            // refresh must only happen once the camera is provably final. Kept as a
            // hook so that changes.
            return g_originalBeginScene(device);
        }

        HRESULT WINAPI HookedPresent(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                                     HWND windowOverride, const RGNDATA* dirtyRegion)
        {
            // Closes the observation window on the frame the game has just finished.
            Report().EndFrame();

            PollHotkeys();

            // Every frame, so focus changes release the pointer even on frames without an
            // interface pass (loading screens), and a dropped clip is put back promptly.
            if (Cfg().enabled)
            {
                UpdateCursorConfinement();
            }

            if (Cfg().enabled)
            {
                EnsureVrResources(device);
                CompositeUiPanel(device);
                SubmitFrame(device);
                FinishStereoFrame(device);
            }

            const LARGE_INTEGER presentStart = Now();
            const HRESULT hr = g_originalPresent(device, source, destination, windowOverride, dirtyRegion);
            {
                const double presentMs = ElapsedMs(presentStart, Now());
                g_pacing.sumPresentMs += presentMs;
                if (presentMs > g_pacing.maxPresentMs) { g_pacing.maxPresentMs = presentMs; }
                if (g_pacing.last.QuadPart != 0)
                {
                    const double interval = ElapsedMs(g_pacing.last, presentStart);
                    g_pacing.sumIntervalMs += interval;
                    ++g_pacing.samples;
                    if (interval > g_pacing.maxIntervalMs) { g_pacing.maxIntervalMs = interval; }
                    UpdateHalfRateGovernor(interval - g_halfRate.lastIdleMs);
                    if (interval > 10.0) { ++g_pacing.over10; }
                    if (interval > 20.0) { ++g_pacing.over20; }
                    if (interval > 50.0) { ++g_pacing.over50; }
                }
                g_pacing.last = presentStart;
            }

            // WaitGetPoses blocks until the compositor wants the next frame, which is
            // also what paces the game to the headset's refresh rate.
            if (Cfg().enabled && Vr().IsActive())
            {
                // The compositor's frame protocol is strictly Submit, then
                // WaitGetPoses, and the compositor is not thread-safe. Called from here
                // while the worker was still submitting, WaitGetPoses opened the next
                // frame early, the late Submit landed in it and the following one was
                // refused ("eye already submitted"): dropped frames and doubled images
                // on every head turn. (The native D3D9 Present blocked long enough to
                // hide it; DXVK's returns at once.) So when the worker has this frame,
                // it makes the WaitGetPoses call itself, right after its Submit, and
                // this thread only waits for the fresh poses. When it does not (dump
                // frames, frames with nothing to submit), this thread calls it, once
                // the worker is entirely idle. A worker stuck in the compositor is
                // waited for a bounded time and then left behind, as before.
                const LARGE_INTEGER poseWaitStart = Now();
                const bool posted = g_presenterJobPostedThisFrame;
                g_presenterJobPostedThisFrame = false;
                bool posesFresh = false;
                if (posted)
                {
                    posesFresh = TryWaitPresenterDone(false, true);
                    g_halfRate.lastIdleMs = posesFresh
                        ? static_cast<double>(g_presenterLastPoseWaitUs) / 1000.0 : 0.0;
                }
                else if (TryWaitPresenterDone(false))
                {
                    const LARGE_INTEGER idleStart = Now();
                    Vr().WaitForFrame();
                    posesFresh = true;
                    g_halfRate.lastIdleMs = ElapsedMs(idleStart, Now());
                }
                // The worker is past WaitGetPoses (or idle), so the compositor is free.
                if (posesFresh)
                {
                    ApplyHalfRateDecision();
                }
                const double poseWaitMs = ElapsedMs(poseWaitStart, Now());
                g_timers.waitMs += poseWaitMs;
                g_pacing.sumWorkerWaitMs += poseWaitMs;
                if (poseWaitMs > g_pacing.maxWorkerWaitMs) { g_pacing.maxWorkerWaitMs = poseWaitMs; }

                // Recentre the moment the headset is first actually worn. Until then
                // "forward" would be measured from wherever it happened to be lying.
                const bool userPresent = Vr().UserIsPresent();
                if (userPresent && !g_userWasPresent)
                {
                    Projection().Recenter();
                    g_uiPanel.RecenterAt(0.0f, Vec3());
                    WOWVR_INFO("Headset picked up; recentring on the current head pose.");
                }
                g_userWasPresent = userPresent;

                // Fresh pose for the frame the game is about to build.
                if (Vr().HasHeadPose())
                {
                    Projection().UpdateFromHeadPose(Vr().HeadToStage());

                    // The interface canvas first: the geometry below follows whether its
                    // layout is in force this frame.
                    Canvas().Install();
                    Canvas().Update(Cfg().panelCanvasScale);
                    {
                        static LARGE_INTEGER last = {};
                        const LARGE_INTEGER now = Now();
                        const float seconds = last.QuadPart != 0
                            ? static_cast<float>(ElapsedMs(last, now) / 1000.0) : 0.011f;
                        last = now;
                        Vignette().Update(seconds, Cfg().vignette);
                    }

                    // The panel no longer follows the head; it only picks up INI changes
                    // to its size here. It moves when Ctrl+Alt+F8 (or a full recentre)
                    // places it again.
                    g_uiPanel.RefreshGeometry();

                    if (Cfg().panelWorldPointing && g_uiPanel.IsReady())
                    {
                        Pointer().Install();
                    }

                    DrawRange().Install();
                    DrawRange().SetScale(Cfg().viewDistanceScale);

                    if (Cfg().billboardsFaceHead)
                    {
                        Billboards().Install();
                    }
                    Billboards().Update(Cfg().billboardsFaceHead && Cfg().headTracking,
                                        Projection().GameViewToHead());
                    Pointer().Update(Cfg().panelWorldPointing && g_uiPanel.IsReady()
                                         && g_panelCompositedLastFrame,
                                     g_uiPanel.Shape(), Projection().HeadOffsetMetres(),
                                     Projection().GameViewToBody(),
                                     Cfg().unitsPerMetre * Cfg().worldScale);
                    UpdatePlateStrip();

                    // The persisted camera blocks are deliberately NOT refreshed here.
                    // At this point the client has not updated its camera for the
                    // frame this pose belongs to, so blocks rebuilt now would carry a
                    // one-update-stale compensation - and they feed the early terrain
                    // draws that happen before the client re-uploads the register.
                    // During a nod that put the near terrain and the rest of the world
                    // a per-frame pitch delta apart, shaking the whole scene. They are
                    // rebuilt in EnsureFreshCameraCompensation instead, at the first
                    // constant upload of the new frame, when the camera is final.
                }
            }
            else
            {
                // No headset frame, so no panel: the client gets its own picking back,
                // and its own draw distance.
                Pointer().Deactivate();
                DrawRange().SetScale(1.0f);
                Canvas().Update(1.0f);
                Billboards().Update(false, Mat4Identity());
            }

            // One line per frame while armed. Reading across a row: 'head' and 'wrote'
            // belong to the frame about to be built; 'facingNow', 'comp' and 'baked'
            // describe the frame just finished. A shake is attributed by whichever
            // column disagrees with 'baked' on the frames the head was moving.
            if (g_pitchTrace)
            {
                float facingYaw = 0.0f;
                float facingPitch = 0.0f;
                GameCam().CameraFacing(facingYaw, facingPitch);
                WOWVR_INFO("pitchtrace f%llu: head %+.4f wrote %+.4f facingNow %+.4f "
                           "comp %+.4f baked %+.4f",
                           g_frameCount, Projection().HeadPitch(), GameCam().AppliedPitch(),
                           facingPitch, Projection().CompensationPitchUsed(),
                           Projection().LastBakedPitch());
            }

            ++g_frameCount;
            if ((g_frameCount % 120) == 0)
            {
                // Unconditional, unlike the cost report, so a collapsed frame
                // rate or a frame loop that never reaches submit is still visible.
                WOWVR_DEBUG("heartbeat: frame %llu, panel drawn %llu, skipped %llu (%s)",
                            g_frameCount, g_panelDrawn, g_panelSkipped, g_lastSkipReason);
            }
            if ((g_frameCount % 600) == 0)
            {
                ReportFrameTimings();
            }

            // The world/UI split is per-frame state.
            g_lastPeriodCount = g_backBufferDrawPeriod;
            g_backBufferDrawPeriod = 0;
            g_wasDrawingToBackBuffer = false;
            g_uiPassStarted = false;
            g_stereoHasContent = false;
            g_uiRendered = false;
            g_uiTargetBound = false;
            // This frame's lifebars have been drawn; the next frame places its own.
            Pointer().BeginPlateFrame();

            // Whether the frame just finished ever had a 3D camera. Used next frame to
            // recognise a loading screen; asking within the frame races the camera
            // upload, which arrives after the first back buffer draw.
            g_previousFrameHadCamera = g_projectionPatchedThisFrame;
            g_projectionPatchedThisFrame = false;

            g_sequenceDumpArmed = g_armSequenceNextFrame;
            g_armSequenceNextFrame = false;
            g_worldDrawIndex = 0;
            if (g_sequenceDumpArmed)
            {
                g_sequenceDumpsWritten = 0;
            }

            // Once the scene projection is known, go and find the camera that
            // produced it. Done once, off the back of a frame that has already been
            // presented, so the one-off scan hitch is not inside a visible frame.
            // Find the camera that produced the projection we are already decoding,
            // then keep re-checking the candidates until exactly one is still telling
            // the truth. Done off the back of a presented frame so the one-off scan
            // hitch is not inside a visible one.
            // One read-only look at the published 3.3.5a offsets, once the projection
            // has been decoded so there is something to validate the field of view
            // against. Cheap, and it needs no scan.
            static bool s_knownOffsetsProbed = false;
            if (Cfg().scanForCamera && !s_knownOffsetsProbed && g_frameCount == 240
                && Projection().HasSceneProjection())
            {
                s_knownOffsetsProbed = true;
                Camera().ProbeKnownOffsets(Projection().SceneVerticalScale(),
                                           Projection().SceneAspect());
                Camera().ScanStaticsForCamera(Projection().SceneVerticalScale());
                Camera().ScanForCameraObject(Projection().SceneVerticalScale(),
                                             Projection().SceneNear(),
                                             Projection().SceneFar());
            }

            // Tell the game how wide the headset actually sees, so it culls, streams and
            // shadows for that instead of for its own 59 degree window. The vertical
            // tangents come straight from the headset; CullFovScale adds margin on top so
            // geometry does not pop in at the edge of a quick head turn.
            // Deliberately not gated on StereoActive(): the redirect flag is already
            // cleared by the time Present runs, so that test silently never fired.
            if (Cfg().cullFovScale > 1.0f && Vr().IsActive())
            {
                // A multiplier on the game's OWN field of view, not on the headset's.
                // Driving it from the headset tangents forced at least 109 degrees for
                // any scale at all, and at 132 the client's rendering fell apart
                // (464k corrupt pixels against a 1.7k baseline). Modest widening is what
                // is wanted here: enough that geometry and shadows exist outside the
                // game's own window, not so much that it destabilises the client.
                Camera().WidenGameFovByFactor(Cfg().cullFovScale);
            }

            if (Cfg().scanForCamera && g_frameCount > 240 && (g_frameCount % 300) == 0
                && Projection().HasSceneProjection())
            {
                Camera().WatchFovField(Projection().SceneVerticalScale());
            }

            // Ctrl+Alt+C starts the camera calibration. Edge-triggered, or holding the
            // keys down would restart it every frame.
            {
                const bool combo = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
                                && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0
                                && (GetAsyncKeyState('C') & 0x8000) != 0;
                if (combo && !g_calibrateKeyDown)
                {
                    CentreCursorOnGame();
                    Camera().BeginCalibration();
                }
                g_calibrateKeyDown = combo;
            }
            // The calibration steers by injecting mouse input, and injected input goes to
            // whatever window currently has focus - so if anything has taken foreground,
            // every nudge lands somewhere else and the search quietly starves. That is not
            // hypothetical: driven by hand right after focusing the window, 1,120 of
            // 155,000 candidates moved on most rounds; left to fire on its own later, only
            // 56 did, and the camera was not among them.
            //
            // Asserting focus from inside the game's own process works where the same call
            // from a driving script does not, because Windows lets a process raise its own
            // window far more readily than a foreign one.
            if (Camera().Calibrating() && !GameHasFocus() && g_gameWindow != nullptr)
            {
                SetForegroundWindow(g_gameWindow);
                CentreCursorOnGame();
            }

            Camera().UpdateCalibration();
            ServiceInjectedInput();

            // The passive search is gone from the frame path.
            //
            // It ran a whole-address-space sweep on the render thread every forty-five
            // frames - `CollectLayoutCandidates` walks every committed read-write region
            // and collects up to a million entries - which is a visible hitch several times
            // a second in the headset. And by its own record it never converged during
            // ordinary play, so it was paying that cost for nothing. The signature sweep
            // below replaces it: one pass, once, when the world is up.
            // Waiting for the WORLD, not merely for a scene.
            //
            // HasSceneProjection is true on the login screen too - it is a 3D scene like
            // any other - so gating on that alone ran the whole search ten seconds in,
            // against a client that had not loaded a world camera yet. It duly found two
            // login-screen buffers, whose yaw copies had collapsed to 0.0 and 0.5 by the
            // time it tried to confirm them.
            //
            // The far plane tells the two apart and is already decoded: the login screen
            // uses 2778, the world 791. Measured, not assumed.
            const bool inWorld = Projection().HasSceneProjection()
                              && Projection().SceneFar() > 1.0f
                              && Projection().SceneFar() < 1500.0f;

            // And settled, not merely arrived: the frame counter runs from launch, so it
            // is already long past any threshold by the time the world appears.
            if (inWorld) { ++g_inWorldFrames; } else { g_inWorldFrames = 0; }

            // Zoomed out before anything else, because the zoom level persists between
            // sessions and the camera had been left in first person. There the orbit
            // radius is zero, which fails verification and excludes the camera from any
            // search keyed on the radius - and nothing in the log says "you are in first
            // person", it just looks like the camera cannot be found. Six clicks out costs
            // a moment and removes the whole class of confusion.
            // What aspect ratio the client actually needs, computed rather than guessed.
            //
            // WoW derives horizontal field of view from the resolution's aspect and holds
            // vertical fixed, so covering the headset is arithmetic: the aspect required is
            // the ratio of the eye's horizontal tangent to the game's vertical one. Logged
            // once so the gxResolution to put in Config.wtf is a measurement, not a guess.
            if (g_frameCount == 900 && Vr().IsActive())
            {
                float tanLeft = 0.0f, tanRight = 0.0f, tanTop = 0.0f, tanBottom = 0.0f;
                Vr().EyeTangents(EyeLeft, tanLeft, tanRight, tanTop, tanBottom);
                const float halfH = (fabsf(tanLeft) > fabsf(tanRight))
                                  ? fabsf(tanLeft) : fabsf(tanRight);
                const float halfV = (fabsf(tanTop) > fabsf(tanBottom))
                                  ? fabsf(tanTop) : fabsf(tanBottom);
                const float* gameHalfFov = reinterpret_cast<const float*>(0x00ABFC38u);
                const float gameTanV = tanf(*gameHalfFov);

                WOWVR_INFO("Eye frustum: %.1f deg horizontal, %.1f deg vertical. "
                           "Game culls %.1f deg vertical.",
                           2.0f * atanf(halfH) * 57.2957795f,
                           2.0f * atanf(halfV) * 57.2957795f,
                           2.0f * (*gameHalfFov) * 57.2957795f);
                WOWVR_INFO("Aspect needed to cover one eye: %.2f (e.g. %dx1080). "
                           "With 25%% margin: %.2f (e.g. %dx1080).",
                           halfH / gameTanV, static_cast<int>(halfH / gameTanV * 1080.0f),
                           halfH / gameTanV * 1.25f,
                           static_cast<int>(halfH / gameTanV * 1.25f * 1080.0f));
            }

            // What the head says, what went into the client, and what the client holds now -
            // on one line, so a sign that is still wrong is visible from the log alone
            // rather than needing another round of screenshots to diagnose.
            if ((g_frameCount % 300) == 0 && Vr().IsActive() && GameCam().Found())
            {
                WOWVR_INFO("Aim: head yaw %+.1f pitch %+.1f deg -> wrote yaw %+.1f pitch "
                           "%+.1f deg (signs %+.0f/%+.0f); camera holds yaw %+.1f pitch "
                           "%+.1f deg.",
                           Projection().HeadYaw() * 57.2957795f,
                           Projection().HeadPitch() * 57.2957795f,
                           GameCam().AppliedYaw() * GameCam().YawSign() * 57.2957795f,
                           GameCam().AppliedPitch() * GameCam().PitchSign() * 57.2957795f,
                           GameCam().YawSign(), GameCam().PitchSign(),
                           GameCam().FreeLookYawField() * 57.2957795f,
                           GameCam().FreeLookPitchField() * 57.2957795f);

                // What the camera is ACTUALLY pointing at, from its own basis vectors.
                //
                // Asked-for and achieved are different things: the client clamps pitch
                // against a limit that is itself a field, and looking straight down is
                // exactly where that bites. Comparing the two is the only scene-independent
                // way to see it - a first-person view of the ground is featureless snow, and
                // every pixel-based test simply reads zero there.
                float facingYaw = 0.0f, facingPitch = 0.0f;
                if (GameCam().CameraFacing(facingYaw, facingPitch))
                {
                    WOWVR_INFO("Camera facing: yaw %+.1f pitch %+.1f deg; base pitch %+.1f, "
                               "pitch-limit field %+.4f; asked for pitch offset %+.1f deg.",
                               facingYaw * 57.2957795f, facingPitch * 57.2957795f,
                               GameCam().BasePitch() * 57.2957795f,
                               GameCam().PitchLimitField(),
                               GameCam().FreeLookPitchField() * 57.2957795f);
                }

                // Positional tracking, reported because "the viewpoint does not move" is
                // otherwise indistinguishable from "the head is not moving": this says
                // whether the displacement reaching the projection is zero or merely
                // being ignored further along.
                Vec3 orbitShiftForLog;
                GameCam().OrbitDisplacementView(GameCam().AppliedYaw(), orbitShiftForLog);

                WOWVR_INFO("Head displacement: (%.3f, %.3f, %.3f) m; orbit radius %.2f "
                           "yards, camera swung (%.2f, %.2f, %.2f) yards; scene fov %.1f deg "
                           "vertical, %.1f deg horizontal.",
                           Projection().HeadOffsetMetres().x,
                           Projection().HeadOffsetMetres().y,
                           Projection().HeadOffsetMetres().z,
                           GameCam().OrbitRadius(), orbitShiftForLog.x, orbitShiftForLog.y,
                           orbitShiftForLog.z,
                           2.0f * atanf(1.0f / Projection().SceneVerticalScale())
                               * 57.2957795f,
                           2.0f * atanf(Projection().SceneAspect()
                                        / Projection().SceneVerticalScale())
                               * 57.2957795f);
            }

            if ((g_frameCount % 300) == 0 && Vr().IsActive())
            {
                WOWVR_INFO("Head orientation: yaw %.1f deg, pitch %.1f deg, roll %.1f deg%s",
                           Projection().HeadYaw() * 57.2957795f,
                           Projection().HeadPitch() * 57.2957795f,
                           Projection().HeadRoll() * 57.2957795f,
                           (fabsf(Projection().HeadRoll()) > 0.17f
                            || fabsf(Projection().HeadPitch()) > 0.26f)
                               ? "  <- tilted; yaw is not a reliable facing" : "");
            }

            // Long enough for the world to appear, and no longer.
            //
            // This used to wait 4000 in-world frames because the page-narrowing phase
            // discarded the camera's page while anything was still streaming. That phase is
            // gone - the signature sweep does not care - and the wait outlived it, which
            // made the feature unreachable for the first three minutes in the world. A user
            // testing at 40 seconds saw no head-driven culling at all and reasonably
            // concluded it did not work; the log showed the search had never even started.
            //
            // Nothing that ships should require the user to wait several minutes before it
            // begins to function.
            if (Cfg().autoLocateCamera && !Camera().Calibrating()
                && !Camera().CameraLocated() && !g_signatureSweepDone
                && !GameCam().Found() && !g_directChainEverWorked
                && g_inWorldFrames > 600)
            {
                g_signatureSweepDone = true;

                // Focus asserted here too, not just during the calibration. The zoom-out
                // runs BEFORE the calibration starts, so it was never covered by the
                // foreground check - its wheel clicks went to whatever else had focus, the
                // camera stayed in first person with a zero orbit radius, and the search
                // then had no radius to recognise it by.
                if (!GameHasFocus() && g_gameWindow != nullptr)
                {
                    SetForegroundWindow(g_gameWindow);
                }
                CentreCursorOnGame();
                g_injectWheelDelta = -120;
                g_injectWheelClicks = 6;
                g_calibrateAfterFrames = 150;
                WOWVR_INFO("Zooming out to third person before locating the camera.");
            }

            if (g_calibrateAfterFrames > 0 && --g_calibrateAfterFrames == 0)
            {
                CentreCursorOnGame();
                Camera().BeginCalibration();
            }

            // Retried on failure. The search depends on the camera's page surviving five
            // rounds of scoring, which it does most of the time but not always - one run in
            // seven ends with a candidate that verification correctly throws out. Leaving
            // the feature silently off after that is the worst outcome, and another attempt
            // costs half a minute.
            if (Cfg().autoLocateCamera && Camera().LocateFailed()
                && !Camera().Calibrating() && !GameCam().Found()
                && !g_directChainEverWorked && g_locateAttempts < 3)
            {
                ++g_locateAttempts;
                Camera().ResetLocate();

                // Zoomed out again on each retry: if the first attempt failed because the
                // camera was still in first person, repeating the search without fixing
                // that just fails the same way.
                if (!GameHasFocus() && g_gameWindow != nullptr)
                {
                    SetForegroundWindow(g_gameWindow);
                }
                CentreCursorOnGame();
                g_injectWheelDelta = -120;
                g_injectWheelClicks = 6;
                g_calibrateAfterFrames = 300;
                WOWVR_INFO("Camera search failed; retrying (attempt %d of 3).",
                           g_locateAttempts + 1);
            }

            // Turn the game's own camera to follow the head, then tell the projection how
            // much of the head rotation has already been accounted for so it does not
            // apply it a second time.
            // The fake yaw exists so the orbit correction can be checked without a headset
            // on: it drives exactly the path a real head turn drives, and the character
            // staying put on screen versus sliding off is what says whether the correction
            // has the right sign. "orbit" applies it, "orbitoff" leaves the radius at zero
            // so the two can be compared in one session.
            // One path, whether the head turn is real or synthetic: the fake yaw is folded
            // into HeadYaw() at its source, so nothing here can tell the difference. The
            // aiming toggle exists to provide the control case - the view turns either way,
            // but only with aiming on does the client cull for where it now points.
            // The camera the client itself uses, reached the way the client reaches it.
            // Two dereferences, so this is re-run every frame rather than located once:
            // the object is destroyed and rebuilt across every loading screen.
            const bool haveGameCamera = GameCam().Update();

            // Once the direct chain has resolved even once, the search-and-calibrate
            // fallback is disabled for the rest of the session. A zone change destroys
            // the camera object for a few hundred frames, and the fallback's gates
            // read that brief absence as "camera never found": the memory sweep then
            // stalled the render thread (the freeze the user felt), the calibration
            // started swinging the view, and a device reset in the middle of it left a
            // probe value in the real camera's pitch-bias field. A client the chain
            // works on never needs the fallback; a client it does not work on never
            // sets this latch.
            if (haveGameCamera)
            {
                g_directChainEverWorked = true;
            }

            // Camera collision has to go for third person to be usable at all: aiming the
            // camera at the head sweeps it through the ground and the client answers by
            // hauling it in, which moves the viewpoint yards at a time. This is a one-byte
            // patch to the client's code and costs nothing to re-assert, because SetCollision
            // returns immediately when the byte is already what it wants.
            // Restored whenever VR is not driving the camera, so a flat session in the same
            // client behaves the way the client shipped.
            GameCam().SetCollision(!(g_disableCameraCollision && Cfg().disableCameraCollision
                                     && Vr().IsActive()));

            if (haveGameCamera && Vr().IsActive() && inWorld && !g_cameraDumped)
            {
                g_cameraDumped = true;
                GameCam().DumpObject(Projection().SceneNear(), Projection().SceneFar(),
                                     *reinterpret_cast<const float*>(0x00ABFC38u));
            }

            // Margin around wherever the head points, so the one frame between aiming the
            // camera and the client culling against it cannot show through. An explicit
            // override from a command wins, so the two can still be compared in one session.
            if (haveGameCamera && g_cullFovOverride <= 0.0f && Vr().IsActive())
            {
                GameCam().SetCullWiden(Cfg().cullWidenScale);
            }

            if (haveGameCamera
                && (g_cullFovOverride > 0.0f || g_cullAspectOverride > 0.0f))
            {
                GameCam().SetCullFrustum(g_cullFovOverride, g_cullAspectOverride);
            }

            // Point the client's culling volume where the head looks, without writing a
            // single byte of its camera. The head pose can only be sampled here, once a
            // frame; everything else the rotation needs - the camera's position and
            // heading - is read live inside the client's own plane builder, where it is
            // already up to date for the frame those planes will cull.
            //
            // Suppressed while the camera is being aimed, because that path has already
            // turned the frustum by the head rotation and turning it again would count
            // the head twice.
            {
                const bool wantFrustum = g_cullRotateEnabled
                                         && Cfg().headDrivenCullFrustum
                                         && !Cfg().aimCameraAtHead;
                CullView().Enable(wantFrustum);
                if (!g_cullSpanConfigured)
                {
                    g_cullSpanConfigured = true;
                    CullView().SetShapeGate(Cfg().cullRotateMinSpanDegrees,
                                            Cfg().cullRotateMaxOffAxisDegrees);
                }
                CullView().SetCameraObject(
                    haveGameCamera ? reinterpret_cast<const void*>(GameCam().Address())
                                   : nullptr);
                CullView().SetHeadRotation(Projection().HeadYaw(), Projection().HeadPitch());
                CullView().SetOpenTerrainCone(g_cullTerrainOpen
                                              && Cfg().cullTerrainAllAround);
                CullView().SetRotateMasterCorners(g_cullMasterRotate
                                                  && Cfg().cullRotateMasterCorners
                                                  && wantFrustum);
                if (!g_masterFeedsApplied && CullView().Enabled()
                    && Cfg().cullMasterShadowFeeds != 0)
                {
                    g_masterFeedsApplied = true;
                    CullView().SetMasterFeedMask(Cfg().cullMasterShadowFeeds);
                }
                const bool active = wantFrustum && Vr().IsActive() && haveGameCamera;
                CullView().SetWmoGroupsAlwaysVisible(active && g_wmoGroupsAllVisible
                                                     && Cfg().wmoGroupsAlwaysVisible);
                CullView().SetActive(active);
                CullView().FrameBoundary();
            }

            // Move the ears to the headset. Head rotation and head displacement are both
            // sampled here, once a frame, for the same reason as above - the pose only
            // exists on this thread - and everything else the correction needs (where the
            // client decided the listener goes and which way it faces) arrives as an
            // argument inside the client's own listener call.
            //
            // Not gated on the game camera: the listener setter is handed its own base
            // position and heading, so it needs nothing found by address.
            {
                const bool wantListener = g_headListenerEnabled
                                          && Cfg().headDrivenSoundListener;
                if (!g_headListenerConfigured)
                {
                    g_headListenerConfigured = true;
                    HeadListener().SetTrackPosition(Cfg().soundListenerFollowsHead);
                    HeadListener().SetEarsAtCamera(Cfg().soundListenerAtCamera);
                }
                HeadListener().Enable(wantListener);
                HeadListener().SetCameraObject(
                    haveGameCamera ? reinterpret_cast<const void*>(GameCam().Address())
                                   : nullptr);
                HeadListener().SetHeadRotation(Projection().HeadYaw(),
                                               Projection().HeadPitch(),
                                               Projection().HeadRoll());
                HeadListener().SetHeadOffsetMetres(Projection().HeadOffsetMetres());
                HeadListener().SetUnitsPerMetre(Cfg().unitsPerMetre);
                HeadListener().SetActive(wantListener && Vr().IsActive()
                                         && Cfg().headTracking);
            }

            if (g_aimEnabled && Cfg().aimCameraAtHead && haveGameCamera)
            {
                // The client ADDS these to the heading the camera would otherwise have, so
                // writing them turns the view and leaves the character alone. Both are
                // absolute intent restated every frame, not deltas: the client owns the
                // fields between our writes. Pitch is stepped, not tracked; see
                // SteppedAimPitch.
                GameCam().SetFreeLook(Projection().HeadYaw(),
                                      g_aimPitch ? SteppedAimPitch(Projection().HeadPitch())
                                                 : 0.0f);

                // And taken straight back out of the projection we substitute, or the head
                // rotation lands twice and the world turns at double rate. Only the yaw
                // intent is set here; the pitch is measured fresh per frame in
                // RefreshCameraCompensation, and measuring it HERE - with the client's
                // camera not yet updated for the frame this pose belongs to - is stale
                // by construction and once masked the very trace built to catch it.
                Projection().SetGameCameraYaw(GameCam().AppliedYaw());
                // Before anything reads the radius: collision is the dominant motion in
                // third person, and pinning it is the only thing that addresses that.
                GameCam().PinOrbitRadius(g_pinOrbitRadius);

                Projection().SetGameCameraOrbitRadius(
                    g_compensateOrbit ? GameCam().OrbitRadius() : 0.0f);

                // Measured from the camera's own position and basis, not predicted from a
                // radius: the client pulls the camera in against terrain, so the radius is
                // not the constant the old model assumed it was.
                Vec3 orbitShift;
                if (g_compensateOrbit
                    && GameCam().OrbitDisplacementView(GameCam().AppliedYaw(), orbitShift))
                {
                    Projection().SetOrbitDisplacement(orbitShift);
                }
                else
                {
                    Projection().SetOrbitDisplacement(Vec3());
                }
            }
            else if (g_aimEnabled && Cfg().aimCameraAtHead && Camera().CameraLocated())
            {
                // Fallback for a client whose layout does not match the one decoded from
                // this build. Kept because it is proven to steer, not because it is good.
                Camera().AimAtHead(Projection().HeadYaw());
                Projection().SetGameCameraYaw(Camera().AppliedYaw());
                Projection().SetGameCameraOrbitRadius(
                    g_compensateOrbit ? Camera().OrbitRadius() : 0.0f);

                if (g_aimPitch)
                {
                    Camera().AimPitchAtHead(Projection().HeadPitch());
                    Projection().SetGameCameraPitch(Camera().AppliedPitch());
                }
                else
                {
                    Projection().SetGameCameraPitch(0.0f);
                }
            }
            else
            {
                if (haveGameCamera) { GameCam().ClearFreeLook(); }
                Projection().SetGameCameraYaw(0.0f);
                Projection().SetGameCameraOrbitRadius(0.0f);
                Projection().SetGameCameraPitch(0.0f);
            }

            Camera().ApplyHeadingWrite();
            Camera().ApplyFovOverride();

            // A one-line file dropped next to the client drives the differential scan.
            // The phases have to line up with the character actually turning, and a file
            // is the one channel that cannot be swallowed by whatever currently has
            // keyboard focus - which is exactly how the first attempt at driving this
            // went wrong.
            if (g_sweepAmplitude != 0.0f)
            {
                const double t = ElapsedMs(g_sweepStart, Now()) / 1000.0;
                g_fakeHeadYaw = g_sweepAmplitude
                    * static_cast<float>(sin(6.283185307 * t / g_sweepPeriod));
                Projection().SetFakeHeadYaw(g_fakeHeadYaw);
            }

            if ((g_frameCount % 15) == 0)
            {
                const std::wstring commandFile = ModuleFile(L"WoWVR_cmd.txt");
                HANDLE handle = CreateFileW(commandFile.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle != INVALID_HANDLE_VALUE)
                {
                    char command[512] = {};
                    DWORD read = 0;
                    ReadFile(handle, command, sizeof(command) - 1, &read, nullptr);
                    CloseHandle(handle);
                    DeleteFileW(commandFile.c_str());

                    // The same calibration Ctrl+Alt+C runs, but reachable without keyboard
                    // focus - SteamVR takes focus while it starts up and silently swallows
                    // the hotkey, which is what made the run impossible to drive from a
                    // script. No prefix collision: "chain" is compared over five characters.
                    if (strncmp(command, "calibrate", 9) == 0)
                    {
                        CentreCursorOnGame();
                        Camera().BeginCalibration();
                    }
                    else if (strncmp(command, "snapshot", 8) == 0) { Camera().SnapshotDifferential(); }
                    else if (strncmp(command, "right", 5) == 0)  { Camera().FilterDifferential(1); }
                    else if (strncmp(command, "left", 4) == 0)   { Camera().FilterDifferential(-1); }
                    else if (strncmp(command, "still", 5) == 0)  { Camera().FilterDifferential(0); }
                    else if (strncmp(command, "report", 6) == 0) { Camera().ReportDifferential(); }
                    else if (strncmp(command, "yawa", 4) == 0)   { Camera().SetHeadingWrite(1, 1.0f); }
                    else if (strncmp(command, "yawb", 4) == 0)   { Camera().SetHeadingWrite(2, 1.0f); }
                    else if (strncmp(command, "yawoff", 6) == 0) { Camera().SetHeadingWrite(0, 0.0f); }
                    // The desktop window only mirrors the interface, so the world can
                    // only be seen by capturing the eye buffer itself.
                    else if (strncmp(command, "shot", 4) == 0)   { g_dumpNextFrame = true; }
                    else if (strncmp(command, "mirrorshot", 10) == 0) { g_dumpMirrorNext = true; }
                    else if (strncmp(command, "platedump", 9) == 0) { g_plateDumpNext = true; }
                    // "holdall" must be tested before "hold", or the prefix swallows it.
                    else if (strncmp(command, "holdall", 7) == 0) { Camera().HoldSurvivors(true, 1.57f); }
                    else if (strncmp(command, "aim", 3) == 0)    { Camera().SetHeadingWrite(1, 1.57f); }
                    else if (strncmp(command, "unaim", 5) == 0)  { Camera().SetHeadingWrite(0, 0.0f); }
                    else if (strncmp(command, "release", 7) == 0) { Camera().HoldHeading(false, 0.0f); }
                    else if (strncmp(command, "pagesnap", 8) == 0) { Camera().SnapshotPages(); }
                    else if (strncmp(command, "pagemoved", 9) == 0) { Camera().FilterPages(true); }
                    else if (strncmp(command, "pagestill", 9) == 0) { Camera().FilterPages(false); }
                    else if (strncmp(command, "promote", 7) == 0)  { Camera().PromotePagesToFloats(); }
                    else if (strncmp(command, "freeall", 7) == 0)  { Camera().HoldSurvivors(false, 0.0f); }
                    else if (strncmp(command, "chain", 5) == 0)    { Camera().FindPointerChain(); }
                    // The stimulus is injected from here rather than from the driving
                    // script, because input sent from outside gets swallowed - a scripted
                    // drag left the yaw completely untouched while the calibration's own
                    // drags, sent from this thread, move it every time. Blocking the render
                    // thread for a second is fine for a diagnostic.
                    // Zooming IN is the more useful stimulus: the camera starts at or near
                    // its maximum distance, so scrolling out has almost nowhere to travel
                    // and produces a change barely above the frame-to-frame noise, while
                    // scrolling in runs all the way to first person.
                    else if (strncmp(command, "wheelin", 7) == 0)
                    {
                        CentreCursorOnGame();
                        g_injectWheelClicks = 8;
                        g_injectWheelDelta = 120;
                        WOWVR_INFO("Queued eight wheel-up clicks, one per frame.");
                    }
                    else if (strncmp(command, "wheelout", 8) == 0)
                    {
                        CentreCursorOnGame();
                        g_injectWheelClicks = 8;
                        g_injectWheelDelta = -120;
                        WOWVR_INFO("Queued eight wheel-down clicks, one per frame.");
                    }
                    else if (strncmp(command, "nudge", 5) == 0)
                    {
                        CentreCursorOnGame();
                        g_injectDragDx = 8; g_injectDragDy = 0;
                        g_injectDragFrames = 15;
                        WOWVR_INFO("Queued one drag nudge, one move per frame.");
                    }
                    // A vertical drag pitches the camera, which is what identifies the
                    // pitch field the same way the horizontal one identified the yaw.
                    else if (strncmp(command, "pitchnudge", 10) == 0)
                    {
                        CentreCursorOnGame();
                        g_injectDragDx = 0; g_injectDragDy = 6;
                        g_injectDragFrames = 15;
                        WOWVR_INFO("Queued one vertical drag nudge.");
                    }
                    // A head turn of an arbitrary size, standing in for the headset.
                    //
                    // This drives the identical path a real head turn drives - the same
                    // AimAtHead, the same yaw written into the camera, the same
                    // compensation in the projection - so what it tests is the mechanism
                    // itself rather than an imitation of it. Only the HMD pose plumbing is
                    // bypassed, and that is independently known to work. It is what makes
                    // head-driven culling testable with the headset sitting still on a desk.
                    // "lua <code>": runs a snippet in the client, in the world only - for
                    // reading what the client itself thinks (cursor position, mouse focus)
                    // when a capture cannot show it.
                    else if (strncmp(command, "lua ", 4) == 0)
                    {
                        WOWVR_INFO("lua: %s", Canvas().RunDebugScript(command + 4)
                                                  ? "ran" : "could not run (not in the world?)");
                    }
                    else if (strncmp(command, "headsweep ", 10) == 0)
                    {
                        float amplitude = 0.0f;
                        float period = 2.0f;
                        if (sscanf_s(command + 10, "%f %f", &amplitude, &period) >= 1)
                        {
                            g_sweepAmplitude = amplitude;
                            g_sweepPeriod = period > 0.1f ? period : 2.0f;
                            g_sweepStart = Now();
                            if (amplitude == 0.0f)
                            {
                                g_fakeHeadYaw = 0.0f;
                                Projection().SetFakeHeadYaw(0.0f);
                            }
                            WOWVR_INFO("Head sweep: +/-%.3f rad every %.1f s.",
                                       g_sweepAmplitude, g_sweepPeriod);
                        }
                    }
                    else if (strncmp(command, "head ", 5) == 0)
                    {
                        g_fakeHeadYaw = static_cast<float>(atof(command + 5));
                        Projection().SetFakeHeadYaw(g_fakeHeadYaw);
                        g_compensateOrbit = true;
                        WOWVR_INFO("Fake head yaw %.3f rad (%.1f deg); aiming %s.",
                                   g_fakeHeadYaw, g_fakeHeadYaw * 57.2957795f,
                                   g_aimEnabled ? "on" : "off");
                    }
                    // Not "aimon"/"aimoff": an earlier test for "aim" over three characters
                    // would swallow both, which is the same prefix trap that once made
                    // "holdall" silently run the wrong experiment.
                    else if (strncmp(command, "headpitch ", 10) == 0)
                    {
                        const float p = static_cast<float>(atof(command + 10));
                        Projection().SetFakeHeadPitch(p);
                        WOWVR_INFO("Fake head pitch %.3f rad (%.1f deg).",
                                   p, p * 57.2957795f);
                    }
                    else if (strncmp(command, "pitchtrace", 10) == 0)
                    {
                        g_pitchTrace = !g_pitchTrace;
                        WOWVR_INFO("Pitch trace %s.", g_pitchTrace ? "ON" : "OFF");
                    }
                    else if (strncmp(command, "pitchstep", 9) == 0)
                    {
                        g_pitchStepEnabled = !g_pitchStepEnabled;
                        WOWVR_INFO("Stepped pitch aiming %s.",
                                   g_pitchStepEnabled ? "ON (camera holds a band centre)"
                                                      : "OFF (camera tracks the head)");
                    }
                    // The frustum the client culls against, now that the renderer's own copy
                    // of it has been found inside the camera object. Reported after the
                    // write as well as before, because the interesting question is not what
                    // we set but whether the client leaves it set.
                    // Longer names first: a three-character prefix test on "cull" would
                    // swallow every one of these.
                    else if (strncmp(command, "cullrotview ", 12) == 0)
                    {
                        CullView().SetViewFilter(atoi(command + 12));
                    }
                    else if (strncmp(command, "cullrotall", 10) == 0)
                    {
                        CullView().SetViewFilter(-1);
                    }
                    else if (strncmp(command, "cullrotstate", 12) == 0)
                    {
                        CullView().Report();
                    }
                    else if (strncmp(command, "cullrotanyframe", 15) == 0)
                    {
                        CullView().SetWorldSpaceOnly(false);
                    }
                    else if (strncmp(command, "cullrotworldonly", 16) == 0)
                    {
                        CullView().SetWorldSpaceOnly(true);
                    }
                    else if (strncmp(command, "cullrotskip ", 12) == 0)
                    {
                        CullView().SetSkipCaller(
                            static_cast<uintptr_t>(strtoul(command + 12, nullptr, 16)));
                    }
                    else if (strncmp(command, "cullrotnoskip", 13) == 0)
                    {
                        CullView().SetSkipCaller(0);
                    }
                    // "cullrotshape <minSpanDeg> <maxOffAxisDeg>". "cullrotshape 0 180"
                    // turns every volume the client builds, including the narrow ones it
                    // clips to a doorway - which is the fault this exists to keep out, and
                    // the A/B for confirming it is the fault without a rebuild.
                    else if (strncmp(command, "cullrotshape ", 13) == 0)
                    {
                        float span = 40.0f, offAxis = 5.0f;
                        sscanf(command + 13, "%f %f", &span, &offAxis);
                        CullView().SetShapeGate(span, offAxis);
                    }
                    else if (strncmp(command, "cullrotdump", 11) == 0)
                    {
                        CullView().RequestDump();
                    }
                    else if (strncmp(command, "cullrotsign ", 12) == 0)
                    {
                        CullView().SetYawSign(static_cast<float>(atof(command + 12)));
                    }
                    else if (strncmp(command, "cullrotpitchsign ", 17) == 0)
                    {
                        CullView().SetPitchSign(static_cast<float>(atof(command + 17)));
                    }
                    else if (strncmp(command, "cullrotforce ", 13) == 0)
                    {
                        CullView().SetForcedYawDegrees(
                            static_cast<float>(atof(command + 13)));
                    }
                    else if (strncmp(command, "cullroton", 9) == 0)
                    {
                        g_cullRotateEnabled = true;
                        WOWVR_INFO("Head-driven cull frustum enabled.");
                    }
                    else if (strncmp(command, "cullrotoff", 10) == 0)
                    {
                        g_cullRotateEnabled = false;
                        WOWVR_INFO("Head-driven cull frustum disabled; the client culls "
                                   "against its own heading.");
                    }
                    // The terrain half of head-driven culling. "cullterrainoff" before
                    // "cullterrainon" out of the same prefix caution as the rest of this
                    // chain, though these two do not actually collide.
                    else if (strncmp(command, "cullterrainoff", 14) == 0)
                    {
                        g_cullTerrainOpen = false;
                        WOWVR_INFO("Terrain view-cone back to the client's own; terrain "
                                   "vanishes beyond ~90 deg of the character's heading.");
                    }
                    else if (strncmp(command, "cullterrainon", 13) == 0)
                    {
                        g_cullTerrainOpen = true;
                        WOWVR_INFO("Terrain view-cone opened all the way round.");
                    }
                    // Toggles the wholesale terrain-culling bit as a fallback
                    // discriminator: if a terrain void survives cullterrainon, this
                    // tells "the cone was not the failing gate" apart from "the horizon
                    // buckets only cover the heading's sector". The setter logs.
                    else if (strncmp(command, "cullterrainbit", 14) == 0)
                    {
                        g_terrainCullBitCleared = !g_terrainCullBitCleared;
                        CullView().SetDisableTerrainCulling(g_terrainCullBitCleared);
                    }
                    // "cullworldplanes" toggles the stomp; a trailing '-' selects the
                    // negative pass distance for the case where the set's inside test
                    // runs the other way (then positive d culls everything, which is
                    // itself the answer to which way the test runs).
                    else if (strncmp(command, "cullworldplanes", 15) == 0)
                    {
                        g_worldPlanesOpen = !g_worldPlanesOpen;
                        CullView().SetOpenWorldPlanes(
                            g_worldPlanesOpen,
                            command[15] == '-' ? -1.0e9f : 1.0e9f);
                    }
                    // "cullmasteroff" before "cullmasteron": "cullmasteron" is not a
                    // prefix of it, but the reverse order reads wrong at a glance and
                    // this chain has been bitten by exactly that before.
                    else if (strncmp(command, "cullmasteroff", 13) == 0)
                    {
                        g_cullMasterRotate = false;
                        WOWVR_INFO("Master corner rotation disabled; terrain culls to "
                                   "the character's heading again.");
                    }
                    else if (strncmp(command, "cullmasteron", 12) == 0)
                    {
                        g_cullMasterRotate = true;
                        WOWVR_INFO("Master corner rotation enabled.");
                    }
                    // "cullwmorange <loHex> <hiHex>", published-image addresses, e.g.
                    // "cullwmorange 7BC500 7BCF00". Longest prefix first: "cullwmoall"
                    // would never match these, but "cullwmorange" must precede any
                    // shorter "cullwmo" test ever added.
                    else if (strncmp(command, "cullwmorange ", 13) == 0)
                    {
                        unsigned lo = 0;
                        unsigned hi = 0;
                        if (sscanf(command + 13, "%x %x", &lo, &hi) == 2 && hi > lo)
                        {
                            CullView().SetWmoBypassRange(
                                (lo < 0x00400000u) ? lo + 0x00400000u : lo,
                                (hi < 0x00400000u) ? hi + 0x00400000u : hi);
                        }
                        else
                        {
                            WOWVR_WARN("cullwmorange wants two hex addresses, low then "
                                       "high.");
                        }
                    }
                    // "cullfeedmask <n>": bit i set sends master read site i to the
                    // UNROTATED shadow, clear returns it to the live (head-rotated)
                    // master. "cullfeed <site> s|l" flips one site. These are the
                    // bisection levers for the portal-vs-terrain split of the master
                    // corner consumers; the setter logs each swap.
                    else if (strncmp(command, "cullfeedmask ", 13) == 0)
                    {
                        unsigned mask = 0;
                        if (sscanf(command + 13, "%u", &mask) == 1)
                        {
                            CullView().SetMasterFeedMask(mask);
                        }
                        else
                        {
                            WOWVR_WARN("cullfeedmask wants a number (bit=shadow).");
                        }
                    }
                    else if (strncmp(command, "cullfeed ", 9) == 0)
                    {
                        int site = -1;
                        char mode = 0;
                        if (sscanf(command + 9, "%d %c", &site, &mode) == 2
                            && (mode == 's' || mode == 'l'))
                        {
                            CullView().SetMasterFeed(site, mode == 's');
                        }
                        else
                        {
                            WOWVR_WARN("cullfeed wants '<site> s' (shadow) or "
                                       "'<site> l' (live).");
                        }
                    }
                    // "pipereadback <0|1>": the copy path's staging ring, live. 1
                    // locks the previous frame's readback (no GPU drain), 0 the
                    // classic same-frame lock - the A/B pair for the frame report's
                    // capture time.
                    else if (strncmp(command, "pipereadback ", 13) == 0)
                    {
                        int on = -1;
                        if (sscanf(command + 13, "%d", &on) == 1 && (on == 0 || on == 1))
                        {
                            for (int eye = 0; eye < EyeCount; ++eye)
                            {
                                g_eyeTargets[eye].SetPipelined(on == 1);
                            }
                            WOWVR_INFO("Pipelined readback -> %s.", (on == 1) ? "on" : "off");
                        }
                        else
                        {
                            WOWVR_WARN("pipereadback wants 0 or 1.");
                        }
                    }
                    // "presenterthread <0|1>": whether upload + submit run on the
                    // worker. Off falls back to the inline game-thread path the
                    // next frame; the frame-top idle wait drains the worker first.
                    else if (strncmp(command, "presenterthread ", 16) == 0)
                    {
                        int on = -1;
                        if (sscanf(command + 16, "%d", &on) == 1 && (on == 0 || on == 1))
                        {
                            g_presenterThreadConfigApplied = true;
                            g_presenterThreadOn = (on == 1);
                            WOWVR_INFO("Presenter thread -> %s.", (on == 1) ? "on" : "off");
                        }
                        else
                        {
                            WOWVR_WARN("presenterthread wants 0 or 1.");
                        }
                    }
                    // Toggles the map-object family bypass; the setter logs both ways.
                    else if (strncmp(command, "cullwmoall", 10) == 0)
                    {
                        g_wmoGroupsAllVisible = !g_wmoGroupsAllVisible;
                        WOWVR_INFO("WMO group bypass toggle -> %s (applies next frame).",
                                   g_wmoGroupsAllVisible ? "on" : "off");
                    }
                    // The head-driven sound listener. Ordered longest-prefix-first, like
                    // the block above and for the same reason: "sndon" tested before
                    // "sndoff" would swallow it.
                    else if (strncmp(command, "sndstate", 8) == 0)
                    {
                        HeadListener().Report();
                    }
                    else if (strncmp(command, "snddump", 7) == 0)
                    {
                        HeadListener().RequestDump();
                    }
                    else if (strncmp(command, "sndpitchsign ", 13) == 0)
                    {
                        HeadListener().SetPitchSign(
                            static_cast<float>(atof(command + 13)));
                    }
                    else if (strncmp(command, "sndrollsign ", 12) == 0)
                    {
                        HeadListener().SetRollSign(static_cast<float>(atof(command + 12)));
                    }
                    else if (strncmp(command, "sndlateralsign ", 15) == 0)
                    {
                        HeadListener().SetLateralSign(
                            static_cast<float>(atof(command + 15)));
                    }
                    else if (strncmp(command, "sndsign ", 8) == 0)
                    {
                        HeadListener().SetYawSign(static_cast<float>(atof(command + 8)));
                    }
                    // "sndforce <degrees>" turns the ears without turning the picture,
                    // which is how this gets judged with the headset sitting still: stand
                    // by something that loops, force 90, and the sound should move to one
                    // ear and hold there.
                    else if (strncmp(command, "sndforce ", 9) == 0)
                    {
                        HeadListener().SetForcedYawDegrees(
                            static_cast<float>(atof(command + 9)));
                    }
                    else if (strncmp(command, "sndmove ", 8) == 0)
                    {
                        HeadListener().SetTrackPosition(atoi(command + 8) != 0);
                    }
                    // "sndcam 1" puts the ears at the viewpoint, "sndcam 0" hands the
                    // position back to the client - which parks it on the character, so
                    // this is the A/B for "does zooming out make anything quieter".
                    else if (strncmp(command, "sndcam ", 7) == 0)
                    {
                        HeadListener().SetEarsAtCamera(atoi(command + 7) != 0);
                    }
                    else if (strncmp(command, "sndon", 5) == 0)
                    {
                        g_headListenerEnabled = true;
                        WOWVR_INFO("Head-driven sound listener enabled.");
                    }
                    else if (strncmp(command, "sndoff", 6) == 0)
                    {
                        g_headListenerEnabled = false;
                        WOWVR_INFO("Head-driven sound listener disabled; the ears go back "
                                   "on the client's camera.");
                    }
                    else if (strncmp(command, "cullaspect ", 11) == 0)
                    {
                        g_cullAspectOverride = static_cast<float>(atof(command + 11));
                        WOWVR_INFO("Cull aspect override %.3f (was reading %.4f).",
                                   g_cullAspectOverride, GameCam().CullAspect());
                    }
                    else if (strncmp(command, "cullfov ", 8) == 0)
                    {
                        g_cullFovOverride = static_cast<float>(atof(command + 8));
                        WOWVR_INFO("Cull fov override %.4f rad (was reading %.4f).",
                                   g_cullFovOverride, GameCam().CullFov());
                    }
                    else if (strncmp(command, "orbitsign ", 10) == 0)
                    {
                        const float s = static_cast<float>(atof(command + 10));
                        Projection().SetOrbitSign(s);
                        WOWVR_INFO("Orbit correction sign %+.0f.", Projection().OrbitSign());
                    }
                    else if (strncmp(command, "unwindpitchfirst", 16) == 0)
                    {
                        Projection().SetUnwindPitchFirst(true);
                        WOWVR_INFO("Unwinding PITCH first (the old order, which rolls).");
                    }
                    else if (strncmp(command, "unwindyawfirst", 14) == 0)
                    {
                        Projection().SetUnwindPitchFirst(false);
                        WOWVR_INFO("Unwinding YAW first (correct for a yaw-then-pitch camera).");
                    }
                    // "headmove <x> <y> <z>" in metres: a lean or a step, without moving the
                    // headset. x is right, y is up, z is forward.
                    else if (strncmp(command, "headmove ", 9) == 0)
                    {
                        Vec3 offset;
                        sscanf(command + 9, "%f %f %f", &offset.x, &offset.y, &offset.z);
                        Projection().SetFakeHeadOffset(offset);
                        WOWVR_INFO("Fake head displacement (%.2f, %.2f, %.2f) m.",
                                   offset.x, offset.y, offset.z);
                    }
                    // "gcsigns <yaw> <pitch>", each +1 or -1.
                    else if (strncmp(command, "gcsigns ", 8) == 0)
                    {
                        float y = 1.0f, p = -1.0f;
                        sscanf(command + 8, "%f %f", &y, &p);
                        GameCam().SetSigns(y, p);
                    }
                    else if (strncmp(command, "cullread", 8) == 0)
                    {
                        WOWVR_INFO("Cull frustum now reads fov %.4f rad, aspect %.4f; "
                                   "overrides are fov %.4f, aspect %.4f.",
                                   GameCam().CullFov(), GameCam().CullAspect(),
                                   g_cullFovOverride, g_cullAspectOverride);
                    }
                    else if (strncmp(command, "pitchaimon", 10) == 0)
                    {
                        g_aimPitch = true;
                        WOWVR_INFO("Pitch aiming ON.");
                    }
                    else if (strncmp(command, "pitchaimoff", 11) == 0)
                    {
                        g_aimPitch = false;
                        WOWVR_INFO("Pitch aiming OFF.");
                    }
                    else if (strncmp(command, "headaimon", 9) == 0)
                    {
                        g_aimEnabled = true;
                        WOWVR_INFO("Camera aiming ON: the client culls where the head looks.");
                    }
                    else if (strncmp(command, "headaimoff", 10) == 0)
                    {
                        g_aimEnabled = false;
                        WOWVR_INFO("Camera aiming OFF: the view turns but the client does not.");
                    }
                    // Toggle the orbit correction and NOTHING else.
                    //
                    // "orbit" and "orbitnone" below also set the fake head yaw - 0.6 rad and
                    // zero respectively - which made them useless as toggles and quietly
                    // invalidated three rounds of measurement: every arm was captured at a
                    // different head angle from the one the test had asked for, so the
                    // comparison was between unrelated views and no arm could win.
                    // "watch <hex field offset>" - trap on writes to that field of the camera
                    // object and name the instructions responsible. "watchradius" is the
                    // shorthand for the camera distance at +0x118.
                    else if (strncmp(command, "watchradius", 11) == 0 ||
                             strncmp(command, "watch ", 6) == 0)
                    {
                        unsigned offset = 0x118u;
                        if (strncmp(command, "watch ", 6) == 0)
                        {
                            sscanf(command + 6, "%x", &offset);
                        }
                        if (!GameCam().Found())
                        {
                            WOWVR_INFO("No game camera to watch yet.");
                        }
                        else
                        {
                            const void* field = reinterpret_cast<const void*>(
                                GameCam().Address() + offset);
                            WOWVR_INFO("Watching camera +0x%X at 0x%p.", offset, field);
                            Watch().Arm(field);
                        }
                    }
                    else if (strncmp(command, "watchreport", 11) == 0)
                    {
                        Watch().Report();
                    }
                    else if (strncmp(command, "watchreset", 10) == 0)
                    {
                        Watch().Reset();
                        WOWVR_INFO("Field watch cleared; next report covers only what follows.");
                    }
                    else if (strncmp(command, "watchoff", 8) == 0)
                    {
                        Watch().Disarm();
                    }
                    // Where the camera actually is, right now. Sampled with aiming on and
                    // again with it off, the difference is the orbit displacement with no
                    // convention involved - which is the only way to check the one that is
                    // computed from angles.
                    else if (strncmp(command, "campos", 6) == 0)
                    {
                        Vec3 p, f, u;
                        if (GameCam().CameraPose(p, f, u))
                        {
                            Vec3 predicted;
                            GameCam().OrbitDisplacementView(GameCam().AppliedYaw(), predicted);
                            WOWVR_INFO("Camera pose: pos (%.4f, %.4f, %.4f) fwd (%.4f, %.4f, "
                                       "%.4f) up (%.4f, %.4f, %.4f) radius %.3f; "
                                       "predicted swing (%.3f, %.3f, %.3f).",
                                       p.x, p.y, p.z, f.x, f.y, f.z, u.x, u.y, u.z,
                                       GameCam().OrbitRadius(),
                                       predicted.x, predicted.y, predicted.z);
                        }
                        else
                        {
                            WOWVR_INFO("Camera pose unavailable.");
                        }
                    }
                    else if (strncmp(command, "collisionoff", 12) == 0)
                    {
                        g_disableCameraCollision = true;
                        GameCam().SetCollision(false);
                    }
                    else if (strncmp(command, "collisionon", 11) == 0)
                    {
                        g_disableCameraCollision = false;
                        GameCam().SetCollision(true);
                    }
                    else if (strncmp(command, "pinradiuson", 11) == 0)
                    {
                        g_pinOrbitRadius = true;
                        WOWVR_INFO("Pinning the camera distance (defeating collision).");
                    }
                    else if (strncmp(command, "pinradiusoff", 12) == 0)
                    {
                        g_pinOrbitRadius = false;
                        WOWVR_INFO("Camera distance left to the client.");
                    }
                    else if (strncmp(command, "orbitcompoff", 12) == 0)
                    {
                        g_compensateOrbit = false;
                        WOWVR_INFO("Orbit correction OFF (head angle untouched).");
                    }
                    else if (strncmp(command, "orbitcompon", 11) == 0)
                    {
                        g_compensateOrbit = true;
                        WOWVR_INFO("Orbit correction ON (head angle untouched).");
                    }
                    // "orbitoff" before "orbit", or the shorter prefix swallows it.
                    else if (strncmp(command, "orbitoff", 8) == 0)
                    {
                        g_fakeHeadYaw = 0.6f;
                        g_compensateOrbit = false;
                        WOWVR_INFO("Fake head turn of 0.6 rad, orbit correction OFF; "
                                   "radius reads %.3f yards.", Camera().OrbitRadius());
                    }
                    else if (strncmp(command, "orbitnone", 9) == 0)
                    {
                        g_fakeHeadYaw = 0.0f;
                        g_compensateOrbit = true;
                        WOWVR_INFO("Fake head turn cleared.");
                    }
                    else if (strncmp(command, "orbit", 5) == 0)
                    {
                        g_fakeHeadYaw = 0.6f;
                        g_compensateOrbit = true;
                        WOWVR_INFO("Fake head turn of 0.6 rad, orbit correction ON; "
                                   "radius reads %.3f yards.", Camera().OrbitRadius());
                    }
                    else if (strncmp(command, "fovscan", 7) == 0)
                    {
                        // The value to look for comes from the global that was already
                        // identified and verified against the projection on the wire.
                        const float* global = reinterpret_cast<const float*>(0x00ABFC38u);
                        Camera().ScanCameraObjectForFov(*global);
                    }
                    // Writing the global is the stimulus, not the fix. It is inert on
                    // screen, but the client copies it into the active camera every frame,
                    // so whatever moves in the camera object when this is written IS the
                    // copy the renderer reads. Guessing at encodings found nothing; this
                    // asks the client to point at the field itself.
                    else if (strncmp(command, "globalfov", 9) == 0)
                    {
                        Camera().WidenGameFovByFactor(1.35f);
                        WOWVR_INFO("Global field of view widened by 1.35 as a stimulus.");
                    }
                    else if (strncmp(command, "fovcopies", 9) == 0)
                    {
                        const float* global = reinterpret_cast<const float*>(0x00ABFC38u);
                        Camera().ScanForFovCopies(*global);
                    }
                    else if (strncmp(command, "fovrestore", 10) == 0)
                    {
                        Camera().RestoreGameFov();
                        WOWVR_INFO("Global field of view restored.");
                    }
                    else if (strncmp(command, "fovwiden", 8) == 0) { Camera().SetFovOverride(1.35f); }
                    else if (strncmp(command, "fovoff", 6) == 0)   { Camera().SetFovOverride(0.0f); }
                    else if (strncmp(command, "sig", 3) == 0)   { Camera().ScanForCameraSignature(); }
                    else if (strncmp(command, "zoomsnap", 8) == 0) { Camera().ZoomSnapshot(); }
                    else if (strncmp(command, "zoomidle", 8) == 0) { Camera().ZoomReport("idle"); }
                    else if (strncmp(command, "zoomed", 6) == 0)   { Camera().ZoomReport("zoomed"); }
                    else if (strncmp(command, "turned", 6) == 0)   { Camera().ZoomReport("turned"); }
                    else if (strncmp(command, "verify", 6) == 0)   { Camera().VerifyCameraChain(); }
                }
            }

            if (Cfg().watchCameraStruct)
            {
                Vec3 cameraPosition;
                const bool havePosition = Projection().CameraWorldPosition(cameraPosition);
                Camera().WatchCameraStruct(cameraPosition, havePosition);

                if ((g_frameCount % 120) == 0)
                {
                    float gameYaw = 0.0f;
                    const bool haveYaw = Projection().CameraYaw(gameYaw);
                    WOWVR_INFO("Game camera yaw: %s%.4f rad (%.1f deg), agreed by %d draws; "
                               "head yaw %.4f rad (%.1f deg)",
                               haveYaw ? "" : "(none) ", gameYaw, gameYaw * 57.2957795f,
                               Projection().CameraYawWeight(),
                               Projection().HeadYaw(), Projection().HeadYaw() * 57.2957795f);
                    for (int i = 0; i < Projection().VoteCount(); ++i)
                    {
                        const Vec3& position = Projection().VotePosition(i);
                        WOWVR_INFO("    %4d draws at %10.2f %10.2f %10.2f",
                                   Projection().VoteWeight(i), position.x, position.y, position.z);
                    }
                }
            }

            if (Cfg().scanForCamera && Projection().HasSceneProjection() && !Camera().Found())
            {
                const bool restart = Camera().ShouldRescan(Projection().SceneFar(),
                                                           Projection().SceneVerticalScale());

                if (g_frameCount == 300 || (restart && (g_frameCount % 300) == 0))
                {
                    Camera().Scan(Projection().SceneNear(), Projection().SceneFar(),
                                  Projection().SceneAspect(), Projection().SceneVerticalScale());
                }
                else if (g_frameCount > 300 && (g_frameCount % 300) == 0)
                {
                    Camera().Verify(Projection().SceneNear(), Projection().SceneFar(),
                                    Projection().SceneAspect(), Projection().SceneVerticalScale());
                }
                else if (g_frameCount > 900 && (g_frameCount % 10) == 0)
                {
                    // Once the stable survivors are known, find out which of them the
                    // game actually builds its projection from.
                    Camera().ActiveTest(Projection().SceneVerticalScale());
                }
            }

            g_depthRangeCount = 0;
            Report().BeginFrame(g_frameCount);

            // Armed at the END of the hook, after every piece of our own device work.
            // Armed at the top, the panel compositing and mirror blits inside this very
            // hook re-entered the constant/transform hooks, consumed the once-per-frame
            // refresh with the client's camera still un-updated, and the whole next
            // frame then drew against a one-frame-stale measurement. Invisible while
            // the camera was still; a full band-width turn for one frame at a pitch
            // re-aim, and the per-frame delta whenever the camera was tracking.
            g_compensationFresh = false;
            return hr;
        }

        HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
        {
            WOWVR_INFO("Device reset requested (%ux%u, windowed=%d). Dropping VR resources first.",
                       parameters != nullptr ? parameters->BackBufferWidth : 0,
                       parameters != nullptr ? parameters->BackBufferHeight : 0,
                       parameters != nullptr ? (parameters->Windowed ? 1 : 0) : -1);

            ReleaseFrameResources();

            const HRESULT hr = g_originalReset(device, parameters);
            if (FAILED(hr))
            {
                WOWVR_ERROR("Device reset failed (0x%08lx).", hr);
            }

            // Resources are rebuilt lazily on the next Present.
            return hr;
        }

        // -------------------------------------------------------------------
        // Diagnostic hooks
        //
        // These exist to answer one question: where does WoW's camera live? They
        // feed the frame report and are near-free when it is not armed. Phase 5
        // takes them over to do the actual per-eye work.
        // -------------------------------------------------------------------
        HRESULT WINAPI HookedSetTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state,
                                          const D3DMATRIX* matrix)
        {
            EnsureFreshCameraCompensation();

            if (Report().IsActive() && matrix != nullptr)
            {
                Report().NoteTransform(static_cast<uint32_t>(state), &matrix->_11);
            }

            if (state == D3DTS_PROJECTION && matrix != nullptr && !g_duplicatingDraw
                && Cfg().enabled && Cfg().patchFixedFunction
                && g_renderingToBackBuffer && !g_uiPassStarted)
            {
                if (Projection().TryPatch(&matrix->_11, g_eyeFixedProjection[EyeLeft],
                                          g_eyeFixedProjection[EyeRight]))
                {
                    memcpy(g_currentFixedProjection, &matrix->_11, sizeof(g_currentFixedProjection));
                    g_haveFixedProjectionSource = true;
                    g_fixedProjectionIsOrtho = false;
                    g_projectionPatchedThisFrame = true;
                    return g_originalSetTransform(
                        device, state,
                        reinterpret_cast<const D3DMATRIX*>(g_eyeFixedProjection[EyeLeft]));
                }

                // Not the scene camera. If it is still a perspective camera then world
                // geometry is about to be drawn through it and it has to follow the head
                // like everything else - a secondary camera with its own field of view is
                // exactly what left small pieces of town geometry pinned to the screen.
                // Only a genuinely orthographic projection is left alone.
                if (Projection().PatchAnyPerspective(&matrix->_11,
                                                     g_eyeFixedProjection[EyeLeft],
                                                     g_eyeFixedProjection[EyeRight]))
                {
                    memcpy(g_currentFixedProjection, &matrix->_11,
                           sizeof(g_currentFixedProjection));
                    g_haveFixedProjectionSource = true;
                    g_fixedProjectionIsOrtho = false;
                    g_projectionPatchedThisFrame = true;
                    return g_originalSetTransform(
                        device, state,
                        reinterpret_cast<const D3DMATRIX*>(g_eyeFixedProjection[EyeLeft]));
                }

                g_haveFixedProjectionSource = false;
                g_fixedProjectionIsOrtho = true;
            }
            else if (state == D3DTS_PROJECTION && matrix != nullptr && !g_duplicatingDraw)
            {
                g_fixedProjectionIsOrtho = false;
            }

            return g_originalSetTransform(device, state, matrix);
        }

        bool StereoActive()
        {
            return Cfg().stereo && g_stereo.IsReady() && Vr().IsActive() && g_stereoRedirected;
        }

        // Duplicated draws only make sense for the pass that is building the player's
        // view. The shadow map and the post-process passes render once and are shared.
        // Called before every back buffer draw. Closes the world pass and opens the UI
        // pass the first time a second batch of drawing starts on the back buffer.
        void UpdateDrawPeriod(IDirect3DDevice9* device)
        {
            // Driven by observed draws rather than by SetRenderTarget calls. At the
            // start of a frame the back buffer is already bound from the last Present,
            // so the game never rebinds it and a bind-driven counter misses the world
            // pass completely.
            // Where every draw actually ended up. If world geometry is going somewhere
            // other than the stereo target, this says so directly instead of leaving it
            // to be inferred from a black screen.
            // Probe at the first world draw, not at Present: the shadow is a live
            // mirror, so by the end of the frame it holds the interface's constants and
            // no longer resembles anything terrain was drawn with.
            // Keeps looking for a while rather than stopping at the first hit: the
            // registers are not all in use on any single frame, and stopping early left
            // terrain's own transform undiscovered.
            // Once per frame at most. UpdateDrawPeriod runs for every draw, so without
            // this the scan fired several hundred times a frame.
            if (g_frameCount > 30 && (g_frameCount % 30) == 0 && g_frameCount != g_lastProbeFrame
                && g_renderingToBackBuffer && !g_uiPassStarted
                && g_combinedRegisterCount < 1)
            {
                g_lastProbeFrame = g_frameCount;
                g_probeCombinedPending = true;
            }

            if (g_probeCombinedPending && g_renderingToBackBuffer && !g_uiPassStarted)
            {
                g_probeCombinedPending = false;
                ProbeForCombinedTransforms();
            }

            if (!g_renderingToBackBuffer)
            {
                ++g_drawsOffscreen;
                if (g_currentOffscreenTarget >= 0)
                {
                    ++g_offscreenTargets[g_currentOffscreenTarget].draws;
                }
            }
            else if (g_uiPassStarted)          { ++g_drawsToUi; }
            else if (g_stereoRedirected)
            {
                ++g_drawsToStereo;
                if (g_vertexFormatIsPreTransformed) { ++g_preTransformedWorldDraws; }
                if (g_scissorTestEnabled) { ++g_scissoredWorldDraws; }
            }
            else                               { ++g_drawsStrayBackBuffer; }

            const bool nowOnBackBuffer = g_renderingToBackBuffer;
            const bool periodStarted = nowOnBackBuffer && !g_wasDrawingToBackBuffer;
            g_wasDrawingToBackBuffer = nowOnBackBuffer;

            if (!periodStarted)
            {
                return;
            }

            ++g_backBufferDrawPeriod;

            // A frame with no 3D camera at all is a loading screen: a full-screen
            // image and nothing else. Treating it as world geometry hands it the eye
            // frustum, which tilts it through the viewer at an angle. It belongs on the
            // panel with the rest of the flat content.
            if (g_backBufferDrawPeriod == 1 && !g_previousFrameHadCamera)
            {
                g_backBufferDrawPeriod = 2;
            }

            // First batch is the world. Make sure it lands in the stereo target even
            // if the game never explicitly asked for the back buffer.
            if (g_backBufferDrawPeriod == 1)
            {
                if (Cfg().stereo && g_stereo.IsReady() && Vr().IsActive() && !g_stereoRedirected)
                {
                    g_originalSetRenderTarget(device, 0, g_stereo.Color());
                    g_originalSetDepthStencilSurface(device, g_stereo.Depth());
                    g_stereoRedirected = true;
                    g_stereoHasContent = true;
                }
                return;
            }

            if (g_uiPassStarted)
            {
                return;
            }

            // Deliberately falls through to the per-draw check below rather than
            // starting the UI here: the first draws of this batch are the bloom
            // composite, not the interface.
        }

        // The interface proper begins at the first blended draw of the second batch.
        // The post-process composite that precedes it is drawn unblended, and it is a
        // full-screen quad - letting it onto the UI layer is what painted the login
        // background across the panel and filled the panel's alpha so it read as an
        // opaque black slab in the world.
        void MaybeStartUiPass(IDirect3DDevice9* device)
        {
            if (g_uiPassStarted || g_backBufferDrawPeriod < 2 || !g_renderingToBackBuffer)
            {
                return;
            }

            // The blended-draw test exists to let the post-process composite through
            // before the interface starts. On a loading screen there is no world pass
            // and no composite, and the image itself is opaque, so waiting for a
            // blended draw would never let it onto the panel.
            const bool loadingScreen = !g_previousFrameHadCamera;
            if (!loadingScreen && !g_alphaBlendEnabled)
            {
                return;
            }

            g_uiPassStarted = true;

            // Give the interface a target of its own, at the game's own resolution.
            // It is laid out for a flat screen and has to be drawn once at that size,
            // not squeezed into an eye viewport.
            if (g_stereoRedirected)
            {
                IDirect3DSurface9* uiSurface =
                    g_uiPanel.IsReady() ? g_uiPanel.Surface() : g_realBackBuffer;

                g_originalSetRenderTarget(device, 0, uiSurface);
                g_originalSetDepthStencilSurface(device, nullptr);
                g_stereoRedirected = false;

                if (g_uiPanel.IsReady())
                {
                    D3DVIEWPORT9 full = {};
                    full.X = 0;
                    full.Y = 0;
                    full.Width = g_uiPanel.Width();
                    full.Height = g_uiPanel.Height();
                    full.MinZ = 0.0f;
                    full.MaxZ = 1.0f;
                    g_originalSetViewport(device, &full);

                    // Fully transparent, so only the pixels the interface actually
                    // covers end up over the world.
                    g_originalClear(device, 0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
                    g_uiRendered = true;

                    // From here the alpha channel means coverage, not opacity, and has
                    // to be blended on its own terms.
                    g_uiTargetBound = true;
                    ApplyUiAlphaBlend(device);
                }
            }
        }

        // True for the post-process composite: the batch after the world but before
        // the interface proper.
        bool ShouldSkipDraw()
        {
            if (!StereoActive() || !g_renderingToBackBuffer || g_duplicatingDraw)
            {
                return false;
            }

            // The post-process composite: after the world, before the interface.
            if (Cfg().skipPostProcess && g_backBufferDrawPeriod >= 2 && !g_uiPassStarted)
            {
                // Characterise what is being discarded. If the client applies any part of
                // its shadowing in a later pass, this rule would be eating it, and that
                // rule is live only when stereo is on - which matches the fault exactly.
                if (Cfg().logSkippedDraws && g_skippedDrawsLogged < 24)
                {
                    ++g_skippedDrawsLogged;
                    WOWVR_INFO("  SKIPPED post-process draw: prims=%u vs=%p ps=%p "
                               "blend=%s ztest=%s period=%d",
                               g_lastPrimitiveCount,
                               static_cast<void*>(g_currentVertexShader),
                               static_cast<void*>(g_currentPixelShader),
                               g_alphaBlendEnabled ? "on" : "off",
                               g_depthTestEnabled ? "on" : "off",
                               g_backBufferDrawPeriod);
                }
                ++g_postProcessDrawsSkipped;
                return true;
            }

            // Screen-space overlays inside the world pass, which cannot be duplicated
            // usefully because their coordinates are absolute.
            //
            // Pre-transformed vertices are the obvious form of this and never actually
            // occurred. The form that does occur is a fixed-function draw made under an
            // ORTHOGRAPHIC projection: small untextured quads issued between the distant
            // terrain and the sky. Being screen-space they have no world position to
            // follow, so no eye transform can anchor them - they simply hang in front of
            // the viewer. Flat-screen effects like these have no meaning in VR.
            if (!Cfg().skipScreenSpaceWorldDraws || g_uiPassStarted)
            {
                return false;
            }

            return g_vertexFormatIsPreTransformed
                || (g_fixedProjectionIsOrtho && g_currentVertexShader == nullptr);
        }

        // Counts world draws during the frame being dumped and snapshots at intervals.
        void NoteWorldDrawForSequence(IDirect3DDevice9* device)
        {
            if (!g_sequenceDumpArmed)
            {
                return;
            }

            ++g_worldDrawIndex;

            if (Cfg().logWorldDrawTo > 0
                && g_worldDrawIndex >= Cfg().logWorldDrawFrom
                && g_worldDrawIndex <= Cfg().logWorldDrawTo)
            {
                IDirect3DBaseTexture9* texture = nullptr;
                device->GetTexture(0, &texture);

                // Every stage, not just the first. A model coming out as a flat colour
                // with the right silhouette looks far more like it is sampling the wrong
                // texture than like a transform fault, so what is bound where matters.
                IDirect3DBaseTexture9* stage[8] = {};
                for (DWORD st = 0; st < 8; ++st)
                {
                    device->GetTexture(st, &stage[st]);
                }

                IDirect3DVertexShader9* shader = nullptr;
                device->GetVertexShader(&shader);

                // The decisive columns are the last two: whether this draw actually had a
                // head-rotated camera put in front of it on either path. Geometry that
                // gets neither is drawn with the game's own camera and ends up welded
                // to the viewer's head.
                WOWVR_INFO("world draw %-4d: prims=%-6u tex=%p vs=%p blend=%s ztest=%s "
                           "depth=%.3f..%.3f  camShader=%d camFixed=%d cwrite=%X atest=%d "
                           "ortho=%d vp=%ux%u+%u+%u ps=%p tex0=%p s4=%p s5=%p s6=%p s7=%p",
                           g_worldDrawIndex, g_lastPrimitiveCount,
                           static_cast<void*>(texture), static_cast<void*>(shader),
                           g_alphaBlendEnabled ? "on" : "off",
                           g_depthTestEnabled ? "on" : "off",
                           g_currentViewport.MinZ, g_currentViewport.MaxZ,
                           g_patchedBlockCount, g_haveFixedProjectionSource ? 1 : 0,
                           g_colorWriteMask, g_alphaTestEnabled ? 1 : 0,
                           g_fixedProjectionIsOrtho ? 1 : 0,
                           g_gameViewport.Width, g_gameViewport.Height,
                           g_gameViewport.X, g_gameViewport.Y,
                           static_cast<void*>(g_currentPixelShader),
                           static_cast<void*>(stage[0]), static_cast<void*>(stage[4]),
                           static_cast<void*>(stage[5]), static_cast<void*>(stage[6]),
                           static_cast<void*>(stage[7]));

                for (DWORD st = 0; st < 8; ++st)
                {
                    if (stage[st] != nullptr) { stage[st]->Release(); }
                }

                // Which registers currently hold something that can be read back as a
                // world-view-projection. This is the question that matters for a draw
                // that will not follow the head: either its transform is in there and we
                // are missing it, or it is not, and the cause lies elsewhere.
                char affine[256];
                int used = 0;
                for (UINT reg = 0; reg + 4 <= 64 && reg + 4 <= kShadowRegisters; ++reg)
                {
                    float l[16];
                    float r[16];
                    const bool plain = Projection().TryPatchCombined(&g_shadowConstants[reg][0], l, r);
                    const bool rigid = plain
                        && Projection().TryPatchCombinedStrict(&g_shadowConstants[reg][0], l, r);
                    if (plain && used < static_cast<int>(sizeof(affine)) - 8)
                    {
                        used += sprintf_s(affine + used, sizeof(affine) - used, "c%u%s ",
                                          reg, rigid ? "*" : "");
                    }
                }
                affine[used] = 0;
                WOWVR_INFO("      recoverable transforms: %s", used ? affine : "(none)");

                // What the shader will actually read for its shadow cascades on THIS
                // draw. The upload order differs between mono and stereo, so only the
                // resident value at draw time is a fair comparison.
                if (Cfg().logShadowConstants)
                {
                    for (UINT reg = 224; reg <= 226; ++reg)
                    {
                        const float* v = &g_shadowConstants[reg][0];
                        WOWVR_INFO("      resident c%u = %+.6f %+.6f %+.6f %+.6f",
                                   reg, v[0], v[1], v[2], v[3]);
                    }
                }

                if (texture != nullptr) { texture->Release(); }
                if (shader != nullptr) { shader->Release(); }
            }
            const int step = Cfg().dumpEveryNWorldDraws;
            if (step > 0 && (g_worldDrawIndex % step) == 0 && g_sequenceDumpsWritten < 24)
            {
                ++g_sequenceDumpsWritten;
                DumpWorldProgress(device, g_worldDrawIndex);
            }
        }

        // Which constant register holds a shader's combined world-view-projection.
        //
        // This has to be keyed on the shader, not on the register: c32 is the water's
        // transform AND the character shaders' bone palette, so any global rule about a
        // register is wrong for somebody. Resolving it per shader, at draw time when the
        // shader is actually bound, is what makes patching the high registers safe.
        struct ShaderCombined
        {
            IDirect3DVertexShader9* shader;
            UINT startRegister;
            int confirmations;
            int attempts;
            bool resolved;
            bool hopeless;

            // The left and right matrices last derived, and the constants they came
            // from. BeginEye runs once per eye with nothing uploaded in between, so the
            // second call always hits this and the recovery is done once per draw
            // rather than twice.
            bool hasCache;
            float cachedSource[16];
            float cachedLeft[16];
            float cachedRight[16];
        };

        const int kMaxShaderCombined = 96;
        ShaderCombined g_shaderCombined[kMaxShaderCombined];
        int g_shaderCombinedCount = 0;

        ShaderCombined* FindShaderCombined(IDirect3DVertexShader9* shader)
        {
            for (int i = 0; i < g_shaderCombinedCount; ++i)
            {
                if (g_shaderCombined[i].shader == shader)
                {
                    return &g_shaderCombined[i];
                }
            }

            if (g_shaderCombinedCount >= kMaxShaderCombined)
            {
                return nullptr;
            }

            ShaderCombined& entry = g_shaderCombined[g_shaderCombinedCount++];
            entry.shader = shader;
            entry.startRegister = 0;
            entry.confirmations = 0;
            entry.attempts = 0;
            entry.resolved = false;
            entry.hopeless = false;
            entry.hasCache = false;
            return &entry;
        }

        // Registers above the first 96 are texture and lighting data on every shader
        // seen so far, and scanning them only costs time.
        const UINT kCombinedSearchLimit = 96;

        // Looks for the current shader's transform in its own constants. A register has
        // to pass on several separate draws before it is trusted, so a matrix that
        // happens to validate once cannot capture the shader.
        void ResolveCombinedRegisterForShader()
        {
            if (!Cfg().perShaderCombined
                || g_currentVertexShader == nullptr || !Projection().HasSceneProjection())
            {
                return;
            }

            ShaderCombined* entry = FindShaderCombined(g_currentVertexShader);
            if (entry == nullptr || entry->resolved || entry->hopeless)
            {
                return;
            }

            if (++entry->attempts > 240)
            {
                entry->hopeless = true;
                return;
            }

            float left[16];
            float right[16];
            for (UINT reg = 0; reg + 4 <= kCombinedSearchLimit && reg + 4 <= kShadowRegisters; ++reg)
            {
                if (!Projection().TryPatchCombinedStrict(&g_shadowConstants[reg][0], left, right))
                {
                    continue;
                }

                if (entry->confirmations > 0 && entry->startRegister == reg)
                {
                    if (++entry->confirmations >= 4)
                    {
                        entry->resolved = true;
                        WOWVR_INFO("Shader %p draws through a combined transform at c%u.",
                                   static_cast<void*>(g_currentVertexShader), reg);
                    }
                }
                else
                {
                    entry->startRegister = reg;
                    entry->confirmations = 1;
                }
                return;
            }
        }

        // Put back what the client had before we started drawing eyes. BeginEye clamps
        // the scissor to one eye's half of the side-by-side target and narrows the
        // viewport to match; leaving those in place meant the client's NEXT offscreen
        // pass - its 2048x2048 shadow map above all - was rendered through a scissor
        // sized for an eye, so only a rectangle of the shadow map ever received casters.
        // That is what turned shadows into blocky shapes that track the player.
        void EndEyes(IDirect3DDevice9* device)
        {
            if (g_scissorTestEnabled)
            {
                g_originalSetScissorRect(device, &g_gameScissor);
            }
            g_originalSetViewport(device, &g_gameViewport);
            g_currentViewport = g_gameViewport;
            ++g_eyeStateRestored;
        }

        bool ShouldDuplicateDraw()
        {
            // The UI is deliberately excluded: it is laid out for a 1920x1080 screen
            // and drawing it into an eye-sized viewport scales it wrongly. It gets its
            // own layer instead.
            return StereoActive() && g_renderingToBackBuffer
                && !g_uiPassStarted && !g_duplicatingDraw;
        }

        void BeginEye(IDirect3DDevice9* device, int eye)
        {
            D3DVIEWPORT9 viewport = {};
            viewport.X = (eye == EyeLeft) ? 0 : g_stereo.EyeWidth();
            viewport.Y = 0;
            viewport.Width = g_stereo.EyeWidth();
            viewport.Height = g_stereo.EyeHeight();

            if (Cfg().fullTargetSingleEye)
            {
                viewport.X = 0;
                viewport.Width = g_stereo.EyeWidth() * 2;
            }

            // The game's depth range is carried over rather than reset to 0..1.
            // Squashing geometry into a narrow slice at the far end of the range is how
            // an engine keeps a distant backdrop behind everything else, and forcing the
            // full range back on every draw throws that away - which is what put the far
            // terrain in front of the world.
            viewport.MinZ = g_gameViewportMinZ;
            viewport.MaxZ = g_gameViewportMaxZ;
            if (viewport.MaxZ <= viewport.MinZ)
            {
                viewport.MinZ = 0.0f;
                viewport.MaxZ = 1.0f;
            }

            g_currentViewport = viewport;
            if (!Cfg().keepGameViewport)
            {
                g_originalSetViewport(device, &viewport);
            }

            // The sky dome is squashed into the very top of the depth range, above the
            // distant backdrop terrain at 0.998..0.999. That slice is the reliable way
            // to recognise it: it needs the head's rotation but neither the head's
            // displacement nor the per-eye offset, or it stops reading as a horizon.
            //
            // Decided HERE, before any camera constant goes back in. It used to be set
            // only just before the fixed-function projection further down, so every
            // sky draw made through a vertex shader - the dome and the zone skybox
            // models - still got the cached per-eye blocks and the combined transform
            // WITH stereo separation and head translation: it sat at its real, finite
            // radius (nearer than distant buildings) and slid against the head when
            // leaning.
            const bool skyDepthSlice = viewport.MinZ >= 0.9985f;
            if (skyDepthSlice) { ++g_skySliceDraws; }
            Projection().SetInfiniteDistance(skyDepthSlice);

            // Only the scene camera gets swapped. UI and other ortho passes never had
            // a patched projection, so they are simply drawn into each half as-is.
            for (int i = 0; i < g_patchedBlockCount; ++i)
            {
                const PatchedBlock& block = g_patchedBlocks[i];

                // Restore only while the register still holds the matrix this block was
                // derived from - except for the register the camera was actually
                // discovered in, which is the camera's own home and must always be put
                // back. Guarding that one too was tried and made things worse, because it
                // dropped the VR camera on frames where a wide upload happened to cover
                // it.
                //
                // The secondary registers do need the guard. c0 joins the table simply by
                // holding the camera for a moment, but it is also the water's combined
                // transform and carries model data for other shaders; stamping the camera
                // back into it on every draw is what rendered models as flat silhouettes.
                const bool isPrimary = !block.combined
                    && block.startRegister == g_primaryCameraRegister;

                if (!isPrimary && block.startRegister + 4 <= kShadowRegisters
                    && !MatricesMatch(&g_shadowConstants[block.startRegister][0], block.source))
                {
                    continue;
                }

                // A sky draw needs the rotation-only version, which the cached eye
                // matrices are not; rebuilt from the game's own matrix with the
                // infinite-distance flag set. Falls back to the cached pair if the
                // rebuild is refused.
                if (skyDepthSlice)
                {
                    float skyLeft[16];
                    float skyRight[16];
                    const bool rebuilt = block.combined
                        ? Projection().TryPatchCombined(block.source, skyLeft, skyRight)
                        : Projection().TryPatchNoRecord(block.source, skyLeft, skyRight);
                    if (rebuilt)
                    {
                        g_originalSetVertexShaderConstantF(
                            device, block.startRegister,
                            eye == EyeLeft ? skyLeft : skyRight, 4);
                        continue;
                    }
                }

                g_originalSetVertexShaderConstantF(
                    device, block.startRegister,
                    eye == EyeLeft ? block.left : block.right, 4);
            }

            // The current shader's own combined transform, rebuilt from the constants
            // as they stand right now. Unlike the remembered blocks this is never
            // cached across draws: the register holds a different object every time.
            if (Cfg().patchCombined && Cfg().perShaderCombined
                && g_currentVertexShader != nullptr)
            {
                ShaderCombined* entry = FindShaderCombined(g_currentVertexShader);
                if (entry != nullptr && entry->resolved)
                {
                    // Recomputed for each eye rather than cached. Caching this on the
                    // shader entry cut the work by two orders of magnitude and flattened
                    // the scene into a single plane, so the reuse was plainly not valid.
                    float left[16];
                    float right[16];
                    if (Projection().TryPatchCombined(&g_shadowConstants[entry->startRegister][0],
                                                     left, right))
                    {
                        g_originalSetVertexShaderConstantF(
                            device, entry->startRegister,
                            eye == EyeLeft ? left : right, 4);
                    }
                }
            }

            // Rebuilt from whatever projection the game currently has set, not from a
            // cached one. The sky dome supplies its own projection with a far plane of
            // its own, and forcing every fixed-function draw through a single cached
            // matrix gave the sky the wrong depth range - which is what put it in front
            // of the world.
            if (g_haveFixedProjectionSource && !Cfg().useGameProjection)
            {
                float left[16];
                float right[16];
                if (Projection().TryPatch(g_currentFixedProjection, left, right))
                {
                    g_originalSetTransform(
                        device, D3DTS_PROJECTION,
                        reinterpret_cast<const D3DMATRIX*>(eye == EyeLeft ? left : right));
                }
            }

            // The scissor rectangle is in the game's screen space, which inside the
            // side-by-side target clips everything to its 1920x1080 corner. Rescale it
            // into whichever half is being drawn.
            // In the world pass the game's scissor rectangle is its entire screen, so
            // it clips nothing - but expressed in the game's coordinates it lands over
            // its own 1920x1080 corner of the wider stereo target and cuts everything
            // off at that edge. Clipping to the whole eye is the faithful translation,
            // and unlike scaling the game's rectangle it cannot land somewhere odd.
            Projection().SetInfiniteDistance(false);

            // Is the shadow map still bound for the second eye? Shadows are correct with
            // one eye and wrong with two while the cascade constants are identical, so
            // what differs has to be device state at the time of the second draw.
            // What is actually bound to the shadow sampler, and how big is it? A fixed,
            // zone-independent shadow pattern suggests the terrain is sampling something
            // that is not the shadow map at all, so identify it by size.
            if (Cfg().logShadowConstants && g_shadowSamplerLogged < 40)
            {
                IDirect3DBaseTexture9* shadowTex = nullptr;
                device->GetTexture(4, &shadowTex);
                if (shadowTex != nullptr)
                {
                    static IDirect3DBaseTexture9* seen[8] = {};
                    static int seenCount = 0;
                    bool known = false;
                    for (int i = 0; i < seenCount; ++i)
                    {
                        if (seen[i] == shadowTex) { known = true; break; }
                    }
                    if (!known && seenCount < 8)
                    {
                        seen[seenCount++] = shadowTex;
                        ++g_shadowSamplerLogged;
                        // GetType() avoids needing the IID symbol from d3d9 at link time.
                        if (shadowTex->GetType() == D3DRTYPE_TEXTURE)
                        {
                            IDirect3DTexture9* tex2d =
                                static_cast<IDirect3DTexture9*>(shadowTex);
                            D3DSURFACE_DESC d = {};
                            tex2d->GetLevelDesc(0, &d);
                            WOWVR_INFO("  sampler s4 texture %p: %ux%u fmt=%d usage=0x%lX "
                                       "levels=%u",
                                       static_cast<void*>(shadowTex), d.Width, d.Height,
                                       static_cast<int>(d.Format), d.Usage,
                                       tex2d->GetLevelCount());
                        }
                        else
                        {
                            WOWVR_INFO("  sampler s4 texture %p: resource type %d",
                                       static_cast<void*>(shadowTex),
                                       static_cast<int>(shadowTex->GetType()));
                        }
                    }
                    shadowTex->Release();
                }
            }

            if (g_scissorTestEnabled)
            {
                RECT scissor;
                scissor.left = static_cast<LONG>(viewport.X);
                scissor.top = 0;
                scissor.right = static_cast<LONG>(viewport.X + viewport.Width);
                scissor.bottom = static_cast<LONG>(viewport.Height);
                g_originalSetScissorRect(device, &scissor);
            }
        }

        // Chooses how the alpha channel should blend, given what the game asked for on
        // colour. An additive draw is recognised by its destination factor leaving the
        // existing pixel intact: those add light and must not claim any coverage.
        void ApplyUiAlphaBlend(IDirect3DDevice9* device)
        {
            if (!g_uiTargetBound || !Cfg().premultipliedUi)
            {
                return;
            }

            const bool additive = (g_destBlend == D3DBLEND_ONE);
            if (additive)
            {
                ++g_additiveUiDraws;
            }

            g_originalSetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
            g_originalSetRenderState(device, D3DRS_SRCBLENDALPHA,
                                     additive ? D3DBLEND_ZERO : D3DBLEND_ONE);
            g_originalSetRenderState(device, D3DRS_DESTBLENDALPHA,
                                     additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
            g_uiAlphaOverrideActive = true;
        }

        void EndUiAlphaOverride(IDirect3DDevice9* device)
        {
            if (!g_uiAlphaOverrideActive)
            {
                return;
            }
            g_originalSetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            g_uiAlphaOverrideActive = false;
        }

        HRESULT WINAPI HookedSetRenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value)
        {
            switch (state)
            {
            case D3DRS_ZENABLE:          g_depthTestEnabled = (value != D3DZB_FALSE); break;
            case D3DRS_ZWRITEENABLE:     g_depthWriteEnabled = (value != 0); break;
            case D3DRS_ALPHABLENDENABLE: g_alphaBlendEnabled = (value != 0); break;
            case D3DRS_ZFUNC:            g_depthFunction = value; break;
            case D3DRS_SCISSORTESTENABLE: g_scissorTestEnabled = (value != 0); break;
            case D3DRS_COLORWRITEENABLE: g_colorWriteMask = value; break;
            case D3DRS_ALPHATESTENABLE:  g_alphaTestEnabled = (value != 0); break;
            default: break;
            }

            // The separate-alpha states belong to us while the panel is bound. The
            // client is from an era that never touched them, but if it ever does, ours
            // has to win or the coverage channel goes wrong again.
            if (g_uiTargetBound && Cfg().premultipliedUi
                && (state == D3DRS_SEPARATEALPHABLENDENABLE
                    || state == D3DRS_SRCBLENDALPHA
                    || state == D3DRS_DESTBLENDALPHA))
            {
                return D3D_OK;
            }

            const HRESULT hr = g_originalSetRenderState(device, state, value);

            if (state == D3DRS_SRCBLEND || state == D3DRS_DESTBLEND)
            {
                if (state == D3DRS_SRCBLEND) { g_srcBlend = value; }
                else                         { g_destBlend = value; }
                ApplyUiAlphaBlend(device);
            }
            return hr;
        }

        // WoW never re-uploads its projection and never turns the depth test off, so
        // neither of those marks the start of the UI. Depth writes and alpha blending
        // are the next most likely tells for a 2D overlay pass.
        void NoteDrawForReport(int kind)
        {

            Report().NoteDraw(kind);

            const bool depthLike = g_depthTestEnabled && g_depthWriteEnabled
                                && g_depthFunction != D3DCMP_ALWAYS;
            Report().NoteDrawContext(g_renderingToBackBuffer, depthLike, g_alphaBlendEnabled,
                                     g_currentViewport.X, g_currentViewport.Width,
                                     ShouldDuplicateDraw(),
                                     g_currentViewport.MinZ, g_currentViewport.MaxZ);
        }

        HRESULT WINAPI HookedSetScissorRect(IDirect3DDevice9* device, const RECT* rect)
        {
            if (g_duplicatingDraw)
            {
                // Ours for the duration of the duplication.
                return D3D_OK;
            }

            if (rect != nullptr)
            {
                g_gameScissor = *rect;
                g_haveGameScissor = true;
            }
            return g_originalSetScissorRect(device, rect);
        }

        HRESULT WINAPI HookedSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport)
        {
            // While a draw is being duplicated the viewport belongs to us; the game's
            // own viewport changes are applied normally otherwise.
            if (g_duplicatingDraw)
            {
                return D3D_OK;
            }
            if (viewport != nullptr)
            {
                g_currentViewport = *viewport;
                g_gameViewport = *viewport;
                g_gameViewportMinZ = viewport->MinZ;
                g_gameViewportMaxZ = viewport->MaxZ;

                // Record the distinct depth ranges the game asks for. Carrying these
                // over fixed the backdrop, so it matters exactly how many there are and
                // which geometry each belongs to.
                if (Report().IsActive())
                {
                    bool known = false;
                    for (int i = 0; i < g_depthRangeCount; ++i)
                    {
                        if (g_depthRanges[i][0] == viewport->MinZ
                            && g_depthRanges[i][1] == viewport->MaxZ)
                        {
                            known = true;
                            break;
                        }
                    }
                    if (!known && g_depthRangeCount < 16)
                    {
                        g_depthRanges[g_depthRangeCount][0] = viewport->MinZ;
                        g_depthRanges[g_depthRangeCount][1] = viewport->MaxZ;
                        ++g_depthRangeCount;
                        WOWVR_INFO("depth range in use: MinZ=%.4f MaxZ=%.4f (viewport %ux%u at %u,%u)",
                                   viewport->MinZ, viewport->MaxZ,
                                   viewport->Width, viewport->Height, viewport->X, viewport->Y);
                    }
                }
            }
            return g_originalSetViewport(device, viewport);
        }

        // D3D9 clears the current viewport, not the whole surface. The game sets a
        // viewport the size of the back buffer it thinks it has, which covers only a
        // corner of the side-by-side target, so without this most of the stereo
        // target keeps last frame's contents and the two eyes disagree about
        // everything outside that rectangle.
        // Logged so the boundary between the shadow pass and the world pass can be
        // seen: the pass that owns the frame is the one that clears before drawing.
        void NoteClearForBoundary(DWORD flags)
        {
            if (Cfg().logWorldDrawTo > 0 && g_sequenceDumpArmed)
            {
                WOWVR_INFO("  >> Clear flags=0x%lX (after draw %d, backBuffer=%d)",
                           flags, g_worldDrawIndex, g_renderingToBackBuffer ? 1 : 0);
            }
        }

        HRESULT WINAPI HookedClear(IDirect3DDevice9* device, DWORD rectCount, const D3DRECT* rects,
                                   DWORD flags, D3DCOLOR colour, float z, DWORD stencil)
        {
            NoteClearForBoundary(flags);

            // The frame-start clear is the game's first act, before any draw, so the
            // redirect has to happen here rather than at the first draw. Otherwise the
            // clear lands on the real back buffer, the stereo depth buffer is never
            // cleared, and every world pixel fails the depth test - a black world with
            // a perfectly good UI floating in front of it.
            if (Cfg().stereo && g_stereo.IsReady() && Vr().IsActive()
                && !g_uiPassStarted && !g_stereoRedirected && g_renderingToBackBuffer
                && !g_duplicatingDraw)
            {
                g_originalSetRenderTarget(device, 0, g_stereo.Color());
                g_originalSetDepthStencilSurface(device, g_stereo.Depth());
                g_stereoRedirected = true;
                g_stereoHasContent = true;
                g_currentTargetIsStereo = true;
            }

            // Any rects the game supplies are deliberately discarded. They describe its
            // own 1920x1080 screen, which covers all of the left eye and only the first
            // third of the right one inside the side-by-side target - leaving a
            // brighter rectangle of cleared sky against stale content everywhere else.
            // Clearing more than asked is always safe; clearing less is not.
            // ONLY when our side-by-side surface is the bound colour target.
            //
            // This used to test StereoActive(), which merely asks whether the redirect is
            // in force - it says nothing about what is bound right now. So the client's
            // clear of its own 2048x2048 SHADOW MAP was given a 2960x1644 viewport. A
            // Clear is confined to the viewport, and that viewport then persisted into
            // the caster pass, so shadow casters were rendered through a 2960-wide
            // mapping into a 2048-wide target: stretched, clipped, and leaving regions
            // that were never written at all. The result was a shadow pattern with fixed
            // rectangular cutouts, identical in every zone, because its shape came from
            // these two surface sizes rather than from the world.
            if (StereoActive() && g_currentTargetIsStereo && !g_duplicatingDraw)
            {
                D3DVIEWPORT9 whole = {};
                whole.X = 0;
                whole.Y = 0;
                whole.Width = g_stereo.EyeWidth() * 2;
                whole.Height = g_stereo.EyeHeight();
                whole.MinZ = 0.0f;
                whole.MaxZ = 1.0f;
                g_originalSetViewport(device, &whole);

                return g_originalClear(device, 0, nullptr, flags, colour, z, stencil);
            }

            return g_originalClear(device, rectCount, rects, flags, colour, z, stencil);
        }

        // D3DCREATE_PUREDEVICE blocks most Get* state queries, but NOT these. If the
        // client asks what it is drawing into and gets our wide side-by-side surface
        // instead of its own 1920x1080 back buffer, anything it derives from those
        // dimensions is wrong. Hand back what it believes it bound.
        HRESULT WINAPI HookedGetRenderTarget(IDirect3DDevice9* device, DWORD index,
                                             IDirect3DSurface9** surface)
        {
            const HRESULT hr = g_originalGetRenderTarget(device, index, surface);
            if (SUCCEEDED(hr) && index == 0 && surface != nullptr && *surface != nullptr
                && g_stereoRedirected && g_stereo.IsReady()
                && *surface == g_stereo.Color() && g_realBackBuffer != nullptr)
            {
                (*surface)->Release();
                g_realBackBuffer->AddRef();
                *surface = g_realBackBuffer;
                ++g_renderTargetQueriesRedirected;
            }
            return hr;
        }

        HRESULT WINAPI HookedGetDepthStencilSurface(IDirect3DDevice9* device,
                                                    IDirect3DSurface9** surface)
        {
            const HRESULT hr = g_originalGetDepthStencilSurface(device, surface);
            if (SUCCEEDED(hr) && surface != nullptr && *surface != nullptr
                && g_stereo.IsReady() && *surface == g_stereo.Depth()
                && g_gameDepthSurface != nullptr)
            {
                (*surface)->Release();
                g_gameDepthSurface->AddRef();
                *surface = g_gameDepthSurface;
                ++g_renderTargetQueriesRedirected;
            }
            return hr;
        }

        HRESULT WINAPI HookedSetDepthStencilSurface(IDirect3DDevice9* device,
                                                    IDirect3DSurface9* surface)
        {
            // The game's own depth buffer is the size of its back buffer. When we have
            // moved rendering into the side-by-side target, that depth buffer is the
            // wrong shape, so ours is substituted.
            if (StereoActive() && surface != nullptr)
            {
                D3DSURFACE_DESC desc = {};
                if (SUCCEEDED(surface->GetDesc(&desc))
                    && desc.Width == g_backBufferWidth && desc.Height == g_backBufferHeight)
                {
                    // ONLY while the scene is actually going into our side-by-side
                    // target. The client also binds a back-buffer-sized depth surface for
                    // its shadow map; substituting ours there sent the casters' depth
                    // into the wrong buffer and left the shadow texture empty, which is
                    // why shadows worked in mono and vanished in stereo.
                    g_gameDepthSurface = surface;

                    // Only when the colour target ACTUALLY IS our side-by-side surface.
                    // Keying this off "are we rendering to the back buffer" was wrong:
                    // the client sets its depth surface BEFORE switching render target,
                    // so at that moment we still believed we were on the back buffer and
                    // handed it our depth buffer. Its shadow-caster pass then wrote depth
                    // into our surface instead of its own, leaving the shadow map holding
                    // whatever was there - a fixed, zone-independent pattern.
                    if (!g_currentTargetIsStereo)
                    {
                        ++g_depthSubstitutionDeclined;
                        return g_originalSetDepthStencilSurface(device, surface);
                    }

                    ++g_depthSubstituted;
                    return g_originalSetDepthStencilSurface(device, g_stereo.Depth());
                }
            }
            return g_originalSetDepthStencilSurface(device, surface);
        }


        HRESULT WINAPI HookedSetVertexShaderConstantF(IDirect3DDevice9* device, UINT startRegister,
                                                      const float* data, UINT vector4Count)
        {
            // These constants were computed from the client's camera, so the camera is
            // final for this frame; the first upload is the earliest provably-safe
            // moment to measure the compensation against it.
            EnsureFreshCameraCompensation();

            if (Report().IsActive())
            {
                Report().NoteVertexShaderConstants(startRegister, data, vector4Count);
            }

            // The terrain block (c12, 22 registers - see the frame report) leads with the
            // fog ramp.
            if (startRegister == 12 && vector4Count == 22 && data != nullptr && data[0] < 0.0f)
            {
                g_terrainFogEnd = -data[1] / data[0];
            }

            // The camera projection sits at the head of its upload, on both of the
            // paths the client uses (c2 transposed, c0 not). Rather than hard-coding
            // either register, every upload long enough to hold a matrix is offered to
            // the patch, which decides for itself whether it is looking at the scene
            // camera. Rejection is a couple of compares.
            const bool couldHoldAMatrix = Cfg().enabled
                && data != nullptr
                && vector4Count >= 4
                && vector4Count <= 256
                && g_renderingToBackBuffer;

            // Shadow what the game uploaded, before any substitution.
            if (data != nullptr && !g_duplicatingDraw)
            {
                if (vector4Count > g_maxVector4Count) { g_maxVector4Count = vector4Count; }
                if (startRegister + vector4Count > g_maxStartPlusCount)
                {
                    g_maxStartPlusCount = startRegister + vector4Count;
                }
                if (vector4Count > 1024) { ++g_oversizeUploads; }
                if (startRegister + vector4Count > kShadowRegisters) { ++g_beyondShadowMirror; }
            }

            if (data != nullptr && !g_duplicatingDraw
                && startRegister < kShadowRegisters)
            {
                const UINT count = (startRegister + vector4Count <= kShadowRegisters)
                    ? vector4Count : (kShadowRegisters - startRegister);
                memcpy(&g_shadowConstants[startRegister][0], data,
                       static_cast<size_t>(count) * 4 * sizeof(float));
            }

            if (Cfg().logWorldDrawTo > 0 && g_sequenceDumpArmed
                && g_worldDrawIndex >= Cfg().logWorldDrawFrom
                && g_worldDrawIndex <= Cfg().logWorldDrawTo
                && !g_duplicatingDraw)
            {
                WOWVR_INFO("    upload at draw %d: c%u..c%u (%u regs)",
                           g_worldDrawIndex, startRegister,
                           startRegister + vector4Count - 1, vector4Count);
            }

            // Widen the shadow cascade so its coverage extends past what the headset can
            // see. Scaling columns 0 and 1 of the light's orthographic projection zooms
            // it out about the NDC origin, which carries the translation terms in m30/m31
            // along with it - so the box grows without the centre drifting.
            static float widened[1024 * 4];
            if (g_renderingShadowMap && Cfg().shadowCoverageScale > 1.0f
                && !g_duplicatingDraw && data != nullptr
                && vector4Count >= 4 && vector4Count <= 1024)
            {
                const float* w = data;
                const bool orthographic = fabsf(w[15] - 1.0f) < 1.0e-3f
                                       && fabsf(w[11]) < 1.0e-3f
                                       && fabsf(w[0]) > 1.0e-6f && fabsf(w[5]) > 1.0e-6f;
                if (orthographic)
                {
                    memcpy(g_lightOrtho, data, sizeof(g_lightOrtho));
                    g_haveLightOrtho = true;
                    memcpy(widened, data,
                           static_cast<size_t>(vector4Count) * 4 * sizeof(float));
                    const float inverse = 1.0f / Cfg().shadowCoverageScale;
                    for (int row = 0; row < 4; ++row)
                    {
                        widened[row * 4 + 0] *= inverse;
                        widened[row * 4 + 1] *= inverse;
                    }
                    ++g_shadowCascadesWidened;
                    return g_originalSetVertexShaderConstantF(device, startRegister,
                                                              widened, vector4Count);
                }
            }

            // While the shadow map is being drawn, report any window that looks like a
            // projection. The light's own view-projection is what decides how much of the
            // world the shadow map covers, and widening it is the lever for the coverage
            // boundary sweeping through the VR view.
            if (g_renderingShadowMap && Cfg().logWorldDrawTo > 0 && !g_duplicatingDraw
                && data != nullptr && vector4Count >= 4)
            {
                for (UINT offset = 0; offset + 4 <= vector4Count; ++offset)
                {
                    const float* w = data + offset * 4;
                    const bool perspective = fabsf(w[15]) < 1.0e-3f && fabsf(w[11]) > 0.5f;
                    const bool orthographic = fabsf(w[15] - 1.0f) < 1.0e-3f
                                           && fabsf(w[11]) < 1.0e-3f
                                           && fabsf(w[0]) > 1.0e-6f && fabsf(w[5]) > 1.0e-6f;
                    if (perspective || orthographic)
                    {
                        WOWVR_INFO("  shadow-map upload c%u..c%u, %s at c%u: "
                                   "[%.5f %.5f] [%.5f %.5f] w=%.3f",
                                   startRegister, startRegister + vector4Count - 1,
                                   perspective ? "PERSPECTIVE" : "ORTHOGRAPHIC",
                                   startRegister + offset, w[0], w[5], w[10], w[14], w[15]);
                    }
                }
            }

            // Only while drawing the scene itself. Offscreen passes - the shadow map
            // above all - render from the light's point of view, not the viewer's, and
            // rewriting their transform with an eye camera renders the shadow map from
            // the wrong place entirely. That is why shadows vanished.
            if (couldHoldAMatrix && !g_duplicatingDraw && g_renderingToBackBuffer)
            {
                // Every four-register window is offered to the patch, not just the
                // first. Different shaders keep the camera at different offsets within
                // an upload, and anything missed here is drawn without the head
                // rotation - which reads as geometry welded to the viewer's face.
                // Sized generously and, crucially, BOUNDED. This previously held
                // 256 float4s and was filled with memcpy(patched, data, vector4Count...)
                // with no check on vector4Count, so any larger upload ran straight off
                // the end of it. That is what corrupted whatever followed in memory and
                // rendered models as flat silhouettes - worse with the client's extended
                // shadows on, because those uploads are bigger.
                const UINT kMaxPatchRegisters = 1024;
                static float patched[kMaxPatchRegisters * 4];
                bool anyPatched = false;

                if (vector4Count > kMaxPatchRegisters)
                {
                    return g_originalSetVertexShaderConstantF(device, startRegister,
                                                              data, vector4Count);
                }

                // The head of the upload is tested by shape, which is how the scene
                // camera gets discovered in the first place. Every other offset is
                // tested by *exact match* against that known matrix - WoW hands the
                // same camera to different shaders at different offsets, and a shape
                // test here matched thousands of unrelated constants per frame.
                for (UINT offset = 0; offset + 4 <= vector4Count; ++offset)
                {
                    const float* window = data + offset * 4;

                    // The shape test exists to BOOTSTRAP discovery of the scene camera
                    // and nothing else. Leaving it live afterwards meant any upload
                    // beginning with something merely perspective-shaped got rewritten
                    // with the eye projection: c0 and c10 were being overwritten
                    // thousands of times a frame, which is what rendered models as flat
                    // silhouettes. Whether such an upload appears at all depends on the
                    // scene - hence the maddening correlation with time of day.
                    //
                    // Once the camera is known, every window must match it exactly. The
                    // register it was discovered in is still allowed to be re-tested by
                    // shape so a genuine change of camera can still be picked up.
                    const bool bootstrapping = !g_haveSceneProjectionRaw;
                    const bool isPrimaryRegister =
                        (startRegister + offset) == g_primaryCameraRegister;

                    if (!bootstrapping && !isPrimaryRegister
                        && !MatricesMatch(window, g_sceneProjectionRaw))
                    {
                        continue;
                    }

                    float left[16];
                    float right[16];
                    if (!Projection().TryPatch(window, left, right))
                    {
                        continue;
                    }

                    if (!anyPatched)
                    {
                        memcpy(patched, data,
                               static_cast<size_t>(vector4Count) * 4 * sizeof(float));
                        anyPatched = true;
                    }

                    memcpy(patched + offset * 4, left, sizeof(left));
                    if (startRegister + offset < kShadowRegisters)
                    {
                        if (offset == 0) { ++g_substShape[startRegister]; }
                        else { ++g_substExact[startRegister + offset]; }
                    }
                    RememberPatchedBlock(startRegister + offset, window, left, right);

                    if (offset == 0)
                    {
                        g_primaryCameraRegister = startRegister;
                    }

                    // Remember the camera itself so every other register holding it can
                    // be recognised.
                    memcpy(g_sceneProjectionRaw, window, sizeof(g_sceneProjectionRaw));
                    g_haveSceneProjectionRaw = true;
                }

                // Drop a known camera register only when its *contents* have stopped
                // being the camera. Testing whether the upload merely covers the
                // register is wrong: WoW uploads a 46-register block from c0 that spans
                // c2 without disturbing the camera sitting there, and dropping on
                // coverage wiped the table every frame.
                if (g_haveSceneProjectionRaw)
                {
                    for (int i = g_patchedBlockCount - 1; i >= 0; --i)
                    {
                        const UINT reg = g_patchedBlocks[i].startRegister;
                        const bool covered = reg >= startRegister
                            && (reg + 4) <= (startRegister + vector4Count);
                        if (!covered || reg + 4 > kShadowRegisters)
                        {
                            continue;
                        }

                        if (g_patchedBlocks[i].combined)
                        {
                            continue;
                        }

                        if (!MatricesMatch(&g_shadowConstants[reg][0], g_sceneProjectionRaw))
                        {
                            if (Report().IsActive() || Cfg().logWorldDrawTo > 0)
                            {
                                WOWVR_INFO("  camera register c%u dropped at world draw %d: "
                                           "overwritten by an upload of %u regs from c%u",
                                           reg, g_worldDrawIndex, vector4Count, startRegister);
                            }
                            g_patchedBlocks[i] = g_patchedBlocks[g_patchedBlockCount - 1];
                            --g_patchedBlockCount;
                        }
                    }
                }

                // Dump the cascade transforms exactly as the client uploads them. Shadows are
            // correct in mono and wrong in stereo, so comparing these two logs says
            // whether the lookup's INPUT differs or whether the fault is downstream.
            if (Cfg().logShadowConstants && !g_duplicatingDraw && data != nullptr
                && !g_renderingShadowMap)
            {
                static int s_logged = 0;
                if (s_logged < 3 && startRegister <= 224
                    && startRegister + vector4Count >= 236)
                {
                    ++s_logged;
                    for (UINT reg = 224; reg <= 235; ++reg)
                    {
                        const float* v = data + (reg - startRegister) * 4;
                        WOWVR_INFO("  cascade c%u = %+.6f %+.6f %+.6f %+.6f",
                                   reg, v[0], v[1], v[2], v[3]);
                    }
                }
            }

            // The terrain shader's shadow-cascade transforms. Disassembling it showed the
            // shadow coordinates are built from the WORLD position, not the projection:
            //   oT5 = (c224, c225, c226, c233) . worldPos     cascade 0
            //   oT6 = (c227, c228, c229, c234) . worldPos     cascade 1
            //   oT7 = (c230, c231, c232, c235) . worldPos     cascade 2
            // Scaling the x and y rows of each widens the area a cascade covers, which
            // has to match whatever the shadow map was rendered with or casters are
            // written at one scale and sampled at another.
            if (Cfg().shadowCoverageScale > 1.0f && !g_renderingShadowMap
                && !g_duplicatingDraw && data != nullptr && vector4Count <= 1024)
            {
                static const UINT kCascadeXY[6] = { 224, 225, 227, 228, 230, 231 };
                const float inverse = 1.0f / Cfg().shadowCoverageScale;
                bool touched = false;

                for (int k = 0; k < 6; ++k)
                {
                    const UINT reg = kCascadeXY[k];
                    if (reg < startRegister || reg >= startRegister + vector4Count)
                    {
                        continue;
                    }

                    if (!anyPatched)
                    {
                        memcpy(patched, data,
                               static_cast<size_t>(vector4Count) * 4 * sizeof(float));
                        anyPatched = true;
                    }

                    const UINT offset = (reg - startRegister) * 4;
                    for (int component = 0; component < 4; ++component)
                    {
                        patched[offset + component] = data[offset + component] * inverse;
                    }
                    touched = true;
                }

                // The compare-depth rows. Their translation component is the bias: making
                // the fragment read as slightly nearer the light stops it shadowing
                // itself once each texel covers more ground.
                if (Cfg().shadowDepthBias != 0.0f)
                {
                    static const UINT kCascadeZ[3] = { 226, 229, 232 };
                    for (int k = 0; k < 3; ++k)
                    {
                        const UINT reg = kCascadeZ[k];
                        if (reg < startRegister || reg >= startRegister + vector4Count)
                        {
                            continue;
                        }
                        if (!anyPatched)
                        {
                            memcpy(patched, data,
                                   static_cast<size_t>(vector4Count) * 4 * sizeof(float));
                            anyPatched = true;
                        }
                        const UINT offset = (reg - startRegister) * 4;
                        patched[offset + 3] = data[offset + 3] - Cfg().shadowDepthBias;
                        touched = true;
                    }
                }

                if (touched) { ++g_shadowCascadeRowsScaled; }
            }

            // Any orthographic matrix appearing in the WORLD pass. If the terrain samples
            // the shadow map through a transform of its own, it has to be one of these.
            if (Cfg().logWorldDrawTo > 0 && !g_renderingShadowMap && !g_duplicatingDraw
                && data != nullptr && vector4Count >= 4 && g_renderingToBackBuffer)
            {
                for (UINT offset = 0; offset + 4 <= vector4Count; ++offset)
                {
                    const float* w = data + offset * 4;
                    const bool ortho = fabsf(w[15] - 1.0f) < 1.0e-3f
                                    && fabsf(w[3]) < 1.0e-3f && fabsf(w[7]) < 1.0e-3f
                                    && fabsf(w[11]) < 1.0e-3f
                                    && fabsf(w[0]) > 1.0e-6f && fabsf(w[5]) > 1.0e-6f;
                    if (ortho)
                    {
                        WOWVR_INFO("  world-pass ORTHO at c%u (upload c%u..c%u): "
                                   "m00=%.5f m11=%.5f m22=%.5f m30=%.3f m31=%.3f",
                                   startRegister + offset, startRegister,
                                   startRegister + vector4Count - 1,
                                   w[0], w[5], w[10], w[12], w[13]);
                    }
                }
            }

            // The same light transform, seen again during the world pass: this is how the
            // terrain samples the shadow map, and it must be scaled by exactly the same
            // factor as the render side or casters are written at one scale and looked
            // up at another.
            if (g_haveLightOrtho && Cfg().shadowCoverageScale > 1.0f && !g_renderingShadowMap
                && !g_duplicatingDraw && data != nullptr && vector4Count >= 4
                && vector4Count <= 1024)
            {
                for (UINT offset = 0; offset + 4 <= vector4Count; ++offset)
                {
                    if (!MatricesMatch(data + offset * 4, g_lightOrtho))
                    {
                        continue;
                    }

                    ++g_lightOrthoSeenInWorld;
                    if (!anyPatched)
                    {
                        memcpy(patched, data,
                               static_cast<size_t>(vector4Count) * 4 * sizeof(float));
                        anyPatched = true;
                    }

                    const float inverse = 1.0f / Cfg().shadowCoverageScale;
                    for (int row = 0; row < 4; ++row)
                    {
                        patched[offset * 4 + row * 4 + 0] =
                            g_lightOrtho[row * 4 + 0] * inverse;
                        patched[offset * 4 + row * 4 + 1] =
                            g_lightOrtho[row * 4 + 1] * inverse;
                    }
                }
            }

            // Matrices derived from the scene projection's inverse. These have to be
                // rebuilt against the projection we are about to substitute, or anything
                // that reconstructs position from depth reads the wrong place - which is
                // what made the entire shadow cascade render as shadowed.
                if (Cfg().patchInverseDerived)
                {
                    for (UINT offset = 0; offset + 4 <= vector4Count; ++offset)
                    {
                        float left[16];
                        float right[16];
                        if (!Projection().TryPatchInverseDerived(data + offset * 4, left, right))
                        {
                            continue;
                        }

                        if (!anyPatched)
                        {
                            memcpy(patched, data,
                                   static_cast<size_t>(vector4Count) * 4 * sizeof(float));
                            anyPatched = true;
                        }
                        memcpy(patched + offset * 4, left, sizeof(left));
                        RememberPatchedBlock(startRegister + offset, data + offset * 4,
                                             left, right, true);
                    }
                }

                // Candidate registers for a combined world-view-projection, tried on
                // every upload that covers them. No discovery step is needed because
                // TryPatchCombined validates itself: it only rewrites a window whose
                // residual against the known camera is genuinely affine, and returns
                // false otherwise. Probing for these proved unreliable - it mostly ran
                // before the world had loaded - while this cannot produce a false
                // positive. All sit below the bone palette at c31.
                // c4 only. c0, c8 and c12 were guesses and they wrecked models: those
                // registers do other work for other shaders, and forcing a transform
                // into them collapsed trees and made the player model vanish. c4 is the
                // one that was actually confirmed to carry terrain's transform.
                static const UINT kCombinedCandidates[] = { 4 };

                for (int k = 0; k < static_cast<int>(sizeof(kCombinedCandidates)
                                                     / sizeof(kCombinedCandidates[0])); ++k)
                {
                    const UINT reg = kCombinedCandidates[k];
                    if (reg < startRegister || reg + 4 > startRegister + vector4Count)
                    {
                        continue;
                    }

                    const UINT offset = reg - startRegister;
                    float left[16];
                    float right[16];
                    if (!Cfg().patchCombined
                        || !Projection().TryPatchCombined(data + offset * 4, left, right))
                    {
                        continue;
                    }

                    if (!anyPatched)
                    {
                        memcpy(patched, data,
                               static_cast<size_t>(vector4Count) * 4 * sizeof(float));
                        anyPatched = true;
                    }
                    memcpy(patched + offset * 4, left, sizeof(left));
                    if (reg < kShadowRegisters) { ++g_substCombined[reg]; }
                    RememberPatchedBlock(reg, data + offset * 4, left, right, true);
                }

                // UseGameProjection doubles as a bisect for this: with it set the game's
                // own constants go through untouched.
                if (anyPatched && !Cfg().useGameProjection && Cfg().patchConstants)
                {
                    g_projectionPatchedThisFrame = true;
                    return g_originalSetVertexShaderConstantF(device, startRegister, patched, vector4Count);
                }
            }

            return g_originalSetVertexShaderConstantF(device, startRegister, data, vector4Count);
        }

        // Writes a shader's bytecode out so it can be disassembled offline. The terrain
        // shader is the one that matters: knowing how it derives its shadow coordinate is
        // the only way left to find out what actually depends on the projection.
        void DumpShaderBytecode(const DWORD* function, IDirect3DVertexShader9* handle)
        {
            if (function == nullptr || !Cfg().dumpShaders)
            {
                return;
            }

            // The token stream ends with 0x0000FFFF.
            size_t words = 0;
            while (words < 65536 && function[words] != 0x0000FFFFu)
            {
                ++words;
            }
            ++words;

            wchar_t name[64];
            swprintf_s(name, L"WoWVR_vs_%p.bin", static_cast<void*>(handle));

            FILE* file = nullptr;
            if (_wfopen_s(&file, ModuleFile(name).c_str(), L"wb") == 0 && file != nullptr)
            {
                fwrite(function, sizeof(DWORD), words, file);
                fclose(file);
                WOWVR_INFO("Vertex shader %p bytecode written (%zu tokens).",
                           static_cast<void*>(handle), words);
            }
        }

        // ------------------------------------------------------------------
        // Resource pool census
        //
        // Exists to answer one architectural question: could the device be
        // silently upgraded to D3D9Ex - which would make shared surfaces legal
        // and the zero-copy presenter possible - without breaking the client?
        // D3DPOOL_MANAGED does not exist on an Ex device, so a non-zero managed
        // count from the game is the veto. The proxy's own creations (cursor
        // cache, eye targets) are excluded.
        // ------------------------------------------------------------------
        typedef HRESULT (WINAPI *CreateTextureFn)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD,
                                                  D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
        typedef HRESULT (WINAPI *CreateVertexBufferFn)(IDirect3DDevice9*, UINT, DWORD, DWORD,
                                                       D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
        typedef HRESULT (WINAPI *CreateIndexBufferFn)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT,
                                                      D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
        CreateTextureFn g_originalCreateTexture = nullptr;
        CreateVertexBufferFn g_originalCreateVertexBuffer = nullptr;
        CreateIndexBufferFn g_originalCreateIndexBuffer = nullptr;

        HRESULT WINAPI HookedCreateTexture(IDirect3DDevice9* device, UINT width, UINT height,
                                           UINT levels, DWORD usage, D3DFORMAT format,
                                           D3DPOOL pool, IDirect3DTexture9** texture,
                                           HANDLE* sharedHandle)
        {
            CountPool(CensusTexture, pool);
            const HRESULT hr = g_originalCreateTexture(device, width, height, levels, usage, format,
                                                       pool, texture, sharedHandle);
            return hr;
        }

        HRESULT WINAPI HookedCreateVertexBuffer(IDirect3DDevice9* device, UINT length, DWORD usage,
                                                DWORD fvf, D3DPOOL pool,
                                                IDirect3DVertexBuffer9** buffer, HANDLE* sharedHandle)
        {
            CountPool(CensusVertexBuffer, pool);
            return g_originalCreateVertexBuffer(device, length, usage, fvf, pool, buffer, sharedHandle);
        }

        HRESULT WINAPI HookedCreateIndexBuffer(IDirect3DDevice9* device, UINT length, DWORD usage,
                                               D3DFORMAT format, D3DPOOL pool,
                                               IDirect3DIndexBuffer9** buffer, HANDLE* sharedHandle)
        {
            CountPool(CensusIndexBuffer, pool);
            return g_originalCreateIndexBuffer(device, length, usage, format, pool, buffer, sharedHandle);
        }

        typedef HRESULT (WINAPI *CreatePixelShaderFn)(IDirect3DDevice9*, const DWORD*,
                                                      IDirect3DPixelShader9**);
        typedef HRESULT (WINAPI *SetPixelShaderFn)(IDirect3DDevice9*, IDirect3DPixelShader9*);
        CreatePixelShaderFn g_originalCreatePixelShader = nullptr;
        SetPixelShaderFn g_originalSetPixelShader = nullptr;

        void DumpPixelShaderBytecode(const DWORD* function, IDirect3DPixelShader9* handle)
        {
            if (function == nullptr || !Cfg().dumpShaders)
            {
                return;
            }

            size_t words = 0;
            while (words < 65536 && function[words] != 0x0000FFFFu) { ++words; }
            ++words;

            wchar_t name[64];
            swprintf_s(name, L"WoWVR_ps_%p.bin", static_cast<void*>(handle));

            FILE* file = nullptr;
            if (_wfopen_s(&file, ModuleFile(name).c_str(), L"wb") == 0 && file != nullptr)
            {
                fwrite(function, sizeof(DWORD), words, file);
                fclose(file);
            }
        }

        HRESULT WINAPI HookedCreatePixelShader(IDirect3DDevice9* device, const DWORD* function,
                                               IDirect3DPixelShader9** shader)
        {
            const HRESULT hr = g_originalCreatePixelShader(device, function, shader);
            if (SUCCEEDED(hr) && shader != nullptr && *shader != nullptr)
            {
                DumpPixelShaderBytecode(function, *shader);
            }
            return hr;
        }

        HRESULT WINAPI HookedSetPixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader)
        {
            if (!g_duplicatingDraw) { g_currentPixelShader = shader; }
            return g_originalSetPixelShader(device, shader);
        }

        HRESULT WINAPI HookedCreateVertexShader(IDirect3DDevice9* device, const DWORD* function,
                                                IDirect3DVertexShader9** shader)
        {
            Report().NoteVertexShaderCreated(0);

            // The world shaders get their fog measured by distance rather than depth
            // (stereo/fog_rewrite.h). Strictly additive: if the rewrite does not apply,
            // or the runtime refuses the result, the client's own bytecode is created
            // exactly as it would have been.
            if (Cfg().enabled && Cfg().vrEnabled && Cfg().radialFog && function != nullptr
                && shader != nullptr)
            {
                std::vector<uint32_t> rewritten;
                const FogRewriteResult fog =
                    RewriteFogToRadial(reinterpret_cast<const uint32_t*>(function), rewritten,
                                       Cfg().fogDistanceScale);
                if (fog.rewritten)
                {
                    const HRESULT rewrittenHr = g_originalCreateVertexShader(
                        device, reinterpret_cast<const DWORD*>(rewritten.data()), shader);
                    if (SUCCEEDED(rewrittenHr) && *shader != nullptr)
                    {
                        if (g_fogShadersRewritten++ == 0)
                        {
                            WOWVR_INFO("Radial fog: first world shader rewritten (view "
                                       "position r%u, distance in r%u, fog distance x%.2f).",
                                       fog.viewRegister, fog.scratchRegister,
                                       Cfg().fogDistanceScale);
                        }
                        DumpShaderBytecode(function, *shader);
                        return rewrittenHr;
                    }
                    if (g_fogRewriteRefused++ == 0)
                    {
                        WOWVR_WARN("Radial fog: the runtime refused a rewritten shader "
                                   "(0x%08lx); using the original for it.", rewrittenHr);
                    }
                }
                else
                {
                    ++g_fogShadersLeft;
                    g_fogLastSkipReason = fog.reason;
                }
            }

            const HRESULT hr = g_originalCreateVertexShader(device, function, shader);
            if (SUCCEEDED(hr) && shader != nullptr && *shader != nullptr)
            {
                DumpShaderBytecode(function, *shader);
            }
            return hr;
        }

        HRESULT WINAPI HookedSetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader)
        {
            if (!g_duplicatingDraw)
            {
                g_currentVertexShader = shader;
            }

            if (Report().IsActive())
            {
                Report().NoteVertexShaderSet(shader);
            }
            return g_originalSetVertexShader(device, shader);
        }

        HRESULT WINAPI HookedSetFVF(IDirect3DDevice9* device, DWORD fvf)
        {
            if (Report().IsActive())
            {
                Report().NoteFixedFunctionVertexPipeline();
            }
            g_vertexFormatIsPreTransformed = ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
            return g_originalSetFVF(device, fvf);
        }

        HRESULT WINAPI HookedSetVertexDeclaration(IDirect3DDevice9* device,
                                                  IDirect3DVertexDeclaration9* declaration)
        {
            g_vertexFormatIsPreTransformed = false;

            if (declaration != nullptr)
            {
                D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1] = {};
                UINT count = 0;
                if (SUCCEEDED(declaration->GetDeclaration(elements, &count)))
                {
                    for (UINT i = 0; i < count; ++i)
                    {
                        if (elements[i].Usage == D3DDECLUSAGE_POSITIONT)
                        {
                            g_vertexFormatIsPreTransformed = true;
                            break;
                        }
                    }
                }
            }

            return g_originalSetVertexDeclaration(device, declaration);
        }

        HRESULT WINAPI HookedSetRenderTarget(IDirect3DDevice9* device, DWORD index,
                                             IDirect3DSurface9* surface)
        {
            if (index == 0)
            {
                uint32_t width = 0;
                uint32_t height = 0;
                uint32_t format = 0;
                if (surface != nullptr)
                {
                    D3DSURFACE_DESC desc = {};
                    if (SUCCEEDED(surface->GetDesc(&desc)))
                    {
                        width = desc.Width;
                        height = desc.Height;
                        format = static_cast<uint32_t>(desc.Format);
                    }
                }

                // Tracked unconditionally, not just while reporting: the projection
                // patch uses it to stay off the shadow map and the post-process
                // passes, which reuse the same constant register as the scene camera.
                g_renderingToBackBuffer = (width == g_backBufferWidth && height == g_backBufferHeight);
                g_currentOffscreenTarget = -1;
                if (!g_renderingToBackBuffer && surface != nullptr)
                {
                    for (int i = 0; i < g_offscreenTargetCount; ++i)
                    {
                        if (g_offscreenTargets[i].surface == surface)
                        {
                            g_currentOffscreenTarget = i;
                            break;
                        }
                    }
                    if (g_currentOffscreenTarget < 0 && g_offscreenTargetCount < kOffscreenTargets)
                    {
                        OffscreenTarget& entry = g_offscreenTargets[g_offscreenTargetCount];
                        entry.surface = surface;
                        entry.width = width;
                        entry.height = height;
                        entry.format = format;
                        g_currentOffscreenTarget = g_offscreenTargetCount++;
                    }
                    if (g_currentOffscreenTarget >= 0)
                    {
                        ++g_offscreenTargets[g_currentOffscreenTarget].binds;
                    }
                }
                const bool nowShadow = (surface != nullptr && width == height && width >= 512);

                // Leaving the shadow map is the moment its contents are complete, so
                // that is when it is worth reading back.
                if (g_renderingShadowMap && !nowShadow && g_dumpShadowMapNext
                    && g_shadowSurface != nullptr)
                {
                    DumpShadowMap(device);
                }

                if (surface != g_realBackBuffer)
                {
                    g_currentTargetIsStereo = (g_stereo.IsReady() && surface == g_stereo.Color());
                }

                g_renderingShadowMap = nowShadow;
                if (nowShadow)
                {
                    g_shadowMapSize = width;
                    g_shadowSurface = surface;
                }

                if (Cfg().logWorldDrawTo > 0 && g_sequenceDumpArmed)
                {
                    WOWVR_INFO("  >> SetRenderTarget %ux%u fmt=%u %s (after draw %d)",
                               width, height, format,
                               (surface == g_realBackBuffer) ? "= REAL BACK BUFFER" : "(offscreen)",
                               g_worldDrawIndex);
                }

                if (Report().IsActive())
                {
                    Report().NoteRenderTarget(surface, width, height, format);
                }

                // Whenever the game aims at its back buffer, aim it at the
                // side-by-side target instead. The game carries on believing it is
                // drawing a single 1920x1080 view; only the viewport differs, and that
                // is set per eye at draw time.
                if (surface != nullptr && surface == g_realBackBuffer)
                {
                    if (Cfg().stereo && g_stereo.IsReady() && Vr().IsActive() && !g_uiPassStarted)
                    {
                        g_stereoRedirected = true;
                        g_stereoHasContent = true;
                        g_currentTargetIsStereo = true;
                        const HRESULT hr = g_originalSetRenderTarget(device, 0, g_stereo.Color());
                        g_originalSetDepthStencilSurface(device, g_stereo.Depth());
                        return hr;
                    }
                }
            }
            const HRESULT rtResult = g_originalSetRenderTarget(device, index, surface);
            if (FAILED(rtResult))
            {
                // Our depth surface is 2960x1644. D3D9 demands the depth surface be at
                // least as large as the colour target in BOTH dimensions, and the
                // client's shadow map is 2048x2048 - taller than ours. If that is what is
                // failing, the caster pass never reaches its own target and the shadow
                // map keeps whatever it already held: a fixed pattern, identical in every
                // zone.
                ++g_setRenderTargetFailures;
                if (g_setRenderTargetFailures < 6)
                {
                    D3DSURFACE_DESC d = {};
                    if (surface != nullptr) { surface->GetDesc(&d); }
                    WOWVR_WARN("SetRenderTarget FAILED (0x%08lx) for %ux%u fmt=%d",
                               rtResult, d.Width, d.Height, static_cast<int>(d.Format));
                }
            }
            return rtResult;
        }

        HRESULT WINAPI HookedDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                           UINT startVertex, UINT primitiveCount)
        {
            if (Report().IsActive())
            {
                NoteDrawForReport(FrameReport::DrawPrimitive);
            }

            if (g_renderingShadowMap && !g_duplicatingDraw)
            {
                ++g_shadowMapDraws;
                g_shadowMapPrims += primitiveCount;

                // Which shaders actually cast. If the character's shader never appears
                // here, the caster is missing and no lookup can produce its shadow.
                if (g_shadowCasterShaderCount < 16)
                {
                    bool known = false;
                    for (int i = 0; i < g_shadowCasterShaderCount; ++i)
                    {
                        if (g_shadowCasterShaders[i] == g_currentVertexShader) { known = true; break; }
                    }
                    if (!known)
                    {
                        g_shadowCasterShaders[g_shadowCasterShaderCount++] = g_currentVertexShader;
                        WOWVR_INFO("  shadow caster shader %p (prims=%u)",
                                   static_cast<void*>(g_currentVertexShader), primitiveCount);
                    }
                }
            }

            UpdateDrawPeriod(device);
            MaybeStartUiPass(device);

            if (ShouldSkipDraw())
            {
                return D3D_OK;
            }

            if (!ShouldDuplicateDraw())
            {
                return g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
            }

            g_duplicatingDraw = true;
            HRESULT hr = D3D_OK;
            const int eyeCount = (Cfg().singleEyeOnly || Cfg().fullTargetSingleEye) ? 1 : EyeCount;
            for (int eye = 0; eye < eyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
            EndEyes(device);
            NoteWorldDrawForSequence(device);
            ResolveCombinedRegisterForShader();
            return hr;
        }


        HRESULT WINAPI HookedDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                                  INT baseVertexIndex, UINT minVertexIndex,
                                                  UINT numVertices, UINT startIndex, UINT primitiveCount)
        {
            if (Report().IsActive())
            {
                NoteDrawForReport(FrameReport::DrawIndexedPrimitive);
            }

            if (g_renderingShadowMap && !g_duplicatingDraw)
            {
                ++g_shadowMapDraws;
                g_shadowMapPrims += primitiveCount;

                // Which shaders actually cast. If the character's shader never appears
                // here, the caster is missing and no lookup can produce its shadow.
                if (g_shadowCasterShaderCount < 16)
                {
                    bool known = false;
                    for (int i = 0; i < g_shadowCasterShaderCount; ++i)
                    {
                        if (g_shadowCasterShaders[i] == g_currentVertexShader) { known = true; break; }
                    }
                    if (!known)
                    {
                        g_shadowCasterShaders[g_shadowCasterShaderCount++] = g_currentVertexShader;
                        WOWVR_INFO("  shadow caster shader %p (prims=%u)",
                                   static_cast<void*>(g_currentVertexShader), primitiveCount);
                    }
                }
            }

            UpdateDrawPeriod(device);
            MaybeStartUiPass(device);

            if (ShouldSkipDraw())
            {
                return D3D_OK;
            }

            g_lastPrimitiveCount = primitiveCount;

            if (!ShouldDuplicateDraw())
            {
                return g_originalDrawIndexedPrimitive(device, type, baseVertexIndex, minVertexIndex,
                                            numVertices, startIndex, primitiveCount);
            }

            g_duplicatingDraw = true;
            HRESULT hr = D3D_OK;
            const int eyeCount = (Cfg().singleEyeOnly || Cfg().fullTargetSingleEye) ? 1 : EyeCount;
            for (int eye = 0; eye < eyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawIndexedPrimitive(
                    device, type, baseVertexIndex, minVertexIndex, numVertices,
                    startIndex, primitiveCount);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
            EndEyes(device);
            NoteWorldDrawForSequence(device);
            ResolveCombinedRegisterForShader();
            return hr;
        }

        HRESULT WINAPI HookedDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                             UINT primitiveCount, const void* vertexData, UINT stride)
        {
            if (Report().IsActive())
            {
                NoteDrawForReport(FrameReport::DrawPrimitiveUP);
            }

            if (g_renderingShadowMap && !g_duplicatingDraw)
            {
                ++g_shadowMapDraws;
                g_shadowMapPrims += primitiveCount;

                // Which shaders actually cast. If the character's shader never appears
                // here, the caster is missing and no lookup can produce its shadow.
                if (g_shadowCasterShaderCount < 16)
                {
                    bool known = false;
                    for (int i = 0; i < g_shadowCasterShaderCount; ++i)
                    {
                        if (g_shadowCasterShaders[i] == g_currentVertexShader) { known = true; break; }
                    }
                    if (!known)
                    {
                        g_shadowCasterShaders[g_shadowCasterShaderCount++] = g_currentVertexShader;
                        WOWVR_INFO("  shadow caster shader %p (prims=%u)",
                                   static_cast<void*>(g_currentVertexShader), primitiveCount);
                    }
                }
            }

            UpdateDrawPeriod(device);
            MaybeStartUiPass(device);

            if (ShouldSkipDraw())
            {
                return D3D_OK;
            }

            if (!ShouldDuplicateDraw())
            {
                return g_originalDrawPrimitiveUP(device, type, primitiveCount, vertexData, stride);
            }

            g_duplicatingDraw = true;
            HRESULT hr = D3D_OK;
            const int eyeCount = (Cfg().singleEyeOnly || Cfg().fullTargetSingleEye) ? 1 : EyeCount;
            for (int eye = 0; eye < eyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawPrimitiveUP(device, type, primitiveCount,
                                                                    vertexData, stride);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
            EndEyes(device);
            NoteWorldDrawForSequence(device);
            ResolveCombinedRegisterForShader();
            return hr;
        }

        HRESULT WINAPI HookedDrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                                    UINT minVertexIndex, UINT numVertices,
                                                    UINT primitiveCount, const void* indexData,
                                                    D3DFORMAT indexFormat, const void* vertexData,
                                                    UINT stride)
        {
            if (Report().IsActive())
            {
                NoteDrawForReport(FrameReport::DrawIndexedPrimitiveUP);
            }

            if (g_renderingShadowMap && !g_duplicatingDraw)
            {
                ++g_shadowMapDraws;
                g_shadowMapPrims += primitiveCount;

                // Which shaders actually cast. If the character's shader never appears
                // here, the caster is missing and no lookup can produce its shadow.
                if (g_shadowCasterShaderCount < 16)
                {
                    bool known = false;
                    for (int i = 0; i < g_shadowCasterShaderCount; ++i)
                    {
                        if (g_shadowCasterShaders[i] == g_currentVertexShader) { known = true; break; }
                    }
                    if (!known)
                    {
                        g_shadowCasterShaders[g_shadowCasterShaderCount++] = g_currentVertexShader;
                        WOWVR_INFO("  shadow caster shader %p (prims=%u)",
                                   static_cast<void*>(g_currentVertexShader), primitiveCount);
                    }
                }
            }

            UpdateDrawPeriod(device);
            MaybeStartUiPass(device);

            if (ShouldSkipDraw())
            {
                return D3D_OK;
            }

            if (!ShouldDuplicateDraw())
            {
                return g_originalDrawIndexedPrimitiveUP(device, type, minVertexIndex, numVertices,
                                                        primitiveCount, indexData, indexFormat,
                                                        vertexData, stride);
            }

            g_duplicatingDraw = true;
            HRESULT hr = D3D_OK;
            const int eyeCount = (Cfg().singleEyeOnly || Cfg().fullTargetSingleEye) ? 1 : EyeCount;
            for (int eye = 0; eye < eyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawIndexedPrimitiveUP(
                    device, type, minVertexIndex, numVertices, primitiveCount,
                    indexData, indexFormat, vertexData, stride);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
            EndEyes(device);
            NoteWorldDrawForSequence(device);
            ResolveCombinedRegisterForShader();
            return hr;
        }

        // Hooks one slot and stores the original, tolerating an already-patched slot.
        template <typename Fn>
        void HookSlot(IDirect3DDevice9* device, unsigned slotIndex, void* replacement,
                      Fn& originalOut, const char* name)
        {
            void* original = nullptr;
            if (HookVTableSlot(device, slotIndex, replacement, &original) && original != nullptr)
            {
                originalOut = reinterpret_cast<Fn>(original);
            }
            else
            {
                WOWVR_ERROR("Failed to hook IDirect3DDevice9::%s (slot %u).", name, slotIndex);
            }
        }

        void InstallDiagnosticHooks(IDirect3DDevice9* device)
        {
            HookSlot(device, slot::device9::SetTransform, &HookedSetTransform,
                     g_originalSetTransform, "SetTransform");
            HookSlot(device, slot::device9::SetVertexShaderConstantF, &HookedSetVertexShaderConstantF,
                     g_originalSetVertexShaderConstantF, "SetVertexShaderConstantF");
            HookSlot(device, slot::device9::CreatePixelShader, &HookedCreatePixelShader,
                     g_originalCreatePixelShader, "CreatePixelShader");
            HookSlot(device, slot::device9::SetPixelShader, &HookedSetPixelShader,
                     g_originalSetPixelShader, "SetPixelShader");
            HookSlot(device, slot::device9::CreateVertexShader, &HookedCreateVertexShader,
                     g_originalCreateVertexShader, "CreateVertexShader");
            HookSlot(device, slot::device9::SetVertexShader, &HookedSetVertexShader,
                     g_originalSetVertexShader, "SetVertexShader");
            HookSlot(device, slot::device9::SetFVF, &HookedSetFVF,
                     g_originalSetFVF, "SetFVF");
            HookSlot(device, slot::device9::SetVertexDeclaration, &HookedSetVertexDeclaration,
                     g_originalSetVertexDeclaration, "SetVertexDeclaration");
            HookSlot(device, slot::device9::SetRenderTarget, &HookedSetRenderTarget,
                     g_originalSetRenderTarget, "SetRenderTarget");
            HookSlot(device, slot::device9::GetRenderTarget, &HookedGetRenderTarget,
                     g_originalGetRenderTarget, "GetRenderTarget");
            HookSlot(device, slot::device9::GetDepthStencilSurface, &HookedGetDepthStencilSurface,
                     g_originalGetDepthStencilSurface, "GetDepthStencilSurface");
            HookSlot(device, slot::device9::SetDepthStencilSurface, &HookedSetDepthStencilSurface,
                     g_originalSetDepthStencilSurface, "SetDepthStencilSurface");
            HookSlot(device, slot::device9::SetViewport, &HookedSetViewport,
                     g_originalSetViewport, "SetViewport");
            HookSlot(device, slot::device9::SetScissorRect, &HookedSetScissorRect,
                     g_originalSetScissorRect, "SetScissorRect");
            HookSlot(device, slot::device9::Clear, &HookedClear,
                     g_originalClear, "Clear");
            HookSlot(device, slot::device9::SetRenderState, &HookedSetRenderState,
                     g_originalSetRenderState, "SetRenderState");
            HookSlot(device, slot::device9::DrawPrimitive, &HookedDrawPrimitive,
                     g_originalDrawPrimitive, "DrawPrimitive");
            HookSlot(device, slot::device9::DrawIndexedPrimitive, &HookedDrawIndexedPrimitive,
                     g_originalDrawIndexedPrimitive, "DrawIndexedPrimitive");
            HookSlot(device, slot::device9::DrawPrimitiveUP, &HookedDrawPrimitiveUP,
                     g_originalDrawPrimitiveUP, "DrawPrimitiveUP");
            HookSlot(device, slot::device9::DrawIndexedPrimitiveUP, &HookedDrawIndexedPrimitiveUP,
                     g_originalDrawIndexedPrimitiveUP, "DrawIndexedPrimitiveUP");
            HookSlot(device, slot::device9::CreateTexture, &HookedCreateTexture,
                     g_originalCreateTexture, "CreateTexture");
            HookSlot(device, slot::device9::CreateVertexBuffer, &HookedCreateVertexBuffer,
                     g_originalCreateVertexBuffer, "CreateVertexBuffer");
            HookSlot(device, slot::device9::CreateIndexBuffer, &HookedCreateIndexBuffer,
                     g_originalCreateIndexBuffer, "CreateIndexBuffer");
        }

        void InstallDeviceHooks(IDirect3DDevice9* device)
        {
            if (g_originalPresent != nullptr)
            {
                return;
            }

            void* original = nullptr;
            if (HookVTableSlot(device, slot::device9::Present, &HookedPresent, &original) && original != nullptr)
            {
                g_originalPresent = reinterpret_cast<PresentFn>(original);
            }

            original = nullptr;
            if (HookVTableSlot(device, slot::device9::Reset, &HookedReset, &original) && original != nullptr)
            {
                g_originalReset = reinterpret_cast<ResetFn>(original);
            }

            original = nullptr;
            if (HookVTableSlot(device, slot::device9::BeginScene, &HookedBeginScene, &original)
                && original != nullptr)
            {
                g_originalBeginScene = reinterpret_cast<BeginSceneFn>(original);
            }

            InstallDiagnosticHooks(device);

            WOWVR_INFO("Device hooks installed (Present=%s, Reset=%s, BeginScene=%s).",
                       g_originalPresent != nullptr ? "ok" : "FAILED",
                       g_originalReset != nullptr ? "ok" : "FAILED",
                       g_originalBeginScene != nullptr ? "ok" : "FAILED");

            if (Cfg().frameReportNumber > 0)
            {
                Report().RequestFrame(static_cast<unsigned long long>(Cfg().frameReportNumber));
                WOWVR_INFO("A frame report is armed for frame %d.", Cfg().frameReportNumber);
            }
        }

        HRESULT WINAPI HookedCreateDevice(IDirect3D9* d3d9, UINT adapter, D3DDEVTYPE deviceType,
                                          HWND focusWindow, DWORD behaviourFlags,
                                          D3DPRESENT_PARAMETERS* parameters,
                                          IDirect3DDevice9** returnedDevice)
        {
            // Before the device exists: the client tests for render-to-texture portraits
            // once, early, and remembers the answer.
            if (Cfg().enabled)
            {
                Portraits().Install();
            }
            const HRESULT hr = g_originalCreateDevice(d3d9, adapter, deviceType, focusWindow,
                                                      behaviourFlags, parameters, returnedDevice);

            // Needed to turn the desktop cursor position into panel coordinates.
            if (parameters != nullptr && parameters->hDeviceWindow != nullptr)
            {
                g_gameWindow = parameters->hDeviceWindow;
            }
            else if (focusWindow != nullptr)
            {
                g_gameWindow = focusWindow;
            }

            CacheSystemCursors();

            if (FAILED(hr) || returnedDevice == nullptr || *returnedDevice == nullptr)
            {
                WOWVR_WARN("CreateDevice failed (0x%08lx).", hr);
                return hr;
            }

            if (parameters != nullptr)
            {
                WOWVR_INFO("CreateDevice: %ux%u fmt=%d windowed=%d backBuffers=%u "
                           "depth=%d msaa=%d interval=0x%08lx flags=0x%08lx",
                           parameters->BackBufferWidth, parameters->BackBufferHeight,
                           static_cast<int>(parameters->BackBufferFormat),
                           parameters->Windowed ? 1 : 0,
                           parameters->BackBufferCount,
                           static_cast<int>(parameters->AutoDepthStencilFormat),
                           static_cast<int>(parameters->MultiSampleType),
                           parameters->PresentationInterval,
                           behaviourFlags);
            }

            if (g_device != nullptr && g_device != *returnedDevice)
            {
                WOWVR_INFO("A second D3D9 device appeared; following the newest one.");
                ReleaseFrameResources();
            }

            if (parameters != nullptr && parameters->BackBufferHeight > 0)
            {
                g_backBufferWidth = parameters->BackBufferWidth;
                g_backBufferFormat = parameters->BackBufferFormat;
                g_backBufferHeight = parameters->BackBufferHeight;
                Projection().SetSceneAspect(static_cast<float>(g_backBufferWidth)
                                          / static_cast<float>(g_backBufferHeight));
            }

            g_device = *returnedDevice;
            InstallDeviceHooks(g_device);
            return hr;
        }
    }

    void InstallD3D9Hooks(IDirect3D9* d3d9)
    {
        if (d3d9 == nullptr || g_originalCreateDevice != nullptr)
        {
            return;
        }

        void* original = nullptr;
        if (HookVTableSlot(d3d9, slot::d3d9::CreateDevice, &HookedCreateDevice, &original)
            && original != nullptr)
        {
            g_originalCreateDevice = reinterpret_cast<CreateDeviceFn>(original);
            WOWVR_INFO("IDirect3D9::CreateDevice hooked.");
        }
        else
        {
            WOWVR_ERROR("Could not hook IDirect3D9::CreateDevice; WoWVR will stay dormant.");
        }
    }

    void ShutdownRenderer()
    {
        // Say plainly what this session captured, so the files can be found afterwards
        // without hunting through the folder by timestamp.
        if (g_stereoDumpIndex > 0)
        {
            WOWVR_INFO("Captured %d screenshot%s this session: WoWVR_shot_%S_00.bmp "
                       "through _%02d.bmp, listed in WoWVR_shots_%S.txt",
                       g_stereoDumpIndex, g_stereoDumpIndex == 1 ? "" : "s",
                       SessionStamp(), g_stereoDumpIndex - 1, SessionStamp());
        }
        else
        {
            WOWVR_INFO("No screenshots were captured this session.");
        }

        ReleaseFrameResources();
        // Not released with the frame resources: these are managed-pool and survive a
        // device reset, so they only need to go when the device itself does.
        ReleaseCursorTextures();

        // Never leave the pointer trapped, whatever else went wrong on the way out.
        if (g_cursorConfined)
        {
            ClipCursor(nullptr);
            g_cursorConfined = false;
        }

        g_d3d12Present.Shutdown();
        g_glInterop.Shutdown();
        g_presenter.Shutdown();
        Vr().Shutdown();
        g_device = nullptr;
    }
}
