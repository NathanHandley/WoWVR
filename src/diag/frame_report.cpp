#include "diag/frame_report.h"

#include "core/log.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string.h>

namespace wowvr
{
    namespace
    {
        FrameReport g_report;

        const char* TransformStateName(uint32_t state)
        {
            switch (state)
            {
            case 2:   return "VIEW";
            case 3:   return "PROJECTION";
            case 256: return "WORLD";
            case 257: return "WORLD1";
            case 258: return "WORLD2";
            case 259: return "WORLD3";
            default:  return "other";
            }
        }

        int TransformSlot(uint32_t state)
        {
            switch (state)
            {
            case 2:   return 0;
            case 3:   return 1;
            case 256: return 2;
            case 257: return 3;
            case 258: return 4;
            case 259: return 5;
            default:  return 6;
            }
        }

        // A perspective projection leaves a very distinctive fingerprint: one column
        // is all zeroes except for a +/-1 that produces w, and the diagonal carries
        // the FOV scales. Combined world-view-projection matrices keep that column,
        // which is what makes them findable without knowing anything about WoW.
        bool LooksLikeProjective(const float* v)
        {
            const float wColumn[4] = { v[3], v[7], v[11], v[15] };
            const float wRow[4]    = { v[12], v[13], v[14], v[15] };

            const bool columnForm = std::fabs(std::fabs(wColumn[2]) - 1.0f) < 0.01f
                                 && std::fabs(wColumn[3]) < 0.01f;
            const bool rowForm = std::fabs(std::fabs(wRow[2]) - 1.0f) < 0.01f
                              && std::fabs(wRow[3]) < 0.01f;
            return columnForm || rowForm;
        }

        bool IsUnitLength(float x, float y, float z)
        {
            const float lengthSquared = x * x + y * y + z * z;
            return std::fabs(lengthSquared - 1.0f) < 0.02f;
        }

        // Classifies a 4x4 by shape alone. A view matrix is affine with an orthonormal
        // rotation part; a projection has the distinctive w column. Because the upload
        // may be transposed for dp4-style shaders, both layouts are tested and the
        // answer says which one matched - that tells us the convention as a side effect.
        const char* DescribeMatrix(const float* v)
        {
            if (LooksLikeProjective(v))
            {
                return "projective";
            }

            // Row-vector layout: rows 0..2 are the basis, row 3 is the translation.
            const bool rowsOrthonormal = IsUnitLength(v[0], v[1], v[2])
                                      && IsUnitLength(v[4], v[5], v[6])
                                      && IsUnitLength(v[8], v[9], v[10]);
            const bool rowAffine = std::fabs(v[3]) < 1e-4f && std::fabs(v[7]) < 1e-4f
                                && std::fabs(v[11]) < 1e-4f && std::fabs(v[15] - 1.0f) < 1e-4f;

            // Column-vector (transposed) layout: columns 0..2 are the basis.
            const bool columnsOrthonormal = IsUnitLength(v[0], v[4], v[8])
                                         && IsUnitLength(v[1], v[5], v[9])
                                         && IsUnitLength(v[2], v[6], v[10]);
            const bool columnAffine = std::fabs(v[12]) < 1e-4f && std::fabs(v[13]) < 1e-4f
                                   && std::fabs(v[14]) < 1e-4f && std::fabs(v[15] - 1.0f) < 1e-4f;

            if (rowAffine && rowsOrthonormal)    { return "rigid transform, row-vector layout"; }
            if (columnAffine && columnsOrthonormal) { return "rigid transform, transposed layout"; }
            if (rowAffine)    { return "affine with scale, row-vector layout"; }
            if (columnAffine) { return "affine with scale, transposed layout"; }
            return "general";
        }

        void LogMatrix(const char* label, const float* v)
        {
            WOWVR_INFO("    %s  [%s]", label, DescribeMatrix(v));
            for (int row = 0; row < 4; ++row)
            {
                WOWVR_INFO("      % 14.4f % 14.4f % 14.4f % 14.4f",
                           v[row * 4 + 0], v[row * 4 + 1], v[row * 4 + 2], v[row * 4 + 3]);
            }
            // WoW world coordinates run to the thousands, so a large translation is a
            // strong hint that this matrix carries the camera rather than an object.
            WOWVR_INFO("      translation row  (% .2f % .2f % .2f)   column (% .2f % .2f % .2f)",
                       v[12], v[13], v[14], v[3], v[7], v[11]);
        }
    }

