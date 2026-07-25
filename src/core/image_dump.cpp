#include "core/image_dump.h"

#include "core/log.h"

#include <windows.h>

namespace wowvr
{
    bool SaveBgraBmp(const wchar_t* path, const void* pixels,
                     uint32_t width, uint32_t height, uint32_t rowPitch)
    {
        if (pixels == nullptr || width == 0 || height == 0)
        {
            return false;
        }

        const uint32_t destinationPitch = width * 4;
        const uint32_t imageBytes = destinationPitch * height;

        BITMAPFILEHEADER fileHeader = {};
        fileHeader.bfType = 0x4D42;  // "BM"
        fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = fileHeader.bfOffBits + imageBytes;

        BITMAPINFOHEADER infoHeader = {};
        infoHeader.biSize = sizeof(BITMAPINFOHEADER);
        infoHeader.biWidth = static_cast<LONG>(width);
        // Negative height stores the rows top-down, matching how the surface is laid
        // out, so a correct frame comes out the right way up.
        infoHeader.biHeight = -static_cast<LONG>(height);
        infoHeader.biPlanes = 1;
        infoHeader.biBitCount = 32;
        infoHeader.biCompression = BI_RGB;
        infoHeader.biSizeImage = imageBytes;

        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            WOWVR_WARN("Could not open %s for the frame dump: %s",
                       LogWide(path), LogSystemError(GetLastError()));
            return false;
        }

        DWORD written = 0;
        BOOL ok = WriteFile(file, &fileHeader, sizeof(fileHeader), &written, nullptr);
        ok = ok && WriteFile(file, &infoHeader, sizeof(infoHeader), &written, nullptr);

        const uint8_t* source = static_cast<const uint8_t*>(pixels);
        for (uint32_t row = 0; ok && row < height; ++row)
        {
            ok = WriteFile(file, source + static_cast<size_t>(row) * rowPitch,
                           destinationPitch, &written, nullptr);
        }

        CloseHandle(file);

        if (!ok)
        {
            WOWVR_WARN("Frame dump to %s failed part way through.", LogWide(path));
            return false;
        }

        WOWVR_INFO("Wrote frame dump %s (%ux%u).", LogWide(path), width, height);
        return true;
    }
}
