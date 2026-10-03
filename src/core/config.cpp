#include "core/config.h"

#include "core/paths.h"

#include <windows.h>

#include <cstdlib>
#include <iterator>
#include <string>

namespace wowvr
{
    namespace
    {
        Config g_config;

        const wchar_t* const kDefaultIni =
            L"; WoWVR - SteamVR support for the 3.3.5a client.\r\n"
            L"; Edit and restart the client. Values shown are the defaults.\r\n"
            L"\r\n"
            L"[General]\r\n"
            L"; 0 turns WoWVR off completely; the DLL then just forwards to the system d3d9.\r\n"
            L"Enabled=1\r\n"
            L"; off | error | warn | info | debug | trace\r\n"
            L"LogLevel=info\r\n"
            L"\r\n"
            L"[VR]\r\n"
            L"; 0 keeps the game flat on the desktop but leaves the rest of WoWVR active.\r\n"
            L"Enabled=1\r\n"
            L"; 1 runs without a headset and shows both eyes side-by-side in the game window.\r\n"
            L"FlatDebug=0\r\n"
            L"; Multiplier on the per-eye render size SteamVR asks for. The default\r\n"
            L"; (copy) presenter pays a GPU readback proportional to pixel count, so\r\n"
            L"; the default backs off; with UseD3D9On12=1 that cost disappears and\r\n"
            L"; this can go to 1.0.\r\n"
            L"RenderScale=0.6\r\n"
            L"; 1 hands frames to the compositor through a shared GPU surface; 0 forces\r\n"
            L"; the old copy through system memory (also the automatic fallback).\r\n"
            L"ZeroCopyPresenter=1\r\n"
            L"; 1 runs the client through Windows' D3D9On12 layer - the only route to a\r\n"
            L"; true zero-copy present on this client, but the layer roughly thirds the\r\n"
            L"; frame rate, so it is off by default.\r\n"
            L"UseD3D9On12=0\r\n"
            L"; Interpupillary distance in metres. 0 uses the value reported by the headset.\r\n"
            L"IpdOverride=0.0\r\n"
            L"; Above 1.0 the world feels bigger, below 1.0 it feels like a diorama.\r\n"
            L"WorldScale=1.0\r\n"
            L"\r\n"
            L"[Camera]\r\n"
            L"; 0 leaves the view entirely on the mouse and uses the headset as a screen.\r\n"
            L"HeadTracking=1\r\n"
            L"; How much wider than the display frustum WoW is told to cull. Raise if\r\n"
            L"; geometry pops in at the edges when you turn your head quickly.\r\n"
            L"CullFovScale=1.6\r\n"
            L"; What shape a culling volume must be before the head is allowed to turn\r\n"
            L"; it: at least this wide, in degrees from the camera's forward axis out to\r\n"
            L"; its widest corner, and with its corners averaging no further than\r\n"
            L"; CullRotateMaxOffAxisDegrees from that axis. The client also builds narrow\r\n"
            L"; volumes clipped to whatever doorway you are looking through, and turning\r\n"
            L"; one of those makes the building beyond the doorway disappear. 0 and 180\r\n"
            L"; turn every volume, which is that fault.\r\n"
            L"CullRotateMinSpanDegrees=40\r\n"
            L"CullRotateMaxOffAxisDegrees=5\r\n"
            L"\r\n"
            L"[Sound]\r\n"
            L"; 1 puts the listener on the headset: sounds pan with your head and get\r\n"
            L"; louder as you step towards them. 0 leaves the ears on the client's camera.\r\n"
            L"HeadDrivenListener=1\r\n"
            L"; 1 also moves the listener as you move about the room; 0 keeps the\r\n"
            L"; rotation only.\r\n"
            L"ListenerFollowsHeadPosition=1\r\n"
            L"; 1 puts the ears at the camera - the viewpoint - instead of on the\r\n"
            L"; character, which is where the client puts them and why zooming out\r\n"
            L"; never made anything quieter.\r\n"
            L"ListenerAtCamera=1\r\n"
            L"\r\n"
            L"[Panel]\r\n"
            L"; The interface, on a curved sheet around you that stays where it is put.\r\n"
            L"; Ctrl+Alt+F8 brings it back in front of you (left/right only, centred at\r\n"
            L"; eye height). Distance is the curve's radius in metres. PixelsPerDegree\r\n"
            L"; sets how big elements look: raise the game's resolution (and lower its\r\n"
            L"; UI scale to match) and the sheet grows instead of everything shrinking.\r\n"
            L"Distance=1.6\r\n"
            L"PixelsPerDegree=27.8\r\n"
            L"MaxArcDegrees=150\r\n"
            L"; 1 aims the mouse into the world from your head through the pointer, and\r\n"
            L"; puts nameplates where your head sees the creature through the sheet.\r\n"
            L"WorldPointing=1\r\n"
            L"Opacity=1.0\r\n"
            L"\r\n"
            L"[Mirror]\r\n"
            L"; Keep drawing the game to the desktop window as well as the headset.\r\n"
            L"DesktopMirror=1\r\n"
            L"\r\n"
            L"[Debug]\r\n"
            L"; Dumps the eye buffer of this frame number to WoWVR_frame.bmp, so the\r\n"
            L"; render path can be checked without wearing the headset. 0 disables it.\r\n"
            L"DumpFrameNumber=0\r\n"
            L"; Logs everything the game does to the device during this frame:\r\n"
            L"; transforms, constant uploads, shaders, draws, render targets. Verbose.\r\n"
            L"FrameReportNumber=0\r\n";

