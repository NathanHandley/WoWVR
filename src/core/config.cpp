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
            L"; Multiplier on the per-eye render size SteamVR asks for. SteamVR's\r\n"
            L"; recommendation for an Index is already supersampled well past native,\r\n"
            L"; and every frame currently makes a round trip through system memory, so\r\n"
            L"; the default backs off. Raise it once the zero-copy presenter lands.\r\n"
            L"RenderScale=0.6\r\n"
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
            L"\r\n"
            L"[Panel]\r\n"
            L"; The floating UI panel. It is body-locked: it stays put while you glance\r\n"
            L"; around and follows once your head passes the dead zone.\r\n"
            L"Distance=1.6\r\n"
            L"Width=2.2\r\n"
            L"DeadzoneDegrees=20.0\r\n"
            L"FollowSpeed=4.0\r\n"
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
        config.ipdOverride = ReadFloat(path, L"VR", L"IpdOverride", config.ipdOverride);
        config.worldScale = ReadFloat(path, L"VR", L"WorldScale", config.worldScale);
        config.unitsPerMetre = ReadFloat(path, L"VR", L"UnitsPerMetre", config.unitsPerMetre);
        config.perShaderCombined =
            ReadInt(path, L"VR", L"PerShaderCombined", config.perShaderCombined ? 1 : 0) != 0;

        config.headTracking = ReadBool(path, L"Camera", L"HeadTracking", config.headTracking);
        config.cullFovScale = ReadFloat(path, L"Camera", L"CullFovScale", config.cullFovScale);
        config.scanForCamera = ReadBool(path, L"Camera", L"ScanForCamera", config.scanForCamera);
        config.probeCameraByWriting =
            ReadBool(path, L"Camera", L"ProbeCameraByWriting", config.probeCameraByWriting);

        config.panelDistance = ReadFloat(path, L"Panel", L"Distance", config.panelDistance);
        config.panelWidth = ReadFloat(path, L"Panel", L"Width", config.panelWidth);
        config.panelDeadzoneDegrees = ReadFloat(path, L"Panel", L"DeadzoneDegrees", config.panelDeadzoneDegrees);
        config.panelFollowSpeed = ReadFloat(path, L"Panel", L"FollowSpeed", config.panelFollowSpeed);
        config.panelOpacity = ReadFloat(path, L"Panel", L"Opacity", config.panelOpacity);

        config.desktopMirror = ReadBool(path, L"Mirror", L"DesktopMirror", config.desktopMirror);

        config.dumpFrameNumber = ReadInt(path, L"Debug", L"DumpFrameNumber", config.dumpFrameNumber);
        config.frameReportNumber = ReadInt(path, L"Debug", L"FrameReportNumber", config.frameReportNumber);
        config.logWorldDrawFrom = ReadInt(path, L"Debug", L"LogWorldDrawFrom", config.logWorldDrawFrom);
        config.logWorldDrawTo = ReadInt(path, L"Debug", L"LogWorldDrawTo", config.logWorldDrawTo);
        config.useGameProjection = ReadBool(path, L"Debug", L"UseGameProjection", config.useGameProjection);
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
