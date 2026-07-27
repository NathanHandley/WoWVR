#include "present/gl_interop.h"

#include "core/log.h"

#include <windows.h>

#include <GL/gl.h>

// WGL_NV_DX_interop. Not in the SDK headers, so declared here from the extension
// specification. Despite the NV suffix it is also implemented by AMD and Intel.
#define WGL_ACCESS_READ_ONLY_NV 0x0000

typedef BOOL   (WINAPI *PFNWGLDXSETRESOURCESHAREHANDLENVPROC)(void* dxObject, HANDLE shareHandle);
typedef HANDLE (WINAPI *PFNWGLDXOPENDEVICENVPROC)(void* dxDevice);
typedef BOOL   (WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE device);
typedef HANDLE (WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(HANDLE device, void* dxObject,
                                                      GLuint name, GLenum type, GLenum access);
typedef BOOL   (WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE device, HANDLE object);
typedef BOOL   (WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE device, GLint count, HANDLE* objects);
typedef BOOL   (WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE device, GLint count, HANDLE* objects);

namespace wowvr
{
    namespace
    {
        PFNWGLDXOPENDEVICENVPROC       g_wglDXOpenDeviceNV = nullptr;
        PFNWGLDXCLOSEDEVICENVPROC      g_wglDXCloseDeviceNV = nullptr;
        PFNWGLDXREGISTEROBJECTNVPROC   g_wglDXRegisterObjectNV = nullptr;
        PFNWGLDXUNREGISTEROBJECTNVPROC g_wglDXUnregisterObjectNV = nullptr;
        PFNWGLDXLOCKOBJECTSNVPROC      g_wglDXLockObjectsNV = nullptr;
        PFNWGLDXUNLOCKOBJECTSNVPROC    g_wglDXUnlockObjectsNV = nullptr;

        const wchar_t* const kWindowClass = L"WoWVRGlInterop";

        bool LoadInteropProcs()
        {
            g_wglDXOpenDeviceNV = reinterpret_cast<PFNWGLDXOPENDEVICENVPROC>(
                wglGetProcAddress("wglDXOpenDeviceNV"));
            g_wglDXCloseDeviceNV = reinterpret_cast<PFNWGLDXCLOSEDEVICENVPROC>(
                wglGetProcAddress("wglDXCloseDeviceNV"));
            g_wglDXRegisterObjectNV = reinterpret_cast<PFNWGLDXREGISTEROBJECTNVPROC>(
                wglGetProcAddress("wglDXRegisterObjectNV"));
            g_wglDXUnregisterObjectNV = reinterpret_cast<PFNWGLDXUNREGISTEROBJECTNVPROC>(
                wglGetProcAddress("wglDXUnregisterObjectNV"));
            g_wglDXLockObjectsNV = reinterpret_cast<PFNWGLDXLOCKOBJECTSNVPROC>(
                wglGetProcAddress("wglDXLockObjectsNV"));
            g_wglDXUnlockObjectsNV = reinterpret_cast<PFNWGLDXUNLOCKOBJECTSNVPROC>(
                wglGetProcAddress("wglDXUnlockObjectsNV"));

            return g_wglDXOpenDeviceNV != nullptr
                && g_wglDXCloseDeviceNV != nullptr
                && g_wglDXRegisterObjectNV != nullptr
                && g_wglDXUnregisterObjectNV != nullptr
                && g_wglDXLockObjectsNV != nullptr
                && g_wglDXUnlockObjectsNV != nullptr;
        }
    }

    bool GlInterop::Init(IDirect3DDevice9* gameDevice)
    {
        if (IsActive())
        {
            return true;
        }
        if (gameDevice == nullptr)
        {
            return false;
        }

        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kWindowClass;
        // Repeated registration fails harmlessly once the class exists.
        RegisterClassW(&windowClass);

        HWND window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 1, 1,
                                      nullptr, nullptr, windowClass.hInstance, nullptr);
        if (window == nullptr)
        {
            WOWVR_WARN("GL interop: hidden window creation failed (%lu).", GetLastError());
            return false;
        }
        m_window = window;
        m_dc = GetDC(window);

        PIXELFORMATDESCRIPTOR descriptor = {};
        descriptor.nSize = sizeof(descriptor);
        descriptor.nVersion = 1;
        descriptor.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW;
        descriptor.iPixelType = PFD_TYPE_RGBA;
        descriptor.cColorBits = 32;

        HDC dc = static_cast<HDC>(m_dc);
        const int format = ChoosePixelFormat(dc, &descriptor);
        if (format == 0 || !SetPixelFormat(dc, format, &descriptor))
        {
            WOWVR_WARN("GL interop: no usable pixel format (%lu).", GetLastError());
            Shutdown();
            return false;
        }

        HGLRC glContext = wglCreateContext(dc);
        if (glContext == nullptr || !wglMakeCurrent(dc, glContext))
        {
            WOWVR_WARN("GL interop: context creation failed (%lu).", GetLastError());
            if (glContext != nullptr)
            {
                wglDeleteContext(glContext);
            }
            Shutdown();
            return false;
        }
        m_glContext = glContext;

        if (!LoadInteropProcs())
        {
            WOWVR_WARN("GL interop: WGL_NV_DX_interop is not exposed by this driver.");
            Shutdown();
            return false;
        }

        m_interopDevice = g_wglDXOpenDeviceNV(gameDevice);
        if (m_interopDevice == nullptr)
        {
            WOWVR_WARN("GL interop: wglDXOpenDeviceNV refused the game's device (%lu).",
                       GetLastError());
            Shutdown();
            return false;
        }

        WOWVR_INFO("GL interop open: %s, %s.",
                   reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                   reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        return true;
    }

    void GlInterop::Shutdown()
    {
        UnregisterEyeTextures();

        if (m_interopDevice != nullptr)
        {
            g_wglDXCloseDeviceNV(m_interopDevice);
            m_interopDevice = nullptr;
        }
        if (m_glContext != nullptr)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(static_cast<HGLRC>(m_glContext));
            m_glContext = nullptr;
        }
        if (m_window != nullptr)
        {
            if (m_dc != nullptr)
            {
                ReleaseDC(static_cast<HWND>(m_window), static_cast<HDC>(m_dc));
                m_dc = nullptr;
            }
            DestroyWindow(static_cast<HWND>(m_window));
            m_window = nullptr;
        }
    }

    bool GlInterop::EnsureContextCurrent()
    {
        if (m_glContext == nullptr)
        {
            return false;
        }
        if (wglGetCurrentContext() == static_cast<HGLRC>(m_glContext))
        {
            return true;
        }
        return wglMakeCurrent(static_cast<HDC>(m_dc), static_cast<HGLRC>(m_glContext)) != 0;
    }

    bool GlInterop::RegisterEyeTexture(int eye, IDirect3DTexture9* texture)
    {
        if (!IsActive() || eye < 0 || eye >= 2 || texture == nullptr || !EnsureContextCurrent())
        {
            return false;
        }

        if (m_interopObject[eye] != nullptr)
        {
            g_wglDXUnregisterObjectNV(m_interopDevice, m_interopObject[eye]);
            m_interopObject[eye] = nullptr;
        }
        if (m_glTexture[eye] == 0)
        {
            glGenTextures(1, &m_glTexture[eye]);
        }

        m_interopObject[eye] = g_wglDXRegisterObjectNV(
            m_interopDevice, texture, m_glTexture[eye], GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
        if (m_interopObject[eye] == nullptr)
        {
            WOWVR_WARN("GL interop: registering the %s eye texture failed (%lu).",
                       eye == 0 ? "left" : "right", GetLastError());
            return false;
        }

        return true;
    }

    void GlInterop::UnregisterEyeTextures()
    {
        EnsureContextCurrent();
        for (int eye = 0; eye < 2; ++eye)
        {
            if (m_interopObject[eye] != nullptr && m_interopDevice != nullptr)
            {
                g_wglDXUnregisterObjectNV(m_interopDevice, m_interopObject[eye]);
                m_interopObject[eye] = nullptr;
            }
            if (m_glTexture[eye] != 0)
            {
                glDeleteTextures(1, &m_glTexture[eye]);
                m_glTexture[eye] = 0;
            }
        }
    }

    bool GlInterop::LockEyes()
    {
        if (!IsActive() || m_interopObject[0] == nullptr || m_interopObject[1] == nullptr
            || !EnsureContextCurrent())
        {
            return false;
        }

        HANDLE objects[2] = { m_interopObject[0], m_interopObject[1] };
        if (!g_wglDXLockObjectsNV(m_interopDevice, 2, objects))
        {
            if (!m_lockFailureLogged)
            {
                WOWVR_ERROR("GL interop: locking the eye textures failed (%lu); "
                            "no frames will reach the headset.", GetLastError());
                m_lockFailureLogged = true;
            }
            return false;
        }
        return true;
    }

    void GlInterop::UnlockEyes()
    {
        if (!IsActive() || m_interopObject[0] == nullptr || m_interopObject[1] == nullptr)
        {
            return;
        }
        HANDLE objects[2] = { m_interopObject[0], m_interopObject[1] };
        g_wglDXUnlockObjectsNV(m_interopDevice, 2, objects);
    }
}