    void FrameReport::RequestFrame(unsigned long long frameNumber)
    {
        m_requested = true;
        m_requestedFrame = frameNumber;
    }

    void FrameReport::BeginFrame(unsigned long long frameNumber)
    {
        if (!m_requested || frameNumber != m_requestedFrame)
        {
            return;
        }

        m_requested = false;
        m_active = true;

        std::memset(m_transformCalls, 0, sizeof(m_transformCalls));
        std::memset(m_sawTransform, 0, sizeof(m_sawTransform));
        std::memset(m_drawCalls, 0, sizeof(m_drawCalls));
        m_constantBlockCount = 0;
        m_constantBlocksDropped = 0;
        m_phaseCount = 0;
        m_drawIndex = 0;
        m_renderTargetCount = 0;
        m_currentRenderTarget = -1;
        m_shaderChanges = 0;
        m_fixedFunctionDraws = 0;
        m_currentShader = nullptr;
        m_currentShaderIsFixedFunction = true;

        WOWVR_INFO("==== Frame report for frame %llu: capture starting ====", frameNumber);
    }

    void FrameReport::NoteTransform(uint32_t state, const float* matrix)
    {
        if (!m_active)
        {
            return;
        }

        const int index = TransformSlot(state);
        ++m_transformCalls[index];
        m_sawTransform[index] = true;
        std::memcpy(&m_lastTransform[index].m[0][0], matrix, sizeof(float) * 16);
    }

    void FrameReport::NoteVertexShaderConstants(uint32_t startRegister, const float* data,
                                                uint32_t vector4Count)
    {
        if (!m_active)
        {
            return;
        }

        for (int i = 0; i < m_constantBlockCount; ++i)
        {
            ConstantBlock& block = m_constantBlocks[i];
            if (block.startRegister != startRegister || block.vector4Count != vector4Count)
            {
                continue;
            }

            ++block.calls;
            if (block.valuesCaptured && !block.valuesEverChanged)
            {
                const size_t bytes = block.capturedRegisters * 4 * sizeof(float);
                if (std::memcmp(block.firstValues, data, bytes) != 0)
                {
                    block.valuesEverChanged = true;
                }
            }
            return;
        }

        if (m_constantBlockCount >= kMaxConstantBlocks)
        {
            ++m_constantBlocksDropped;
            return;
        }

        ConstantBlock& block = m_constantBlocks[m_constantBlockCount++];
        block.startRegister = startRegister;
        block.vector4Count = vector4Count;
        block.calls = 1;
        block.valuesEverChanged = false;
        block.capturedRegisters = 0;

        if (vector4Count >= 4 && data != nullptr)
        {
            block.capturedRegisters =
                (vector4Count < kCapturedRegisters) ? vector4Count : kCapturedRegisters;
            std::memcpy(block.firstValues, data, block.capturedRegisters * 4 * sizeof(float));
            block.valuesCaptured = true;
        }
    }

    void FrameReport::NoteVertexShaderCreated(uint32_t bytecodeBytes)
    {
        (void)bytecodeBytes;
        ++m_shadersCreatedTotal;
    }

    void FrameReport::NoteVertexShaderSet(void* shader)
    {
        if (!m_active)
        {
            return;
        }
        if (shader != m_currentShader)
        {
            ++m_shaderChanges;
            m_currentShader = shader;
        }
        m_currentShaderIsFixedFunction = (shader == nullptr);
    }

    void FrameReport::NoteFixedFunctionVertexPipeline()
    {
        if (!m_active)
        {
            return;
        }
        m_currentShaderIsFixedFunction = true;
    }

    void FrameReport::NoteDraw(int kind)
    {
        if (!m_active || kind < 0 || kind >= DrawKindCount)
        {
            return;
        }

        ++m_drawCalls[kind];
        if (m_currentShaderIsFixedFunction)
        {
            ++m_fixedFunctionDraws;
        }
        if (m_currentRenderTarget >= 0)
        {
            ++m_renderTargets[m_currentRenderTarget].drawsWhileBound;
        }
    }

