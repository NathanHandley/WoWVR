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
        ClearFn g_originalClear = nullptr;
        SetRenderStateFn g_originalSetRenderState = nullptr;

        // The device is PUREDEVICE, so GetRenderState is unavailable and anything we
        // want to know about device state has to be tracked as it is set.
        bool g_depthTestEnabled = true;
        bool g_depthWriteEnabled = true;
        bool g_alphaBlendEnabled = false;
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

            if (Report().IsActive())
            {
                WOWVR_INFO("  camera register c%u registered (table now %d)",
                           startRegister, g_patchedBlockCount);
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

        // The game uses the hardware cursor, which never reaches the render target, so
        // in the headset there is nothing to aim with. This draws one onto the panel.
        HWND g_gameWindow = nullptr;
        IDirect3DTexture9* g_cursorTexture = nullptr;
        bool g_cursorTextureFailed = false;
        unsigned long long g_cursorDrawn = 0;
        float g_lastCursorU = -1.0f;
        float g_lastCursorV = -1.0f;

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
            WOWVR_INFO("Cursor: window %p, texture %s, drawn %llu, last position %.3f, %.3f",
                       static_cast<void*>(g_gameWindow),
                       g_cursorTexture != nullptr ? "ok" : "MISSING",
                       g_cursorDrawn, g_lastCursorU, g_lastCursorV);
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
        // A plain arrow, built here rather than shipped as a file. 'X' is the outline,
        // '.' the fill, everything else transparent.
        void EnsureCursorTexture(IDirect3DDevice9* device)
        {
            if (g_cursorTexture != nullptr || g_cursorTextureFailed)
            {
                return;
            }

            static const char* kArrow[16] = {
                "X               ",
                "XX              ",
                "X.X             ",
                "X..X            ",
                "X...X           ",
                "X....X          ",
                "X.....X         ",
                "X......X        ",
                "X.......X       ",
                "X........X      ",
                "X.....XXXXX     ",
                "X..X..X         ",
                "X.X X..X        ",
                "XX  X..X        ",
                "X    X..X       ",
                "      XX        ",
            };

            // Managed pool so it survives a device reset without needing to be rebuilt.
            if (FAILED(device->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                             &g_cursorTexture, nullptr))
                || g_cursorTexture == nullptr)
            {
                g_cursorTextureFailed = true;
                WOWVR_WARN("Could not create the cursor texture; the pointer will not be drawn.");
                return;
            }

            D3DLOCKED_RECT locked = {};
            if (FAILED(g_cursorTexture->LockRect(0, &locked, nullptr, 0)))
            {
                g_cursorTexture->Release();
                g_cursorTexture = nullptr;
                g_cursorTextureFailed = true;
                return;
            }

            for (int y = 0; y < 16; ++y)
            {
                uint32_t* row = reinterpret_cast<uint32_t*>(
                    static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
                for (int x = 0; x < 16; ++x)
                {
                    const char pixel = kArrow[y][x];
                    row[x] = (pixel == 'X') ? 0xFF000000u
                           : (pixel == '.') ? 0xFFFFFFFFu
                           : 0x00000000u;
                }
            }
            g_cursorTexture->UnlockRect(0);
        }

        // Where the pointer sits inside the game window, 0..1. False when it is outside
        // or the window is not known yet.
        bool CursorPanelPosition(float& u, float& v)
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

            u = static_cast<float>(point.x) / static_cast<float>(client.right - client.left);
            v = static_cast<float>(point.y) / static_cast<float>(client.bottom - client.top);
            return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
        }

        void CompositeUiPanel(IDirect3DDevice9* device)
        {
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

            EnsureCursorTexture(device);

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
            g_originalSetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
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
                const bool havePointer = CursorPanelPosition(u, v);
                g_lastCursorU = havePointer ? u : -1.0f;
                g_lastCursorV = havePointer ? v : -1.0f;
                if (g_cursorTexture != nullptr && havePointer)
                {
                    ++g_cursorDrawn;
                    const float size = width * 0.030f;
                    const float left = -halfWidth + u * width;
                    const float top = halfHeight - v * height;

                    const PanelVertex pointer[4] = {
                        { left,        top,        0.0f, 0.0f, 0.0f },
                        { left + size, top,        0.0f, 1.0f, 0.0f },
                        { left,        top - size, 0.0f, 0.0f, 1.0f },
                        { left + size, top - size, 0.0f, 1.0f, 1.0f },
                    };

                    device->SetTexture(0, g_cursorTexture);
                    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                    g_originalDrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, pointer,
                                              sizeof(PanelVertex));
                    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
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
                    // Numbered as well as fixed-name, so a run can take a series of
                    // shots without each one overwriting the last.
                    wchar_t numbered[64];
                    swprintf_s(numbered, L"WoWVR_stereo_%02d.bmp", g_stereoDumpIndex);
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
                && Cfg().enabled && g_renderingToBackBuffer && !g_uiPassStarted)
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

                IDirect3DVertexShader9* shader = nullptr;
                device->GetVertexShader(&shader);

                // The decisive columns are the last two: whether this draw actually had a
                // head-rotated camera put in front of it on either path. Geometry that
                // gets neither is drawn with the game's own camera and ends up welded
                // to the viewer's head.
                WOWVR_INFO("world draw %-4d: prims=%-6u tex=%p vs=%p blend=%s ztest=%s "
                           "depth=%.3f..%.3f  camShader=%d camFixed=%d cwrite=%X atest=%d "
                           "ortho=%d",
                           g_worldDrawIndex, g_lastPrimitiveCount,
                           static_cast<void*>(texture), static_cast<void*>(shader),
                           g_alphaBlendEnabled ? "on" : "off",
                           g_depthTestEnabled ? "on" : "off",
                           g_currentViewport.MinZ, g_currentViewport.MaxZ,
                           g_patchedBlockCount, g_haveFixedProjectionSource ? 1 : 0,
                           g_colorWriteMask, g_alphaTestEnabled ? 1 : 0,
                           g_fixedProjectionIsOrtho ? 1 : 0);

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
            g_originalSetViewport(device, &viewport);

            // Only the scene camera gets swapped. UI and other ortho passes never had
            // a patched projection, so they are simply drawn into each half as-is.
            for (int i = 0; i < g_patchedBlockCount; ++i)
            {
                const PatchedBlock& block = g_patchedBlocks[i];

                // A combined transform belongs to one object. Re-applying it to every
                // draw overwrites whatever else that register is being used for, so it
                // is only restored while the register still holds the matrix it was
                // derived from.
                if (block.combined && block.startRegister + 4 <= kShadowRegisters
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
            if (Cfg().perShaderCombined && g_currentVertexShader != nullptr)
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
            if (g_haveFixedProjectionSource)
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
            return g_originalSetRenderState(device, state, value);
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
        HRESULT WINAPI HookedClear(IDirect3DDevice9* device, DWORD rectCount, const D3DRECT* rects,
                                   DWORD flags, D3DCOLOR colour, float z, DWORD stencil)
        {
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
            }

            // Any rects the game supplies are deliberately discarded. They describe its
            // own 1920x1080 screen, which covers all of the left eye and only the first
            // third of the right one inside the side-by-side target - leaving a
            // brighter rectangle of cleared sky against stale content everywhere else.
            // Clearing more than asked is always safe; clearing less is not.
            if (StereoActive() && !g_duplicatingDraw)
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
                static float patched[256 * 4];
                bool anyPatched = false;

                // The head of the upload is tested by shape, which is how the scene
                // camera gets discovered in the first place. Every other offset is
                // tested by *exact match* against that known matrix - WoW hands the
                // same camera to different shaders at different offsets, and a shape
                // test here matched thousands of unrelated constants per frame.
                for (UINT offset = 0; offset + 4 <= vector4Count; ++offset)
                {
                    const float* window = data + offset * 4;

                    if (offset != 0)
                    {
                        if (!g_haveSceneProjectionRaw
                            || !MatricesMatch(window, g_sceneProjectionRaw))
                        {
                            continue;
                        }
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
                    RememberPatchedBlock(startRegister + offset, window, left, right);

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
                    if (!Projection().TryPatchCombined(data + offset * 4, left, right))
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
                    RememberPatchedBlock(reg, data + offset * 4, left, right, true);
                }

                if (anyPatched)
                {
                    g_projectionPatchedThisFrame = true;
                    return g_originalSetVertexShaderConstantF(device, startRegister, patched, vector4Count);
                }
            }

            return g_originalSetVertexShaderConstantF(device, startRegister, data, vector4Count);
        }

        HRESULT WINAPI HookedCreateVertexShader(IDirect3DDevice9* device, const DWORD* function,
                                                IDirect3DVertexShader9** shader)
        {
            Report().NoteVertexShaderCreated(0);
            return g_originalCreateVertexShader(device, function, shader);
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
                        const HRESULT hr = g_originalSetRenderTarget(device, 0, g_stereo.Color());
                        g_originalSetDepthStencilSurface(device, g_stereo.Depth());
                        return hr;
                    }
                }
            }
            return g_originalSetRenderTarget(device, index, surface);
        }

        HRESULT WINAPI HookedDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                           UINT startVertex, UINT primitiveCount)
        {
            if (Report().IsActive())
            {
                NoteDrawForReport(FrameReport::DrawPrimitive);
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
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawPrimitive(device, type, startVertex, primitiveCount);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
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
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawIndexedPrimitive(
                    device, type, baseVertexIndex, minVertexIndex, numVertices,
                    startIndex, primitiveCount);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
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
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawPrimitiveUP(device, type, primitiveCount,
                                                                    vertexData, stride);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
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
            for (int eye = 0; eye < EyeCount; ++eye)
            {
                BeginEye(device, eye);
                const HRESULT eyeResult = g_originalDrawIndexedPrimitiveUP(
                    device, type, minVertexIndex, numVertices, primitiveCount,
                    indexData, indexFormat, vertexData, stride);
                if (FAILED(eyeResult)) { hr = eyeResult; }
            }
            g_duplicatingDraw = false;
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
        ReleaseFrameResources();
        g_presenter.Shutdown();
        Vr().Shutdown();
        g_device = nullptr;
    }
}