        void WriteDefaultIniIfMissing(const std::wstring& path)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                return;
            }

            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return;
            }

            // UTF-16LE with a BOM, which is what GetPrivateProfile* expects for
            // anything that is not plain ANSI.
            const wchar_t bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(file, &bom, sizeof(bom), &written, nullptr);
            WriteFile(file, kDefaultIni,
                      static_cast<DWORD>(wcslen(kDefaultIni) * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(file);
        }

        // Reads a key, and writes the default back if it is not there yet.
        //
        // Without the write-back, any setting added in a later build stays invisible
        // forever to anyone who already has a WoWVR.ini, because the default file is
        // only ever created once. Self-migrating keeps the file honest about what
        // WoWVR actually supports.
        std::wstring ReadString(const std::wstring& path, const wchar_t* section,
                                const wchar_t* key, const wchar_t* fallback)
        {
            static const wchar_t* const kAbsent = L"\x01";

            wchar_t buffer[256];
            GetPrivateProfileStringW(section, key, kAbsent, buffer,
                                     static_cast<DWORD>(std::size(buffer)), path.c_str());

            if (wcscmp(buffer, kAbsent) == 0)
            {
                WritePrivateProfileStringW(section, key, fallback, path.c_str());
                return std::wstring(fallback);
            }

            return std::wstring(buffer);
        }

        bool ReadBool(const std::wstring& path, const wchar_t* section,
                      const wchar_t* key, bool fallback)
        {
            const std::wstring text = ReadString(path, section, key, fallback ? L"1" : L"0");
            return _wtoi(text.c_str()) != 0;
        }

        int ReadInt(const std::wstring& path, const wchar_t* section,
                    const wchar_t* key, int fallback)
        {
            wchar_t fallbackText[32];
            swprintf_s(fallbackText, L"%d", fallback);
            return _wtoi(ReadString(path, section, key, fallbackText).c_str());
        }

        float ReadFloat(const std::wstring& path, const wchar_t* section,
                        const wchar_t* key, float fallback)
        {
            wchar_t fallbackText[64];
            swprintf_s(fallbackText, L"%f", fallback);

            const std::wstring text = ReadString(path, section, key, fallbackText);
            wchar_t* end = nullptr;
            const double value = wcstod(text.c_str(), &end);
            if (end == text.c_str())
            {
                return fallback;
            }
            return static_cast<float>(value);
        }
    }

    void LoadConfig()
    {
        const std::wstring path = ModuleFile(L"WoWVR.ini");
        WriteDefaultIniIfMissing(path);

        Config config;

        config.enabled = ReadBool(path, L"General", L"Enabled", config.enabled);
        config.logLevel = LogLevelFromString(ReadString(path, L"General", L"LogLevel", L"info").c_str());

        config.vrEnabled = ReadBool(path, L"VR", L"Enabled", config.vrEnabled);
        config.flatDebug = ReadBool(path, L"VR", L"FlatDebug", config.flatDebug);
        config.stereo = ReadBool(path, L"VR", L"Stereo", config.stereo);
        config.skipPostProcess = ReadBool(path, L"VR", L"SkipPostProcess", config.skipPostProcess);
        config.skipScreenSpaceWorldDraws =
            ReadBool(path, L"VR", L"SkipScreenSpaceWorldDraws", config.skipScreenSpaceWorldDraws);
        config.renderScale = ReadFloat(path, L"VR", L"RenderScale", config.renderScale);
        config.zeroCopyPresenter =
            ReadBool(path, L"VR", L"ZeroCopyPresenter", config.zeroCopyPresenter);
        config.pipelinedReadback =
            ReadBool(path, L"VR", L"PipelinedReadback", config.pipelinedReadback);
        config.presenterThread =
            ReadBool(path, L"VR", L"PresenterThread", config.presenterThread);
        config.useD3D9On12 = ReadBool(path, L"VR", L"UseD3D9On12", config.useD3D9On12);
        config.ipdOverride = ReadFloat(path, L"VR", L"IpdOverride", config.ipdOverride);
        config.worldScale = ReadFloat(path, L"VR", L"WorldScale", config.worldScale);
        config.unitsPerMetre = ReadFloat(path, L"VR", L"UnitsPerMetre", config.unitsPerMetre);
        config.patchCombined =
            ReadInt(path, L"VR", L"PatchCombined", config.patchCombined ? 1 : 0) != 0;
        config.dumpShaders = ReadBool(path, L"Debug", L"DumpShaders", config.dumpShaders);
        config.patchInverseDerived =
            ReadInt(path, L"VR", L"PatchInverseDerived", config.patchInverseDerived ? 1 : 0) != 0;
        config.perShaderCombined =
            ReadInt(path, L"VR", L"PerShaderCombined", config.perShaderCombined ? 1 : 0) != 0;

        config.headTracking = ReadBool(path, L"Camera", L"HeadTracking", config.headTracking);
        config.cullFovScale = ReadFloat(path, L"Camera", L"CullFovScale", config.cullFovScale);
        config.cullWidenScale =
            ReadFloat(path, L"Camera", L"CullWidenScale", config.cullWidenScale);
        config.shadowDepthBias =
            ReadFloat(path, L"Camera", L"ShadowDepthBias", config.shadowDepthBias);
        config.shadowCoverageScale =
            ReadFloat(path, L"Camera", L"ShadowCoverageScale", config.shadowCoverageScale);
        config.scanForCamera = ReadBool(path, L"Camera", L"ScanForCamera", config.scanForCamera);
        config.probeCameraByWriting =
            ReadBool(path, L"Camera", L"ProbeCameraByWriting", config.probeCameraByWriting);
        config.watchCameraStruct =
            ReadBool(path, L"Camera", L"WatchCameraStruct", config.watchCameraStruct);
        config.autoLocateCamera =
            ReadBool(path, L"Camera", L"AutoLocateCamera", config.autoLocateCamera);
        config.aimCameraAtHead =
            ReadBool(path, L"Camera", L"AimCameraAtHead", config.aimCameraAtHead);
        config.headDrivenCullFrustum =
            ReadBool(path, L"Camera", L"HeadDrivenCullFrustum", config.headDrivenCullFrustum);
        config.cullRotateMinSpanDegrees =
            ReadFloat(path, L"Camera", L"CullRotateMinSpanDegrees",
                      config.cullRotateMinSpanDegrees);
        config.cullRotateMaxOffAxisDegrees =
            ReadFloat(path, L"Camera", L"CullRotateMaxOffAxisDegrees",
                      config.cullRotateMaxOffAxisDegrees);
        config.cullTerrainAllAround =
            ReadBool(path, L"Camera", L"CullTerrainAllAround", config.cullTerrainAllAround);
        config.cullRotateMasterCorners =
            ReadBool(path, L"Camera", L"CullRotateMasterCorners",
                     config.cullRotateMasterCorners);
        config.wmoGroupsAlwaysVisible =
            ReadBool(path, L"Camera", L"WmoGroupsAlwaysVisible",
                     config.wmoGroupsAlwaysVisible);
        config.cullMasterShadowFeeds = static_cast<unsigned>(
            ReadInt(path, L"Camera", L"CullMasterShadowFeeds",
                    static_cast<int>(config.cullMasterShadowFeeds)));
        config.disableCameraCollision =
            ReadBool(path, L"Camera", L"DisableCameraCollision", config.disableCameraCollision);

        config.headDrivenSoundListener =
            ReadBool(path, L"Sound", L"HeadDrivenListener", config.headDrivenSoundListener);
        config.soundListenerFollowsHead =
            ReadBool(path, L"Sound", L"ListenerFollowsHeadPosition",
                     config.soundListenerFollowsHead);
        config.soundListenerAtCamera =
            ReadBool(path, L"Sound", L"ListenerAtCamera", config.soundListenerAtCamera);

        config.panelDistance = ReadFloat(path, L"Panel", L"Distance", config.panelDistance);
        config.panelPixelsPerDegree =
            ReadFloat(path, L"Panel", L"PixelsPerDegree", config.panelPixelsPerDegree);
        config.panelMaxArcDegrees =
            ReadFloat(path, L"Panel", L"MaxArcDegrees", config.panelMaxArcDegrees);
        config.panelWorldPointing =
            ReadBool(path, L"Panel", L"WorldPointing", config.panelWorldPointing);
        config.panelOpacity = ReadFloat(path, L"Panel", L"Opacity", config.panelOpacity);
        config.cursorScale = ReadFloat(path, L"Panel", L"CursorScale", config.cursorScale);
        config.confineCursor = ReadBool(path, L"Panel", L"ConfineCursor", config.confineCursor);
        config.premultipliedUi =
            ReadBool(path, L"Panel", L"PremultipliedUi", config.premultipliedUi);

        config.desktopMirror = ReadBool(path, L"Mirror", L"DesktopMirror", config.desktopMirror);

        config.dumpFrameNumber = ReadInt(path, L"Debug", L"DumpFrameNumber", config.dumpFrameNumber);
        config.frameReportNumber = ReadInt(path, L"Debug", L"FrameReportNumber", config.frameReportNumber);
        config.logWorldDrawFrom = ReadInt(path, L"Debug", L"LogWorldDrawFrom", config.logWorldDrawFrom);
        config.logWorldDrawTo = ReadInt(path, L"Debug", L"LogWorldDrawTo", config.logWorldDrawTo);
        config.useGameProjection = ReadBool(path, L"Debug", L"UseGameProjection", config.useGameProjection);
        config.patchConstants = ReadBool(path, L"Debug", L"PatchConstants", config.patchConstants);
        config.logShadowConstants =
            ReadBool(path, L"Debug", L"LogShadowConstants", config.logShadowConstants);
        config.logSkippedDraws = ReadBool(path, L"Debug", L"LogSkippedDraws", config.logSkippedDraws);
        config.logCursorDecisions =
            ReadBool(path, L"Debug", L"LogCursorDecisions", config.logCursorDecisions);
        config.fullTargetSingleEye =
            ReadBool(path, L"Debug", L"FullTargetSingleEye", config.fullTargetSingleEye);
        config.keepGameViewport = ReadBool(path, L"Debug", L"KeepGameViewport", config.keepGameViewport);
        config.symmetricEyeProjection =
            ReadBool(path, L"Debug", L"SymmetricEyeProjection", config.symmetricEyeProjection);
        config.singleEyeOnly = ReadBool(path, L"Debug", L"SingleEyeOnly", config.singleEyeOnly);
        config.eyeProjectionPassThrough =
            ReadBool(path, L"Debug", L"EyeProjectionPassThrough", config.eyeProjectionPassThrough);
        config.patchFixedFunction =
            ReadBool(path, L"Debug", L"PatchFixedFunction", config.patchFixedFunction);
        config.dumpEveryNWorldDraws =
            ReadInt(path, L"Debug", L"DumpEveryNWorldDraws", config.dumpEveryNWorldDraws);
        config.projectionProbeXScale =
            ReadFloat(path, L"Debug", L"ProjectionProbeXScale", config.projectionProbeXScale);

        // Clamp the values that would produce something unusable or unsafe to render.
        if (config.renderScale < 0.25f) { config.renderScale = 0.25f; }
        if (config.renderScale > 2.0f)  { config.renderScale = 2.0f; }
        if (config.worldScale < 0.1f)   { config.worldScale = 0.1f; }
        if (config.worldScale > 10.0f)  { config.worldScale = 10.0f; }
        if (config.cullFovScale < 1.0f) { config.cullFovScale = 1.0f; }
        if (config.cullFovScale > 4.0f) { config.cullFovScale = 4.0f; }
        if (config.cullWidenScale < 1.0f) { config.cullWidenScale = 1.0f; }
        if (config.cullWidenScale > 3.0f) { config.cullWidenScale = 3.0f; }
        if (config.panelDistance < 0.3f) { config.panelDistance = 0.3f; }
        if (config.panelOpacity < 0.05f) { config.panelOpacity = 0.05f; }
        if (config.panelOpacity > 1.0f)  { config.panelOpacity = 1.0f; }

        g_config = config;
    }

    const Config& Cfg()
    {
        return g_config;
    }
}
