#include "stereo/fog_rewrite.h"

#include <cstring>

namespace wowvr
{
    namespace
    {
        // D3D9 shader token layout (d3d9types.h), kept local so this file needs no
        // DirectX headers and can be built into a stand-alone test.
        const uint32_t kEndToken = 0x0000FFFFu;
        const uint32_t kCommentOpcode = 0xFFFEu;

        enum Opcode : uint32_t
        {
            OpMov = 1, OpAdd = 2, OpSub = 3, OpMad = 4, OpMul = 5, OpRcp = 6, OpRsq = 7,
            OpDp3 = 8, OpDp4 = 9, OpMin = 10, OpMax = 11, OpLit = 16,
            OpCall = 25, OpCallNz = 26, OpLoop = 27, OpRet = 28, OpEndLoop = 29, OpLabel = 30,
            OpDcl = 31, OpPow = 32,
            OpRep = 38, OpEndRep = 39, OpIf = 40, OpIfc = 41, OpElse = 42, OpEndIf = 43,
            OpBreak = 44, OpBreakc = 45,
            OpDefB = 47, OpDefI = 48, OpDef = 81
        };

        enum RegisterType : uint32_t
        {
            RegTemp = 0, RegConst = 2, RegRastOut = 4, RegOutput = 6
        };

        const uint32_t kUsageFog = 11;
        const uint32_t kRastOutFog = 1;
        const uint32_t kSwizzleIdentity = 0xE4u;    // .xyzw
        const uint32_t kSwizzleReplicateX = 0x00u;  // .xxxx
        const uint32_t kSwizzleReplicateZ = 0xAAu;  // .zzzz
        const uint32_t kRelativeBit = 0x00002000u;

        uint32_t RegType(uint32_t token)
        {
            return ((token >> 28) & 0x7u) | ((token >> 8) & 0x18u);
        }
        uint32_t RegNum(uint32_t token) { return token & 0x7FFu; }
        uint32_t Swizzle(uint32_t token) { return (token >> 16) & 0xFFu; }
        uint32_t WriteMask(uint32_t token) { return (token >> 16) & 0xFu; }

        struct Instruction
        {
            size_t start = 0;       // index of the opcode token
            uint32_t opcode = 0;
            uint32_t length = 0;    // parameter tokens after the opcode token
            bool hasDestination = false;
            // Indices (into the token stream) of the destination and source register
            // tokens; relative-address tokens are skipped.
            size_t destination = 0;
            size_t sources[4] = {};
            int sourceCount = 0;
        };

        bool WritesDestination(uint32_t opcode)
        {
            return (opcode >= OpMov && opcode <= 24) || (opcode >= OpPow && opcode <= 37)
                || opcode == 46 /* mova */ || opcode == 94 /* setp */ || opcode == 95 /* texldl */
                || opcode == 66 /* tex */;
        }

        bool IsFlowControl(uint32_t opcode)
        {
            return (opcode >= OpCall && opcode <= OpLabel) || (opcode >= OpRep && opcode <= OpBreakc);
        }

        bool Parse(const uint32_t* tokens, std::vector<Instruction>& out, size_t& endIndex,
                   int& lastFlowControl, const char*& reason)
        {
            lastFlowControl = -1;
            size_t i = 1;
            for (int guard = 0; guard < 65536; ++guard)
            {
                const uint32_t token = tokens[i];
                if (token == kEndToken)
                {
                    endIndex = i;
                    return true;
                }
                const uint32_t opcode = token & 0xFFFFu;
                if (opcode == kCommentOpcode)
                {
                    i += 1 + ((token >> 16) & 0x7FFFu);
                    continue;
                }

                Instruction ins;
                ins.start = i;
                ins.opcode = opcode;
                ins.length = (token >> 24) & 0xFu;

                // Loops and branches are tolerated only BEFORE the fog ramp (the
                // procedural water runs its wave loop first); the ramp itself must be
                // straight-line code, checked by the caller.
                if (IsFlowControl(opcode))
                {
                    lastFlowControl = static_cast<int>(out.size());
                }

                if (opcode != OpDcl && opcode != OpDef && opcode != OpDefI && opcode != OpDefB)
                {
                    size_t p = i + 1;
                    const size_t end = i + 1 + ins.length;
                    bool first = true;
                    while (p < end)
                    {
                        const uint32_t param = tokens[p];
                        if (first && WritesDestination(opcode))
                        {
                            ins.hasDestination = true;
                            ins.destination = p;
                        }
                        else if (ins.sourceCount < 4)
                        {
                            ins.sources[ins.sourceCount++] = p;
                        }
                        first = false;
                        p += (param & kRelativeBit) ? 2 : 1;
                    }
                }
                else if (opcode == OpDcl && ins.length >= 2)
                {
                    ins.hasDestination = true;
                    ins.destination = i + 2;
                }

                out.push_back(ins);
                i += 1 + ins.length;
            }
            reason = "no end token";
            return false;
        }

