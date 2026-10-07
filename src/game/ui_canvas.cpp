#include "game/ui_canvas.h"

#include "core/log.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wowvr
{
    namespace
    {
        const uintptr_t kPublishedImageBase = 0x00400000u;

        // ClntObjMgrGetActivePlayer: the local player's GUID in edx:eax, 0 until the
        // character is in the world. Reads the object manager through the main thread's
        // TLS slot, so it must be called from the render thread (which is the main one).
        const uintptr_t kActivePlayerRva = 0x004D3790u - kPublishedImageBase;
        const uint8_t kActivePlayerEntry[7] = { 0x64u, 0x8Bu, 0x0Du, 0x2Cu, 0x00u, 0x00u, 0x00u };
        typedef unsigned long long(__cdecl* ActivePlayerFn)();

        // FrameScript_Execute(code, source, 0), cdecl - how the client runs its own
        // snippets (e.g. at 0x00819D1E).
        const uintptr_t kExecuteRva = 0x00819210u - kPublishedImageBase;
        const uint8_t kExecuteEntry[4] = { 0x55u, 0x8Bu, 0xECu, 0x51u };
        typedef void(__cdecl* ExecuteFn)(const char* code, const char* source, int unused);

        ActivePlayerFn g_activePlayer = nullptr;
        ExecuteFn g_execute = nullptr;
        bool g_installed = false;
        bool g_failed = false;

        // What the layout currently is, as far as we have set it.
        float g_appliedScale = 1.0f;
        unsigned g_framesUntilCheck = 0;

        // Idempotent: does nothing if the interface is already laid out for this factor.
        // WOWVR_CANVAS_SCALE remembers the UIParent scale we set, so a scale the client
        // has since put back (login, /reload, a UI-scale change) is recognised as the
        // new base and the layout is rebuilt from it.
        const char* const kApplyLua =
            "if InCombatLockdown() then return end "
            // Bags placed as if UIParent were the whole screen. MoveAnything's replacement
            // for the bag layout anchors bags GetScreenWidth() from UIParent's LEFT edge,
            // which on the wider canvas is past its right edge. Every bag frame's SetPoint
            // is hooked, so whoever places a bag, the correction happens in that same call,
            // before the bag is ever drawn. WOWVR_FIXBAG moves a bag only when it reaches
            // past UIParent's right edge, and by exactly the extra width, so its own
            // SetPoint (which comes back through the hook) changes nothing more. The whole
            // set is also checked on every pass of this script as a backstop.
            "if not WOWVR_FIXBAG then "
            "  WOWVR_FIXBAG=function(c) "
            "    if WOWVR_FIXING or not c:IsShown() then return end "
            "    local u=UIParent local d=GetScreenWidth()-u:GetWidth() if d<1 then return end "
            "    local p,r,rp,x,y=c:GetPoint(1) "
            "    if p~='BOTTOMLEFT' or r~=u or rp~='BOTTOMLEFT' or not x then return end "
            "    local k=u:GetEffectiveScale()/c:GetEffectiveScale() "
            "    if x+c:GetWidth()>u:GetWidth()*k+1 then "
            "      WOWVR_FIXING=1 c:SetPoint(p,r,rp,x-d*k,y) WOWVR_FIXING=nil end end "
            "  WOWVR_FIXBAGS=function() for i=1,(NUM_CONTAINER_FRAMES or 13) do "
            "    local c=_G['ContainerFrame'..i] if c then WOWVR_FIXBAG(c) end end end "
            "  for i=1,(NUM_CONTAINER_FRAMES or 13) do local c=_G['ContainerFrame'..i] "
            "    if c then hooksecurefunc(c,'SetPoint',WOWVR_FIXBAG) "
            "      c:HookScript('OnShow',WOWVR_FIXBAG) end end end "
            "WOWVR_FIXBAGS() "
            "local f=%.4f "
            "local u=UIParent "
            "local s=u:GetScale() "
            "if WOWVR_CANVAS_SCALE and math.abs(s-WOWVR_CANVAS_SCALE)<0.00001 "
            "  and WOWVR_CANVAS_F==f then return end "
            "local base=s "
            "if WOWVR_CANVAS_SCALE and math.abs(s-WOWVR_CANVAS_SCALE)<0.00001 then "
            "  base=WOWVR_CANVAS_BASE end "
            "local ws=WorldFrame:GetEffectiveScale() "
            "local W,H=WorldFrame:GetWidth()*ws,WorldFrame:GetHeight()*ws "
            "WorldFrame:SetScale(1/f) "
            "u:SetScale(base/f) "
            "u:ClearAllPoints() "
            "u:SetPoint('CENTER',WorldFrame,'CENTER') "
            "u:SetWidth(W/base) u:SetHeight(H/base) "
            "WOWVR_CANVAS_BASE=base WOWVR_CANVAS_F=f WOWVR_CANVAS_SCALE=u:GetScale() "
            "if updateContainerFrameAnchors then updateContainerFrameAnchors() end";

        const char* const kRestoreLua =
            "if InCombatLockdown() or not WOWVR_CANVAS_SCALE then return end "
            "local u=UIParent "
            "WorldFrame:SetScale(1) "
            "u:SetScale(WOWVR_CANVAS_BASE) "
            "u:ClearAllPoints() "
            "u:SetPoint('TOPLEFT',WorldFrame,'TOPLEFT') "
            "u:SetPoint('BOTTOMRIGHT',WorldFrame,'BOTTOMRIGHT') "
            "WOWVR_CANVAS_SCALE=nil WOWVR_CANVAS_F=nil";

        uintptr_t ImageBase()
        {
            return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        }

        bool InWorld()
        {
            if (g_activePlayer == nullptr)
            {
                return false;
            }
            __try
            {
                return g_activePlayer() != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool Run(const char* code)
        {
            __try
            {
                g_execute(code, "WoWVR", 0);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    bool UiCanvas::RunDebugScript(const char* code)
    {
        if (!Install() || code == nullptr || !InWorld())
        {
            return false;
        }
        return Run(code);
    }

    bool UiCanvas::Install()
    {
        if (g_installed)
        {
            return true;
        }
        if (g_failed)
        {
            return false;
        }
        const uint8_t* player = reinterpret_cast<const uint8_t*>(ImageBase() + kActivePlayerRva);
        const uint8_t* execute = reinterpret_cast<const uint8_t*>(ImageBase() + kExecuteRva);
        if (memcmp(player, kActivePlayerEntry, sizeof(kActivePlayerEntry)) != 0
            || memcmp(execute, kExecuteEntry, sizeof(kExecuteEntry)) != 0)
        {
            g_failed = true;
            WOWVR_WARN("UI canvas: the client's player and script entry points are not the "
                       "3.3.5a (12340) code this was written against; the canvas stays as is.");
            return false;
        }
        g_activePlayer = reinterpret_cast<ActivePlayerFn>(const_cast<uint8_t*>(player));
        g_execute = reinterpret_cast<ExecuteFn>(const_cast<uint8_t*>(execute));
        g_installed = true;
        return true;
    }

    void UiCanvas::Update(float wanted)
    {
        if (!g_installed)
        {
            g_appliedScale = 1.0f;
            return;
        }
        if (!(wanted >= 1.0f)) { wanted = 1.0f; }
        if (wanted > 3.0f) { wanted = 3.0f; }

        if (!InWorld())
        {
            // The login screens: their interface is a different Lua state that knows
            // nothing of this, and the layout goes with the world it was set in.
            if (g_appliedScale != 1.0f)
            {
                WOWVR_INFO("UI canvas: left the world; ordinary panel.");
            }
            g_appliedScale = 1.0f;
            g_framesUntilCheck = 0;
            return;
        }

        // In the world: re-assert twice a second. Cheap (the script returns at once
        // when nothing changed) and catches the client resetting the scale.
        if (g_framesUntilCheck > 0 && wanted == g_appliedScale)
        {
            --g_framesUntilCheck;
            return;
        }
        g_framesUntilCheck = 45;

        if (wanted > 1.0f)
        {
            char code[4096];
            sprintf_s(code, kApplyLua, wanted);
            if (Run(code))
            {
                if (g_appliedScale != wanted)
                {
                    WOWVR_INFO("UI canvas: interface laid out on a %.1fx larger canvas.", wanted);
                }
                g_appliedScale = wanted;
            }
        }
        else if (g_appliedScale != 1.0f)
        {
            if (Run(kRestoreLua))
            {
                WOWVR_INFO("UI canvas: ordinary interface layout restored.");
            }
            g_appliedScale = 1.0f;
        }
    }

    float UiCanvas::PanelScale() const
    {
        return g_appliedScale;
    }

    UiCanvas& Canvas()
    {
        static UiCanvas instance;
        return instance;
    }
}
