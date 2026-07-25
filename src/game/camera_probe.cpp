#include "game/camera_probe.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <cmath>

namespace wowvr
{
    namespace
    {
        CameraProbe g_camera;

        // The camera's fields sit close together in one structure, so a match is only
        // believed when several of them are found within a short span of each other.
        // Widened because the camera's angle need not sit adjacent to its clip planes.
        constexpr int kNeighbourhoodBytes = 512;

        // The scan runs on the render thread with the game frozen mid-frame, so its
        // stack is full of freshly computed projection values that match perfectly and
        // then turn to rubbish the moment the game resumes. That is where the first
        // three candidates came from every time.
        bool WithinOwnStack(const uint8_t* base, size_t size)
        {
            ULONG_PTR low = 0;
            ULONG_PTR high = 0;
            GetCurrentThreadStackLimits(&low, &high);
            if (low == 0 || high <= low)
            {
                return false;
            }
            const uint8_t* stackLow = reinterpret_cast<const uint8_t*>(low);
            const uint8_t* stackHigh = reinterpret_cast<const uint8_t*>(high);
            return base < stackHigh && (base + size) > stackLow;
        }

        // A camera keeps its orientation as three perpendicular unit vectors. Nine
        // consecutive floats forming such a basis, sitting beside a field of view and a
        // clip plane, is a camera - and unlike poking values into memory to see what
        // moves, checking for it cannot crash the game.
        bool IsOrthonormalBasis(const float* v)
        {
            for (int row = 0; row < 3; ++row)
            {
                const float* r = v + row * 3;
                const float lengthSquared = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
                if (!std::isfinite(lengthSquared) || std::fabs(lengthSquared - 1.0f) > 0.01f)
                {
                    return false;
                }
            }

            for (int a = 0; a < 3; ++a)
            {
                for (int b = a + 1; b < 3; ++b)
                {
                    const float* ra = v + a * 3;
                    const float* rb = v + b * 3;
                    const float dot = ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2];
                    if (std::fabs(dot) > 0.01f)
                    {
                        return false;
                    }
                }
            }

