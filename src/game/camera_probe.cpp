#include "game/camera_probe.h"

#include "core/config.h"
#include "core/log.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

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

    namespace
    {
        // Every field is initialised here rather than at each construction site, because
        // one of those sites forgot. CollectPage - the path the calibration actually uses -
        // filled in the first six fields and left the three scoring fields holding whatever
        // was on the heap, so the ranking that chooses the camera at the end was reading
        // garbage for all 75,000 candidates and could never pick anything. The layout
        // collector set all nine, which is why the fault only ever showed up on the page
        // route.
        struct DifferentialCandidate
        {
            float* address = nullptr;
            float value = 0.0f;
            int signWhenTurningRight = 0;   // 0 until a right turn has been seen
            float referenceDelta = 0.0f;    // how far it moved on the first turn
            int changes = 0;                // how often it moved while the camera moved
            int stillChanges = 0;           // how often it moved when the camera did not
            float deltaSum = 0.0f;          // calibration: how far it moved, summed
            float deltaSqSum = 0.0f;        // and the squares, for a spread
            int deltaCount = 0;
        };

        std::vector<DifferentialCandidate> g_differential;
        int g_differentialPass = 0;

        // Addresses that have already failed confirmation.
        //
        // Without this the search loops forever: collection is deterministic, so after a
        // rejection the very next sweep offers the same false positive first, and it is
        // rejected again. Remembering failures is what lets it move past them. Kept
        // across sweeps deliberately - that is the whole point.
        std::vector<const float*> g_rejected;

        bool AlreadyRejected(const float* address)
        {
            for (size_t i = 0; i < g_rejected.size(); ++i)
            {
                if (g_rejected[i] == address) { return true; }
            }
            return false;
        }

        // The camera object repeats its yaw at these offsets. Both came out of the
        // differential scan - two separate runs found the same spacing at completely
        // different base addresses - so this is measured, not a published layout.
        const uintptr_t kCameraObjectPointer = 0x00C1FA1Cu;
        const uintptr_t kCameraYawOffset = 0xDF4u;
        const uintptr_t kCameraYawMirrorA = 0x144u;
        const uintptr_t kCameraYawMirrorB = 0x148u;

        // The orbit radius sits 4 bytes before the yaw, with a copy here. Both offsets came
        // out of the zoom probe: eight wheel clicks moved them together, one yard each.
        const uintptr_t kCameraRadiusMirror = 0xCCu;

        // Pitch sits immediately after the yaw, with its own copy - the layout is
        // symmetric about the yaw. Both offsets were measured with a vertical drag.
        const uintptr_t kCameraPitchMirror = 0x114u;

        // Room for the whole candidate set. The tolerant layout test admits far more
        // places than exact equality did, and at 400,000 the collection was hitting the
        // cap and stopping - quite possibly before reaching the camera, which would
        // explain both the pool never narrowing and the implausible winner. A million
        // entries is about 36 MB, affordable even in a 32-bit process.
        const size_t kMaxLayoutCandidates = 1000000u;

        // How closely the three copies must agree. Loose enough to survive being sampled
        // mid-update, tight enough not to swamp the search.
        const float kLayoutTolerance = 0.01f;

        // Looser than kLayoutTolerance, and deliberately so: the signature sweep samples a
        // live object whose copies are updated one field at a time, so exact agreement is
        // the exception rather than the rule.
        const float kSignatureTolerance = 0.05f;

        // An angle in radians, however the client chooses to wrap it. Anything outside
        // this cannot be an orientation, which throws away the overwhelming majority of
        // floats before any of them have to be tracked.
        bool PlausibleAngle(float value)
        {
            return std::isfinite(value) && value >= -7.0f && value <= 7.0f;
        }

        // The exe's real extent, read from its own headers.
        //
        // This was previously assumed to be 0x00400000..0x01000000, which was never
        // checked. The assumption produced two "static" addresses that behaved perfectly
        // for a session and then read as zero on the next launch - because they were not
        // static at all, but heap allocations that happened to land inside the guessed
        // range. Anything identified under that assumption has to be treated as suspect.
        void ImageBounds(uintptr_t& begin, uintptr_t& end)
        {
            begin = 0x00400000u;
            end = 0x00400000u;

            HMODULE exe = GetModuleHandleW(nullptr);
            if (exe == nullptr)
            {
                return;
            }

            const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            {
                return;
            }

            const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
            {
                return;
            }

            begin = reinterpret_cast<uintptr_t>(exe);
            end = begin + nt->OptionalHeader.SizeOfImage;
        }

        // The client's own writable static data.
        void ForEachWritableStaticPage(void (*visit)(uint8_t*, size_t))
        {
            uintptr_t imageBase = 0;
            uintptr_t imageEnd = 0;
            ImageBounds(imageBase, imageEnd);

            uintptr_t address = imageBase;
            while (address < imageEnd)
            {
                MEMORY_BASIC_INFORMATION info = {};
                if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
                {
                    break;
                }

                const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                                     | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
                if (info.State == MEM_COMMIT && (info.Protect & writable) != 0
                    && (info.Protect & PAGE_GUARD) == 0)
                {
                    visit(static_cast<uint8_t*>(info.BaseAddress), info.RegionSize);
                }

                address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            }
        }

        // Searching the heap as well, without exhausting a 32-bit address space.
        //
        // The static scan found a faithful mirror of the camera yaw, but pinning it from
        // a dedicated thread moved nothing, so the client writes that copy and never
        // reads it. The authoritative camera is therefore heap-allocated, and the heap is
        // far too large to hold a 16-byte record for every float in it - that alone would
        // run to hundreds of megabytes inside a process that only has two gigabytes.
        //
        // So the heap is narrowed a page at a time first. One checksum per 4 KB page is
        // a quarter of a percent of the memory a per-float list would need, and the same
        // change-versus-stand-still logic applies: pages that move when the camera orbits
        // and stay put otherwise are the only ones worth examining float by float.
        // Scored rather than filtered.
        //
        // Hard filtering discarded the camera's own page: that object holds per-frame
        // state as well as the yaw, so it keeps changing during intervals when the
        // player is not touching the camera, and a pass that demands perfect stillness
        // throws away exactly what it is meant to preserve. Counting hits instead
        // tolerates that - what matters is that a page changes on almost every motion
        // interval and hardly ever otherwise.
        struct PageRecord
        {
            const uint8_t* base;
            uint32_t checksum;
            uint16_t movedHits;
            uint16_t stillHits;
        };

        std::vector<PageRecord> g_pages;

        uint32_t ChecksumPage(const uint8_t* page)
        {
            // FNV-1a over the page, stepped by four bytes. Exactness does not matter;
            // only that a changed page reliably produces a different number.
            uint32_t hash = 2166136261u;
            const uint32_t* words = reinterpret_cast<const uint32_t*>(page);
            for (int i = 0; i < 1024; ++i)
            {
                hash ^= words[i];
                hash *= 16777619u;
            }
            return hash;
        }

        bool WorthSearching(const MEMORY_BASIC_INFORMATION& info)
        {
            if (info.State != MEM_COMMIT)
            {
                return false;
            }

            const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if ((info.Protect & writable) == 0 || (info.Protect & PAGE_GUARD) != 0)
            {
                return false;
            }

            // Our own module holds copies of the very values being looked for, and the
            // calling thread's stack is full of transient camera arithmetic that matches
            // anything and then evaporates.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&ChecksumPage), &self);
            const uintptr_t base = reinterpret_cast<uintptr_t>(info.AllocationBase);
            if (base == reinterpret_cast<uintptr_t>(self))
            {
                return false;
            }

            ULONG_PTR stackLow = 0;
            ULONG_PTR stackHigh = 0;
            GetCurrentThreadStackLimits(&stackLow, &stackHigh);
            const uintptr_t regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
            const uintptr_t regionEnd = regionStart + info.RegionSize;
            if (regionEnd > stackLow && regionStart < stackHigh)
            {
                return false;
            }

            return true;
        }

        // Reads the three copies without asking the kernel whether the memory is valid.
        //
        // The obvious guard - VirtualQuery before each read - is a system call, and with
        // a few hundred thousand candidates checked several times a second it cost half a
        // second of stalled frames every pass. The addresses all came from committed
        // regions; the only real risk is a region being freed underneath us, which an
        // exception handler covers for nothing when it does not happen.
        //
        // No C++ objects in here: __try cannot coexist with anything needing unwinding.
        __declspec(noinline) bool ReadTriple(const float* address, float& value,
                                             float& copyA, float& copyB)
        {
            __try
            {
                value = address[0];
                copyA = address[kCameraYawMirrorA / sizeof(float)];
                copyB = address[kCameraYawMirrorB / sizeof(float)];
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        // One region swept for the camera signature, behind an exception handler.
        //
        // A region reported as committed can still be freed by another thread between the
        // VirtualQuery and the read, and that took the client down the first time this ran.
        // Guarding per read with VirtualQuery would be a syscall per candidate; a handler
        // costs nothing until it is needed, and losing the tail of one region when it does
        // fire is of no consequence.
        //
        // No C++ objects in here - __try cannot coexist with anything needing unwinding -
        // so hits go into a caller-supplied array and the vector is filled outside.
        const size_t kSignatureHitsPerRegion = 256;

        __declspec(noinline) size_t ScanRegionForSignature(uint8_t* base, size_t size,
                                                           float** out, size_t maxOut)
        {
            size_t found = 0;
            __try
            {
                const size_t before = sizeof(float);
                const size_t after = kCameraYawMirrorB + sizeof(float);
                const size_t last = size - after;

                for (size_t offset = before; offset <= last && found < maxOut;
                     offset += sizeof(float))
                {
                    // A real heading, not approximately nothing.
                    //
                    // Testing only for exact zero is not enough once the copies are
                    // compared loosely: a region of zeroed memory satisfies every
                    // "these fields agree" test trivially, and allowing a zero orbit
                    // radius removed the last thing excluding it. That is how this sweep
                    // returned 204,892 candidates beginning at 0x00010008, and how a
                    // buffer holding -0.0056 rad came to be verified as the camera.
                    float* yaw = reinterpret_cast<float*>(base + offset);
                    if (!PlausibleAngle(*yaw) || fabsf(*yaw) < 0.01f)
                    {
                        continue;
                    }

                    // Every false positive that ever got written to was an exact binary
                    // fraction - 1.0000, 0.7500, 0.1250. An angle arrived at by turning a
                    // camera is essentially never an exact eighth.
                    if (*yaw * 8.0f == floorf(*yaw * 8.0f))
                    {
                        continue;
                    }

                    const float* copyA =
                        reinterpret_cast<const float*>(base + offset + kCameraYawMirrorA);
                    const float* copyB =
                        reinterpret_cast<const float*>(base + offset + kCameraYawMirrorB);
                    if (fabsf(*copyA - *yaw) > kSignatureTolerance
                        || fabsf(*copyB - *yaw) > kSignatureTolerance)
                    {
                        continue;
                    }

                    // Radius may legitimately be zero - that is first person, not a
                    // mismatch - and the copies are compared loosely, because the client
                    // updates them one at a time and a sample caught mid-update disagrees.
                    // Both mistakes were made the first time this sweep was tried and
                    // between them they excluded the camera from its own candidate set.
                    const float* radius = reinterpret_cast<const float*>(base + offset - before);
                    const float* radiusCopy =
                        reinterpret_cast<const float*>(base + offset + kCameraRadiusMirror);
                    if (!std::isfinite(*radius) || *radius < 0.0f || *radius > 60.0f
                        || fabsf(*radiusCopy - *radius) > kSignatureTolerance)
                    {
                        continue;
                    }

                    // Pitch and its copy, the two fields the first attempt did not know
                    // about. Seven constrained fields is a far narrower net than five.
                    const float* pitch = reinterpret_cast<const float*>(base + offset + sizeof(float));
                    const float* pitchCopy =
                        reinterpret_cast<const float*>(base + offset + kCameraPitchMirror);
                    if (!std::isfinite(*pitch) || *pitch < -1.6f || *pitch > 1.6f
                        || fabsf(*pitchCopy - *pitch) > kSignatureTolerance)
                    {
                        continue;
                    }

                    out[found] = yaw;
                    ++found;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return found;
        }

        __declspec(noinline) size_t ScanRegionForValue(uint8_t* base, size_t size,
                                                       float wanted, float** out, size_t maxOut)
        {
            size_t found = 0;
            __try
            {
                const size_t count = size / sizeof(float);
                float* values = reinterpret_cast<float*>(base);
                for (size_t i = 0; i < count && found < maxOut; ++i)
                {
                    if (std::isfinite(values[i]) && fabsf(values[i] - wanted) < 0.0005f)
                    {
                        out[found] = &values[i];
                        ++found;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
            return found;
        }

        void CollectPage(uint8_t* base, size_t size)
        {
            const size_t count = size / sizeof(float);
            float* values = reinterpret_cast<float*>(base);
            for (size_t i = 0; i < count; ++i)
            {
                if (PlausibleAngle(values[i]))
                {
                    DifferentialCandidate candidate;
                    candidate.address = &values[i];
                    candidate.value = values[i];
                    candidate.signWhenTurningRight = 0;
                    candidate.referenceDelta = 0.0f;
                    candidate.changes = 0;
                    candidate.stillChanges = 0;
                    g_differential.push_back(candidate);
                }
            }
        }
    }

    // Collects every place in memory that carries the camera object's layout.
    //
    // This inverts the order the search was built in, and that was the mistake. Narrowing
    // by pages first kept discarding the camera's own page: the object holds per-frame
    // state as well as the yaw, so it is never quiet, and every version of the "stood
    // still" test threw it away.
    //
    // The layout is the better opening move precisely because it is cheap. Three equal
    // plausible angles at +0x144 and +0x148 matches a couple of hundred thousand places -
    // far too many to identify anything, but only a few megabytes to hold, and the camera
    // is guaranteed to be among them. Behaviour then picks it out of that set, which is
    // what behaviour is actually good at.
    void CameraProbe::CollectLayoutCandidates()
    {
        g_differential.clear();
        g_differentialPass = 0;

        uintptr_t address = 0x00010000u;
        const uintptr_t limit = 0x7FFF0000u;
        while (address < limit && g_differential.size() < kMaxLayoutCandidates)
        {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
            {
                break;
            }

            if (WorthSearching(info) && info.RegionSize > kCameraYawMirrorB + sizeof(float))
            {
                uint8_t* base = static_cast<uint8_t*>(info.BaseAddress);
                const size_t last = info.RegionSize - kCameraYawMirrorB - sizeof(float);
                for (size_t offset = 0; offset <= last; offset += sizeof(float))
                {
                    float* first = reinterpret_cast<float*>(base + offset);
                    if (!PlausibleAngle(*first) || *first == 0.0f)
                    {
                        continue;
                    }

                    // Compared with a tolerance, not exactly. The client updates the three
                    // copies one at a time, so a sample taken mid-update finds them
                    // briefly disagreeing - and demanding exact equality here excludes the
                    // real camera from the candidate set before the search even begins,
                    // which is what left it converging on coincidences.
                    const float* copyA =
                        reinterpret_cast<const float*>(base + offset + kCameraYawMirrorA);
                    const float* copyB =
                        reinterpret_cast<const float*>(base + offset + kCameraYawMirrorB);
                    if (fabsf(*copyA - *first) > kLayoutTolerance || fabsf(*copyB - *first) > kLayoutTolerance)
                    {
                        continue;
                    }

                    DifferentialCandidate candidate;
                    candidate.address = first;
                    candidate.value = *first;
                    candidate.signWhenTurningRight = 0;
                    candidate.referenceDelta = 0.0f;
                    candidate.changes = 0;
                    candidate.stillChanges = 0;
                    candidate.deltaSum = 0.0f;
                    candidate.deltaSqSum = 0.0f;
                    candidate.deltaCount = 0;
                    g_differential.push_back(candidate);

                    if (g_differential.size() >= kMaxLayoutCandidates) { break; }
                }
            }

            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }

        WOWVR_INFO("Camera layout candidates: %zu places carry three equal angles at "
                   "+0x144 and +0x148.", g_differential.size());
    }

    // The camera object identified by its whole measured layout at once.
    //
    // The old sweep looked only for three equal angles at +0, +0x144 and +0x148, which
    // matched around 280,000 places - so weak that every version of the search needed a
    // long behavioural phase afterwards to pick the real one out, and that phase is what
    // never converged during ordinary play and cost a whole-address-space walk on the
    // render thread every three quarters of a second.
    //
    // The zoom probe added two more fields to test: an orbit radius at -0x4 with a copy at
    // +0xCC. Five constrained fields together is a far more specific signature than three,
    // and it costs the same single pass. Whether that is specific enough to identify the
    // object outright is exactly what the logged count answers - so it is measured here
    // rather than assumed, before anything is built on top of it.
    //
    // Note the object must be in third person to be found: in first person the radius is
    // zero, and zero is far too common in memory to carry any signal, so it is excluded.
    void CameraProbe::ScanForCameraSignature()
    {
        g_differential.clear();
        g_differentialPass = 0;

        // The span touched around a candidate: 4 bytes before it for the radius, and
        // 0x148 plus a float after it for the second yaw copy.
        const size_t kBefore = sizeof(float);
        const size_t kAfter = kCameraYawMirrorB + sizeof(float);

        int regionsSearched = 0;
        uintptr_t address = 0x00010000u;
        const uintptr_t limit = 0x7FFF0000u;
        while (address < limit && g_differential.size() < kMaxLayoutCandidates)
        {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
            {
                break;
            }

            if (WorthSearching(info) && info.RegionSize > kBefore + kAfter)
            {
                ++regionsSearched;
                float* hits[kSignatureHitsPerRegion];
                const size_t found = ScanRegionForSignature(
                    static_cast<uint8_t*>(info.BaseAddress), info.RegionSize,
                    hits, kSignatureHitsPerRegion);

                for (size_t i = 0; i < found; ++i)
                {
                    DifferentialCandidate candidate;
                    candidate.address = hits[i];
                    candidate.value = *hits[i];
                    g_differential.push_back(candidate);
                }
            }

            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }

        WOWVR_INFO("Camera signature sweep over %d regions: %zu places carry a non-zero "
                   "heading at +0/+0x144/+0x148, a radius at -0x4/+0xCC and a pitch at "
                   "+0x4/+0x114.",
                   regionsSearched, g_differential.size());

        for (size_t i = 0; i < g_differential.size() && i < 12; ++i)
        {
            const float* at = g_differential[i].address;
            WOWVR_INFO("  candidate 0x%08X: yaw %.4f rad (%.1f deg), radius %.3f yards.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(at)),
                       *at, *at * 57.2957795f, *(at - 1));
        }
    }

    void CameraProbe::SnapshotPages()
    {
        g_pages.clear();
        g_differential.clear();
        g_differentialPass = 0;

        uintptr_t address = 0x00010000u;
        const uintptr_t limit = 0x7FFF0000u;
        while (address < limit)
        {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
            {
                break;
            }

            if (WorthSearching(info))
            {
                const uint8_t* start = static_cast<const uint8_t*>(info.BaseAddress);
                const size_t pages = info.RegionSize / 4096u;
                for (size_t i = 0; i < pages; ++i)
                {
                    PageRecord record;
                    record.base = start + i * 4096u;
                    record.checksum = ChecksumPage(record.base);
                    record.movedHits = 0;
                    record.stillHits = 0;
                    g_pages.push_back(record);
                }
            }

            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }

        WOWVR_INFO("Page scan: %zu pages (%zu MB) under watch.", g_pages.size(),
                   (g_pages.size() * 4096u) / (1024u * 1024u));
    }

    void CameraProbe::FilterPages(bool expectedToChange)
    {
        ++g_differentialPass;

        size_t kept = 0;
        for (size_t i = 0; i < g_pages.size(); ++i)
        {
            PageRecord& record = g_pages[i];
            if (!Readable(record.base, 4096))
            {
                continue;
            }

            const uint32_t now = ChecksumPage(record.base);
            if (now != record.checksum)
            {
                if (expectedToChange) { ++record.movedHits; }
                else                  { ++record.stillHits; }
            }
            record.checksum = now;

            g_pages[kept] = record;
            ++kept;
        }
        g_pages.resize(kept);
    }

    // Keeps pages that moved with the camera nearly every time and were otherwise quiet.
    void CameraProbe::SelectScoredPages(int motionRounds)
    {
        const int wanted = (motionRounds >= 4) ? (motionRounds - 1) : motionRounds;

        size_t kept = 0;
        for (size_t i = 0; i < g_pages.size(); ++i)
        {
            if (g_pages[i].movedHits < wanted || g_pages[i].stillHits > 1)
            {
                continue;
            }
            g_pages[kept] = g_pages[i];
            ++kept;
        }
        g_pages.resize(kept);

        WOWVR_INFO("Page scoring over %d motion rounds: %zu pages moved with the camera "
                   "at least %d times and were quiet otherwise.",
                   motionRounds, g_pages.size(), wanted);
    }

    void CameraProbe::PromotePagesToFloats()
    {
        g_differential.clear();
        for (size_t i = 0; i < g_pages.size(); ++i)
        {
            CollectPage(const_cast<uint8_t*>(g_pages[i].base), 4096);
        }
        g_differentialPass = 0;
        WOWVR_INFO("Promoted %zu surviving pages to %zu angle-shaped floats.",
                   g_pages.size(), g_differential.size());
    }

    void CameraProbe::SnapshotDifferential()
    {
        g_differential.clear();
        g_differential.reserve(1u << 20);
        g_differentialPass = 0;
        ForEachWritableStaticPage(&CollectPage);
        WOWVR_INFO("Differential scan: %zu angle-shaped floats to start from.",
                   g_differential.size());
    }

    void CameraProbe::FilterDifferential(int direction)
    {
        ++g_differentialPass;

        const bool expectedToChange = (direction != 0);

        size_t kept = 0;
        for (size_t i = 0; i < g_differential.size(); ++i)
        {
            DifferentialCandidate& candidate = g_differential[i];
            if (!Readable(candidate.address, sizeof(float)))
            {
                continue;
            }

            const float now = *candidate.address;
            if (!PlausibleAngle(now))
            {
                continue;
            }

            // An angle wraps, and a wrap looks like an enormous jump the other way.
            // Correcting for it here is what lets a genuine heading survive turning
            // past its own discontinuity.
            float delta = now - candidate.value;
            const float twoPi = 6.28318530718f;
            if (delta > 3.14159265f)       { delta -= twoPi; }
            else if (delta < -3.14159265f) { delta += twoPi; }

            const bool changed = fabsf(delta) > 0.004f;
            if (changed != expectedToChange)
            {
                continue;
            }

            if (expectedToChange)
            {
                // The discriminator that matters. A heading moves one way for a right
                // turn and the other way for a left turn, always. Anything DERIVED from
                // it - a sine, a cosine, a matrix element - changes sign depending on
                // where the angle currently is, not on which way the player turned, so
                // this is what separates the angle itself from its consequences.
                const int sign = (delta > 0.0f) ? 1 : -1;
                const int signIfRight = (direction > 0) ? sign : -sign;

                if (candidate.signWhenTurningRight == 0)
                {
                    candidate.signWhenTurningRight = signIfRight;
                }
                else if (candidate.signWhenTurningRight != signIfRight)
                {
                    continue;
                }

                // The discriminator that actually separates an angle from its own sine
                // and cosine. The client turns at a fixed rate, so equal-length turns
                // move a heading by equal amounts no matter which way it was already
                // facing. A sine or cosine does not: how far it moves depends on where
                // the angle currently is, because that is its derivative. Repeating the
                // same turn from different orientations therefore keeps headings and
                // discards everything computed from them.
                const float magnitude = fabsf(delta);
                if (candidate.referenceDelta == 0.0f)
                {
                    candidate.referenceDelta = magnitude;
                }
                // Loose on purpose. A synthesised drag does not deliver exactly the same
                // movement every time - the client samples the mouse on its own schedule
                // - and too tight a band throws away the real answer as noise. It did
                // exactly that once: the set reached a single candidate and then lost it
                // two passes later.
                else if (magnitude < candidate.referenceDelta * 0.55f
                      || magnitude > candidate.referenceDelta * 1.80f)
                {
                    continue;
                }
            }

            candidate.value = now;
            g_differential[kept] = candidate;
            ++kept;
        }
        g_differential.resize(kept);

        const char* what = (direction == 0) ? "stood still"
                         : (direction > 0)  ? "turned right" : "turned left";
        WOWVR_INFO("Differential pass %d (%s): %zu candidates remain.", g_differentialPass,
                   what, g_differential.size());

        // Report as soon as the set is small, rather than only at the end. A later pass
        // can eliminate the right answer on noise, and losing it unseen wastes the whole
        // run - the addresses are worth having even if nothing survives to the finish.
        if (!g_differential.empty() && g_differential.size() <= 10u)
        {
            for (size_t i = 0; i < g_differential.size(); ++i)
            {
                WOWVR_INFO("  surviving at pass %d: 0x%08X = %.5f rad (%.2f deg)",
                           g_differentialPass,
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_differential[i].address)),
                           g_differential[i].value,
                           g_differential[i].value * 57.2957795f);
            }
        }
    }

    int CameraProbe::DifferentialCandidates() const
    {
        return static_cast<int>(g_differential.size());
    }

    // The two headings the differential scan converged on. Written every frame while a
    // test is active, because the client recomputes its camera from its own input each
    // frame and would otherwise simply undo a one-off write.
    namespace
    {
        float* g_headingWriteTarget = nullptr;
        float g_headingWriteOffset = 0.0f;
        bool g_headingWriteActive = false;
    }

    // Holding the camera yaw at a constant from a thread of its own.
    //
    // Writing it once a frame at Present changed nothing, but that proves only that the
    // client rewrites it somewhere between there and the next frame's culling - not that
    // culling ignores it. Our earliest hook in a frame is already after culling has run,
    // so from the render thread that window is unreachable by construction. A separate
    // thread writing continuously does not care about frame boundaries: if any of its
    // writes land after the client's camera update and before it culls, the view will
    // visibly swing. That distinguishes "we cannot write at the right moment" from
    // "this value is not what the renderer reads", which are the two possibilities the
    // one-off test could not tell apart.
    namespace
    {
        HANDLE g_holdThread = nullptr;
        volatile LONG g_holdRunning = 0;
        volatile float g_holdValue = 0.0f;
        float* g_holdTarget = nullptr;

        // When the survivors are being held instead of a single fixed address. Heap
        // addresses differ from run to run, so a candidate found this session can only
        // be tested this session - there is nothing to hard-code.
        std::vector<DifferentialCandidate> g_held;

        DWORD WINAPI HoldHeadingThread(LPVOID)
        {
            while (InterlockedCompareExchange(&g_holdRunning, 1, 1) == 1)
            {
                if (g_holdTarget != nullptr)
                {
                    *g_holdTarget = g_holdValue;
                }
                for (size_t i = 0; i < g_held.size(); ++i)
                {
                    *g_held[i].address = g_held[i].value;
                }
                // Deliberately tight. This is a diagnostic that runs for a few seconds,
                // and the whole point is to hit a window inside the client's frame.
                YieldProcessor();
            }
            return 0;
        }
    }

    void CameraProbe::HoldSurvivors(bool on, float offsetRadians)
    {
        if (!on)
        {
            g_held.clear();
            HoldHeading(false, 0.0f);
            return;
        }

        g_held.clear();
        for (size_t i = 0; i < g_differential.size(); ++i)
        {
            if (!Readable(g_differential[i].address, sizeof(float)))
            {
                continue;
            }

            DifferentialCandidate held = g_differential[i];
            held.value = *held.address + offsetRadians;
            g_held.push_back(held);
            WOWVR_INFO("Holding 0x%08X at %.4f rad (was %.4f).",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(held.address)),
                       held.value, *held.address);
        }

        if (g_held.empty())
        {
            WOWVR_WARN("No surviving candidates to hold.");
            return;
        }

        if (g_holdThread == nullptr)
        {
            InterlockedExchange(&g_holdRunning, 1);
            g_holdThread = CreateThread(nullptr, 0, &HoldHeadingThread, nullptr, 0, nullptr);
        }
    }

    void CameraProbe::HoldHeading(bool on, float offsetRadians)
    {
        if (!on)
        {
            if (g_holdThread != nullptr)
            {
                InterlockedExchange(&g_holdRunning, 0);
                WaitForSingleObject(g_holdThread, 2000);
                CloseHandle(g_holdThread);
                g_holdThread = nullptr;
                g_holdTarget = nullptr;
            }
            WOWVR_INFO("Heading hold released.");
            return;
        }

        if (g_holdThread != nullptr)
        {
            return;
        }

        // Whatever the locator settled on this session. There is no fixed address to
        // fall back to - the object is allocated afresh every launch.
        float* target = m_cameraYawField;
        if (target == nullptr || !Readable(target, sizeof(float)) || !PlausibleAngle(*target))
        {
            WOWVR_WARN("Camera not located yet; nothing to hold.");
            return;
        }

        float wanted = *target + offsetRadians;
        const float twoPi = 6.28318530718f;
        while (wanted > 3.14159265f)  { wanted -= twoPi; }
        while (wanted < -3.14159265f) { wanted += twoPi; }

        g_holdTarget = target;
        g_holdValue = wanted;
        InterlockedExchange(&g_holdRunning, 1);
        g_holdThread = CreateThread(nullptr, 0, &HoldHeadingThread, nullptr, 0, nullptr);

        WOWVR_INFO("Holding camera yaw at %.4f rad (%.1f deg), was %.4f. If culling reads "
                   "this, the view will swing.", wanted, wanted * 57.2957795f, *target);
    }

    // Points the game's own camera wherever the head is looking, so that everything it
    // decides on the CPU - culling, tile streaming, level of detail - is decided for what
    // the headset actually shows rather than for where the mouse last pointed.
    //
    // The awkward part is not the write but the bookkeeping. The client may or may not
    // recompute this field from its own input before we next see it, and the two cases
    // need opposite handling: if our value survived, the offset we added is already in
    // there and must come off before a new one goes on, or it accumulates and the camera
    // spins. Rather than assume either way, compare against what we last wrote.
    void CameraProbe::AimAtHead(float headYawRadians)
    {
        if (m_cameraYawField == nullptr)
        {
            return;
        }

        float* copyA = m_cameraYawField + (kCameraYawMirrorA / sizeof(float));
        float* copyB = m_cameraYawField + (kCameraYawMirrorB / sizeof(float));
        if (!Readable(m_cameraYawField, sizeof(float)) || !Readable(copyB, sizeof(float)))
        {
            return;
        }

        const float current = *m_cameraYawField;
        if (!PlausibleAngle(current))
        {
            return;
        }

        const bool oursSurvived = m_haveAimed
            && fabsf(WrapRadians(current - m_lastAimWritten)) < 0.002f;
        const float base = oursSurvived ? (current - m_appliedYaw) : current;

        float wanted = base + headYawRadians;
        const float twoPi = 6.28318530718f;
        while (wanted >= twoPi) { wanted -= twoPi; }
        while (wanted < 0.0f)   { wanted += twoPi; }

        // All three copies, or the layout re-check sees them disagree next frame and
        // decides the object has been reallocated.
        *m_cameraYawField = wanted;
        *copyA = wanted;
        *copyB = wanted;

        m_lastAimWritten = wanted;
        m_appliedYaw = headYawRadians;
        m_haveAimed = true;
    }

    float CameraProbe::AppliedYaw() const
    {
        return m_haveAimed ? m_appliedYaw : 0.0f;
    }

    void CameraProbe::SetHeadingWrite(int which, float offsetRadians)
    {
        g_headingWriteActive = false;
        g_headingWriteTarget = nullptr;

        if (which == 0)
        {
            WOWVR_INFO("Heading write disabled.");
            return;
        }

        // Once per frame from the render thread, not from a spinning thread of its own.
        // The spinning version was only ever a diagnostic to rule out frame timing, and
        // hammering a live object from another thread took the client down with it.
        float* target = m_cameraYawField;
        if (target == nullptr || !Readable(target, sizeof(float)) || !PlausibleAngle(*target))
        {
            WOWVR_WARN("Camera not located yet; nothing to aim.");
            return;
        }

        g_headingWriteTarget = target;
        g_headingWriteOffset = offsetRadians;
        g_headingWriteActive = true;
        WOWVR_INFO("Aiming the camera: 0x%08X offset by %.3f rad (%.1f deg).",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(target)),
                   offsetRadians, offsetRadians * 57.2957795f);
    }

    void CameraProbe::ApplyHeadingWrite()
    {
        if (!g_headingWriteActive || g_headingWriteTarget == nullptr)
        {
            return;
        }

        // Re-check the object still looks like the camera before every write. If it has
        // been freed and something else now occupies the memory, writing an angle into it
        // is exactly how the client gets taken down.
        const float current = *g_headingWriteTarget;
        const float* copyA = g_headingWriteTarget + (kCameraYawMirrorA / sizeof(float));
        const float* copyB = g_headingWriteTarget + (kCameraYawMirrorB / sizeof(float));
        if (!PlausibleAngle(current) || !Readable(copyB, sizeof(float))
            || *copyA != current || *copyB != current)
        {
            return;
        }

        // Offset from whatever the client just computed, rather than an absolute value,
        // so the test shows displacement from the game's own heading and cannot wander.
        float wanted = current + g_headingWriteOffset;
        const float twoPi = 6.28318530718f;
        while (wanted > 3.14159265f)  { wanted -= twoPi; }
        while (wanted < -3.14159265f) { wanted += twoPi; }
        *g_headingWriteTarget = wanted;
    }

    // Working back from a heap address to something stable enough to use at startup.
    //
    // The camera object is allocated afresh every launch, so the addresses the scan finds
    // are worthless on their own. What does not move is the exe's static data, and
    // somewhere in it there has to be a pointer that reaches this object - that is how
    // the client itself finds it. So: look for any static word holding an address that
    // lands inside the object, and record how far into it that lands. A pointer plus a
    // fixed offset survives relaunching; an address does not.
    // The camera object, reached the way the client reaches it.
    //
    // A static slot holds the pointer, and the yaw sits at a fixed offset inside. Both
    // numbers came from the differential scan plus a pointer trace, so nothing here is a
    // published offset taken on trust - and every step is checked for readability, since
    // a stale or null pointer during a loading screen is normal rather than exceptional.

    // Finding the camera by the SHAPE of the object rather than by a pointer to it.
    //
    // The pointer trace was a false lead: it found a single static word holding an address
    // that happened to land in the right 4 KB, and following it on a later run reached a
    // field that sat at zero and never moved. One coincidental hit in a window that size
    // is not evidence.
    //
    // What is distinctive is the object's own layout. The camera keeps its yaw in three
    // places at fixed spacing - +0, +0x144 and +0x148 - and all three hold the same angle.
    // Three identical plausible angles at exactly that spacing is a strong signature, it
    // needs no chain to survive relaunching, and it checks itself: if the match is wrong,
    // the value will not track the camera.
    float* CameraProbe::FindCameraByShape()
    {
        // Shape alone is nowhere near specific enough - three equal plausible angles at
        // that spacing matched 214,087 places, because any run of repeated small floats
        // satisfies it. What makes it unique is matching the VALUE as well.
        //
        // The static mirror is the way in. It is useless to write, but it is a perfectly
        // reliable *reading* of the camera's yaw, so it says what number the real camera
        // object must be holding right now. Shape plus that value is a specific enough
        // signature to identify the object outright, and it re-checks itself every time
        // it is used because the value moves as the camera does.
        const float* mirror = reinterpret_cast<const float*>(0x00D38B3Cu);
        if (!Readable(mirror, sizeof(float)) || !PlausibleAngle(*mirror))
        {
            WOWVR_WARN("Camera mirror unreadable; cannot identify the camera object yet.");
            return nullptr;
        }

        // The mirror runs -pi..pi and the object runs 0..2pi, so compare wrapped.
        const float twoPi = 6.28318530718f;
        float wanted = *mirror;
        if (wanted < 0.0f) { wanted += twoPi; }

        float* best = nullptr;
        int matches = 0;

        uintptr_t address = 0x00010000u;
        const uintptr_t limit = 0x7FFF0000u;
        while (address < limit)
        {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
            {
                break;
            }

            if (WorthSearching(info) && info.RegionSize > kCameraYawMirrorB)
            {
                uint8_t* base = static_cast<uint8_t*>(info.BaseAddress);
                const size_t last = info.RegionSize - kCameraYawMirrorB - sizeof(float);
                for (size_t offset = 0; offset <= last; offset += sizeof(float))
                {
                    const float* first = reinterpret_cast<const float*>(base + offset);
                    if (!PlausibleAngle(*first) || fabsf(*first - wanted) > 0.002f)
                    {
                        continue;
                    }

                    const float* mirrorA =
                        reinterpret_cast<const float*>(base + offset + kCameraYawMirrorA);
                    const float* mirrorB =
                        reinterpret_cast<const float*>(base + offset + kCameraYawMirrorB);
                    if (*mirrorA != *first || *mirrorB != *first)
                    {
                        continue;
                    }

                    ++matches;
                    if (best == nullptr)
                    {
                        best = const_cast<float*>(first);
                    }
                }
            }

            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }

        WOWVR_INFO("Camera search for yaw %.5f: %d match%s%s.", wanted, matches,
                   (matches == 1) ? "" : "es",
                   (best != nullptr) ? "" : " - camera not located");
        return best;
    }

    float* CameraProbe::ResolveCameraYaw()
    {
        const uintptr_t* slot = reinterpret_cast<const uintptr_t*>(kCameraObjectPointer);
        if (!Readable(slot, sizeof(uintptr_t)))
        {
            return nullptr;
        }

        const uintptr_t object = *slot;
        if (object < 0x10000u)
        {
            return nullptr;
        }

        float* yaw = reinterpret_cast<float*>(object + kCameraYawOffset);
        if (!Readable(yaw, sizeof(float)) || !PlausibleAngle(*yaw))
        {
            return nullptr;
        }
        return yaw;
    }

    void CameraProbe::VerifyCameraChain()
    {
        float* yaw = FindCameraByShape();
        if (yaw == nullptr)
        {
            return;
        }

        WOWVR_INFO("Camera by shape: 0x%08X holding %.5f rad (%.2f deg).",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(yaw)),
                   *yaw, *yaw * 57.2957795f);
    }

    // Reading the camera's yaw without trusting any address.
    //
    // The mirror is useless to write but perfectly reliable to read, so it says what the
    // real camera object must be holding at this instant. Returns false before the world
    // exists, which is normal rather than an error.
    // Whether the player is moving the camera right now, decided from the input devices
    // rather than from anything in the game's memory.
    //
    // The previous version read a "mirror" address for this, which was a mistake twice
    // over: the address was not actually static, and using game memory to find game
    // memory is circular anyway. The mouse and keyboard are outside the process, cost
    // nothing to read, and cannot go stale.
    bool CameraProbe::CameraIsMoving() const
    {
        // Either mouse button held is how the camera is turned in this client, and the
        // keyboard turn keys do it too.
        const bool dragging = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
                           || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        const bool turning = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0
                          || (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0
                          || (GetAsyncKeyState('A') & 0x8000) != 0
                          || (GetAsyncKeyState('D') & 0x8000) != 0
                          || (GetAsyncKeyState('Q') & 0x8000) != 0
                          || (GetAsyncKeyState('E') & 0x8000) != 0;
        return dragging || turning;
    }

    // Keeps only candidates that still agree with the mirror.
    //
    // This replaces the change-detection used for the offline scan, and is far stronger.
    // Change-detection only knows that something moved; matching the mirror's value knows
    // *what it moved to*, and repeating that across samples taken at different camera
    // angles leaves nothing but the camera itself. It also needs no synthesised input and
    // no assumption about turn rate, so it works off ordinary play.
    // Keeps candidates that moved exactly when the camera did, AND that still carry the
    // camera object's layout: the same angle repeated at +0x144 and +0x148.
    //
    // Neither test is enough alone. The layout matched 214,087 places on its own, and
    // change-detection alone leaves every value derived from the camera. Together they
    // are specific, and neither needs a known address to start from.
    void CameraProbe::FilterByBehaviour(bool expectedToChange)
    {
        size_t kept = 0;
        for (size_t i = 0; i < g_differential.size(); ++i)
        {
            DifferentialCandidate& candidate = g_differential[i];

            float now = 0.0f;
            float copyA = 0.0f;
            float copyB = 0.0f;
            if (!ReadTriple(candidate.address, now, copyA, copyB))
            {
                continue;
            }

            if (!PlausibleAngle(now))
            {
                continue;
            }

            float delta = now - candidate.value;
            const float twoPi = 6.28318530718f;
            if (delta > 3.14159265f)       { delta -= twoPi; }
            else if (delta < -3.14159265f) { delta += twoPi; }

            // Scored, for the same reason the pages are: one stray sample must not be
            // able to discard the answer.
            const bool changed = fabsf(delta) > 0.004f;
            if (changed)
            {
                if (expectedToChange) { ++candidate.changes; }
                else                  { ++candidate.stillChanges; }
            }

            candidate.value = now;
            g_differential[kept] = candidate;
            ++kept;
        }
        g_differential.resize(kept);
    }

    void CameraProbe::UpdateAutoLocate()
    {
        if (m_locateState == LocateState::Locked)
        {
            // Keep checking the layout still holds. If the object is freed or reallocated
            // the three copies stop agreeing, and writing on regardless would be writing
            // into whatever now occupies that memory.
            if (++m_locateTick < 240) { return; }
            m_locateTick = 0;

            const float* copyA = m_cameraYawField + (kCameraYawMirrorA / sizeof(float));
            const float* copyB = m_cameraYawField + (kCameraYawMirrorB / sizeof(float));
            if (m_cameraYawField == nullptr || !Readable(m_cameraYawField, sizeof(float))
                || !Readable(copyB, sizeof(float))
                || !PlausibleAngle(*m_cameraYawField)
                || *copyA != *m_cameraYawField || *copyB != *m_cameraYawField)
            {
                WOWVR_WARN("Camera object no longer has the expected layout; searching again.");
                m_cameraYawField = nullptr;
                m_locateState = LocateState::WaitingForMotion;
                m_locatePasses = 0;
            }
            return;
        }

        if (m_locateState == LocateState::Failed)
        {
            return;
        }

        // Sampled rather than every frame: the camera has to have actually moved between
        // one sample and the next for the classification to mean anything.
        if (++m_locateTick < 45)
        {
            return;
        }
        m_locateTick = 0;

        // Sampled twice: the camera has to have been moving for the WHOLE interval for
        // the classification to be honest, so both ends of the interval are checked.
        const bool movingNow = CameraIsMoving();
        const bool moved = movingNow && m_wasMoving;
        const bool still = !movingNow && !m_wasMoving;
        m_wasMoving = movingNow;

        // An interval that started moving and ended still, or the reverse, says nothing
        // reliable about what changed during it.
        if (!moved && !still)
        {
            return;
        }

        if (m_locateState == LocateState::Confirming)
        {
            float value = 0.0f;
            float copyA = 0.0f;
            float copyB = 0.0f;
            const bool readable = ReadTriple(m_cameraYawField, value, copyA, copyB);
            const bool changed = readable
                && fabsf(WrapRadians(value - m_confirmLastValue)) > 0.004f;
            m_confirmLastValue = readable ? value : m_confirmLastValue;

            // It must move when the camera moves and hold when it does not. Either
            // failure is disqualifying: a value that drifts on its own is not a heading,
            // and one that ignores the camera is not this camera's.
            if (!readable || copyA != value || copyB != value
                || (moved && !changed) || (still && changed))
            {
                ++m_confirmFailures;
            }
            else if (moved)
            {
                ++m_confirmMoves;
            }

            if (m_confirmFailures > 0)
            {
                WOWVR_WARN("Candidate at 0x%08X failed confirmation; discarding it and "
                           "continuing the search. Nothing was written.",
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_cameraYawField)));
                if (g_rejected.size() < 64u)
                {
                    g_rejected.push_back(m_cameraYawField);
                }
                DiscardCandidate(m_cameraYawField);
                m_cameraYawField = nullptr;
                m_locateState = LocateState::NarrowingFloats;
                m_motionRounds = 0;
            }
            else if (m_confirmMoves >= 3)
            {
                m_locateState = LocateState::Locked;
                m_locateTick = 0;
                WOWVR_INFO("Camera confirmed at 0x%08X, holding %.4f rad (%.1f deg). "
                           "Yaw is now under our control.",
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_cameraYawField)),
                           value, value * 57.2957795f);
            }
            return;
        }

        switch (m_locateState)
        {
        case LocateState::WaitingForMotion:
            // Nothing can be classified until the camera is alive and moving.
            if (moved)
            {
                CollectLayoutCandidates();
                m_locateState = LocateState::NarrowingFloats;
                m_locatePasses = 0;
                m_motionRounds = 0;
            }
            break;

        case LocateState::NarrowingFloats:
            FilterByBehaviour(moved);
            ++m_locatePasses;
            if (moved) { ++m_motionRounds; }
            if (DifferentialCandidates() == 0)
            {
                WOWVR_WARN("Camera search lost every candidate; starting again.");
                m_locateState = LocateState::WaitingForMotion;
                m_locatePasses = 0;
                m_motionRounds = 0;
            }
            else if (m_motionRounds >= 4)
            {
                // Every candidate already had the layout when it was collected; this
                // re-checks it now, so anything whose copies have since diverged - a
                // coincidence rather than the camera - is dropped.
                m_cameraYawField = FindLayoutMatch(true);
                if (m_cameraYawField != nullptr)
                {
                    // Nothing is written yet. A candidate that has passed every test so
                    // far can still be wrong, and writing to the wrong object corrupts
                    // whatever really owns it - that is exactly what happened when this
                    // settled on a buffer holding 1.0 and started writing head yaw into
                    // it. So it now has to prove itself while still read-only.
                    m_locateState = LocateState::Confirming;
                    m_locateTick = 0;
                    m_confirmMoves = 0;
                    m_confirmFailures = 0;
                    m_confirmLastValue = *m_cameraYawField;
                    WOWVR_INFO("Camera candidate at 0x%08X holding %.4f rad (%.1f deg); "
                               "confirming before anything is written.",
                               static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_cameraYawField)),
                               *m_cameraYawField, *m_cameraYawField * 57.2957795f);
                }
                else if (m_motionRounds >= 8)
                {
                    WOWVR_WARN("Narrowed to %d candidates over %d motion rounds but none "
                               "has the camera layout; starting again.",
                               DifferentialCandidates(), m_motionRounds);
                    m_locateState = LocateState::WaitingForMotion;
                    m_locatePasses = 0;
                    m_motionRounds = 0;
                }
            }
            break;

        default:
            break;
        }
    }

    int CameraProbe::PageCount() const
    {
        return static_cast<int>(g_pages.size());
    }

    // Watching the neighbourhood of the confirmed yaw field.
    //
    // The zoom distance almost certainly lives in the same object, so there is no need to
    // search memory again - only to see which nearby float moves when the wheel is turned
    // and stays put otherwise. A window either side of the yaw is a few kilobytes, which
    // is nothing to snapshot every time.
    // Comparing everything against one stale snapshot cannot work here: the object holds
    // per-frame state, so by the time the wheel has been turned half the window has moved
    // for reasons of its own. So the same differential the camera search itself uses, at a
    // much smaller scale - intervals in which nothing is done to the camera mark the fields
    // that move anyway, and the answer is what moved on the wheel and never moved then.
    namespace
    {
        const int kZoomWindowFloats = 1024;                  // 4 KB either side
        float g_zoomBefore[kZoomWindowFloats * 2] = {};
        bool g_zoomExcluded[kZoomWindowFloats * 2] = {};
        bool g_zoomPrimed = false;

        // A third-person camera distance in WoW's yards. Anything outside this is some
        // other quantity that happens to move with the wheel.
        bool PlausibleOrbitRadius(float value)
        {
            return std::isfinite(value) && value > 0.2f && value < 60.0f;
        }

        // One float, not three. ReadTriple also touches +0x144 and +0x148, which is right
        // when testing the camera's own layout and wrong here: a slot near the end of the
        // window fails only because those two lie past the allocation, and the perfectly
        // readable field in front of them gets dropped.
        __declspec(noinline) bool ReadFloatSafe(const float* address, float& value)
        {
            __try
            {
                value = *address;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }
    }

    void CameraProbe::ZoomSnapshot()
    {
        if (m_cameraYawField == nullptr)
        {
            WOWVR_WARN("Camera not confirmed yet; nothing to snapshot around.");
            return;
        }

        const float* window = m_cameraYawField - kZoomWindowFloats;
        for (int i = 0; i < kZoomWindowFloats * 2; ++i)
        {
            float value = 0.0f;
            const bool readable = ReadFloatSafe(window + i, value);
            g_zoomBefore[i] = readable ? value : 0.0f;

            // A slot that could not be read at priming is excluded outright rather than
            // recorded as zero. Recording zero manufactures an enormous delta the moment
            // it does become readable, and those fake deltas were the whole of the first
            // report.
            g_zoomExcluded[i] = !readable;
        }
        g_zoomPrimed = true;
        WOWVR_INFO("Zoom watch primed around 0x%08X; exclusions cleared.",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_cameraYawField)));
    }

    // The camera's pitch, found the same way the radius was: a vertical drag moved this
    // field by 0.199 rad while the quiet intervals either side left it alone, and a copy at
    // +0x114 moved with it. The object's layout is symmetric about the yaw - radius at -0x4,
    // pitch at +0x4 - which is a good sign that all three are one record.
    float CameraProbe::CameraPitch() const
    {
        if (m_cameraYawField == nullptr)
        {
            return 0.0f;
        }

        float value = 0.0f;
        float mirrored = 0.0f;
        if (!ReadFloatSafe(m_cameraYawField + 1, value)
            || !ReadFloatSafe(m_cameraYawField + (kCameraPitchMirror / sizeof(float)), mirrored))
        {
            return 0.0f;
        }

        // A camera pitch, not an arbitrary angle: the client clamps it well short of
        // straight up or down, so anything outside that is some other quantity.
        if (!std::isfinite(value) || value < -1.6f || value > 1.6f
            || fabsf(value - mirrored) > 0.05f)
        {
            return 0.0f;
        }

        return value;
    }

    // Pitch follows the head for the same reason yaw does: the client culls a band only
    // 58.9 degrees tall, and if that band stays level while the head looks up, everything
    // above the band is geometry the client never submitted.
    void CameraProbe::AimPitchAtHead(float headPitchRadians)
    {
        if (m_cameraYawField == nullptr)
        {
            return;
        }

        float* pitch = m_cameraYawField + 1;
        float* copy = m_cameraYawField + (kCameraPitchMirror / sizeof(float));

        float current = 0.0f;
        float mirrored = 0.0f;
        if (!ReadFloatSafe(pitch, current) || !ReadFloatSafe(copy, mirrored)
            || !std::isfinite(current) || fabsf(current - mirrored) > 0.05f)
        {
            return;
        }

        const bool oursSurvived = m_havePitched
            && fabsf(current - m_lastPitchWritten) < 0.002f;
        const float base = oursSurvived ? (current - m_appliedPitch) : current;

        // Clamped to what the client itself allows. Writing past its own limits is asking
        // for the kind of arithmetic the renderer was never built to survive.
        float wanted = base + headPitchRadians;
        if (wanted > 1.4f)  { wanted = 1.4f; }
        if (wanted < -1.4f) { wanted = -1.4f; }

        *pitch = wanted;
        *copy = wanted;

        m_lastPitchWritten = wanted;
        m_appliedPitch = wanted - base;
        m_havePitched = true;
    }

    float CameraProbe::AppliedPitch() const
    {
        return m_havePitched ? m_appliedPitch : 0.0f;
    }

    // Both copies are required to agree before the value is believed. A single field that
    // happens to hold something in range is not evidence; the pair at that exact spacing
    // is, and it is also how a freed or reallocated object gets noticed.
    float CameraProbe::OrbitRadius() const
    {
        if (m_cameraYawField == nullptr)
        {
            return 0.0f;
        }

        const float* radius = m_cameraYawField - 1;                     // -0x4
        const float* copy = m_cameraYawField + (0xCCu / sizeof(float)); // +0xCC

        float value = 0.0f;
        float mirrored = 0.0f;
        if (!ReadFloatSafe(radius, value) || !ReadFloatSafe(copy, mirrored))
        {
            return 0.0f;
        }

        if (!std::isfinite(value) || value < 0.0f || value > 60.0f
            || fabsf(value - mirrored) > 0.05f)
        {
            return 0.0f;
        }

        return value;
    }

    namespace
    {
        // Fields inside the camera object holding the field of view, and the factor to
        // apply to them. Several, because the client keeps more than one copy of most
        // things in there and it is not yet known which one the culler reads.
        const int kMaxFovFields = 16;
        float* g_fovFields[kMaxFovFields] = {};
        float g_fovOriginal[kMaxFovFields] = {};
        int g_fovFieldCount = 0;
        float g_fovFactor = 0.0f;
    }

    void CameraProbe::ScanCameraObjectForFov(float expectedHalfFov)
    {
        g_fovFieldCount = 0;
        if (m_cameraYawField == nullptr)
        {
            WOWVR_WARN("Camera not located; nothing to search for a field of view.");
            return;
        }

        // Both the half-angle and the whole angle, since it is not known which convention
        // the per-camera copy uses - the global holds the half.
        const float wanted[2] = { expectedHalfFov, expectedHalfFov * 2.0f };

        float* window = m_cameraYawField - kZoomWindowFloats;
        for (int i = 0; i < kZoomWindowFloats * 2 && g_fovFieldCount < kMaxFovFields; ++i)
        {
            float value = 0.0f;
            if (!ReadFloatSafe(window + i, value) || !std::isfinite(value))
            {
                continue;
            }

            for (int w = 0; w < 2; ++w)
            {
                if (fabsf(value - wanted[w]) > 0.004f)
                {
                    continue;
                }

                const int offset = (i - kZoomWindowFloats) * static_cast<int>(sizeof(float));
                WOWVR_INFO("  field of view candidate at yaw%+d: %.5f rad (%.2f deg), "
                           "matches the %s angle.",
                           offset, value, value * 57.2957795f,
                           (w == 0) ? "half" : "whole");
                g_fovFields[g_fovFieldCount] = window + i;
                g_fovOriginal[g_fovFieldCount] = value;
                ++g_fovFieldCount;
                break;
            }
        }

        WOWVR_INFO("Field-of-view sweep of the camera object: %d candidate(s) near %.5f rad.",
                   g_fovFieldCount, expectedHalfFov);
    }

    // Every copy of the widened field of view, anywhere in the process.
    //
    // Searching beside the camera found nothing, and guessing at encodings found nothing.
    // But once the global has been written to an unusual value, that value is its own
    // marker: anything holding it either is the global or was copied from it, and a copy
    // is exactly what the renderer is believed to read. This says whether such a copy
    // exists at all.
    void CameraProbe::ScanForFovCopies(float wanted)
    {
        int found = 0;
        uintptr_t address = 0x00010000u;
        const uintptr_t limit = 0x7FFF0000u;

        while (address < limit && found < 40)
        {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
            {
                break;
            }

            if (WorthSearching(info) && info.RegionSize >= sizeof(float))
            {
                float* hits[32];
                const size_t n = ScanRegionForValue(static_cast<uint8_t*>(info.BaseAddress),
                                                    info.RegionSize, wanted, hits, 32);
                for (size_t i = 0; i < n && found < 40; ++i)
                {
                    const uintptr_t at = reinterpret_cast<uintptr_t>(hits[i]);
                    WOWVR_INFO("  copy of the widened field of view at 0x%08X%s",
                               static_cast<unsigned>(at),
                               (at == 0x00ABFC38u) ? "  <- the global itself" : "");
                    ++found;
                }
            }

            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }

        WOWVR_INFO("Sweep for %.5f rad: %d location(s) hold it.", wanted, found);
    }

    void CameraProbe::SetFovOverride(float factor)
    {
        g_fovFactor = factor;
        WOWVR_INFO("Field-of-view override %s (%d field(s), factor %.2f).",
                   (factor > 0.0f) ? "ON" : "off", g_fovFieldCount, factor);
    }

    // Written every frame, because the client refreshes the copy from its own setting each
    // time round - which is exactly why writing the setting alone did nothing.
    void CameraProbe::ApplyFovOverride()
    {
        if (g_fovFactor <= 0.0f || g_fovFieldCount == 0)
        {
            return;
        }

        for (int i = 0; i < g_fovFieldCount; ++i)
        {
            float current = 0.0f;
            if (!ReadFloatSafe(g_fovFields[i], current) || !std::isfinite(current))
            {
                continue;
            }

            // Against the recorded original rather than the live value, or multiplying a
            // value we already multiplied runs away within a few frames.
            const float wanted = g_fovOriginal[i] * g_fovFactor;
            if (wanted > 0.05f && wanted < 1.5f)
            {
                *g_fovFields[i] = wanted;
            }
        }
    }

    // "idle" and "turned" only rule fields out; "zoomed" is what answers the question.
    //
    // Ruling out on a turn matters as much as ruling out on an idle interval: the whole
    // object swings when the camera does, so without it every orientation field in the
    // window would look like a zoom distance.
    void CameraProbe::ZoomReport(const char* label)
    {
        if (m_cameraYawField == nullptr || !g_zoomPrimed)
        {
            WOWVR_WARN("Zoom watch not primed; nothing to report.");
            return;
        }

        const bool excluding = (strcmp(label, "zoomed") != 0);

        float* window = m_cameraYawField - kZoomWindowFloats;
        int moved = 0;
        int reported = 0;
        for (int i = 0; i < kZoomWindowFloats * 2; ++i)
        {
            float now = 0.0f;
            if (!ReadFloatSafe(window + i, now))
            {
                continue;
            }

            const float was = g_zoomBefore[i];
            const bool changed = std::isfinite(now) && std::isfinite(was)
                              && fabsf(now - was) > 0.0005f;

            if (changed)
            {
                ++moved;
            }

            if (excluding)
            {
                // Re-primed as it goes, so each interval is measured against the one
                // before it rather than against the beginning of the run.
                if (changed)
                {
                    g_zoomExcluded[i] = true;
                }
                g_zoomBefore[i] = now;
                continue;
            }

            if (!changed || g_zoomExcluded[i] || reported >= 40)
            {
                continue;
            }

            const int offset = (i - kZoomWindowFloats) * static_cast<int>(sizeof(float));
            WOWVR_INFO("  zoomed: yaw%+d = %.4f (was %.4f, delta %+.4f)%s",
                       offset, now, was, now - was,
                       PlausibleOrbitRadius(now) ? "  <- plausible orbit radius" : "");
            ++reported;
        }

        // The yaw itself is printed every interval, because "nothing changed" is otherwise
        // ambiguous: it means either that the interval was genuinely quiet or that the
        // stimulus never reached the client at all. A turn that leaves the yaw untouched
        // is the second case, and that is worth knowing immediately rather than after the
        // measurement has been built on top of it.
        if (excluding)
        {
            WOWVR_INFO("  %s: %d of %d fields moved and are now excluded; yaw now %.4f rad.",
                       label, moved, kZoomWindowFloats * 2, *m_cameraYawField);
        }
        else if (reported == 0)
        {
            WOWVR_INFO("  zoomed: %d fields moved but every one was already excluded; "
                       "yaw now %.4f rad.", moved, *m_cameraYawField);
        }
    }

    // Calibration: the search generates its own camera movement instead of waiting for
    // the player's.
    //
    // Watching ordinary play does not converge. The offline version of this search worked
    // because every turn was the SAME length, which makes a heading move by the same
    // amount each time while its sine and cosine do not - by far the sharpest test
    // available. Real drags vary in length, so that test has to be dropped, and what
    // remains is too blunt: twenty-four rounds of play produced one rejection and thirteen
    // restarts.
    //
    // So the stimulus is synthesised. A few identical nudges, injected here, restore the
    // filter that actually works. The cost is that the camera visibly swings for a few
    // seconds, which is why this is on a hotkey rather than automatic.
    // One calibration round: keep whatever moved in the same direction as every other
    // round, and record HOW FAR it moved rather than judging it.
    //
    // The earlier version filtered on proportionality with a fixed band, and that band
    // kept eliminating the real camera - runs ended with zero candidates, or with one
    // whose neighbouring field held 374.0 and so was never the camera at all. A threshold
    // can throw away the right answer; a ranking cannot. So consistency is now measured
    // here and used to choose at the end.
    void CameraProbe::FilterCalibrationRound()
    {
        // Nothing is removed here any more. Requiring a candidate to move on EVERY round
        // meant one injected nudge failing to register killed the real camera along with
        // everything else - the count fell 52, 13, 9, 5, 3, 3, 1, 0 and ended with nothing.
        // Rounds now only accumulate evidence, and the choice is made at the end.
        for (size_t i = 0; i < g_differential.size(); ++i)
        {
            DifferentialCandidate& candidate = g_differential[i];

            float now = 0.0f;
            float copyA = 0.0f;
            float copyB = 0.0f;
            if (!ReadTriple(candidate.address, now, copyA, copyB) || !PlausibleAngle(now))
            {
                continue;
            }

            float delta = now - candidate.value;
            const float twoPi = 6.28318530718f;
            if (delta > 3.14159265f)       { delta -= twoPi; }
            else if (delta < -3.14159265f) { delta += twoPi; }

            candidate.value = now;

            if (fabsf(delta) <= 0.004f)
            {
                continue;               // did not move this round; simply no evidence
            }

            // Every nudge goes the same way, so a heading moves the same way each time.
            // A round that disagrees is counted against the candidate rather than
            // disqualifying it outright.
            const int sign = (delta > 0.0f) ? 1 : -1;
            if (candidate.signWhenTurningRight == 0)
            {
                candidate.signWhenTurningRight = sign;
            }
            else if (candidate.signWhenTurningRight != sign)
            {
                ++candidate.stillChanges;   // reused here as "rounds that disagreed"
                continue;
            }

            const float magnitude = fabsf(delta);
            candidate.deltaSum += magnitude;
            candidate.deltaSqSum += magnitude * magnitude;
            ++candidate.deltaCount;
        }
    }

    // The candidate whose movement was most consistent across identical nudges. A heading
    // turns by the same amount every time; anything derived from it does not.
    float* CameraProbe::MostConsistentCandidate(int rounds) const
    {
        float* best = nullptr;
        float bestSpread = 0.0f;
        int reported = 0;

        // Counted separately, because when this returns nothing there is no way to tell
        // which of its conditions did the rejecting - and guessing at exactly that
        // question has already cost several build-and-run cycles. Each count is over the
        // whole set, independent of the others, so a zero names the culprit outright.
        int movedAtAll = 0;
        int passedMovement = 0;
        int passedDirection = 0;
        int passedBothMotion = 0;
        int passedLayout = 0;
        int passedRadius = 0;
        int passedEverything = 0;

        struct Contender
        {
            float* address = nullptr;
            float value = 0.0f;
            float spread = 0.0f;
            int moved = 0;
        };
        static const int kTopContenders = 6;
        Contender top[kTopContenders];

        for (size_t i = 0; i < g_differential.size(); ++i)
        {
            const DifferentialCandidate& candidate = g_differential[i];

            float value = 0.0f;
            float copyA = 0.0f;
            float copyB = 0.0f;
            const bool readable = ReadTriple(candidate.address, value, copyA, copyB);
            const bool layoutHolds = readable
                                  && fabsf(copyA - value) <= 0.05f
                                  && fabsf(copyB - value) <= 0.05f;

            const bool movement = (candidate.deltaCount * 3 >= rounds * 2);
            const bool direction = (candidate.stillChanges <= 1);

            // The whole record, not just the yaw copies - but nothing that depends on the
            // zoom level.
            //
            // Requiring a non-zero orbit radius looked right and was wrong: it assumes the
            // zoom-out landed, and when that silently fails the camera is in first person
            // with radius zero and the filter excludes the very thing it is looking for.
            // Zero is therefore allowed; only disagreement between the copies is not.
            //
            // The zoom-independent discriminator is the yaw itself. The client stores a
            // heading in [0, 2pi) and the real camera reads 4.08, while every false winner
            // so far held roughly nothing - 0.0363, -0.0212, -0.0000 - three runs running,
            // at 25-33% spread against the real camera's 0.4%. A negative value is not how
            // the client stores this at all.
            float radius = 0.0f;
            float radiusCopy = 0.0f;
            const bool radiusHolds =
                ReadFloatSafe(candidate.address - 1, radius)
                && ReadFloatSafe(candidate.address + (kCameraRadiusMirror / sizeof(float)),
                                 radiusCopy)
                && std::isfinite(radius) && radius >= 0.0f && radius < 60.0f
                && fabsf(radius - radiusCopy) <= 0.05f;

            const bool headingShaped = (value > 0.05f && value < 6.23f);

            if (candidate.deltaCount > 0)   { ++movedAtAll; }
            if (movement)                   { ++passedMovement; }
            if (direction)                  { ++passedDirection; }
            if (movement && direction)      { ++passedBothMotion; }
            if (layoutHolds)                { ++passedLayout; }
            if (radiusHolds && headingShaped) { ++passedRadius; }
            if (movement && direction && layoutHolds && radiusHolds && headingShaped)
            {
                ++passedEverything;
            }

            // Finiteness first, explicitly.
            //
            // NaN defeats every "reject if outside tolerance" test in this function,
            // because all comparisons against NaN are false - so a field full of garbage
            // passes the layout check, passes the spread check, and wins with a spread of
            // 0.0%. Four of the top six contenders in one run held -nan. This is the one
            // guard that cannot be left implicit.
            if (!std::isfinite(value) || !PlausibleAngle(value))
            {
                continue;
            }

            // Layout only, as hard gates.
            //
            // The radius and heading tests above are kept for the record they print, not as
            // filters: they were added during a failing streak and never once turned a
            // failure into a success, and a filter that has not earned its place is just
            // another way to discard the answer. This is the configuration that located the
            // camera six times running earlier.
            if (!movement || !direction || !layoutHolds)
            {
                continue;
            }

            const float mean = candidate.deltaSum / candidate.deltaCount;
            if (mean <= 0.0001f)
            {
                continue;
            }
            const float variance =
                (candidate.deltaSqSum / candidate.deltaCount) - (mean * mean);
            const float spread = (variance > 0.0f) ? (sqrtf(variance) / mean) : 0.0f;

            if (best == nullptr || spread < bestSpread)
            {
                best = candidate.address;
                bestSpread = spread;
            }

            // The runners-up are what say whether the winner stands out or is simply the
            // least bad of a uniformly poor field - so they are kept by rank rather than
            // by passing a threshold. The previous version only printed candidates under
            // 5% spread, which meant that on the run where the winner came in at 29% it
            // printed nothing at all, exactly when the comparison was most needed.
            if (reported < kTopContenders || spread < top[kTopContenders - 1].spread)
            {
                int slot = (reported < kTopContenders) ? reported : (kTopContenders - 1);
                while (slot > 0 && spread < top[slot - 1].spread)
                {
                    top[slot] = top[slot - 1];
                    --slot;
                }
                top[slot].address = candidate.address;
                top[slot].value = value;
                top[slot].spread = spread;
                top[slot].moved = candidate.deltaCount;
                if (reported < kTopContenders)
                {
                    ++reported;
                }
            }
        }

        for (int i = 0; i < reported; ++i)
        {
            WOWVR_INFO("  contender %d: 0x%08X, %.4f rad, moved %d/%d rounds, spread %.1f%%",
                       i + 1,
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(top[i].address)),
                       top[i].value, top[i].moved, rounds, top[i].spread * 100.0f);
        }

        WOWVR_INFO("Selection over %zu candidates and %d rounds: %d moved at all, "
                   "%d moved on >=2/3 of rounds, %d disagreed on direction at most once, "
                   "%d passed both, %d carry the three-copy layout, %d carry an orbit "
                   "radius, %d passed everything.",
                   g_differential.size(), rounds, movedAtAll, passedMovement,
                   passedDirection, passedBothMotion, passedLayout, passedRadius,
                   passedEverything);

        if (best != nullptr)
        {
            WOWVR_INFO("Most consistent candidate 0x%08X varies by %.1f%% across nudges.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(best)),
                       bestSpread * 100.0f);
        }
        return best;
    }

    void CameraProbe::BeginCalibration()
    {
        if (m_calState != CalibrationState::Idle)
        {
            WOWVR_INFO("Calibration already running.");
            return;
        }

        // Seeded from the signature sweep, with the page phase dropped.
        //
        // Page narrowing is the fragile part: it keeps only pages that changed on every
        // nudge and were quiet between, and the camera's page does not reliably satisfy
        // that - it worked six runs in a row and then failed every run afterwards with the
        // drags still demonstrably turning the camera (verified by screenshot, MAD 6.99).
        //
        // The first attempt at seeding from the signature failed for reasons now
        // understood and fixed: it demanded the copies agree exactly at the instant of the
        // sweep, and it required a non-zero orbit radius, which excludes a first-person
        // camera outright. With loose comparisons, radius zero allowed, and pitch and its
        // copy added - seven constrained fields instead of five - the camera is in the set.
        ScanForCameraSignature();
        m_calPhase = 1;
        WOWVR_INFO("Calibration starting: game window %s focus.",
                   (GetForegroundWindow() == GetActiveWindow()) ? "appears to have" : "may not have");
        m_calState = CalibrationState::Nudging;
        m_calFrame = 0;
        m_calRound = 0;
        m_calNudged = 0;
        m_calRestoring = false;
        WOWVR_INFO("Camera calibration started: narrowing pages first, then floats. The "
                   "view will swing and then be put back.");
    }

    // The settled check that authorises writing. Runs after the calibration has finished,
    // which is why it sits ahead of the idle test rather than inside the state machine.
    void CameraProbe::UpdateVerification()
    {
        if (m_locateState != LocateState::Verifying || m_cameraYawField == nullptr)
        {
            return;
        }

        if (--m_verifyDelay > 0)
        {
            return;
        }

        float value = 0.0f;
        float copyA = 0.0f;
        float copyB = 0.0f;
        const bool yawOk = ReadTriple(m_cameraYawField, value, copyA, copyB)
                        && PlausibleAngle(value)
                        && fabsf(copyA - value) <= 0.05f
                        && fabsf(copyB - value) <= 0.05f;

        const float radius = OrbitRadius();

        // The same guard at the gate that authorises writing. A heading of nearly zero
        // beside a radius of nearly zero is zeroed memory, not a camera, and this is the
        // last check before head yaw starts being written into it.
        const bool headingShaped = fabsf(value) > 0.01f;

        if (!yawOk || !headingShaped || radius <= 0.0f)
        {
            WOWVR_WARN("Camera candidate 0x%08X failed verification once settled "
                       "(yaw %.4f, +0x144 %.4f, +0x148 %.4f, radius %.3f). "
                       "Nothing has been written.",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_cameraYawField)),
                       value, copyA, copyB, radius);
            m_cameraYawField = nullptr;
            m_locateState = LocateState::Failed;
            return;
        }

        m_locateState = LocateState::Locked;
        m_locateTick = 0;
        WOWVR_INFO("Camera VERIFIED at 0x%08X: yaw %.4f rad (%.1f deg), radius %.3f yards. "
                   "Head aiming is now live.",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_cameraYawField)),
                   value, value * 57.2957795f, radius);
    }

    void CameraProbe::UpdateCalibration()
    {
        UpdateVerification();

        if (m_calState == CalibrationState::Idle)
        {
            return;
        }

        const int step = 8;                 // pixels of drag per frame
        const int nudgeFrames = 15;         // 120 px per nudge
        const int settleFrames = 20;

        switch (m_calState)
        {
        case CalibrationState::Nudging:
            if (m_calFrame == 0)
            {
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            }
            mouse_event(MOUSEEVENTF_MOVE, m_calRestoring ? -step : step, 0, 0, 0);
            if (++m_calFrame >= nudgeFrames)
            {
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                m_calFrame = 0;
                m_calState = CalibrationState::Settling;
            }
            break;

        case CalibrationState::Settling:
            if (++m_calFrame < settleFrames)
            {
                break;
            }
            m_calFrame = 0;

            if (m_calRestoring)
            {
                // The view has been put back; nothing left to measure.
                if (++m_calNudged >= kCalibrationRounds)
                {
                    FinishCalibration();
                }
                else
                {
                    m_calState = CalibrationState::Nudging;
                }
                break;
            }

            if (m_calPhase == 0)
            {
                // The camera just moved, so its page must have changed.
                FilterPages(true);
                m_calState = CalibrationState::Pausing;
            }
            else
            {
                // Scored, never eliminated.
                //
                // Hard filtering was tried here and died exactly as it has every previous
                // time: 4841 candidates went to 36 on the first round and to 0 on the
                // second, because one injected nudge that fails to register discards the
                // right answer along with everything else. Rounds only accumulate
                // evidence; the choice happens once, at the end, by lowest spread.
                FilterCalibrationRound();
                ++m_calRound;
                WOWVR_INFO("Calibration float round %d over %d candidates.", m_calRound,
                           DifferentialCandidates());
                if (m_calRound >= kCalibrationRounds || DifferentialCandidates() == 0)
                {
                    m_calRestoring = true;
                    m_calNudged = 0;
                }
                m_calState = CalibrationState::Nudging;
            }
            break;

        case CalibrationState::Pausing:
            // Nothing is being done to the camera during this interval, which is what
            // makes it worth anything: a page that changes anyway is not the camera's.
            if (++m_calFrame < settleFrames)
            {
                break;
            }
            m_calFrame = 0;
            FilterPages(false);
            ++m_calRound;
            WOWVR_INFO("Calibration page round %d: %d pages.", m_calRound, PageCount());

            if (m_calRound >= kCalibrationPageRounds)
            {
                SelectScoredPages(m_calRound);
                PromotePagesToFloats();
                m_calPhase = 1;
                m_calRound = 0;
            }
            m_calState = CalibrationState::Nudging;
            break;

        default:
            break;
        }
    }

    void CameraProbe::FinishCalibration()
    {
        m_calState = CalibrationState::Idle;
        m_calRestoring = false;

        float* found = MostConsistentCandidate(m_calRound);
        if (found == nullptr)
        {
            WOWVR_WARN("Calibration finished with %d candidates but none carries the "
                       "camera layout. Nothing has been written.", DifferentialCandidates());
            for (int i = 0; i < DifferentialCandidates() && i < 5; ++i)
            {
                float* address = CandidateAt(i);
                float value = 0.0f;
                float copyA = 0.0f;
                float copyB = 0.0f;
                const bool ok = ReadTriple(address, value, copyA, copyB);
                WOWVR_INFO("  survivor 0x%08X: read %s, value %.5f, +0x144 %.5f, +0x148 %.5f, "
                           "roundish %s, rejected %s",
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(address)),
                           ok ? "ok" : "FAILED", value, copyA, copyB,
                           (value * 8.0f == floorf(value * 8.0f)) ? "yes" : "no",
                           AlreadyRejected(address) ? "yes" : "no");
            }
            return;
        }

        // Held read-only until the camera has stopped moving and the full layout - both
        // yaw copies and both radius copies - agrees. Nothing may be written before that:
        // locking onto a wrong buffer and writing head yaw into it is what corrupted the
        // client before, and CameraLocated() stays false throughout this phase.
        m_cameraYawField = found;
        m_locateState = LocateState::Verifying;
        m_verifyDelay = 120;
        WOWVR_INFO("Calibration proposes the camera at 0x%08X holding %.4f rad (%.1f deg); "
                   "verifying the layout once it settles.",
                   static_cast<unsigned>(reinterpret_cast<uintptr_t>(found)), *found,
                   *found * 57.2957795f);
    }

    bool CameraProbe::CameraLocated() const
    {
        return m_cameraYawField != nullptr && m_locateState == LocateState::Locked;
    }

    bool CameraProbe::LocateFailed() const
    {
        return m_locateState == LocateState::Failed;
    }

    void CameraProbe::ResetLocate()
    {
        m_cameraYawField = nullptr;
        m_locateState = LocateState::WaitingForMotion;
        m_locatePasses = 0;
        m_motionRounds = 0;
        m_haveAimed = false;
        m_havePitched = false;
    }

    void CameraProbe::DiscardCandidate(const float* address)
    {
        for (size_t i = 0; i < g_differential.size(); ++i)
        {
            if (g_differential[i].address == address)
            {
                g_differential.erase(g_differential.begin() + i);
                return;
            }
        }
    }

    float* CameraProbe::FirstCandidate() const
    {
        return g_differential.empty() ? nullptr : g_differential[0].address;
    }

    float* CameraProbe::CandidateAt(int index) const
    {
        return (index >= 0 && index < static_cast<int>(g_differential.size()))
            ? g_differential[index].address : nullptr;
    }

    // The camera object repeats its yaw at +0x144 and +0x148. Over a set already narrowed
    // by behaviour, that layout is what picks the camera out of the values derived from it.
    float* CameraProbe::FindLayoutMatch(bool requireBehaviourScore) const
    {
        for (size_t i = 0; i < g_differential.size(); ++i)
        {
            // The behaviour counters are only filled in by the passive search. Calibration
            // establishes the same thing far more strongly - identical nudges, and a
            // proportionality test the passive path cannot use - so demanding them there
            // rejected a candidate that the calibration had already narrowed to one.
            if (requireBehaviourScore
                && (g_differential[i].changes < 3 || g_differential[i].stillChanges > 1))
            {
                continue;
            }

            if (AlreadyRejected(g_differential[i].address))
            {
                continue;
            }

            // Reject round constants. Every false positive so far has been an exact
            // binary fraction - 1.0000, 0.7500, 0.1250 - sitting in a buffer that happens
            // to repeat it at the right spacing. A heading arrived at by turning a camera
            // is essentially never an exact eighth.
            const float scaled = g_differential[i].value * 8.0f;
            if (scaled == floorf(scaled))
            {
                continue;
            }

            float* candidate = g_differential[i].address;
            float value = 0.0f;
            float copyA = 0.0f;
            float copyB = 0.0f;
            if (!ReadTriple(candidate, value, copyA, copyB))
            {
                continue;
            }
            if (fabsf(copyA - value) <= 0.05f && fabsf(copyB - value) <= 0.05f
                && PlausibleAngle(value))
            {
                return candidate;
            }
        }
        return nullptr;
    }

    void CameraProbe::FindPointerChain()
    {
        if (g_differential.empty())
        {
            WOWVR_WARN("No camera field to trace a pointer chain from.");
            return;
        }

        const uintptr_t field = reinterpret_cast<uintptr_t>(g_differential[0].address);
        // A pointer to the object will land at or before the field, within about a page.
        const uintptr_t lowest = field - 0x1000u;
        const uintptr_t highest = field + 0x10u;

        WOWVR_INFO("Tracing pointers that land inside the camera object (field 0x%08X):",
                   static_cast<unsigned>(field));

        int found = 0;
        uintptr_t address = 0x00400000u;
        const uintptr_t imageEnd = 0x01000000u;
        while (address < imageEnd && found < 40)
        {
            MEMORY_BASIC_INFORMATION info = {};
            if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0)
            {
                break;
            }

            const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
            if (info.State == MEM_COMMIT && (info.Protect & writable) != 0
                && (info.Protect & PAGE_GUARD) == 0)
            {
                const uintptr_t* words = static_cast<const uintptr_t*>(info.BaseAddress);
                const size_t count = info.RegionSize / sizeof(uintptr_t);
                for (size_t i = 0; i < count && found < 40; ++i)
                {
                    const uintptr_t value = words[i];
                    if (value >= lowest && value <= highest)
                    {
                        const uintptr_t holder =
                            reinterpret_cast<uintptr_t>(info.BaseAddress) + i * sizeof(uintptr_t);
                        WOWVR_INFO("  static 0x%08X -> 0x%08X, field at +0x%X",
                                   static_cast<unsigned>(holder), static_cast<unsigned>(value),
                                   static_cast<unsigned>(field - value));
                        ++found;
                    }
                }
            }

            address = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        }

        if (found == 0)
        {
            WOWVR_INFO("  none - no static word points into the object, so the chain has "
                       "at least one heap hop and needs a second level.");
        }
    }

    void CameraProbe::ReportDifferential()
    {
        WOWVR_INFO("Differential scan finished with %zu candidates:", g_differential.size());
        const size_t show = (g_differential.size() < 40u) ? g_differential.size() : 40u;
        for (size_t i = 0; i < show; ++i)
        {
            WOWVR_INFO("    0x%08X = %.5f rad (%.2f deg)",
                       static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_differential[i].address)),
                       g_differential[i].value,
                       g_differential[i].value * 57.2957795f);
        }
    }

    void CameraProbe::WatchCameraStruct(const Vec3& cameraPosition, bool havePosition)
    {
        // The struct that owns the field of view. The setting lives at +0xC4 of it, so
        // the object starts here; the window is walked as raw floats rather than as any
        // assumed layout, because the layout is exactly what is not known.
        const uintptr_t kFovField = 0x00ABFC38u;
        const uintptr_t kStructBase = kFovField - 0xC4u;
        const int kFloats = 192;                 // 768 bytes either side of the setting

        const float* fields = reinterpret_cast<const float*>(kStructBase);
        if (!Readable(fields, kFloats * sizeof(float)))
        {
            return;
        }

        static float previous[kFloats] = {};
        static bool primed = false;
        static int frame = 0;
        ++frame;

        if (!primed)
        {
            primed = true;
            for (int i = 0; i < kFloats; ++i) { previous[i] = fields[i]; }
            WOWVR_INFO("Watching the camera struct at 0x%08X (%d floats). Move and turn: "
                       "fields that change are per-frame camera state, fields that do not "
                       "are settings.",
                       static_cast<unsigned>(kStructBase), kFloats);
            return;
        }

        // Only every so often: the point is to see what moves as the player does, not
        // to record every frame.
        if ((frame % 60) != 0)
        {
            return;
        }

        if (havePosition)
        {
            WOWVR_INFO("Camera position off the wire: %.3f %.3f %.3f",
                       cameraPosition.x, cameraPosition.y, cameraPosition.z);
        }

        for (int i = 0; i < kFloats; ++i)
        {
            const float now = fields[i];
            const float was = previous[i];
            if (!std::isfinite(now) || !std::isfinite(was))
            {
                previous[i] = now;
                continue;
            }

            const float change = fabsf(now - was);
            if (change <= 1e-4f)
            {
                continue;
            }
            previous[i] = now;

            // Flag anything that looks like it could be one of the coordinates we
            // decoded, so a match jumps out of the log rather than having to be
            // spotted by eye.
            const char* note = "";
            if (havePosition)
            {
                if (fabsf(now - cameraPosition.x) < 0.5f) { note = "  <- matches camera X"; }
                else if (fabsf(now - cameraPosition.y) < 0.5f) { note = "  <- matches camera Y"; }
                else if (fabsf(now - cameraPosition.z) < 0.5f) { note = "  <- matches camera Z"; }
            }

            WOWVR_INFO("  +0x%03X = %12.4f (was %12.4f)%s",
                       static_cast<unsigned>(i * sizeof(float)), now, was, note);
        }
    }

    bool CameraProbe::WidenGameFov(float desiredHalfAngleRadians)
    {
        // The address the static sweep converged on. Two independent statics pointed at
        // it and its value tracked the projection decoded off the wire, so it is treated
        // as known rather than searched for again.
        if (m_fovField == nullptr)
        {
            m_fovField = reinterpret_cast<float*>(0x00ABFC38u);
        }

        if (!Readable(m_fovField, sizeof(float)))
        {
            return false;
        }

        // Never write over something that is not a plausible half field of view. This is
        // the one guard standing between a targeted patch and the kind of scattergun
        // write that crashed the client twice before.
        const float current = *m_fovField;
        const bool plausible = current > 0.15f && current < 1.4f;

        if (!m_fovWidened)
        {
            if (!plausible)
            {
                WOWVR_WARN("Not widening the game's field of view: 0x%08X holds %.5f, "
                           "which is not a half angle.",
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(m_fovField)),
                           current);
                return false;
            }

            DWORD previous = 0;
            if (!VirtualProtect(m_fovField, sizeof(float), PAGE_READWRITE, &previous))
            {
                WOWVR_WARN("Could not make the field of view field writable.");
                return false;
            }

            m_originalHalfFov = current;
            m_fovWidened = true;
            WOWVR_INFO("Widening the game's field of view: %.5f rad (%.2f deg full) -> "
                       "%.5f rad (%.2f deg full).",
                       m_originalHalfFov, m_originalHalfFov * 2.0f * 57.2957795f,
                       desiredHalfAngleRadians,
                       desiredHalfAngleRadians * 2.0f * 57.2957795f);
        }

        // Rewritten every frame: if the client refreshes this from a console variable we
        // would otherwise be overwritten straight back.
        *m_fovField = desiredHalfAngleRadians;
        return true;
    }

    void CameraProbe::ScanForCameraObject(float verticalScale, float nearPlane, float farPlane)
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

        const float wantHalfFov = atanf(1.0f / verticalScale);
        int hits = 0;
        int walked = 0;

        WOWVR_INFO("Camera object scan: looking for fov %.5f, near %.4f, far %.1f "
                   "together in one object.", wantHalfFov, nearPlane, farPlane);

        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && hits < 24; ++i, ++section)
        {
            if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0)
            {
                continue;
            }

            const uintptr_t start = imageBase + section->VirtualAddress;
            const size_t size = section->Misc.VirtualSize;

            for (size_t offset = 0; offset + sizeof(uintptr_t) <= size && hits < 24;
                 offset += sizeof(uintptr_t))
            {
                if ((offset & 0xFFFu) == 0
                    && !Readable(reinterpret_cast<const void*>(start + offset), 0x1000))
                {
                    offset += 0x1000 - sizeof(uintptr_t);
                    continue;
                }

                const uintptr_t level1 = *reinterpret_cast<const uintptr_t*>(start + offset);
                if (level1 < 0x10000u || (level1 & 3u) != 0
                    || !Readable(reinterpret_cast<const void*>(level1), 0x200))
                {
                    continue;
                }

                // Two levels: the static may point at the camera, or at something that
                // holds it. Every aligned pointer in the first 0x100 bytes is followed.
                for (int depth = 0; depth < 2; ++depth)
                {
                    for (unsigned slot = 0; slot < (depth == 0 ? 1u : 64u); ++slot)
                    {
                        uintptr_t object = level1;
                        if (depth == 1)
                        {
                            object = *reinterpret_cast<const uintptr_t*>(level1 + slot * 4);
                            if (object < 0x10000u || (object & 3u) != 0)
                            {
                                continue;
                            }
                        }

                        if (!Readable(reinterpret_cast<const void*>(object), 0x200))
                        {
                            continue;
                        }

                        ++walked;
                        const uint8_t* base = reinterpret_cast<const uint8_t*>(object);

                        for (unsigned f = 0; f + 4 <= 0x200 && hits < 24; f += 4)
                        {
                            const float value = *reinterpret_cast<const float*>(base + f);
                            if (!AllFinite(&value, 1) || fabsf(value - wantHalfFov) > 0.004f)
                            {
                                continue;
                            }

                            // Field of view found; now require the clip planes nearby.
                            bool sawNear = false;
                            bool sawFar = false;
                            unsigned nearAt = 0;
                            unsigned farAt = 0;
                            const unsigned lo = (f > 0x40) ? f - 0x40 : 0;
                            for (unsigned g = lo; g + 4 <= f + 0x40 && g + 4 <= 0x200; g += 4)
                            {
                                const float v = *reinterpret_cast<const float*>(base + g);
                                if (!AllFinite(&v, 1)) { continue; }
                                if (!sawNear && fabsf(v - nearPlane) < 0.01f)
                                {
                                    sawNear = true; nearAt = g;
                                }
                                if (!sawFar && fabsf(v - farPlane) < farPlane * 0.01f)
                                {
                                    sawFar = true; farAt = g;
                                }
                            }

                            if (sawNear && sawFar)
                            {
                                WOWVR_INFO("  CAMERA OBJECT 0x%08X via static 0x%08X "
                                           "depth %d: fov at +0x%X, near at +0x%X, "
                                           "far at +0x%X",
                                           static_cast<unsigned>(object),
                                           static_cast<unsigned>(start + offset), depth,
                                           f, nearAt, farAt);
                                ++hits;
                            }
                        }
                    }
                }
            }
        }

        WOWVR_INFO("Camera object scan: %d objects walked, %d carried fov+near+far together.",
                   walked, hits);
    }

    bool CameraProbe::WidenGameFovByFactor(float factor)
    {
        if (m_fovField == nullptr)
        {
            m_fovField = reinterpret_cast<float*>(0x00ABFC38u);
        }

        if (!Readable(m_fovField, sizeof(float)))
        {
            return false;
        }

        // The factor applies to the value the game shipped with, captured once, so that
        // rewriting every frame cannot compound.
        if (!m_fovWidened)
        {
            const float current = *m_fovField;
            if (!(current > 0.15f && current < 1.4f))
            {
                return false;
            }
            m_originalHalfFov = current;
        }

        float target = m_originalHalfFov * factor;
        if (target > 1.05f) { target = 1.05f; }

        return WidenGameFov(target);
    }

    void CameraProbe::RestoreGameFov()
    {
        if (m_fovWidened && m_fovField != nullptr && Readable(m_fovField, sizeof(float)))
        {
            *m_fovField = m_originalHalfFov;
            m_fovWidened = false;
            WOWVR_INFO("Restored the game's field of view to %.5f rad.", m_originalHalfFov);
        }
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
