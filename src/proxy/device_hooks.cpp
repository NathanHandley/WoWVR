#include "proxy/device_hooks.h"

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/image_dump.h"
#include "core/log.h"
#include "core/paths.h"
#include "diag/frame_report.h"
#include "game/camera_probe.h"
#include "present/presenter.h"
#include "proxy/d3d9_slots.h"
#include "proxy/vtable_hook.h"
#include "render/eye_targets.h"
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
        unsigned long long g_panelDrawn = 0;
        unsigned long long g_panelSkipped = 0;
        const char* g_lastSkipReason = "none";

        // Draw routing counters, summed over the reporting interval.
        unsigned long long g_drawsToStereo = 0;
        unsigned long long g_drawsToUi = 0;
        unsigned long long g_drawsOffscreen = 0;
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
        const int kCursorCacheSize = 32;
        CursorArt g_cursorCache[kCursorCacheSize];
        int g_cursorCacheCount = 0;
        bool g_cursorCacheFullLogged = false;
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

        // Whether the pointer is currently confined to the game window, so ClipCursor
        // is only called when the answer actually changes.
        bool g_cursorConfined = false;
        RECT g_confinedTo = {};

        bool GameHasFocus()
        {
            return g_gameWindow != nullptr && GetForegroundWindow() == g_gameWindow;
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
        LARGE_INTEGER g_qpcFrequency = {};

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
            Projection().LogLastDecision();
            WOWVR_INFO("UI panel: composited %llu frames, skipped %llu (last reason: %s)",
                       g_panelDrawn, g_panelSkipped, g_lastSkipReason);
            WOWVR_INFO("Draw routing: stereo %llu, ui %llu, offscreen %llu, "
                       "STRAY-to-backbuffer %llu; periods last frame %d; "
                       "sky-slice eye draws %llu",
                       g_drawsToStereo, g_drawsToUi, g_drawsOffscreen,
                       g_drawsStrayBackBuffer, g_lastPeriodCount, g_skySliceDraws);
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

            WOWVR_INFO("Cursor: window %p, %d shape(s) cached, current %s, drawn %llu, "
                       "last position %.3f, %.3f",
                       static_cast<void*>(g_gameWindow), g_cursorCacheCount,
                       g_activeCursor != nullptr ? "shown" : "hidden",
                       g_cursorDrawn, g_lastCursorU, g_lastCursorV);
            WOWVR_INFO("UI panel compositing: %s, %llu additive interface draws given a "
                       "no-coverage alpha blend",
                       Cfg().premultipliedUi ? "premultiplied" : "straight alpha",
                       g_additiveUiDraws);
            WOWVR_INFO("Cursor: %llu stale-pointer substitutions after focus changes, "
                       "last game art %p, focus %s, confined %s",
                       g_staleCursorSubstitutions, static_cast<void*>(g_lastGameCursor),
                       GameHasFocus() ? "held" : "elsewhere",
                       g_cursorConfined ? "yes" : "no");
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
            g_drawsStrayBackBuffer = 0;
        }

        void ReleaseFrameResources()
        {
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                g_eyeTargets[eye].Destroy();
            }
            g_stereo.Destroy();
            g_uiPanel.Destroy();

            if (g_realBackBuffer != nullptr)
            {
                g_realBackBuffer->Release();
                g_realBackBuffer = nullptr;
            }

            g_stereoRedirected = false;
            g_haveEyeProjections = false;
            g_resourcesReady = false;
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

            bool targetsReady = true;
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                targetsReady = g_eyeTargets[eye].Create(device, width, height) && targetsReady;
            }

            const bool presenterReady = g_presenter.IsReady()
                ? (g_presenter.Width() == width && g_presenter.Height() == height)
                : g_presenter.Init(Vr().PreferredAdapterIndex(), width, height);

            // The side-by-side target the game will be redirected into, plus a
            // reference to the real back buffer so the redirect can recognise it.
            bool stereoReady = true;
            if (Cfg().stereo)
            {
                g_stereo.SetBackBufferFormat(static_cast<uint32_t>(g_backBufferFormat));
                stereoReady = g_stereo.Create(device, width, height);
                stereoReady = g_uiPanel.Create(device, g_backBufferWidth, g_backBufferHeight)
                              && stereoReady;

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

        // Draws the interface texture as a quad in front of the viewer, once per eye,
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

            // Only when it changes: the window can be moved, and re-clipping every
            // frame regardless would be a syscall per frame for nothing.
            if (g_cursorConfined
                && screen.left == g_confinedTo.left && screen.top == g_confinedTo.top
                && screen.right == g_confinedTo.right && screen.bottom == g_confinedTo.bottom)
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
            if (FAILED(device->CreateTexture(static_cast<UINT>(width), static_cast<UINT>(height),
                                             1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                             &texture, nullptr))
                || texture == nullptr)
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

            // A hidden pointer stays hidden: that is the game suppressing it while you
            // hold the right button to look around, and it must not be second-guessed.
            HCURSOR shape = nullptr;
            if (haveGlobal && (cursorInfo.flags & CURSOR_SHOWING) != 0)
            {
                shape = cursorInfo.hCursor;
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
                               "lastGameArt=%p flags=0x%lx substitutions=%llu",
                               focus ? "yes" : "no", static_cast<void*>(cursorInfo.hCursor),
                               IsSystemCursor(cursorInfo.hCursor) ? "standard" : "game art",
                               static_cast<void*>(shape),
                               static_cast<void*>(g_lastGameCursor),
                               cursorInfo.flags, g_staleCursorSubstitutions);
                }
            }

            if (shape == nullptr)
            {
                return nullptr;
            }

            for (int i = 0; i < g_cursorCacheCount; ++i)
            {
                if (g_cursorCache[i].source == shape)
                {
                    // A cached entry with no texture is a shape that already failed to
                    // build; remembering that is what stops it being retried per frame.
                    return (g_cursorCache[i].texture != nullptr) ? &g_cursorCache[i] : nullptr;
                }
            }

            if (g_cursorCacheCount >= kCursorCacheSize)
            {
                if (!g_cursorCacheFullLogged)
                {
                    g_cursorCacheFullLogged = true;
                    WOWVR_WARN("Saw more than %d cursor shapes; the pointer will keep the "
                               "last one it managed to cache.", kCursorCacheSize);
                }
                return g_activeCursor;
            }

            CursorArt art;
            const bool built = BuildCursorArt(device, shape, art);
            if (!built)
            {
                art.source = shape;
                art.texture = nullptr;
            }
            g_cursorCache[g_cursorCacheCount] = art;
            ++g_cursorCacheCount;

            if (!built)
            {
                WOWVR_WARN("Could not read cursor shape %p; it will not be drawn.", shape);
                return nullptr;
            }

            WOWVR_INFO("Cached cursor shape %d: %dx%d, hotspot %d,%d.", g_cursorCacheCount,
                       art.width, art.height, art.hotspotX, art.hotspotY);
            return &g_cursorCache[g_cursorCacheCount - 1];
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
            g_cursorCacheFullLogged = false;
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

        void CompositeUiPanel(IDirect3DDevice9* device)
        {
            // The interface pass is over either way, so the alpha override has to come
            // off before anything else is drawn - including this composite.
            EndUiAlphaOverride(device);
            g_uiTargetBound = false;

            if (!g_uiRendered || !g_uiPanel.IsReady() || !g_stereoHasContent)
            {
                ++g_panelSkipped;
                g_lastSkipReason = !g_uiPanel.IsReady() ? "panel target missing"
                                 : !g_stereoHasContent ? "no stereo content"
                                 : "UI pass never started";
                return;
            }
            ++g_panelDrawn;

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

            const float width = Cfg().panelWidth;
            const float height = width * static_cast<float>(g_uiPanel.Height())
                                       / static_cast<float>(g_uiPanel.Width());
            const float halfWidth = width * 0.5f;
            const float halfHeight = height * 0.5f;

            const PanelVertex quad[4] = {
                { -halfWidth,  halfHeight, 0.0f, 0.0f, 0.0f },
                {  halfWidth,  halfHeight, 0.0f, 1.0f, 0.0f },
                { -halfWidth, -halfHeight, 0.0f, 0.0f, 1.0f },
                {  halfWidth, -halfHeight, 0.0f, 1.0f, 1.0f },
            };

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

                g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(PanelVertex));

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
                    const float scale = Cfg().cursorScale;
                    const float pixel = (width / clientWidth) * scale;
                    const float artWidth = g_activeCursor->width * pixel;
                    const float artHeight = g_activeCursor->height * pixel;

                    // Shifted by the hotspot so the part of the image that does the
                    // pointing lands on the pixel being pointed at, rather than the
                    // image's top-left corner. Panel Y runs up, image Y runs down.
                    const float left = -halfWidth + u * width - g_activeCursor->hotspotX * pixel;
                    const float top = halfHeight - v * height + g_activeCursor->hotspotY * pixel;

                    const PanelVertex pointer[4] = {
                        { left,            top,             0.0f, 0.0f, 0.0f },
                        { left + artWidth, top,             0.0f, 1.0f, 0.0f },
                        { left,            top - artHeight, 0.0f, 0.0f, 1.0f },
                        { left + artWidth, top - artHeight, 0.0f, 1.0f, 1.0f },
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

            const LARGE_INTEGER captureStart = Now();

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

            const LARGE_INTEGER uploadStart = Now();

            for (int eye = 0; eye < EyeCount; ++eye)
            {
                if (!stereo && eye == EyeRight)
                {
                    break;
                }

                if (!g_eyeTargets[eye].Lock())
                {
                    continue;
                }

                g_presenter.Upload(eye, g_eyeTargets[eye].LockedPixels(), g_eyeTargets[eye].LockedPitch());

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
                }

                g_eyeTargets[eye].Unlock();
            }

            const LARGE_INTEGER submitStart = Now();
            // Without stereo both eyes get the left image, which is flat but at least
            // consistent; with stereo each eye gets its own render.
            void* leftTexture = g_presenter.EyeTexture(EyeLeft);
            Vr().SubmitEye(EyeLeft, leftTexture);
            Vr().SubmitEye(EyeRight, stereo ? g_presenter.EyeTexture(EyeRight) : leftTexture);
            Vr().PostSubmit();
            const LARGE_INTEGER submitEnd = Now();

            g_timers.captureMs += ElapsedMs(captureStart, uploadStart);
            g_timers.uploadMs += ElapsedMs(uploadStart, submitStart);
            g_timers.submitMs += ElapsedMs(submitStart, submitEnd);
            ++g_timers.samples;
        }

        // Puts the device back on its real back buffer so that Present, and anything
        // the game does before the next frame starts, behave normally. Also mirrors
        // the left eye to the desktop window so the game is still watchable on screen.
        void FinishStereoFrame(IDirect3DDevice9* device)
        {
            if (!g_stereoRedirected || g_realBackBuffer == nullptr)
            {
                return;
            }

            if (Cfg().desktopMirror)
            {
                RECT half;
                half.left = 0;
                half.top = 0;
                half.right = static_cast<LONG>(g_stereo.EyeWidth());
                half.bottom = static_cast<LONG>(g_stereo.EyeHeight());
                device->StretchRect(g_stereo.Color(), &half, g_realBackBuffer, nullptr, D3DTEXF_LINEAR);
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
                WOWVR_INFO("F10: eye buffers will be written on the next frame%s.",
                           g_armSequenceNextFrame ? ", with a mid-frame draw sequence" : "");
            }

            if (HotkeyPressed(Hotkey::Recenter))
            {
                Projection().Recenter();
                g_uiPanel.Recenter(0.0f);
                WOWVR_INFO("F11: recentre requested.");
            }

            if (HotkeyPressed(Hotkey::ReloadConfig))
            {
                LoadConfig();
                LogSetLevel(Cfg().logLevel);
                WOWVR_INFO("F12: WoWVR.ini reloaded.");
            }
        }

        HRESULT WINAPI HookedPresent(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
                                     HWND windowOverride, const RGNDATA* dirtyRegion)
        {
            // Closes the observation window on the frame the game has just finished.
            Report().EndFrame();

            PollHotkeys();

            if (Cfg().enabled)
            {
                EnsureVrResources(device);
                CompositeUiPanel(device);
                SubmitFrame(device);
                FinishStereoFrame(device);
            }

            const HRESULT hr = g_originalPresent(device, source, destination, windowOverride, dirtyRegion);

            // WaitGetPoses blocks until the compositor wants the next frame, which is
            // also what paces the game to the headset's refresh rate.
            if (Cfg().enabled && Vr().IsActive())
            {
                const LARGE_INTEGER waitStart = Now();
                Vr().WaitForFrame();
                g_timers.waitMs += ElapsedMs(waitStart, Now());

                // Recentre the moment the headset is first actually worn. Until then
                // "forward" would be measured from wherever it happened to be lying.
                const bool userPresent = Vr().UserIsPresent();
                if (userPresent && !g_userWasPresent)
                {
                    Projection().Recenter();
                    WOWVR_INFO("Headset picked up; recentring on the current head pose.");
                }
                g_userWasPresent = userPresent;

                // Fresh pose for the frame the game is about to build.
                if (Vr().HasHeadPose())
                {
                    Projection().UpdateFromHeadPose(Vr().HeadToStage());
                    g_uiPanel.Update(Projection().HeadYaw(), 1.0f / Vr().DisplayFrequency());

                    FindAllCameraRegisters();
                    RefreshPatchedBlocks();

                }
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

            if (!g_renderingToBackBuffer)      { ++g_drawsOffscreen; }
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

            // The sky dome is squashed into the very top of the depth range, above the
            // distant backdrop terrain at 0.998..0.999. That slice is the reliable way
            // to recognise it: it needs the head's rotation but neither the head's
            // displacement nor the per-eye offset, or it stops reading as a horizon.
            const bool skyDepthSlice = viewport.MinZ >= 0.9985f;
            if (skyDepthSlice) { ++g_skySliceDraws; }
            Projection().SetInfiniteDistance(skyDepthSlice);

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
            if (Report().IsActive())
            {
                Report().NoteVertexShaderConstants(startRegister, data, vector4Count);
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

            InstallDiagnosticHooks(device);

            WOWVR_INFO("Device hooks installed (Present=%s, Reset=%s).",
                       g_originalPresent != nullptr ? "ok" : "FAILED",
                       g_originalReset != nullptr ? "ok" : "FAILED");

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

        g_presenter.Shutdown();
        Vr().Shutdown();
        g_device = nullptr;
    }
}