        // The component a source supplies to the first component its instruction
        // writes.
        uint32_t SourceComponent(const uint32_t* tokens, const Instruction& ins, size_t source)
        {
            uint32_t mask = ins.hasDestination ? WriteMask(tokens[ins.destination]) : 1u;
            uint32_t first = 0;
            while (first < 4 && (mask & (1u << first)) == 0) { ++first; }
            if (first >= 4) { first = 0; }
            return (Swizzle(tokens[source]) >> (2 * first)) & 0x3u;
        }

        // The last instruction before 'before' that writes component 'component' of
        // temporary register 'reg'.
        int FindWriter(const uint32_t* tokens, const std::vector<Instruction>& list, int before,
                       uint32_t reg, uint32_t component)
        {
            for (int k = before - 1; k >= 0; --k)
            {
                const Instruction& ins = list[k];
                if (!ins.hasDestination || ins.opcode == OpDcl)
                {
                    continue;
                }
                const uint32_t dst = tokens[ins.destination];
                if (RegType(dst) == RegTemp && RegNum(dst) == reg
                    && (WriteMask(dst) & (1u << component)) != 0)
                {
                    return k;
                }
            }
            return -1;
        }

        const uint32_t kNoInput = 0xFFFFFFFFu;
        const uint32_t kRegInput = 1;

        // The vertex input register an instruction reads, if it reads exactly one.
        uint32_t InputOperand(const uint32_t* tokens, const Instruction& ins)
        {
            uint32_t found = kNoInput;
            for (int s = 0; s < ins.sourceCount; ++s)
            {
                const uint32_t src = tokens[ins.sources[s]];
                if (RegType(src) == kRegInput)
                {
                    if (found != kNoInput && found != RegNum(src))
                    {
                        return kNoInput;
                    }
                    found = RegNum(src);
                }
            }
            return found;
        }

        void NoteTemp(uint32_t token, bool& anyTemp, uint32_t& maxTemp)
        {
            if (RegType(token) == RegTemp)
            {
                anyTemp = true;
                if (RegNum(token) > maxTemp) { maxTemp = RegNum(token); }
            }
        }

        // A one-source instruction: opcode, destination, source.
        void Emit(std::vector<uint32_t>& out, uint32_t opcode, uint32_t dst, uint32_t src)
        {
            out.push_back(opcode | (2u << 24));
            out.push_back(dst);
            out.push_back(src);
        }
    }

