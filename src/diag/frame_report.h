#pragma once

#include "core/math3d.h"

#include <cstdint>

namespace wowvr
{
    // A one-frame X-ray of what the game does to the D3D9 device.
    //
    // Everything downstream of here depends on knowing where WoW's camera actually
    // lives: whether it uses fixed-function transforms or vertex shader constants,
    // whether it uploads a shared view-projection once per frame or a combined
    // world-view-projection per draw, and which constant registers hold it. Guessing
    // at that would be a good way to waste days, so the answer gets measured.
    //
    // Costs one predicate per hooked call while inactive.
    class FrameReport
    {
    public:
        static constexpr int DrawPrimitive = 0;
        static constexpr int DrawIndexedPrimitive = 1;
        static constexpr int DrawPrimitiveUP = 2;
        static constexpr int DrawIndexedPrimitiveUP = 3;
        static constexpr int DrawKindCount = 4;

        void RequestFrame(unsigned long long frameNumber);
        void BeginFrame(unsigned long long frameNumber);
        bool IsActive() const { return m_active; }
        void EndFrame();

        void NoteTransform(uint32_t state, const float* matrix);
        void NoteVertexShaderConstants(uint32_t startRegister, const float* data, uint32_t vector4Count);
        void NoteVertexShaderCreated(uint32_t bytecodeBytes);
        void NoteVertexShaderSet(void* shader);
        void NoteFixedFunctionVertexPipeline();
        void NoteDraw(int kind);

        // Records what the device looked like for this draw, so the boundary between
        // the world pass and the UI pass can be found instead of guessed at.
        void NoteDrawContext(bool toBackBuffer, bool usingSceneCamera, bool depthTestOn);
        void NoteRenderTarget(void* surface, uint32_t width, uint32_t height, uint32_t format);

    private:
        static constexpr int kMaxConstantBlocks = 96;
        static constexpr int kMaxRenderTargets = 24;

        // Wide enough to hold several matrices' worth of a bulk upload. WoW pushes
        // its camera block as one call of 100+ registers, so capturing only the first
        // four shows the corner of it and hides everything that matters.
        static constexpr int kCapturedRegisters = 32;

        struct ConstantBlock
        {
            uint32_t startRegister = 0;
            uint32_t vector4Count = 0;
            uint32_t calls = 0;
            uint32_t capturedRegisters = 0;
            float firstValues[kCapturedRegisters * 4] = {};
            bool valuesCaptured = false;
            bool valuesEverChanged = false;
        };

        struct RenderTargetUse
        {
            void* surface = nullptr;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t format = 0;
            uint32_t drawsWhileBound = 0;
        };

        void LogConstantBlocks() const;
        void LogRenderTargets() const;
        void LogDrawPhases() const;

        // One entry per change of drawing context rather than per draw, which turns
        // 1100 draws into a handful of readable lines.
        static constexpr int kMaxPhases = 32;
        struct DrawPhase
        {
            bool toBackBuffer = false;
            bool usingSceneCamera = false;
            bool depthTestOn = false;
            uint32_t draws = 0;
            uint32_t firstDrawIndex = 0;
        };
        DrawPhase m_phases[kMaxPhases];
        int m_phaseCount = 0;
        uint32_t m_drawIndex = 0;

        bool m_active = false;
        bool m_requested = false;
        unsigned long long m_requestedFrame = 0;

        uint32_t m_transformCalls[8] = {};
        Mat4 m_lastTransform[8];
        bool m_sawTransform[8] = {};

        ConstantBlock m_constantBlocks[kMaxConstantBlocks];
        int m_constantBlockCount = 0;
        uint32_t m_constantBlocksDropped = 0;

        RenderTargetUse m_renderTargets[kMaxRenderTargets];
        int m_renderTargetCount = 0;
        int m_currentRenderTarget = -1;

        uint32_t m_drawCalls[DrawKindCount] = {};
        uint32_t m_shaderChanges = 0;
        uint32_t m_shadersCreatedTotal = 0;   // lifetime, never reset per frame
        uint32_t m_fixedFunctionDraws = 0;
        void* m_currentShader = nullptr;
        bool m_currentShaderIsFixedFunction = true;
    };

    FrameReport& Report();
}