    void FrameReport::NoteDrawContext(bool toBackBuffer, bool usingSceneCamera, bool depthTestOn,
                                      uint32_t viewportX, uint32_t viewportWidth, bool duplicated,
                                      float minZ, float maxZ)
    {
        if (!m_active)
        {
            return;
        }

        ++m_drawIndex;

        if (m_phaseCount > 0)
        {
            DrawPhase& current = m_phases[m_phaseCount - 1];
            if (current.toBackBuffer == toBackBuffer
                && current.usingSceneCamera == usingSceneCamera
                && current.depthTestOn == depthTestOn
                && current.viewportX == viewportX
                && current.viewportWidth == viewportWidth
                && current.duplicated == duplicated
                && current.minZ == minZ && current.maxZ == maxZ)
            {
                ++current.draws;
                return;
            }
        }

        if (m_phaseCount >= kMaxPhases)
        {
            return;
        }

        DrawPhase& phase = m_phases[m_phaseCount++];
        phase.toBackBuffer = toBackBuffer;
        phase.usingSceneCamera = usingSceneCamera;
        phase.depthTestOn = depthTestOn;
        phase.draws = 1;
        phase.firstDrawIndex = m_drawIndex;
        phase.viewportX = viewportX;
        phase.viewportWidth = viewportWidth;
        phase.duplicated = duplicated;
        phase.minZ = minZ;
        phase.maxZ = maxZ;
    }

    void FrameReport::LogDrawPhases() const
    {
        WOWVR_INFO("  Draw phases (a new line each time the drawing context changes):");
        for (int i = 0; i < m_phaseCount; ++i)
        {
            const DrawPhase& phase = m_phases[i];
            WOWVR_INFO("    from draw %-5u  %-5u draws  target=%-10s depth=%-7s blend=%-3s "
                       "viewport x=%-5u w=%-5u depth=%.4f..%.4f  %s",
                       phase.firstDrawIndex, phase.draws,
                       phase.toBackBuffer ? "backbuffer" : "offscreen",
                       phase.usingSceneCamera ? "tested" : "ignored",
                       phase.depthTestOn ? "on" : "off",
                       phase.viewportX, phase.viewportWidth,
                       phase.minZ, phase.maxZ,
                       phase.duplicated ? "PER-EYE" : "once");
        }
        if (m_phaseCount >= kMaxPhases)
        {
            WOWVR_INFO("    (phase table full, later changes not recorded)");
        }
    }

    void FrameReport::NoteRenderTarget(void* surface, uint32_t width, uint32_t height, uint32_t format)
    {
        if (!m_active)
        {
            return;
        }

        for (int i = 0; i < m_renderTargetCount; ++i)
        {
            if (m_renderTargets[i].surface == surface)
            {
                m_currentRenderTarget = i;
                return;
            }
        }

        if (m_renderTargetCount >= kMaxRenderTargets)
        {
            m_currentRenderTarget = -1;
            return;
        }

        RenderTargetUse& use = m_renderTargets[m_renderTargetCount];
        use.surface = surface;
        use.width = width;
        use.height = height;
        use.format = format;
        use.drawsWhileBound = 0;
        m_currentRenderTarget = m_renderTargetCount++;
    }

