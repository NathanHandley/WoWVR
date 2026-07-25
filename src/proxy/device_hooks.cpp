#include "proxy/device_hooks.h"

#include "core/config.h"
#include "core/hotkeys.h"
#include "core/image_dump.h"
#include "core/log.h"
#include "core/paths.h"
#include "diag/frame_report.h"
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
        typedef HRESULT (WINAPI *SetRenderTargetFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
        typedef HRESULT (WINAPI *SetDepthStencilSurfaceFn)(IDirect3DDevice9*, IDirect3DSurface9*);
        typedef HRESULT (WINAPI *SetViewportFn)(IDirect3DDevice9*, const D3DVIEWPORT9*);
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
        SetRenderTargetFn g_originalSetRenderTarget = nullptr;
        SetDepthStencilSurfaceFn g_originalSetDepthStencilSurface = nullptr;
        SetViewportFn g_originalSetViewport = nullptr;
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
        float g_eyeProjection[EyeCount][16] = {};
        bool g_haveEyeProjections = false;
        UINT g_projectionRegister = 0;

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
            g_panelDrawn = 0;
            g_panelSkipped = 0;
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
            }

            device->SetTexture(0, nullptr);
            g_originalSetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
            g_originalSetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
            g_originalSetRenderState(device, D3DRS_ZWRITEENABLE, TRUE);

            // Release the stereo surface before anyone tries to read it back: D3D9
            // refuses StretchRect and GetRenderTargetData on a target that is still
            // bound, and the capture that follows does both.
            g_originalSetRenderTarget(device, 0, g_realBackBuffer);
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
                WOWVR_INFO("F9: frame report armed for frame %llu.", g_frameCount + 1);
            }

            if (HotkeyPressed(Hotkey::FrameDump))
            {
                g_dumpNextFrame = true;
                WOWVR_INFO("F10: eye buffer will be written on the next frame.");
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
            g_backBufferDrawPeriod = 0;
            g_wasDrawingToBackBuffer = false;
            g_uiPassStarted = false;
            g_stereoHasContent = false;
            g_uiRendered = false;

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
            const bool nowOnBackBuffer = g_renderingToBackBuffer;
            const bool periodStarted = nowOnBackBuffer && !g_wasDrawingToBackBuffer;
            g_wasDrawingToBackBuffer = nowOnBackBuffer;

            if (!periodStarted)
            {
                return;
            }

            ++g_backBufferDrawPeriod;

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
            if (g_uiPassStarted || g_backBufferDrawPeriod < 2
                || !g_renderingToBackBuffer || !g_alphaBlendEnabled)
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
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            g_originalSetViewport(device, &viewport);

            // Only the scene camera gets swapped. UI and other ortho passes never had
            // a patched projection, so they are simply drawn into each half as-is.
            if (g_haveEyeProjections)
            {
                g_originalSetVertexShaderConstantF(device, g_projectionRegister,
                                                   g_eyeProjection[eye], 4);
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
            Report().NoteDrawContext(g_renderingToBackBuffer, depthLike, g_alphaBlendEnabled);
        }

        HRESULT WINAPI HookedSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport)
        {
            // While a draw is being duplicated the viewport belongs to us; the game's
            // own viewport changes are applied normally otherwise.
            if (g_duplicatingDraw)
            {
                return D3D_OK;
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

            if (StereoActive() && !g_duplicatingDraw && rectCount == 0)
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

            if (couldHoldAMatrix && !g_duplicatingDraw)
            {
                static float patched[256 * 4];
                if (Projection().TryPatch(data, g_eyeProjection[EyeLeft], g_eyeProjection[EyeRight]))
                {
                    // Remember where it went so each duplicated draw can put the right
                    // eye's version back into the same registers.
                    g_projectionRegister = startRegister;
                    g_haveEyeProjections = true;

                    memcpy(patched, g_eyeProjection[EyeLeft], sizeof(float) * 16);
                    memcpy(patched + 16, data + 16,
                           (static_cast<size_t>(vector4Count) - 4) * 4 * sizeof(float));
                    return g_originalSetVertexShaderConstantF(device, startRegister, patched, vector4Count);
                }

                // Something else has taken over these registers, so the cached eye
                // matrices no longer describe what is about to be drawn.
                if (g_haveEyeProjections && startRegister == g_projectionRegister)
                {
                    g_haveEyeProjections = false;
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
            return g_originalSetFVF(device, fvf);
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
            HookSlot(device, slot::device9::SetRenderTarget, &HookedSetRenderTarget,
                     g_originalSetRenderTarget, "SetRenderTarget");
            HookSlot(device, slot::device9::SetDepthStencilSurface, &HookedSetDepthStencilSurface,
                     g_originalSetDepthStencilSurface, "SetDepthStencilSurface");
            HookSlot(device, slot::device9::SetViewport, &HookedSetViewport,
                     g_originalSetViewport, "SetViewport");
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
