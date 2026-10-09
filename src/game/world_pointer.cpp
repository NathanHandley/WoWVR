#include "game/world_pointer.h"

#include "core/log.h"
#include "game/ui_canvas.h"

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace wowvr
{
    namespace
    {
        // Rebased against the running image, so a relocated client still works.
        const uintptr_t kPublishedImageBase = 0x00400000u;

        // Screen -> ray. Only caller is CGWorldFrame's 0x004F6450.
        const uintptr_t kScreenToRayRva = 0x004BF0F0u - kPublishedImageBase;
        const uint8_t kScreenToRayEntry[9] =
            { 0x55u, 0x8Bu, 0xECu, 0x81u, 0xECu, 0xF8u, 0x00u, 0x00u, 0x00u };
            // push ebp; mov ebp,esp; sub esp,0xF8

        // World -> screen, CGWorldFrame::GetScreenCoordinates.
        const uintptr_t kWorldToScreenRva = 0x004F6D20u - kPublishedImageBase;
        const uint8_t kWorldToScreenEntry[6] =
            { 0x55u, 0x8Bu, 0xECu, 0x83u, 0xECu, 0x24u };
            // push ebp; mov ebp,esp; sub esp,0x24

        const uintptr_t kWorldFramePtrRva = 0x00B7436Cu - kPublishedImageBase;

        // Width and height of the screen in the client's diagonal-normalised units
        // (width^2 + height^2 = 1), which is what GetScreenCoordinates hands back.
        // Scaled by 0x0047BFF0 on both the mouse and the projection side.
        const uintptr_t kDdcWidthRva = 0x00AC0CB4u - kPublishedImageBase;
        const uintptr_t kDdcHeightRva = 0x00AC0CB8u - kPublishedImageBase;

        // CGWorldFrame fields read by GetScreenCoordinates (see the disassembly from
        // 0x004F6E22): the projection rectangle the 0..1 result is scaled by, the
        // frame's own rectangle the on-screen test is made against, and its camera.
        const uintptr_t kFrameBottom = 0x64u;
        const uintptr_t kFrameLeft = 0x68u;
        const uintptr_t kFrameTop = 0x6Cu;
        const uintptr_t kFrameRight = 0x70u;
        const uintptr_t kProjRect0 = 0x330u;   // y extent is [0x338] - [0x330]
        const uintptr_t kProjRect1 = 0x334u;   // x extent is [0x33C] - [0x334]
        const uintptr_t kProjRect2 = 0x338u;
        const uintptr_t kProjRect3 = 0x33Cu;
        const uintptr_t kMouseRect = 0x320u;   // 0x004F6450's normalisation, logged only
        const uintptr_t kViewProjection = 0x340u;   // camera-relative view-projection, 4x4
        const uintptr_t kActiveCamera = 0x7E20u;

        // The camera object (CLIENT_INTERNALS.md section 2.1).
        const uintptr_t kCamPosition = 0x08u;
        const uintptr_t kCamForward = 0x14u;
        const uintptr_t kCamUp = 0x2Cu;
        const uintptr_t kCamNear = 0x38u;
        const uintptr_t kCamFar = 0x3Cu;

        // Where 0x00715720 (world-anchored frames: nameplates, chat bubbles) returns
        // to after calling GetScreenCoordinates at 0x0071574A.
        const uintptr_t kWorldAnchorReturnRva = 0x0071574Fu - kPublishedImageBase;
        // Where the chat bubble update (0x0056C340) returns to after its own call at
        // 0x0056C3BE. Bubbles hang off WorldFrame too, and are placed the same way.
        const uintptr_t kChatBubbleReturnRva = 0x0056C3C3u - kPublishedImageBase;
        // Where the world text update (0x007E70D0, WorldText.cpp: the damage and heal
        // numbers over units, DAMAGE_TEXT_FONT) returns to after its call at 0x007E71FE.
        const uintptr_t kWorldTextReturnRva = 0x007E7203u - kPublishedImageBase;

        typedef void(__cdecl* ScreenToRayFn)(float nx, float ny, float* start, float* end);

        // thiscall mirrored as fastcall with a dummy edx: the callee pops its three
        // stack arguments either way, which is what the client's 'ret 0xC' does. The
        // result is a full 32-bit value rather than bool: the client's flagless path
        // sets all of eax (mov eax, 1 / xor eax, eax), and a bool only promises al.
        typedef uint32_t(__fastcall* WorldToScreenFn)(uint8_t* worldFrame, void* unusedEdx,
                                                  const float* world, float* out,
                                                  uint32_t* flags);

        ScreenToRayFn g_screenToRayOriginal = nullptr;
        WorldToScreenFn g_worldToScreenOriginal = nullptr;
        bool g_installed = false;
        bool g_installFailed = false;

        struct State
        {
            bool active = false;
            pointing::Frame frame;
        };
        State g_state;

        // Lifebars in the world: the strip layout and this frame's plates.
        struct PlateStrip
        {
            bool on = false;
            float topV = 0.0f;
            float cellU = 0.0f;
            float cellV = 0.0f;
            int columns = 0;
            int rows = 0;
            float anchorV = 0.5f;
            float topBandV = -1.0f;   // the second band's top, or < 0 for none
        };
        PlateStrip g_strip;
        const int kMaxPlates = 64;
        WorldPlate g_plates[kMaxPlates];
        Vec3 g_plateWorld[kMaxPlates];
        int g_plateCount = 0;
        unsigned long long g_platesDropped = 0;

        uintptr_t ImageBase();

        // Chat bubbles in the world: this frame's bubbles. The bubble update (0x0056C340,
        // ChatBubbleFrame.cpp) holds its CGChatBubbleFrame in esi across the call; the
        // frame's font string is at +0x2A4 (constructor, 0x0056CCE8), and a font string
        // keeps its text at +0xF4 (FontString:GetText, 0x0048D730) and its colour at
        // +0xA4 alpha, +0xAC/+0xAD/+0xAE blue/green/red when +0xA8 is 1 (0x00487AB0).
        bool g_bubbleCaptureOn = false;
        const int kMaxBubbles = 16;
        WorldBubbleCard g_bubbles[kMaxBubbles];
        int g_bubbleCount = 0;
        unsigned long long g_bubbleCalls = 0;
        unsigned long long g_bubbleReads = 0;
        char g_lastBubbleLogged[64] = {};
        const uintptr_t kBubbleFontString = 0x2A4u;
        const uintptr_t kFontStringText = 0xF4u;
        const uintptr_t kFontStringHasColour = 0xA8u;
        const uintptr_t kFontStringColour = 0xACu;

        bool ReadBubble(uintptr_t bubble, WorldBubbleCard& card)
        {
            __try
            {
                const uintptr_t fontString = *reinterpret_cast<const uintptr_t*>(bubble + kBubbleFontString);
                if (fontString == 0)
                {
                    return false;
                }
                const char* text = *reinterpret_cast<const char* const*>(fontString + kFontStringText);
                if (text == nullptr || text[0] == 0)
                {
                    return false;
                }
                int n = 0;
                while (n < 255 && text[n] != 0) { card.text[n] = text[n]; ++n; }
                card.text[n] = 0;
                card.rgb = 0xFFFFFFu;
                if (*reinterpret_cast<const int*>(fontString + kFontStringHasColour) == 1)
                {
                    const uint8_t* c = reinterpret_cast<const uint8_t*>(fontString + kFontStringColour);
                    card.rgb = (static_cast<uint32_t>(c[2]) << 16) | (static_cast<uint32_t>(c[1]) << 8) | c[0];
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // World text in the world: this frame's pieces.
        bool g_textCaptureOn = false;
        const int kMaxTexts = 64;
        WorldTextCard g_texts[kMaxTexts];
        int g_textCount = 0;
        unsigned long long g_textCalls = 0;

        // The world text object (CWorldText string, WorldText.cpp) being placed: its
        // update (0x007E70D0) holds it in esi across the call, and esi is preserved into
        // the detour by WorldToScreenThunk. Its layout, from its constructor (0x007E6C60)
        // and the add (0x007E6DC0): +0x08 type (0..10, a row of the timing table at
        // 0x00AF474C), +0x10 half height in screen units, +0x20 colour (BGRA; the update
        // writes the faded alpha into +0x23 before placing it), +0x28 its text object,
        // +0x3C the text (64 bytes).
        uintptr_t g_callerEsi = 0;
        const uintptr_t kTextType = 0x08u;
        const uintptr_t kTextHalfHeight = 0x10u;
        const uintptr_t kTextColour = 0x20u;
        const uintptr_t kTextObject = 0x28u;
        const uintptr_t kTextString = 0x3Cu;
        // The text object's colour and shadow setters, as the update calls them
        // (0x007E6A0A and 0x007E6A1C): cdecl (object, colour*) and (object, colour*,
        // offset*), the offset the client passes being the one at 0x00AF487C.
        const uintptr_t kSetTextColourRva = 0x006BD070u - kPublishedImageBase;
        const uintptr_t kSetTextShadowRva = 0x006BD0C0u - kPublishedImageBase;
        const uintptr_t kShadowOffsetRva = 0x00AF487Cu - kPublishedImageBase;
        const uint8_t kSetTextColourEntry[6] = { 0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x4Du, 0x08u };
        const uint8_t kSetTextShadowEntry[6] = { 0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x4Du, 0x08u };
        typedef int(__cdecl* SetTextColourFn)(void* object, const uint32_t* colour);
        typedef int(__cdecl* SetTextShadowFn)(void* object, const uint32_t* colour, const void* offset);
        int g_textSettersChecked = 0;   // 0 unknown, 1 good, -1 not the expected code

        // Reads the piece; false if it does not look like one.
        bool ReadWorldText(uintptr_t object, WorldTextCard& card, void*& textObject)
        {
            __try
            {
                const int type = *reinterpret_cast<const int*>(object + kTextType);
                if (type < 0 || type > 10)
                {
                    return false;
                }
                const char* text = reinterpret_cast<const char*>(object + kTextString);
                int n = 0;
                while (n < 63 && text[n] != 0) { card.text[n] = text[n]; ++n; }
                card.text[n] = 0;
                if (n == 0)
                {
                    return false;
                }
                card.colour = *reinterpret_cast<const uint32_t*>(object + kTextColour);
                const float half = *reinterpret_cast<const float*>(object + kTextHalfHeight);
                card.heightOfScreen = half;   // screen units here; a fraction once placed
                textObject = *reinterpret_cast<void* const*>(object + kTextObject);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // The client's own copy, transparent for this frame (the update sets its colour
        // again every frame before placing it, so this never sticks).
        void HideWorldText(void* textObject)
        {
            if (textObject == nullptr)
            {
                return;
            }
            if (g_textSettersChecked == 0)
            {
                g_textSettersChecked =
                    (memcmp(reinterpret_cast<const void*>(ImageBase() + kSetTextColourRva),
                            kSetTextColourEntry, 6) == 0
                     && memcmp(reinterpret_cast<const void*>(ImageBase() + kSetTextShadowRva),
                               kSetTextShadowEntry, 6) == 0) ? 1 : -1;
            }
            if (g_textSettersChecked < 0)
            {
                return;
            }
            static const uint32_t clear = 0;
            __try
            {
                reinterpret_cast<SetTextColourFn>(ImageBase() + kSetTextColourRva)(textObject, &clear);
                reinterpret_cast<SetTextShadowFn>(ImageBase() + kSetTextShadowRva)(
                    textObject, &clear, reinterpret_cast<const void*>(ImageBase() + kShadowOffsetRva));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        // Diagnostics, all on the one thread.
        unsigned long long g_rayCalls = 0;
        unsigned long long g_projectCalls = 0;
        unsigned long long g_projectOnPanel = 0;
        unsigned long long g_basisFallbacks = 0;
        float g_lastRayU = -1.0f;
        float g_lastRayV = -1.0f;
        float g_lastRayOffAxisDegrees = 0.0f;
        bool g_frameLogged = false;

        uintptr_t ImageBase()
        {
            static uintptr_t base = 0;
            if (base == 0)
            {
                base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            }
            return base;
        }

        using pointing::CameraBasis;

        float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        float Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

        Vec3 Cross(const Vec3& a, const Vec3& b)
        {
            Vec3 r;
            r.x = a.y * b.z - a.z * b.y;
            r.y = a.z * b.x - a.x * b.z;
            r.z = a.x * b.y - a.y * b.x;
            return r;
        }

        bool Finite(float value) { return value == value && std::fabs(value) < 1.0e30f; }

        // Copies raw floats out of client memory. Kept free of anything with a
        // destructor so __try is allowed here.
        bool CopyFloats(uintptr_t address, float* out, int count)
        {
            __try
            {
                memcpy(out, reinterpret_cast<const void*>(address),
                       static_cast<size_t>(count) * sizeof(float));
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool ReadPointer(uintptr_t address, uintptr_t& out)
        {
            __try
            {
                out = *reinterpret_cast<const uintptr_t*>(address);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        uintptr_t CurrentWorldFrame()
        {
            uintptr_t frame = 0;
            if (!ReadPointer(ImageBase() + kWorldFramePtrRva, frame))
            {
                return 0;
            }
            return frame;
        }

        bool ReadCamera(uintptr_t worldFrame, CameraBasis& out)
        {
            uintptr_t camera = 0;
            if (worldFrame == 0 || !ReadPointer(worldFrame + kActiveCamera, camera)
                || camera == 0)
            {
                return false;
            }

            // +0x08 .. +0x3C in one read: position, forward, left, up, near, far.
            float raw[14] = {};
            if (!CopyFloats(camera + kCamPosition, raw, 14))
            {
                return false;
            }
            out.position = { raw[0], raw[1], raw[2] };
            out.nearPlane = raw[(kCamNear - kCamPosition) / 4u];
            out.farPlane = raw[(kCamFar - kCamPosition) / 4u];
            if (!Finite(out.position.x) || !Finite(out.position.y) || !Finite(out.position.z))
            {
                return false;
            }

            // The axes come from the world frame's camera-relative view-projection at
            // +0x340 - the matrix GetScreenCoordinates itself projects with, captured
            // by the same camera setup the pick runs - NOT from the camera object's own
            // basis at +0x14 / +0x2C. On a transport those are kept in the transport's
            // rotated frame (measured on the Kelethin lift: ~150 degrees apart from where
            // the client's own ray went), so a ray built from them missed everything
            // off the lift. Row-vector layout (0x004C2270: out[c] = sum v[r] * M[r][c]):
            // clip.w = view depth, so column 3 is the forward axis; columns 0 and 1 are
            // right and up scaled by the projection.
            float m[16] = {};
            if (CopyFloats(worldFrame + kViewProjection, m, 16))
            {
                Vec3 forward = { m[3], m[7], m[11] };
                Vec3 right = { m[0], m[4], m[8] };
                Vec3 up = { m[1], m[5], m[9] };
                const float fl = Length(forward);
                const float rl = Length(right);
                const float ul = Length(up);
                if (fl > 1.0e-4f && rl > 1.0e-4f && ul > 1.0e-4f && Finite(fl) && Finite(rl)
                    && Finite(ul))
                {
                    forward = { forward.x / fl, forward.y / fl, forward.z / fl };
                    right = { right.x / rl, right.y / rl, right.z / rl };
                    up = { up.x / ul, up.y / ul, up.z / ul };
                    if (std::fabs(Dot(forward, right)) < 0.05f && std::fabs(Dot(forward, up)) < 0.05f
                        && std::fabs(Dot(right, up)) < 0.05f)
                    {
                        out.forward = forward;
                        out.right = right;
                        out.up = up;
                        return true;
                    }
                }
            }

            // Fallback: the camera object's own basis (correct off transports).
            const int forwardIndex = static_cast<int>((kCamForward - kCamPosition) / 4u);
            const int upIndex = static_cast<int>((kCamUp - kCamPosition) / 4u);
            out.forward = { raw[forwardIndex], raw[forwardIndex + 1], raw[forwardIndex + 2] };
            out.up = { raw[upIndex], raw[upIndex + 1], raw[upIndex + 2] };
            // +0x20 is the LEFT axis (WoW is right-handed and Z-up); right is built as
            // forward x up instead of being read and negated (CLIENT_INTERNALS 2.3).
            out.right = Cross(out.forward, out.up);
            ++g_basisFallbacks;

            const float forwardLength = Length(out.forward);
            const float upLength = Length(out.up);
            const float rightLength = Length(out.right);
            if (std::fabs(forwardLength - 1.0f) > 0.05f || std::fabs(upLength - 1.0f) > 0.05f
                || std::fabs(rightLength - 1.0f) > 0.05f)
            {
                return false;
            }
            return true;
        }

        Vec3 ViewToWorldDirection(const CameraBasis& camera, const Vec3& view)
        {
            Vec3 world;
            world.x = camera.right.x * view.x + camera.up.x * view.y + camera.forward.x * view.z;
            world.y = camera.right.y * view.x + camera.up.y * view.y + camera.forward.y * view.z;
            world.z = camera.right.z * view.x + camera.up.z * view.y + camera.forward.z * view.z;
            return world;
        }

        Vec3 WorldToViewDirection(const CameraBasis& camera, const Vec3& world)
        {
            Vec3 view;
            view.x = Dot(world, camera.right);
            view.y = Dot(world, camera.up);
            view.z = Dot(world, camera.forward);
            return view;
        }

        void LogFrameOnce(uintptr_t worldFrame)
        {
            if (g_frameLogged)
            {
                return;
            }
            g_frameLogged = true;

            float frameRect[4] = {};
            float projRect[4] = {};
            float mouseRect[4] = {};
            float ddc[2] = {};
            if (CopyFloats(worldFrame + kFrameBottom, frameRect, 4)
                && CopyFloats(worldFrame + kProjRect0, projRect, 4)
                && CopyFloats(worldFrame + kMouseRect, mouseRect, 4)
                && CopyFloats(ImageBase() + kDdcWidthRva, ddc, 1)
                && CopyFloats(ImageBase() + kDdcHeightRva, ddc + 1, 1))
            {
                // Everything here should describe a world frame covering the whole
                // screen: frame and mouse rectangles spanning 0..ddc, projection
                // rectangle 0..1. Anything else means an addon has moved WorldFrame,
                // and the panel-to-screen mapping below assumes it has not.
                WOWVR_INFO("World pointing: screen %.4f x %.4f; world frame b/l/t/r "
                           "%.4f %.4f %.4f %.4f; projection rect %.4f %.4f %.4f %.4f; "
                           "mouse rect %.4f %.4f %.4f %.4f.", ddc[0], ddc[1],
                           frameRect[0], frameRect[1], frameRect[2], frameRect[3],
                           projRect[0], projRect[1], projRect[2], projRect[3],
                           mouseRect[0], mouseRect[1], mouseRect[2], mouseRect[3]);
            }
        }

        // Entered in place of 0x004BF0F0. The client's own answer is computed first:
        // its near and far distances are kept so the trace keeps the reach the client
        // gave it, and only the direction and origin are replaced.
        void __cdecl ScreenToRayDetour(float nx, float ny, float* start, float* end)
        {
            g_screenToRayOriginal(nx, ny, start, end);

            if (!g_state.active || start == nullptr || end == nullptr)
            {
                return;
            }

            const uintptr_t worldFrame = CurrentWorldFrame();
            CameraBasis camera;
            if (!ReadCamera(worldFrame, camera))
            {
                return;
            }
            LogFrameOnce(worldFrame);

            // nx, ny are fractions of the world frame with y up; the panel texture is
            // the whole screen with v down.
            const float u = nx;
            const float v = 1.0f - ny;

            Vec3 originWorld;
            Vec3 directionWorld;
            if (!pointing::PanelToWorldRay(g_state.frame, camera, u, v, originWorld,
                                           directionWorld))
            {
                return;
            }

            const Vec3 originalStart = { start[0], start[1], start[2] };
            const Vec3 originalEnd = { end[0], end[1], end[2] };
            float nearDistance = Length(originalStart);
            float farDistance = Length(originalEnd);
            if (!Finite(nearDistance) || !Finite(farDistance) || !(farDistance > nearDistance))
            {
                // The client refused the input (it does outside 0..1) and wrote nothing.
                nearDistance = camera.nearPlane;
                farDistance = camera.farPlane;
            }
            if (!(farDistance > 0.0f))
            {
                return;
            }
            start[0] = originWorld.x + directionWorld.x * nearDistance;
            start[1] = originWorld.y + directionWorld.y * nearDistance;
            start[2] = originWorld.z + directionWorld.z * nearDistance;
            end[0] = originWorld.x + directionWorld.x * farDistance;
            end[1] = originWorld.y + directionWorld.y * farDistance;
            end[2] = originWorld.z + directionWorld.z * farDistance;

            ++g_rayCalls;
            g_lastRayU = u;
            g_lastRayV = v;
            const float along = Dot(directionWorld, camera.forward);
            g_lastRayOffAxisDegrees =
                std::acos(along > 1.0f ? 1.0f : (along < -1.0f ? -1.0f : along)) * 57.29578f;
        }

        // Entered in place of GetScreenCoordinates. Replicates the client's contract
        // exactly - output in screen units relative to the frame, depth in out[2], the
        // four on-screen bits in *flags and the return value - but measures it on the
        // panel, from the head.
        uint32_t __fastcall WorldToScreenDetour(uint8_t* worldFrame, void* unusedEdx,
                                            const float* world, float* out, uint32_t* flags)
        {
            if (!g_state.active || worldFrame == nullptr || world == nullptr || out == nullptr)
            {
                return g_worldToScreenOriginal(worldFrame, unusedEdx, world, out, flags);
            }

            const uintptr_t frame = reinterpret_cast<uintptr_t>(worldFrame);
            CameraBasis camera;
            float frameRect[4] = {};     // bottom, left, top, right
            float projRect[4] = {};
            float ddc[2] = {};
            float point[3] = {};
            if (!ReadCamera(frame, camera)
                || !CopyFloats(frame + kFrameBottom, frameRect, 4)
                || !CopyFloats(frame + kProjRect0, projRect, 4)
                || !CopyFloats(ImageBase() + kDdcWidthRva, ddc, 1)
                || !CopyFloats(ImageBase() + kDdcHeightRva, ddc + 1, 1)
                || !CopyFloats(reinterpret_cast<uintptr_t>(world), point, 3))
            {
                return g_worldToScreenOriginal(worldFrame, unusedEdx, world, out, flags);
            }

            ++g_projectCalls;

            const Vec3 target = { point[0], point[1], point[2] };

            // Chat bubbles in the world: read the bubble, and send the client's frame
            // far off the canvas (it is placed wherever this says, and a 'no' would only
            // leave it where it was, on the interface).
            if (g_bubbleCaptureOn
                && _ReturnAddress() == reinterpret_cast<void*>(ImageBase() + kChatBubbleReturnRva))
            {
                ++g_bubbleCalls;
                if (g_bubbleCount < kMaxBubbles)
                {
                    WorldBubbleCard& card = g_bubbles[g_bubbleCount];
                    if (ReadBubble(g_callerEsi, card))
                    {
                        ++g_bubbleReads;
                        pointing::WorldToBody(g_state.frame, camera, target, card.body);
                        const Vec3 d = { card.body.x - g_state.frame.headBody.x,
                                         card.body.y - g_state.frame.headBody.y,
                                         card.body.z - g_state.frame.headBody.z };
                        card.distanceMetres = Length(d);
                        if (strncmp(g_lastBubbleLogged, card.text, sizeof(g_lastBubbleLogged) - 1) != 0)
                        {
                            strncpy_s(g_lastBubbleLogged, card.text, _TRUNCATE);
                            WOWVR_INFO("Chat bubble: \"%.60s\" colour %06X, %.1f m away, at body "
                                       "(%.2f, %.2f, %.2f).", card.text, card.rgb, card.distanceMetres,
                                       card.body.x, card.body.y, card.body.z);
                        }
                        ++g_bubbleCount;
                    }
                }
                out[0] = -20.0f * (frameRect[3] - frameRect[1]);
                out[1] = -20.0f * (frameRect[2] - frameRect[0]);
                out[2] = 1.0f;
                if (flags != nullptr) { *flags = 0x0Fu; }
                return 1u;
            }

            // World text in the world: read it, hide the client's copy, and answer "on
            // screen" whatever the head is doing - a 'no' makes the client delete the piece
            // (0x007E5205), and in a headset it is still there to be seen beside or behind.
            if (g_textCaptureOn
                && _ReturnAddress() == reinterpret_cast<void*>(ImageBase() + kWorldTextReturnRva))
            {
                ++g_textCalls;
                if (g_textCount < kMaxTexts)
                {
                    WorldTextCard& card = g_texts[g_textCount];
                    void* textObject = nullptr;
                    if (ReadWorldText(g_callerEsi, card, textObject))
                    {
                        pointing::WorldToBody(g_state.frame, camera, target, card.body);
                        const Vec3 d = { card.body.x - g_state.frame.headBody.x,
                                         card.body.y - g_state.frame.headBody.y,
                                         card.body.z - g_state.frame.headBody.z };
                        card.distanceMetres = Length(d);
                        const float fraction = ddc[1] > 0.0f ? 2.0f * card.heightOfScreen / ddc[1] : 0.0f;
                        if ((g_textCalls % 1200u) == 1u)
                        {
                            WOWVR_INFO("World text sample: \"%s\" colour %08X, half height %.4f of "
                                       "screen height %.4f -> %.3f of the screen, %.1f m away.",
                                       card.text, card.colour, card.heightOfScreen, ddc[1], fraction,
                                       card.distanceMetres);
                        }
                        card.heightOfScreen = (fraction > 0.0f && fraction < 0.5f) ? fraction : 0.0f;
                        ++g_textCount;
                        HideWorldText(textObject);
                    }
                }
                out[0] = 0.5f * (frameRect[3] - frameRect[1]);
                out[1] = 0.5f * (frameRect[2] - frameRect[0]);
                out[2] = 1.0f;
                if (flags != nullptr) { *flags = 0x0Fu; }
                return 1u;
            }
            float u = 0.0f;
            float v = 0.0f;
            float distanceYards = 0.0f;
            if (!pointing::WorldToPanel(g_state.frame, camera, target, u, v, distanceYards))
            {
                // The client's own early out (behind the near plane) writes nothing and
                // returns false; this is the same answer for "nowhere on the panel".
                return 0u;
            }

            // Lifebars in the world: this one goes to a strip cell; where its unit really
            // is gets remembered for the 3D pass. The same point asked for twice in a
            // frame keeps its cell.
            const void* caller = _ReturnAddress();
            const bool plateCall =
                caller == reinterpret_cast<void*>(ImageBase() + kWorldAnchorReturnRva);
            if (plateCall && g_strip.on)
            {
                // The client asks for every lifebar candidate before applying its own
                // range limit (about 41 yards, 0x0072B2DB on), so units far beyond it
                // come through here too. They would never show; they must not take cells
                // a real lifebar needs.
                if (distanceYards > 60.0f)
                {
                    return 0u;
                }
                int slot = -1;
                for (int i = 0; i < g_plateCount; ++i)
                {
                    if (g_plateWorld[i].x == target.x && g_plateWorld[i].y == target.y
                        && g_plateWorld[i].z == target.z)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0)
                {
                    const int bands = g_strip.topBandV >= 0.0f ? 2 : 1;
                    if (g_plateCount >= kMaxPlates
                        || g_plateCount >= g_strip.columns * g_strip.rows * bands)
                    {
                        ++g_platesDropped;
                        return 0u;
                    }
                    slot = g_plateCount++;
                    g_plateWorld[slot] = target;
                    WorldPlate& plate = g_plates[slot];
                    Vec3 body;
                    pointing::WorldToBody(g_state.frame, camera, target, body);
                    plate.body = body;
                    plate.distanceMetres = distanceYards / g_state.frame.unitsPerMetre;
                    plate.slotU = (static_cast<float>(slot % g_strip.columns) + 0.5f) * g_strip.cellU;
                    plate.row = slot / g_strip.columns;
                    const bool upper = plate.row >= g_strip.rows;
                    plate.slotV = (upper ? g_strip.topBandV : g_strip.topV)
                                + (static_cast<float>(upper ? plate.row - g_strip.rows : plate.row)
                                   + g_strip.anchorV) * g_strip.cellV;
                }
                u = g_plates[slot].slotU;
                v = g_plates[slot].slotV;
            }

            // The panel IS the screen: u across it, v down it. The client's scaling
            // from there on is reproduced as-is (0x004F6E22 onwards).
            const float x01 = u;
            const float y01 = 1.0f - v;
            float x = ddc[0] * x01 * (projRect[3] - projRect[1]);
            float y = ddc[1] * y01 * (projRect[2] - projRect[0]);
            if (frameRect[1] < 0.0f) { x -= frameRect[1]; }
            if (frameRect[0] < 0.0f) { y -= frameRect[0]; }


            // Frames anchored to a world position - lifebars (nameplates), placed by
            // 0x00715720, and chat bubbles, by 0x0056C340 - hand this point to their
            // frame without allowing for WorldFrame's scale. On the stretched interface canvas
            // (game/ui_canvas.h) WorldFrame is scaled down by the canvas factor, so they
            // would land that much closer to the corner; scaled up here, they sit where
            // the point really is. Only for those two callers: picking, world text and the rest
            // want the true point. The on-screen test below stays on the true point.
            float placeX = x;
            float placeY = y;
            const float canvas = Canvas().PanelScale();
            if (canvas > 1.0f
                && (caller == reinterpret_cast<void*>(ImageBase() + kWorldAnchorReturnRva)
                    || caller == reinterpret_cast<void*>(ImageBase() + kChatBubbleReturnRva)))
            {
                placeX = x * canvas;
                placeY = y * canvas;
            }
            out[0] = placeX;
            out[1] = placeY;
            // Depth: the client hands back the camera-space depth here. Distance from
            // the head along the line actually looked down is the same quantity for a
            // headset, and stays meaningful for things beside or behind the camera.
            out[2] = distanceYards;

            const float width = frameRect[3] - frameRect[1];
            const float height = frameRect[2] - frameRect[0];
            const uint32_t bits = (x >= 0.0f ? 1u : 0u)
                                | (y >= 0.0f ? 2u : 0u)
                                | (x <= width ? 4u : 0u)
                                | (y <= height ? 8u : 0u);
            if (flags != nullptr)
            {
                *flags = bits;
            }
            if (bits == 0x0Fu)
            {
                ++g_projectOnPanel;
                return 1u;
            }
            return 0u;
        }

        // GetScreenCoordinates is entered here first: the caller's esi (the world text
        // object, for the world text update) is kept, then the detour runs as if called
        // directly - same stack, same return address.
        __declspec(naked) void WorldToScreenThunk()
        {
            __asm
            {
                mov g_callerEsi, esi
                jmp WorldToScreenDetour
            }
        }

        bool InstallDetour(uintptr_t rva, const uint8_t* expected, size_t length,
                           const void* detour, void** trampolineOut, const char* name)
        {
            uint8_t* site = reinterpret_cast<uint8_t*>(ImageBase() + rva);

            uint8_t patched[16] = {};
            patched[0] = 0xE9u;
            const int32_t rel = static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(detour) - (reinterpret_cast<uintptr_t>(site) + 5));
            memcpy(patched + 1, &rel, 4);
            for (size_t i = 5; i < length; ++i)
            {
                patched[i] = 0x90u;
            }

            if (memcmp(site, patched, length) == 0)
            {
                return *trampolineOut != nullptr;
            }
            if (memcmp(site, expected, length) != 0)
            {
                WOWVR_WARN("World pointing: %s at 0x%08X does not hold the expected bytes; "
                           "leaving the client's own version in place.", name,
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
                return false;
            }

            // Trampoline: the displaced whole instructions, then a jump back to the
            // first byte after them. Never freed - the detour may be entered for as
            // long as the process lives.
            uint8_t* stub = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (stub == nullptr)
            {
                WOWVR_WARN("World pointing: could not allocate the %s trampoline.", name);
                return false;
            }
            size_t n = 0;
            memcpy(stub, expected, length);
            n += length;
            stub[n++] = 0xE9u;
            const uintptr_t resume = reinterpret_cast<uintptr_t>(site) + length;
            const int32_t back = static_cast<int32_t>(
                resume - (reinterpret_cast<uintptr_t>(stub) + n + 4));
            memcpy(stub + n, &back, 4);
            n += 4;
            FlushInstructionCache(GetCurrentProcess(), stub, n);

            // Published before the jump goes in, so the detour can never run without
            // somewhere to hand the call on to.
            *trampolineOut = stub;

            DWORD previous = 0;
            if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &previous))
            {
                WOWVR_WARN("World pointing: %s at 0x%08X could not be made writable.", name,
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
                return false;
            }
            memcpy(site, patched, length);
            VirtualProtect(site, length, previous, &previous);
            FlushInstructionCache(GetCurrentProcess(), site, length);
            WOWVR_INFO("World pointing: %s hooked at 0x%08X.", name,
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(site)));
            return true;
        }
    }

    bool WorldPointer::Install()
    {
        if (g_installed)
        {
            return true;
        }
        if (g_installFailed)
        {
            return false;
        }

        // Both or neither: one without the other would make the pointer and the
        // nameplates disagree, which is worse than the client's consistent mistake.
        const uint8_t* rayCode = reinterpret_cast<const uint8_t*>(ImageBase() + kScreenToRayRva);
        const uint8_t* projectCode =
            reinterpret_cast<const uint8_t*>(ImageBase() + kWorldToScreenRva);
        if (memcmp(rayCode, kScreenToRayEntry, sizeof(kScreenToRayEntry)) != 0
            || memcmp(projectCode, kWorldToScreenEntry, sizeof(kWorldToScreenEntry)) != 0)
        {
            g_installFailed = true;
            WOWVR_WARN("World pointing: the client's pick and projection entry points are "
                       "not the 3.3.5a (12340) code this was written against; left alone.");
            return false;
        }

        void* rayTrampoline = nullptr;
        void* projectTrampoline = nullptr;
        // If either fails, whatever did go in stays inert: Update only ever activates
        // the pair together.
        const bool projectOk = InstallDetour(kWorldToScreenRva, kWorldToScreenEntry,
                                             sizeof(kWorldToScreenEntry),
                                             reinterpret_cast<const void*>(&WorldToScreenThunk),
                                             &projectTrampoline, "GetScreenCoordinates");
        if (projectOk)
        {
            g_worldToScreenOriginal = reinterpret_cast<WorldToScreenFn>(projectTrampoline);
        }
        const bool rayOk = projectOk
            && InstallDetour(kScreenToRayRva, kScreenToRayEntry, sizeof(kScreenToRayEntry),
                             reinterpret_cast<const void*>(&ScreenToRayDetour),
                             &rayTrampoline, "screen-to-ray");
        if (rayOk)
        {
            g_screenToRayOriginal = reinterpret_cast<ScreenToRayFn>(rayTrampoline);
        }

        if (!projectOk || !rayOk)
        {
            g_installFailed = true;
            return false;
        }

        g_installed = true;
        return true;
    }

    void WorldPointer::Update(bool active, const PanelShape& shape, const Vec3& headBody,
                              const Mat4& gameViewToBody, float unitsPerMetre)
    {
        g_state.frame.shape = shape;
        g_state.frame.headBody = headBody;
        g_state.frame.gameViewToBody = gameViewToBody;
        g_state.frame.bodyToGameView = Mat4Transpose(gameViewToBody);
        g_state.frame.unitsPerMetre = (unitsPerMetre > 0.0f) ? unitsPerMetre : 1.0936f;
        g_state.active = active && g_installed && g_screenToRayOriginal != nullptr
                         && g_worldToScreenOriginal != nullptr;
    }

    void WorldPointer::SetPlateStrip(bool on, float stripTopV, float cellU, float cellV,
                                     int columns, int rows, float anchorV, float topBandV)
    {
        g_strip.anchorV = anchorV;
        g_strip.topBandV = topBandV;
        g_strip.on = on && columns > 0 && rows > 0 && cellU > 0.0f && cellV > 0.0f;
        g_strip.topV = stripTopV;
        g_strip.cellU = cellU;
        g_strip.cellV = cellV;
        g_strip.columns = columns;
        g_strip.rows = rows;
    }

    bool WorldPointer::PlateStripOn() const
    {
        return g_strip.on && g_state.active;
    }

    int WorldPointer::PlateCount() const
    {
        return g_plateCount;
    }

    const WorldPlate& WorldPointer::Plate(int index) const
    {
        return g_plates[index];
    }

    void WorldPointer::BeginPlateFrame()
    {
        g_plateCount = 0;
        g_textCount = 0;
        g_bubbleCount = 0;
    }

    void WorldPointer::SetWorldTextCapture(bool on)
    {
        g_textCaptureOn = on;
    }

    void WorldPointer::SetBubbleCapture(bool on)
    {
        g_bubbleCaptureOn = on;

        // The client gives no chat bubble to a unit that has a lifebar (the bubble
        // creation, 0x00720010, leaves when the unit's nameplate frame at +0xC38 is set):
        // on a flat screen the lifebar sits where the bubble would. With bubbles in the
        // world they no longer share a spot, so while this is on that test is skipped:
        //   00720020  0F 85 01 01 00 00   jne 0x00720127
        // becomes six nops.
        static const uint8_t kExpected[6] = { 0x0Fu, 0x85u, 0x01u, 0x01u, 0x00u, 0x00u };
        static const uint8_t kPatched[6] = { 0x90u, 0x90u, 0x90u, 0x90u, 0x90u, 0x90u };
        static bool s_patched = false;
        static bool s_refused = false;
        if (on == s_patched || s_refused)
        {
            return;
        }
        uint8_t* site = reinterpret_cast<uint8_t*>(ImageBase() + (0x00720020u - kPublishedImageBase));
        const uint8_t* from = on ? kExpected : kPatched;
        const uint8_t* to = on ? kPatched : kExpected;
        if (memcmp(site, from, sizeof(kExpected)) != 0)
        {
            WOWVR_WARN("Chat bubbles: 0x00720020 does not hold the expected bytes; units with "
                       "lifebars keep getting no bubble.");
            s_refused = true;
            return;
        }
        DWORD previous = 0;
        if (!VirtualProtect(site, sizeof(kExpected), PAGE_EXECUTE_READWRITE, &previous))
        {
            WOWVR_WARN("Chat bubbles: 0x00720020 could not be made writable.");
            s_refused = true;
            return;
        }
        memcpy(site, to, sizeof(kExpected));
        VirtualProtect(site, sizeof(kExpected), previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(kExpected));
        s_patched = on;
        WOWVR_INFO("Chat bubbles: units with lifebars %s.",
                   on ? "now get bubbles too" : "get no bubble again (the client's own rule)");
    }

    int WorldPointer::BubbleCount() const
    {
        return g_bubbleCount;
    }

    const WorldBubbleCard& WorldPointer::Bubble(int index) const
    {
        return g_bubbles[index];
    }

    int WorldPointer::TextCount() const
    {
        return g_textCount;
    }

    const WorldTextCard& WorldPointer::Text(int index) const
    {
        return g_texts[index];
    }

    unsigned long long WorldPointer::TextCalls() const
    {
        return g_textCalls;
    }

    unsigned long long WorldPointer::BubbleCalls() const
    {
        return g_bubbleCalls;
    }

    unsigned long long WorldPointer::BubbleReads() const
    {
        return g_bubbleReads;
    }

    bool WorldPointer::IsActive() const
    {
        return g_installed && g_state.active;
    }

    bool WorldPointer::WorldToBodyNow(const Vec3& world, Vec3& body) const
    {
        if (!IsActive())
        {
            return false;
        }
        CameraBasis camera;
        if (!ReadCamera(CurrentWorldFrame(), camera))
        {
            return false;
        }
        pointing::WorldToBody(g_state.frame, camera, world, body);
        return true;
    }

    void WorldPointer::Deactivate()
    {
        g_state.active = false;
    }

    void WorldPointer::LogStatus() const
    {
        WOWVR_INFO("World pointing: %s; %llu pointer rays (last at u %.3f v %.3f, %.1f deg "
                   "off the camera's axis), "
                   "%llu projections, %llu on the panel, %llu on the camera-object fallback "
                   "basis.",
                   g_installed ? (g_state.active ? "active" : "installed, idle")
                               : (g_installFailed ? "not installed" : "pending"),
                   g_rayCalls, g_lastRayU, g_lastRayV, g_lastRayOffAxisDegrees,
                   g_projectCalls, g_projectOnPanel, g_basisFallbacks);
        g_basisFallbacks = 0;
        g_rayCalls = 0;
        g_projectCalls = 0;
        g_projectOnPanel = 0;
    }

    namespace pointing
    {
        bool PanelToWorldRay(const Frame& frame, const CameraBasis& camera, float u, float v,
                             Vec3& originRelative, Vec3& direction)
        {
            const Vec3 onPanel = frame.shape.BodyPoint(u, v);
            Vec3 body = { onPanel.x - frame.headBody.x, onPanel.y - frame.headBody.y,
                          onPanel.z - frame.headBody.z };
            const float length = Length(body);
            if (!(length > 1.0e-4f))
            {
                return false;
            }
            body.x /= length;
            body.y /= length;
            body.z /= length;

            // Body metres -> game view yards -> world. The head sits at headBody in the
            // body frame, whose origin is the camera (the neutral head position).
            const float upm = frame.unitsPerMetre;
            const Vec3 headYards = { frame.headBody.x * upm, frame.headBody.y * upm,
                                     frame.headBody.z * upm };
            originRelative = ViewToWorldDirection(
                camera, Mat4TransformDirection(headYards, frame.bodyToGameView));
            direction = ViewToWorldDirection(
                camera, Mat4TransformDirection(body, frame.bodyToGameView));
            return true;
        }

        void WorldToBody(const Frame& frame, const CameraBasis& camera, const Vec3& world,
                         Vec3& body)
        {
            const Vec3 relative = { world.x - camera.position.x, world.y - camera.position.y,
                                    world.z - camera.position.z };
            const Vec3 viewYards = WorldToViewDirection(camera, relative);
            const Vec3 bodyYards = Mat4TransformDirection(viewYards, frame.gameViewToBody);
            const float upm = frame.unitsPerMetre;
            body = { bodyYards.x / upm, bodyYards.y / upm, bodyYards.z / upm };
        }

        bool WorldToPanel(const Frame& frame, const CameraBasis& camera, const Vec3& world,
                          float& u, float& v, float& distanceYards)
        {
            // The point in the camera's view space, then the body frame, in metres.
            const Vec3 relative = { world.x - camera.position.x, world.y - camera.position.y,
                                    world.z - camera.position.z };
            const Vec3 viewYards = WorldToViewDirection(camera, relative);
            const Vec3 bodyYards = Mat4TransformDirection(viewYards, frame.gameViewToBody);
            const float upm = frame.unitsPerMetre;
            const Vec3 body = { bodyYards.x / upm, bodyYards.y / upm, bodyYards.z / upm };

            const Vec3 fromHead = { body.x - frame.headBody.x, body.y - frame.headBody.y,
                                    body.z - frame.headBody.z };
            const float distanceMetres = Length(fromHead);
            if (!(distanceMetres > 1.0e-4f))
            {
                return false;
            }
            if (!frame.shape.IntersectBody(frame.headBody, fromHead, u, v))
            {
                return false;
            }
            distanceYards = distanceMetres * upm;
            return true;
        }
    }

    WorldPointer& Pointer()
    {
        static WorldPointer instance;
        return instance;
    }
}