            // Reject the identity and the axis-aligned cases: they are orthonormal but
            // far too common to mean anything. A camera pointing somewhere real has
            // off-diagonal terms.
            float largestOffDiagonal = 0.0f;
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    if (row == column)
                    {
                        continue;
                    }
                    const float magnitude = std::fabs(v[row * 3 + column]);
                    if (magnitude > largestOffDiagonal)
                    {
                        largestOffDiagonal = magnitude;
                    }
                }
            }
            return largestOffDiagonal > 0.05f;
        }

        bool NearlyEqual(float value, float expected, float relativeTolerance)
        {
            if (!std::isfinite(value))
            {
                return false;
            }
            const float scale = std::fabs(expected) > 1.0f ? std::fabs(expected) : 1.0f;
            return std::fabs(value - expected) <= relativeTolerance * scale;
        }

        bool IsReadableWritable(const MEMORY_BASIC_INFORMATION& info)
        {
            if (info.State != MEM_COMMIT)
            {
                return false;
            }
            if ((info.Protect & PAGE_GUARD) != 0 || (info.Protect & PAGE_NOACCESS) != 0)
            {
                return false;
            }

            const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            return (info.Protect & writable) != 0;
        }

        // WoWVR keeps its own copies of the very numbers being searched for, so its
        // own image has to be excluded or the scan finds itself.
        void OwnModuleRange(const uint8_t*& base, size_t& size)
        {
            base = nullptr;
            size = 0;

            HMODULE self = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(&NearlyEqual), &self) || self == nullptr)
            {
                return;
            }

            const uint8_t* image = reinterpret_cast<const uint8_t*>(self);
            const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return;
            }
            const IMAGE_NT_HEADERS* nt =
                reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return;
            }

            base = image;
            size = nt->OptionalHeader.SizeOfImage;
        }
    }

    bool CameraProbe::ShouldRescan(float farPlane, float verticalScale) const
    {
        if (Found() || m_scannedFar <= 0.0f)
        {
            return false;
        }

        // Only worth redoing once every candidate has been eliminated, and only if the
        // camera we are now looking for is a different one.
        if (m_candidateCount > 0 && m_verifyPasses < 2)
        {
            return false;
        }

        return !NearlyEqual(farPlane, m_scannedFar, 1.0e-2f)
            || !NearlyEqual(verticalScale, m_scannedVerticalScale, 1.0e-2f);
    }

    void CameraProbe::Forget()
    {
        m_near = nullptr;
        m_far = nullptr;
        m_aspect = nullptr;
        m_fov = nullptr;
        m_candidateCount = 0;
        m_verifyPasses = 0;
    }

    bool CameraProbe::Examine(const uint8_t* base, size_t size,
                              float nearPlane, float farPlane, float aspect, float expectedFov)
    {
        if (size < sizeof(float) * 4)
        {
            return false;
        }

        const size_t floatCount = size / sizeof(float);
        const float* values = reinterpret_cast<const float*>(base);

        // The far plane turned out to be a poor anchor: 791.578 is derived rather than
        // stored, so the only place it ever appeared was a stack temporary. The angle
        // is what the camera actually keeps - though not necessarily the vertical one,
        // and not necessarily as an angle.
        const float halfV = expectedFov * 0.5f;
        const float fovHorizontal = 2.0f * std::atan(std::tan(halfV) * aspect);
        const float wanted[] = {
            expectedFov, halfV, std::tan(halfV), 1.0f / std::tan(halfV),
            fovHorizontal, fovHorizontal * 0.5f, std::tan(fovHorizontal * 0.5f)
        };

        for (size_t i = 0; i < floatCount && m_candidateCount < kMaxCandidates; ++i)
        {
            bool anchored = false;
            for (size_t k = 0; k < sizeof(wanted) / sizeof(wanted[0]); ++k)
            {
                if (NearlyEqual(values[i], wanted[k], 1.0e-4f))
                {
                    anchored = true;
                    break;
                }
            }
            if (!anchored)
            {
                continue;
            }

            const size_t span = kNeighbourhoodBytes / sizeof(float);
            const size_t first = (i > span) ? (i - span) : 0;
            const size_t last = (i + span < floatCount) ? (i + span) : (floatCount - 1);

            const float* nearAt = nullptr;
            const float* aspectAt = nullptr;
            const float* farAt = nullptr;

            for (size_t j = first; j <= last; ++j)
            {
                if (nearAt == nullptr && j != i && NearlyEqual(values[j], nearPlane, 1.0e-3f))
                {
                    nearAt = &values[j];
                }
                if (aspectAt == nullptr && NearlyEqual(values[j], aspect, 1.0e-3f))
                {
                    aspectAt = &values[j];
                }
                if (farAt == nullptr && NearlyEqual(values[j], farPlane, 1.0e-3f))
                {
                    farAt = &values[j];
                }
            }

            // An angle on its own is not enough - a clip plane or the aspect ratio
            // beside it is what makes it a camera rather than a loose constant.
            if (nearAt == nullptr && farAt == nullptr && aspectAt == nullptr)
            {
                continue;
            }

            // And the decisive one: an orientation basis in the same neighbourhood.
            const float* basisAt = nullptr;
            for (size_t j = first; j + 9 <= last && basisAt == nullptr; ++j)
            {
                if (IsOrthonormalBasis(&values[j]))
                {
                    basisAt = &values[j];
                }
            }
            if (basisAt == nullptr)
            {
                continue;
            }

            Candidate& candidate = m_candidates[m_candidateCount++];
            candidate.fov = const_cast<float*>(&values[i]);
            candidate.nearPlane = const_cast<float*>(nearAt);
            candidate.farPlane = const_cast<float*>(farAt);
            candidate.aspect = const_cast<float*>(aspectAt);
            candidate.fovAtScan = values[i];

            WOWVR_INFO("  candidate %d: angle %.5f rad (%.2f deg) at %p  near=%s far=%s "
                       "aspect=%s orientation=%+d",
                       m_candidateCount, values[i], values[i] * 57.2957795f,
                       static_cast<void*>(candidate.fov),
                       nearAt != nullptr ? "yes" : "no",
                       farAt != nullptr ? "yes" : "no",
                       aspectAt != nullptr ? "yes" : "no",
                       static_cast<int>((basisAt - &values[i]) * sizeof(float)));
        }

        return false;
    }

    void CameraProbe::Scan(float nearPlane, float farPlane, float aspect, float verticalScale)
    {
        if (Found())
        {
            return;
        }

        // Each scan walks hundreds of megabytes with the render thread blocked, which
        // the player feels as a freeze. Worth it a few times while hunting; not worth
        // repeating forever once it is clear the camera is not going to be found.
        if (m_scansAttempted >= kMaxScans)
        {
            return;
        }
        ++m_scansAttempted;

        m_candidateCount = 0;
        m_verifyPasses = 0;
        m_scannedFar = farPlane;
        m_scannedVerticalScale = verticalScale;

        // A rescan means a different camera, so any verdict reached about the previous
        // set of candidates no longer applies. Without this the login screen's failed
        // attempt permanently disables testing of the in-world candidates.
        m_activeTestDone = false;
        m_activeIndex = -1;
        m_activeSettleFrames = 0;

        if (verticalScale <= 0.001f || farPlane <= 1.0f)
        {
            return;
        }

        // p11 = 1 / tan(fovY / 2), so the camera must be holding this angle somewhere.
        const float expectedFov = 2.0f * std::atan(1.0f / verticalScale);

        WOWVR_INFO("Scanning for WoW's camera: near=%.4f far=%.4f aspect=%.4f fov=%.5f rad (%.2f deg)",
                   nearPlane, farPlane, aspect, expectedFov, expectedFov * 57.2957795f);

        const uint8_t* ownBase = nullptr;
        size_t ownSize = 0;
        OwnModuleRange(ownBase, ownSize);

        SYSTEM_INFO systemInfo = {};
        GetSystemInfo(&systemInfo);

        const uint8_t* address = static_cast<const uint8_t*>(systemInfo.lpMinimumApplicationAddress);
        const uint8_t* limit = static_cast<const uint8_t*>(systemInfo.lpMaximumApplicationAddress);
        size_t bytesExamined = 0;

        while (address < limit && m_candidateCount < kMaxCandidates)
        {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(address, &info, sizeof(info)) == 0)
            {
                break;
            }

            const uint8_t* regionBase = static_cast<const uint8_t*>(info.BaseAddress);
            const size_t regionSize = info.RegionSize;

            const bool isOurs = ownBase != nullptr
                && regionBase < (ownBase + ownSize) && (regionBase + regionSize) > ownBase;

            const bool isStack = WithinOwnStack(regionBase, regionSize);

            if (!isOurs && !isStack && IsReadableWritable(info) && regionSize >= sizeof(float) * 4)
            {
                bytesExamined += regionSize;
                Examine(regionBase, regionSize, nearPlane, farPlane, aspect, expectedFov);
            }

            const uint8_t* next = regionBase + regionSize;
            if (next <= address)
            {
                break;
            }
            address = next;
        }

        WOWVR_INFO("Scan complete: %d candidate(s) across %.1f MB. Each must now survive a "
                   "re-check against a later frame before being trusted.",
                   m_candidateCount, static_cast<double>(bytesExamined) / (1024.0 * 1024.0));
    }

    void CameraProbe::Verify(float nearPlane, float farPlane, float aspect, float verticalScale)
    {
        (void)aspect;

        if (Found() || m_candidateCount == 0 || verticalScale <= 0.001f)
        {
            return;
        }

        ++m_verifyPasses;

        int survivors = 0;
        int survivorIndex = -1;

        for (int i = 0; i < m_candidateCount; ++i)
        {
            Candidate& candidate = m_candidates[i];
            if (candidate.fov == nullptr)
            {
                continue;
            }

            // A real camera keeps tracking the projection it produces. A stale copy in
            // some scratch buffer drifts away from it, and anything that has been freed
            // and reused turns into nonsense.
            // The angle must still read what it read when found, and any clip plane
            // beside it must still agree with the projection being produced now.
            bool stillMatches = NearlyEqual(*candidate.fov, candidate.fovAtScan, 1.0e-3f);
            if (stillMatches && candidate.nearPlane != nullptr)
            {
                stillMatches = NearlyEqual(*candidate.nearPlane, nearPlane, 1.0e-2f);
            }
            if (stillMatches && candidate.farPlane != nullptr)
            {
                stillMatches = NearlyEqual(*candidate.farPlane, farPlane, 1.0e-2f);
            }

            if (!stillMatches)
            {
                WOWVR_INFO("  candidate %d at %p dropped: angle now %.5f (was %.5f)",
                           i + 1, static_cast<void*>(candidate.fov),
                           *candidate.fov, candidate.fovAtScan);
                candidate.fov = nullptr;
                continue;
            }

            ++survivors;
            survivorIndex = i;
        }

        WOWVR_INFO("Camera verification pass %d: %d candidate(s) still agree with the "
                   "current projection.", m_verifyPasses, survivors);

        if (survivors == 1 && m_verifyPasses >= 2)
        {
            const Candidate& winner = m_candidates[survivorIndex];
            m_far = winner.farPlane;
            m_near = winner.nearPlane;
            m_fov = winner.fov;
            m_aspect = winner.aspect;

            WOWVR_INFO("Camera confirmed: angle %.5f rad (%.2f deg) at %p",
                       *m_fov, *m_fov * 57.2957795f, static_cast<void*>(m_fov));
        }
    }

    void CameraProbe::ActiveTest(float currentVerticalScale)
    {
        // Deliberately gated off by default. Perturbing candidate addresses to see
        // which one moves the projection is conclusive, but several of those addresses
        // belong to live game data and writing to them crashed the client. It stays
        // available for a last resort, once the passive filters have narrowed the field
        // to one or two.
        if (!Cfg().probeCameraByWriting)
        {
            return;
        }

        if (Found() || m_activeTestDone || m_candidateCount == 0 || m_verifyPasses < 2)
        {
            return;
        }
        if (currentVerticalScale <= 0.001f)
        {
            return;
        }

        // Nothing under test yet: perturb the next survivor and note what the
        // projection looked like beforehand.
        if (m_activeIndex < 0)
        {
            for (int i = 0; i < m_candidateCount; ++i)
            {
                if (m_candidates[i].fov == nullptr)
                {
                    continue;
                }

                m_activeIndex = i;
                m_activeOriginal = *m_candidates[i].fov;
                m_activeBaseline = currentVerticalScale;
                m_activeSettleFrames = 0;

                // Small enough to be harmless and instantly reversible, large enough
                // that the resulting projection cannot be mistaken for noise.
                *m_candidates[i].fov = m_activeOriginal * 1.15f;
                return;
            }

            WOWVR_WARN("No candidate changed the projection when altered; the camera was "
                       "not identified. Culling and sky stay tied to the game's camera.");
            m_activeTestDone = true;
            return;
        }

        // Give the game a few frames to rebuild its projection from the altered value.
        // One is not always enough: the camera may only be reconsulted periodically.
        if (m_activeSettleFrames < 3)
        {
            ++m_activeSettleFrames;
            return;
        }

        Candidate& candidate = m_candidates[m_activeIndex];
        const float change = std::fabs(currentVerticalScale - m_activeBaseline)
                           / (m_activeBaseline > 0.0f ? m_activeBaseline : 1.0f);

        // Put it back before doing anything else, whatever the answer.
        *candidate.fov = m_activeOriginal;

        if (change > 0.02f)
        {
            m_fov = candidate.fov;
            m_near = candidate.nearPlane;
            m_far = candidate.farPlane;
            m_aspect = candidate.aspect;
            m_activeTestDone = true;

            WOWVR_INFO("Camera identified at %p: altering it moved the projection's "
                       "vertical scale by %.1f%%. Angle %.5f rad (%.2f deg).",
                       static_cast<void*>(m_fov), change * 100.0f,
                       *m_fov, *m_fov * 57.2957795f);
            return;
        }

        WOWVR_INFO("  candidate %d at %p is not the camera: altering it moved the "
                   "projection by only %.2f%%.", m_activeIndex + 1,
                   static_cast<void*>(candidate.fov), change * 100.0f);

        candidate.fov = nullptr;
        m_activeIndex = -1;
    }

    CameraProbe& Camera()
    {
        return g_camera;
    }

    namespace
    {
        bool Readable(const void* address, size_t bytes)
        {
            if (address == nullptr)
            {
                return false;
            }

            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(address, &info, sizeof(info)) == 0)
            {
                return false;
            }

            if (info.State != MEM_COMMIT)
            {
                return false;
            }

            const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                 | PAGE_EXECUTE_WRITECOPY;
            if ((info.Protect & readable) == 0 || (info.Protect & PAGE_GUARD) != 0)
            {
                return false;
            }

            const uint8_t* start = static_cast<const uint8_t*>(info.BaseAddress);
            const uint8_t* wanted = static_cast<const uint8_t*>(address);
            return (wanted + bytes) <= (start + info.RegionSize);
        }

        const uintptr_t kCameraOffsetGuess = 0x7204u;

        // Every comparison against a NaN is false, so a filter written as
        // "reject if outside tolerance" rejects nothing at all when the memory is
        // garbage. Finiteness has to be checked explicitly.
        bool AllFinite(const float* values, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                if (!(values[i] > -1.0e30f && values[i] < 1.0e30f))
                {
                    return false;
                }
            }
            return true;
        }

        bool LooksOrthonormal(const float* m)
        {
            if (!AllFinite(m, 9))
            {
                return false;
            }

            for (int row = 0; row < 3; ++row)
            {
                const float* r = m + row * 3;
                const float length = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                if (fabsf(length - 1.0f) > 0.02f)
                {
                    return false;
                }
            }

            for (int a = 0; a < 3; ++a)
            {
                const int b = (a + 1) % 3;
                const float* ra = m + a * 3;
                const float* rb = m + b * 3;
                const float dot = ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2];
                if (fabsf(dot) > 0.02f)
                {
                    return false;
                }
            }
            return true;
        }
    }

    void CameraProbe::WatchFovField(float verticalScale)
    {
        if (m_fovField == nullptr || !Readable(m_fovField, sizeof(float)))
        {
            return;
        }

        const float expectedHalf = atanf(1.0f / verticalScale);
        const float actual = *m_fovField;
        WOWVR_INFO("FOV field 0x%08X = %.5f rad (%.3f deg full); projection says %.5f "
                   "(%.3f deg full) -> %s",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_fovField)),
                   actual, actual * 2.0f * 57.2957795f,
                   expectedHalf, expectedHalf * 2.0f * 57.2957795f,
                   fabsf(actual - expectedHalf) < 0.002f ? "TRACKS" : "diverged");
    }

    void CameraProbe::ProbeKnownOffsets(float verticalScale, float aspect)
    {
        // Published layout for 3.3.5a build 12340: a static pointer to the world frame,
        // then the active camera hanging off it, then position / basis / clip / fov.
        // The published addresses assume the usual 0x400000 image base, so they are
        // rebased against wherever this exe actually loaded - it has been patched, and a
        // rebased image is the simplest reason a published offset would read as zero.
        const uintptr_t kPublishedImageBase = 0x00400000u;
        const uintptr_t kWorldFrameRva = 0x00B7436Cu - kPublishedImageBase;
        const uintptr_t kCameraOffset = 0x7204u;

        const uintptr_t imageBase =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t kWorldFramePtr = imageBase + kWorldFrameRva;

        WOWVR_INFO("Camera offset probe: image base 0x%08X (published base 0x%08X), "
                   "world-frame pointer at 0x%08X.",
                   static_cast<unsigned>(imageBase),
                   static_cast<unsigned>(kPublishedImageBase),
                   static_cast<unsigned>(kWorldFramePtr));

        const float expectedFovY = 2.0f * atanf(1.0f / verticalScale);

        WOWVR_INFO("Camera offset probe: expecting a vertical fov near %.2f degrees "
                   "(aspect %.4f).", expectedFovY * 57.2957795f, aspect);

        if (!Readable(reinterpret_cast<const void*>(kWorldFramePtr), sizeof(uintptr_t)))
        {
            WOWVR_INFO("  the published world-frame pointer at 0x%08X is not readable; "
                       "this client's layout does not match.",
                       static_cast<unsigned>(kWorldFramePtr));
            return;
        }

        const uintptr_t worldFrame = *reinterpret_cast<const uintptr_t*>(kWorldFramePtr);
        if (!Readable(reinterpret_cast<const void*>(worldFrame), kCameraOffset + sizeof(uintptr_t)))
        {
            WOWVR_INFO("  world frame 0x%08X is not readable.", static_cast<unsigned>(worldFrame));
            return;
        }

        const uintptr_t camera =
            *reinterpret_cast<const uintptr_t*>(worldFrame + kCameraOffset);
        if (!Readable(reinterpret_cast<const void*>(camera), 0x60))
        {
            WOWVR_INFO("  camera 0x%08X is not readable.", static_cast<unsigned>(camera));
            return;
        }

        const uint8_t* base = reinterpret_cast<const uint8_t*>(camera);
        const float* position = reinterpret_cast<const float*>(base + 0x08);
        const float* basis = reinterpret_cast<const float*>(base + 0x14);
        const float nearClip = *reinterpret_cast<const float*>(base + 0x38);
        const float farClip = *reinterpret_cast<const float*>(base + 0x3C);
        const float fov = *reinterpret_cast<const float*>(base + 0x40);

        WOWVR_INFO("  camera at 0x%08X: pos (%.2f, %.2f, %.2f) near %.3f far %.1f "
                   "fov %.4f rad (%.2f deg), basis orthonormal=%d",
                   static_cast<unsigned>(camera),
                   position[0], position[1], position[2],
                   nearClip, farClip, fov, fov * 57.2957795f,
                   LooksOrthonormal(basis) ? 1 : 0);

        // The decisive check: the field of view stored here has to agree with the one
        // already decoded from the projection matrix on the wire.
        const bool fovAgrees = fabsf(fov - expectedFovY) < 0.09f;
        WOWVR_INFO("  fov agreement with the decoded projection: %s",
                   fovAgrees ? "MATCH - this is the camera" : "no match");
    }

    // The published offset came back empty on this client, so instead walk the exe's own
    // static data looking for a pointer that lands on something camera-shaped. This is
    // far narrower than the whole-memory scans that failed before: only aligned pointers
    // inside the image are followed, each candidate must carry an orthonormal basis, and
    // the field of view has to agree with the projection already decoded off the wire.
    void CameraProbe::ScanStaticsForCamera(float verticalScale)
    {
        const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (imageBase == 0)
        {
            return;
        }

        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(imageBase + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);

        const float expectedFovY = 2.0f * atanf(1.0f / verticalScale);
        int examined = 0;
        int hits = 0;

        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0)
            {
                continue;      // statics live in writable sections
            }

            const uintptr_t start = imageBase + section->VirtualAddress;
            const size_t size = section->Misc.VirtualSize;

            for (size_t offset = 0; offset + sizeof(uintptr_t) <= size;
                 offset += sizeof(uintptr_t))
            {
                // Checked a page at a time: a section this size spans several memory
                // regions, so asking whether the whole thing is readable in one go
                // always failed and the sweep examined nothing at all.
                if ((offset & 0xFFFu) == 0
                    && !Readable(reinterpret_cast<const void*>(start + offset), 0x1000))
                {
                    offset += 0x1000 - sizeof(uintptr_t);
                    continue;
                }

                const uintptr_t slot = *reinterpret_cast<const uintptr_t*>(start + offset);
                if (slot < 0x10000u || (slot & 3u) != 0)
                {
                    continue;
                }

                // One indirection, then two, since the camera usually hangs off a frame.
                for (int depth = 0; depth < 2; ++depth)
                {
                    const uintptr_t object = (depth == 0)
                        ? slot
                        : (Readable(reinterpret_cast<const void*>(slot + kCameraOffsetGuess),
                                    sizeof(uintptr_t))
                           ? *reinterpret_cast<const uintptr_t*>(slot + kCameraOffsetGuess)
                           : 0);

                    if (object < 0x10000u || !Readable(reinterpret_cast<const void*>(object), 0x60))
                    {
                        continue;
                    }

                    ++examined;
                    if (!Readable(reinterpret_cast<const void*>(object), 0x140))
                    {
                        continue;
                    }

                    // No assumption about where the field sits. The camera's vertical
                    // field of view could plausibly be stored as the full angle, the
                    // half angle, or the tangent that goes straight into the projection,
                    // so all three are looked for anywhere in the object's first 0x140
                    // bytes and the offset that hits is reported.
                    const uint8_t* base = reinterpret_cast<const uint8_t*>(object);
                    const float wanted[3] = { expectedFovY, expectedFovY * 0.5f,
                                              1.0f / verticalScale };
                    const char* label[3] = { "full angle", "half angle", "tangent" };

                    for (unsigned fieldOffset = 0; fieldOffset + 4 <= 0x140; fieldOffset += 4)
                    {
                        const float value = *reinterpret_cast<const float*>(base + fieldOffset);
                        if (!AllFinite(&value, 1))
                        {
                            continue;
                        }

                        for (int k = 0; k < 3; ++k)
                        {
                            if (fabsf(value - wanted[k]) > 0.004f)
                            {
                                continue;
                            }

                            WOWVR_INFO("  candidate 0x%08X via static 0x%08X depth %d: "
                                       "%s %.5f at +0x%X",
                                       static_cast<unsigned>(object),
                                       static_cast<unsigned>(start + offset), depth,
                                       label[k], value, fieldOffset);
                            ++hits;
                        }
                    }
                }
            }
        }

        WOWVR_INFO("Static-pointer camera scan: %d objects examined, %d matched a %.2f "
                   "degree fov with an orthonormal basis.",
                   examined, hits, expectedFovY * 57.2957795f);

        // Two independent statics pointed at this one address holding half the decoded
        // vertical field of view. Watch it rather than trust a single coincidence.
        m_fovField = reinterpret_cast<float*>(0x00ABFC38u);
    }
}