    void FrameReport::LogConstantBlocks() const
    {
        WOWVR_INFO("  Vertex shader constant uploads: %d distinct (start, count) blocks%s",
                   m_constantBlockCount,
                   m_constantBlocksDropped > 0 ? " (table full, some dropped)" : "");

        // Blocks uploaded a handful of times per frame with constant contents are the
        // shared camera matrices. Blocks uploaded thousands of times with changing
        // contents are per-object world matrices.
        for (int i = 0; i < m_constantBlockCount; ++i)
        {
            const ConstantBlock& block = m_constantBlocks[i];
            WOWVR_INFO("    c%-3u x%-3u  calls=%-6u  %s",
                       block.startRegister, block.vector4Count, block.calls,
                       block.valuesCaptured
                           ? (block.valuesEverChanged ? "contents vary" : "contents constant")
                           : "");
        }

        // WoW uploads its camera as one bulk call spanning many registers, so the
        // interesting matrices sit at unknown offsets inside a block rather than at
        // its start. Slide a 4-register window over everything captured and report
        // only the windows whose shape is actually matrix-like.
        WOWVR_INFO("  Matrix-shaped windows inside constant uploads:");
        int hits = 0;
        for (int i = 0; i < m_constantBlockCount && hits < 24; ++i)
        {
            const ConstantBlock& block = m_constantBlocks[i];
            if (!block.valuesCaptured || block.capturedRegisters < 4)
            {
                continue;
            }

            for (uint32_t offset = 0; offset + 4 <= block.capturedRegisters && hits < 24; ++offset)
            {
                const float* window = &block.firstValues[offset * 4];
                const char* shape = DescribeMatrix(window);

                const bool interesting = (std::strcmp(shape, "projective") == 0)
                    || (std::strcmp(shape, "rigid transform, row-vector layout") == 0)
                    || (std::strcmp(shape, "rigid transform, transposed layout") == 0);
                if (!interesting)
                {
                    continue;
                }

                char label[128];
                sprintf_s(label, "c%u (upload of %u regs from c%u, %u calls, %s)",
                          block.startRegister + offset, block.vector4Count, block.startRegister,
                          block.calls, block.valuesEverChanged ? "varies" : "constant");
                LogMatrix(label, window);
                ++hits;
            }
        }
        if (hits == 0)
        {
            WOWVR_INFO("    none found");
        }

        // The most heavily uploaded varying block is the per-object transform. If it
        // turns out to be a combined world-view-projection, Phase 5 has to decompose
        // it per draw; if it is just a world matrix, the shared camera can be patched
        // once per frame instead, which is far cheaper.
        const ConstantBlock* busiest = nullptr;
        for (int i = 0; i < m_constantBlockCount; ++i)
        {
            const ConstantBlock& block = m_constantBlocks[i];
            if (!block.valuesCaptured || !block.valuesEverChanged)
            {
                continue;
            }
            if (busiest == nullptr || block.calls > busiest->calls)
            {
                busiest = &block;
            }
        }
        if (busiest != nullptr)
        {
            WOWVR_INFO("  Busiest per-draw matrix block:");
            char label[80];
            sprintf_s(label, "c%u of %u (uploaded %u times, sample)",
                      busiest->startRegister, busiest->vector4Count, busiest->calls);
            LogMatrix(label, busiest->firstValues);
        }
    }

    void FrameReport::LogRenderTargets() const
    {
        WOWVR_INFO("  Render targets bound this frame: %d", m_renderTargetCount);
        for (int i = 0; i < m_renderTargetCount; ++i)
        {
            const RenderTargetUse& use = m_renderTargets[i];
            WOWVR_INFO("    %p  %ux%u fmt=%u  draws=%u",
                       use.surface, use.width, use.height, use.format, use.drawsWhileBound);
        }
    }

    void FrameReport::EndFrame()
    {
        if (!m_active)
        {
            return;
        }
        m_active = false;

        const uint32_t totalDraws = m_drawCalls[DrawPrimitive] + m_drawCalls[DrawIndexedPrimitive]
                                  + m_drawCalls[DrawPrimitiveUP] + m_drawCalls[DrawIndexedPrimitiveUP];

        WOWVR_INFO("  Draw calls: %u total (DrawPrimitive=%u DrawIndexed=%u UP=%u IndexedUP=%u)",
                   totalDraws, m_drawCalls[DrawPrimitive], m_drawCalls[DrawIndexedPrimitive],
                   m_drawCalls[DrawPrimitiveUP], m_drawCalls[DrawIndexedPrimitiveUP]);
        WOWVR_INFO("  Draws with no vertex shader (fixed function): %u", m_fixedFunctionDraws);
        WOWVR_INFO("  Vertex shader switches this frame: %u; vertex shaders created since "
                   "the hooks went in: %u", m_shaderChanges, m_shadersCreatedTotal);

        WOWVR_INFO("  SetTransform calls: VIEW=%u PROJECTION=%u WORLD=%u other=%u",
                   m_transformCalls[0], m_transformCalls[1], m_transformCalls[2], m_transformCalls[6]);

        for (int slot = 0; slot < 7; ++slot)
        {
            if (!m_sawTransform[slot])
            {
                continue;
            }
            const uint32_t state = (slot == 0) ? 2u : (slot == 1) ? 3u : (slot == 6) ? 0u : (254u + slot);
            LogMatrix(TransformStateName(state), &m_lastTransform[slot].m[0][0]);
        }

        LogRenderTargets();
        LogDrawPhases();
        LogConstantBlocks();

        WOWVR_INFO("==== Frame report complete ====");
    }

    FrameReport& Report()
    {
        return g_report;
    }
}