    FogRewriteResult RewriteFogToRadial(const uint32_t* tokens, std::vector<uint32_t>& out,
                                        float distanceScale)
    {
        FogRewriteResult result;
        out.clear();
        if (tokens == nullptr)
        {
            result.reason = "no bytecode";
            return result;
        }

        const uint32_t version = tokens[0];
        const uint32_t major = (version >> 8) & 0xFFu;
        if ((version & 0xFFFF0000u) != 0xFFFE0000u || major < 2 || major > 3)
        {
            result.reason = "not a vs_2_0 / vs_3_0 shader";
            return result;
        }
        const uint32_t tempLimit = (major == 3) ? 32u : 12u;

        std::vector<Instruction> list;
        size_t endIndex = 0;
        int lastFlowControl = -1;
        if (!Parse(tokens, list, endIndex, lastFlowControl, result.reason))
        {
            return result;
        }

        // Which output register carries fog.
        bool haveFog = false;
        uint32_t fogType = RegRastOut;
        uint32_t fogNum = kRastOutFog;
        uint32_t maxTemp = 0;
        bool anyTemp = false;
        for (const Instruction& ins : list)
        {
            if (ins.opcode == OpDcl && ins.hasDestination)
            {
                const uint32_t usage = tokens[ins.start + 1] & 0x1Fu;
                const uint32_t dst = tokens[ins.destination];
                if (major == 3 && usage == kUsageFog && RegType(dst) == RegOutput)
                {
                    fogType = RegOutput;
                    fogNum = RegNum(dst);
                    haveFog = true;
                }
                continue;
            }
            if (ins.hasDestination) { NoteTemp(tokens[ins.destination], anyTemp, maxTemp); }
            for (int s = 0; s < ins.sourceCount; ++s)
            {
                NoteTemp(tokens[ins.sources[s]], anyTemp, maxTemp);
            }
        }
        if (major == 2)
        {
            haveFog = true;   // oFog is a fixed rasteriser output in vs_2_0
        }
        if (!haveFog)
        {
            result.reason = "no fog output";
            return result;
        }

        const uint32_t scratch = anyTemp ? maxTemp + 1 : 0;
        if (scratch >= tempLimit)
        {
            result.reason = "no free temporary register";
            return result;
        }

        // The last write of the fog output.
        int fogWrite = -1;
        for (int k = static_cast<int>(list.size()) - 1; k >= 0; --k)
        {
            const Instruction& ins = list[k];
            if (ins.hasDestination && ins.opcode != OpDcl
                && RegType(tokens[ins.destination]) == fogType
                && RegNum(tokens[ins.destination]) == fogNum)
            {
                fogWrite = k;
                break;
            }
        }
        if (fogWrite < 0)
        {
            result.reason = "fog output never written";
            return result;
        }

        // Walk back through the fog ramp (min <- pow <- max <- mad) to the instruction
        // that reads view-space depth: a temp with a replicated .z swizzle combined with
        // a constant.
        int ramp = -1;
        size_t depthOperand = 0;
        int current = fogWrite;
        for (int depth = 0; depth < 6 && current >= 0; ++depth)
        {
            const Instruction& ins = list[current];
            const uint32_t op = ins.opcode;
            // Only straight-line code after the last loop or branch: there, "the last
            // earlier write" really is the value the instruction reads.
            if (current <= lastFlowControl)
            {
                break;
            }
            // lit is how some shader model 2 variants raise the ramp to its power.
            if (op != OpMov && op != OpAdd && op != OpMul && op != OpMad && op != OpMax
                && op != OpMin && op != OpPow && op != OpLit)
            {
                break;
            }

            if (op == OpMad || op == OpMul || op == OpAdd)
            {
                bool hasConst = false;
                int depthSource = -1;
                for (int s = 0; s < ins.sourceCount; ++s)
                {
                    const uint32_t src = tokens[ins.sources[s]];
                    if (RegType(src) == RegConst) { hasConst = true; }
                    if (RegType(src) == RegTemp && Swizzle(src) == kSwizzleReplicateZ
                        && (src & kRelativeBit) == 0)
                    {
                        depthSource = s;
                    }
                }
                if (hasConst && depthSource >= 0)
                {
                    ramp = current;
                    depthOperand = ins.sources[depthSource];
                    break;
                }
            }

            // Follow the first temporary source further back.
            int next = -1;
            for (int s = 0; s < ins.sourceCount; ++s)
            {
                const uint32_t src = tokens[ins.sources[s]];
                if (RegType(src) == RegTemp)
                {
                    next = FindWriter(tokens, list, current, RegNum(src),
                                      SourceComponent(tokens, ins, ins.sources[s]));
                    break;
                }
            }
            current = next;
        }
        if (ramp < 0)
        {
            result.reason = "fog is not a ramp on view depth";
            return result;
        }

        const uint32_t view = RegNum(tokens[depthOperand]);

        // Where the distance is taken: straight after the last write of rV.z before the
        // ramp, NOT at the ramp. Shader compilers reuse the position register once the
        // position has been projected - a reflection or light vector packed into rV.xy
        // while rV.z is kept for the fog - so at the ramp only z is still the
        // position. Right after z's last write, x and y have not been touched yet.
        const int depthWriter = FindWriter(tokens, list, ramp, view, 2);
        if (depthWriter < 0 || depthWriter <= lastFlowControl)
        {
            result.reason = "view depth written inside flow control";
            return result;
        }

        // And x and y must really be the position at that point. Only the two shapes
        // the client's compilers produce are accepted: one instruction writing xyz
        // together (mad/add of a matrix row chain), or three dp4s, one per axis, all
        // transforming the same vertex input (skinned models blend their bone rows in
        // between, so the three are not necessarily adjacent). Anything else - one
        // effect shader builds its position's x and y AFTER z - keeps the client's own
        // fog.
        const int xWriter = FindWriter(tokens, list, depthWriter + 1, view, 0);
        const int yWriter = FindWriter(tokens, list, depthWriter + 1, view, 1);
        const bool together = (xWriter == depthWriter && yWriter == depthWriter);
        const uint32_t input = InputOperand(tokens, list[depthWriter]);
        const bool perAxis = xWriter >= 0 && yWriter >= 0
            && list[xWriter].opcode == OpDp4 && list[yWriter].opcode == OpDp4
            && list[depthWriter].opcode == OpDp4 && input != kNoInput
            && InputOperand(tokens, list[xWriter]) == input
            && InputOperand(tokens, list[yWriter]) == input;
        if (!together && !perAxis)
        {
            result.reason = "position not a recognised xyz block";
            return result;
        }

        // dst rT.x: temp type (0), write mask x.
        // A register for the distance scale, if one is wanted: unused by the shader,
        // and below every relatively addressed array, whose reach is unknowable.
        const bool scaled = distanceScale > 0.0f
            && (distanceScale < 0.999f || distanceScale > 1.001f);
        uint32_t scaleRegister = 0;
        if (scaled)
        {
            bool used[256] = {};
            uint32_t limit = 256;
            for (const Instruction& ins : list)
            {
                if (ins.opcode == OpDef || ins.opcode == OpDefI || ins.opcode == OpDefB)
                {
                    const uint32_t dst = tokens[ins.start + 1];
                    if (RegType(dst) == RegConst && RegNum(dst) < 256) { used[RegNum(dst)] = true; }
                    continue;
                }
                for (int s = 0; s < ins.sourceCount; ++s)
                {
                    const uint32_t src = tokens[ins.sources[s]];
                    if (RegType(src) != RegConst || RegNum(src) >= 256)
                    {
                        continue;
                    }
                    used[RegNum(src)] = true;
                    if ((src & kRelativeBit) != 0 && RegNum(src) < limit)
                    {
                        limit = RegNum(src);
                    }
                }
            }
            bool found = false;
            for (uint32_t r = limit; r-- > 0;)
            {
                if (!used[r])
                {
                    scaleRegister = r;
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                result.reason = "no free constant register for the fog distance";
                return result;
            }
        }

        const uint32_t scratchX = 0x80000000u | (0x1u << 16) | scratch;
        const uint32_t viewXyz = 0x80000000u | (kSwizzleIdentity << 16) | view;
        const uint32_t scratchSrcX = 0x80000000u | (kSwizzleReplicateX << 16) | scratch;

        const size_t insertAt = list[depthWriter].start + 1 + list[depthWriter].length;
        // The version token, then the scale's def (definitions belong at the top),
        // then the shader up to the insertion point.
        out.assign(tokens, tokens + 1);
        if (scaled)
        {
            const float inverse = 1.0f / distanceScale;
            uint32_t bits = 0;
            memcpy(&bits, &inverse, sizeof(bits));
            out.push_back(OpDef | (5u << 24));
            out.push_back(0x80000000u | (RegConst << 28) | (0xFu << 16) | scaleRegister);
            out.push_back(bits);
            out.push_back(0u);
            out.push_back(0u);
            out.push_back(0u);
        }
        out.insert(out.end(), tokens + 1, tokens + insertAt);
        out.push_back(OpDp3 | (3u << 24));    // dp3 rT.x, rV, rV
        out.push_back(scratchX);
        out.push_back(viewXyz);
        out.push_back(viewXyz);
        Emit(out, OpRsq, scratchX, scratchSrcX);
        Emit(out, OpRcp, scratchX, scratchSrcX);
        if (scaled)
        {
            out.push_back(OpMul | (3u << 24));    // mul rT.x, rT.x, cK.x
            out.push_back(scratchX);
            out.push_back(scratchSrcX);
            out.push_back(0x80000000u | (RegConst << 28) | (kSwizzleReplicateX << 16)
                          | scaleRegister);
        }

        // Everything from here on, the ramp included, has moved by the inserted tokens.
        const size_t rampOffset = out.size() - insertAt;
        out.insert(out.end(), tokens + insertAt, tokens + endIndex + 1);

        // Point the ramp at rT.x, keeping the operand's own modifier (negate etc.).
        const uint32_t original = tokens[depthOperand];
        out[depthOperand + rampOffset] = 0x80000000u | (original & 0x0F000000u)
                                       | (kSwizzleReplicateX << 16) | scratch;

        result.rewritten = true;
        result.viewRegister = view;
        result.scratchRegister = scratch;
        return result;
    }
}
