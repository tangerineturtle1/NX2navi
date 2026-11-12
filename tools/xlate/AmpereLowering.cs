// Switch 2 (NVN2) shader programs are SM86 (Ampere) code; the shader translator decodes Maxwell (SM5x) only. This file
// rewrites each SM86 instruction as one or more Maxwell instructions with the same meaning, so its Maxwell decoder,
// IR and SPIR-V backend run unchanged. Instructions this file does not know make the whole program fail with the opcode
// and the reason, so coverage grows by census (python nvn2_shaders.py over a NX2NAVI_SHADER_DUMP directory).
//
// Encoding facts: the SM70+ instruction field layouts; the Maxwell side is the translator's Decoders/InstTable.cs and
// InstDecoders.cs. Where SM86 and the translator's Maxwell model differ: MUFU.SIN/COS take turns, BSSY/BSYNC are
// reconvergence hints, bank 24 is never bound.
//
// Layout: graphics programs keep the first 0x50 bytes of the 0x80-byte Turing+ header (the same fields as Maxwell's
// header), then the instructions; Maxwell puts a scheduling word first in every 32 bytes counted from the first
// instruction (the translator's Decoder skips them), written as 0. Compute programs start at address 0.
//
// Registers: guest registers keep their numbers. Uniform registers live just above the highest register the program
// uses (UrBase, even so pairs stay aligned; URZ is RZ) and must end below R239. R239-R254 are this file's scratch
// registers; R237 and R238 hold the uniform predicate state (see "uniform predicates" below). A program that leaves
// no room for them (task and mesh programs use up to R252) is lowered again in spill mode: guest registers from
// SpillFrom up live in local memory after the program's own, and an instruction that names one works on a block of
// four temporaries (R188-R203, one block per aligned group of four guest registers, so pairs and quads stay
// contiguous) loaded before it and stored after it; uniform registers then take blocks of four from R204-R235 (eight
// groups: an Ocarina mesh program used more than the four R220-R235 held). A program with more than eight groups of
// uniform registers is lowered once more in the wide spill layout: guest registers from R156 spilled, temporaries
// R156-R171, uniform registers R172-R235 (sixteen groups, UR0-UR62). Programs that fit keep the first layout.
using System;
using System.Collections.Generic;

static class AmpereLowering
{
    const int RZ = 255, PT = 7, ScratchBase = 239;
    // Uniform predicates: UPk is bit k of RUP. An instruction that reads or writes uniform predicates runs with UPk
    // swapped into Pk: the guest's Pk saved to USAVE (P2R), UPk loaded (R2P), the instruction lowered as if its
    // predicate fields named P registers, written UPs merged back into RUP (P2R), the guest's P restored (R2P).
    const int RUP = 238, USAVE = 237, UrLimit = 237;
    // Scratch: operand A/B/C materialisation, a spare, texture coordinates, texture meta values, texture results.
    const int SA = 252, SB = 253, SC = 254, SX = 239, TexCoord = 240, TexMeta = 244, TexOut = 248;

    public class LoweringException : Exception
    {
        public LoweringException(string message) : base(message) { }
    }

    // The registers do not fit the plain layout: Lower retries in spill mode.
    sealed class RegisterPressureException : LoweringException
    {
        public RegisterPressureException(string message) : base(message) { }
    }

    // The uniform registers do not fit the spill layout's pool: Lower retries in the wide spill layout.
    sealed class UrPoolException : LoweringException
    {
        public UrPoolException(string message) : base(message) { }
    }

    // Spill layouts: guest registers from SpillFrom up in local memory, the temporaries from there, the uniform pool
    // after them up to UrPoolEnd.
    readonly record struct SpillLayout(int SpillFrom, int UrPool)
    {
        public int TempBase => SpillFrom;
        public int SpillBytes => (256 - SpillFrom) * 4;
    }
    const int UrPoolEnd = 236;
    static readonly SpillLayout Spill = new(188, 204), SpillWide = new(156, 172);

    // Task (NVN2 stage 6) and mesh (stage 7) programs. Both are lowered as compute programs: the stage buffer they
    // write with ISBEWR and read with ISBERD becomes shared memory, laid out by this record, and MeshSpirv.cs turns
    // the compute SPIR-V into a TaskEXT or MeshEXT entry point that hands the buffer to Vulkan.
    //   Mesh: vertex outputs at Attr: 32-vertex groups, a 32-word line per output component, word
    //         ((v >> 5) * Lines + line) * 32 + (v & 31); the primitive count at Map + 0 (ISBEWR region 1) and the
    //         primitive's vertex indices as bytes from Map + 4 (region 0); the task payload at Pay (ISBERD region 7).
    //   Task: the payload at Pay: the mesh workgroup count at Pay + 4, the data from Pay + 0x10 (region 7).
    // ISBERD of address 0 in region 15 (no task) or 7 reads the workgroup's index. Control section fields
    // (runtime/gfx_shader.c build_request): +0x58 low 16 bits the workgroup size, bits 24-31 the payload size in 256
    // bytes; +0x5c byte 3 the primitive type (1 points, 2 lines, 3 triangles), byte 2 set when a task stage feeds the
    // mesh stage; +0x60 low 16 bits the vertex limit, high 16 bits the primitive limit.
    public sealed class MeshInfo
    {
        public int Stage, Threads, MaxVertices, MaxPrimitives, Topology, PayloadBytes;
        public bool HasTask;
        // Output lines in order: Maxwell attribute address of each component (0x60.. system values, 0x80 + 16g + 4c
        // generic g component c).
        public readonly List<int> Lines = new();
        public int Attr, Map, Pay, SharedBytes;
        public int Groups => (MaxVertices + 31) / 32;
        public int IndicesPerPrimitive => Topology switch { 1 => 1, 2 => 2, _ => 3 };

        // Lines from the output map of the program header (Turing+ VTG header, the same bytes as Maxwell's: system
        // values B at byte 0x35, attributes 0x60-0x7c one bit each; generic vectors from byte 0x36, a nibble each,
        // component x in bit 0). System values A (0x34) and C (0x48) are not expected in mesh programs.
        public void Layout(byte[] sph)
        {
            if (Stage == 7)
            {
                if (sph[0x34] != 0 || sph[0x48] != 0 || sph[0x49] != 0) throw new LoweringException("mesh output map: system values A or C");
                for (int b = 0; b < 8; b++) if ((sph[0x35] >> b & 1) != 0) Lines.Add(0x60 + 4 * b);
                for (int g = 0; g < 32; g++)
                    for (int c = 0; c < 4; c++)
                        if ((sph[0x36 + g / 2] >> (4 * (g & 1) + c) & 1) != 0) Lines.Add(0x80 + 16 * g + 4 * c);
                Attr = 0;
                Map = (Attr + Groups * Lines.Count * 128 + 15) & ~15;
                Pay = (Map + 4 + IndicesPerPrimitive * MaxPrimitives + 15) & ~15;
                SharedBytes = Pay + (HasTask ? PayloadBytes : 0);
            }
            else { Pay = 0; SharedBytes = PayloadBytes; }
            if (SharedBytes == 0) SharedBytes = 16;
        }
    }

    struct Insn
    {
        public ulong Lo, Hi;
        public uint W0 => (uint)Lo;
        public uint W1 => (uint)(Lo >> 32);
        public uint W2 => (uint)Hi;
        public uint Bits(int offset, int count)
        {
            ulong v = offset >= 64 ? Hi >> (offset - 64) : offset + count <= 64 ? Lo >> offset : (Lo >> offset) | (Hi << (64 - offset));
            return (uint)(v & ((1UL << count) - 1));
        }
        public bool Bit(int offset) => Bits(offset, 1) != 0;
    }

    sealed class Lowerer
    {
        readonly Insn[] _code;
        readonly List<ulong> _out = new();
        readonly List<(int output, int targetSource)> _branches = new();
        readonly int[] _firstOutput;
        Insn _i;
        int _index;
        ulong _guard;   // the SM86 instruction's predicate, in Maxwell position (bits 16-19)

        readonly byte[] _header;   // the program header, null for compute
        readonly int UrBase;   // R(UrBase + n) holds URn; guest registers are below it
        readonly bool _compute;
        readonly int[] _table;   // NVN2 compute binding table: start byte, texture entries, image entries (or null)
        readonly MeshInfo _mesh;   // task or mesh program (lowered as compute), else null
        public byte[] C1;          // the program's c1 constants (NVN2: after the code; BRX jump tables), or null
        readonly int _spillAt = -1;   // spill mode: byte offset of guest register SpillFrom in local memory; -1 otherwise
        readonly SpillLayout _sl;      // spill mode: the layout
        int SpillFrom => _sl.SpillFrom;
        readonly Dictionary<int, int> _temps = new();   // spill mode: guest register group (r & ~3) -> its temporaries, this instruction
        readonly Dictionary<int, int> _urBlocks = new();   // spill mode: uniform register group (n & ~3) -> its registers
        public Lowerer(Insn[] code, byte[] header, bool compute, int[] table, MeshInfo mesh, int spillAt = -1, SpillLayout layout = default)
        {
            _code = code; _header = header; _compute = compute; _table = table; _mesh = mesh; _firstOutput = new int[code.Length + 1];
            _spillAt = spillAt;
            _sl = layout.SpillFrom != 0 ? layout : Spill;
            int highest = HighestGpr(code);
            if (spillAt < 0 && highest >= UrLimit) throw new RegisterPressureException($"guest registers up to R{highest}");
            UrBase = spillAt >= 0 ? SpillFrom : (highest + 2) & ~1;
            LastUrBase = UrBase;
        }

        // Spill mode: the temporary that holds guest register r for this instruction, loading its group on first use.
        int Spilled(int r)
        {
            int g = r & ~3;
            if (!_temps.TryGetValue(g, out int t))
            {
                if (_temps.Count == 4) throw Fail("more than four spilled register groups in one instruction");
                t = _sl.TempBase + 4 * _temps.Count;
                _temps[g] = t;
                for (int k = 0; k < 4; k++)   // LDL.32 t+k, [RZ + slot]
                    Emit(Unguarded(0xef40000000000000UL | (uint)(t + k) | ((ulong)RZ << 8) | ((ulong)(uint)(_spillAt + 4 * (g - SpillFrom + k)) << 20) | (4UL << 48)));
            }
            return t + (r & 3);
        }

        // Spill mode: the temporaries of this instruction back to local memory.
        void SpillStore()
        {
            foreach (var (g, t) in _temps)
                for (int k = 0; k < 4; k++)   // STL.32 [RZ + slot], t+k
                    Emit(Unguarded(0xef50000000000000UL | (uint)(t + k) | ((ulong)RZ << 8) | ((ulong)(uint)(_spillAt + 4 * (g - SpillFrom + k)) << 20) | (4UL << 48)));
            _temps.Clear();
        }

        // The highest general register the program names (destination, A, B in the register form, C), counting the
        // register fields of uniform-datapath instructions too: an overestimate only moves the uniform registers up.
        static int HighestGpr(Insn[] code)
        {
            int max = -1;
            foreach (var i in code)
            {
                void see(uint r) { if (r < RZ && r > max) max = (int)r; }
                uint op = i.Bits(0, 12), b = op & 0x1ff;
                if (b is >= 0x140 and <= 0x15f) continue;   // branches, calls, exits, barriers: no register fields
                see(i.Bits(16, 8)); see(i.Bits(24, 8));
                // Register ranges the base fields do not show: LDSM writes Rd..Rd+count-1; TXD reads Ra..Ra+2 and Rb..Rb+3
                // and writes the second destination pair; HMMA reads A (K/4 registers), B (K/8) and C and writes D, where
                // C and D are a pair (.F16) or four registers (.F32, bit 76).
                if (op == 0x83b && i.Bits(16, 8) != RZ) see(i.Bits(16, 8) + (1u << (int)i.Bits(72, 2)) - 1);
                if (op == 0xb6c) { see(i.Bits(16, 8) + 1); see(i.Bits(24, 8) + 2); see(i.Bits(32, 8) + 3); see(i.Bits(64, 8) + 1); }
                if (op == 0x23c)
                {
                    uint k16 = i.Bits(75, 1), cd = i.Bit(76) ? 3u : 1u;
                    see(i.Bits(16, 8) + cd); see(i.Bits(24, 8) + 1 + 2 * k16); see(i.Bits(32, 8) + k16); see(i.Bits(64, 8) + cd);
                }
                // 64- and 128-bit loads write Rd..Rd+1 or Rd..Rd+3, the stores read Rb..Rb+1 or Rb..Rb+3 (size 73-75: 5, 6).
                // A program that names only Rd of its highest quad would otherwise put UR0 and UR1 in its top registers.
                if (op is 0x381 or 0x981 or 0x984 or 0x983 or 0x386 or 0x986 or 0x388 or 0x988 or 0x387 or 0x987)
                {
                    uint size = i.Bits(73, 3), reg = op is 0x381 or 0x981 or 0x984 or 0x983 ? i.Bits(16, 8) : i.Bits(32, 8);
                    if (size is 5 or 6 && reg < RZ) see(reg + (size == 6 ? 3u : 1u));
                }
                if (op == 0x3a9) see(i.Bits(64, 8));                                // ATOMG.CAS: the new value
                if (op == 0x593 || (op == 0xb66 && i.Bit(78))) see(i.Bits(32, 8));   // SUATOM data, TLD.MS sample index
                if (b < 0x080 || b is >= 0x160 and <= 0x16f) see(i.Bits(64, 8));   // C of ALU forms, TEX second destinations
                if (i.Bits(9, 3) == 1) see(i.Bits(32, 8));
            }
            return max;
        }

        LoweringException Fail(string reason) =>
            new($"sm86 instruction {_index} (0x{_index * 16:x}) op {_i.Bits(0, 12):x3} [{_i.W0:x8} {_i.W1:x8} {_i.W2:x8} {(uint)(_i.Hi >> 32):x8}]: {reason}");

        void Emit(ulong word) => _out.Add(word);
        // Guarded by the instruction's own predicate.
        void EmitG(ulong word) => _out.Add(word | _guard);
        static ulong Unguarded(ulong word) => word | ((ulong)PT << 16);

        // ---- Maxwell building blocks -------------------------------------------------------------------------------
        // guarded: under the instruction's predicate (a guest-visible write); otherwise unconditional (scratch).
        void Put(ulong word, bool guarded) => _out.Add(guarded ? word | _guard : Unguarded(word));
        void Mov(int dst, int src, bool guarded = true) => Put(0x5c98000000000000UL | (15UL << 39) | ((ulong)src << 20) | (uint)dst, guarded);
        void Mov32i(int dst, uint imm, bool guarded = false) => Put(0x0100000000000000UL | (15UL << 12) | ((ulong)imm << 20) | (uint)dst, guarded);
        void MovC(int dst, int bank, int byteOffset, bool guarded = false) =>
            Put(0x4c98000000000000UL | (15UL << 39) | ((ulong)(byteOffset / 4) << 20) | ((ulong)bank << 34) | (uint)dst, guarded);
        void Lop32i(int dst, int a, uint imm, int op) => Emit(Unguarded(0x0400000000000000UL | ((ulong)op << 53) | ((ulong)imm << 20) | ((ulong)a << 8) | (uint)dst));
        void Fmul32i(int dst, int a, uint imm) => Emit(Unguarded(0x1e00000000000000UL | ((ulong)imm << 20) | ((ulong)a << 8) | (uint)dst));
        void Iadd3(int dst, int a, int b, int c, bool guarded, bool negB = false) =>
            Put(0x5cc0000000000000UL | ((ulong)c << 39) | ((ulong)b << 20) | ((ulong)a << 8) | (uint)dst | (negB ? 1UL << 50 : 0), guarded);
        // Rd = p ? a : b (p: index | negate << 3)
        void Sel(int dst, int a, int b, uint p, bool guarded) =>
            Put(0x5ca0000000000000UL | ((ulong)(p & 7) << 39) | ((ulong)(p >> 3) << 42) | ((ulong)b << 20) | ((ulong)a << 8) | (uint)dst, guarded);
        // Pd = a < b, unsigned (the carry out of a + b is sum < a)
        void IsetpLtU(int pd, int a, int b) =>
            EmitG(0x5b60000000000000UL | (1UL << 49) | ((ulong)PT << 39) | ((ulong)a << 8) | ((ulong)b << 20) | ((ulong)pd << 3) | PT);

        // ---- SM86 operand decoding ---------------------------------------------------------------------------------
        uint Form => _i.Bits(9, 3);
        int Dst => (int)_i.Bits(16, 8);
        int RegA => (int)_i.Bits(24, 8);

        int Ur(uint reg)
        {
            if (reg == 63) return RZ;
            if (reg > 62) throw Fail($"UR{reg}");
            if (_spillAt >= 0)
            {
                int q = (int)reg & ~3;
                if (!_urBlocks.TryGetValue(q, out int b))
                {
                    b = _sl.UrPool + 4 * _urBlocks.Count;
                    if (b + 4 > UrPoolEnd) throw new UrPoolException(Fail("more uniform register groups than spill mode has room for").Message);
                    _urBlocks[q] = b;
                }
                return b + ((int)reg & 3);
            }
            if (UrBase + (int)reg >= UrLimit) throw new RegisterPressureException($"UR{reg} has no free register (guest registers up to R{UrBase - 1})");
            return UrBase + (int)reg;
        }

        // While a uniform-datapath instruction is lowered through its vector twin (UniformAlu), register fields name
        // uniform registers.
        bool _uniform;

        int Gpr(uint reg)
        {
            if (_uniform) return reg == RZ ? RZ : Ur(reg);
            if (reg != RZ && _spillAt >= 0 && reg >= SpillFrom) return Spilled((int)reg);
            if (reg != RZ && reg >= UrBase) throw Fail($"R{reg} above the registers the program was scanned for");
            return (int)reg;
        }

        // Constant bank 24 holds NVN2 driver data the shaders read (fullscreen passes: texel size, viewport rectangle);
        // Maxwell has 18 banks, so it is bank 17 here and the runtime binds it there.
        static int Bank(uint bank) => bank == 24 ? 17 : (int)bank;

        // Constant-buffer operand: byte offset 38-53, bank 54-58.
        (int bank, int offset) Cbuf()
        {
            int bank = Bank(_i.Bits(54, 5)), offset = (int)_i.Bits(38, 16);
            if (bank >= 18 || (offset & 3) != 0 || (_i.Bit(91) && !_uniform)) throw Fail($"constant buffer operand c[{_i.Bits(54, 5)}][0x{offset:x}]");
            return (bank, offset);
        }

        // Source 0 (A), 1 (B) or 2 (C) with its float modifiers. Forms (bits 9-11): 1 B and C registers, 2 C immediate,
        // 3 C constant, 4 B immediate, 5 B constant, 6 B uniform, 7 C uniform. A wide (immediate/constant/uniform) operand
        // uses bits 32-63; the register operand that would sit there then moves to bits 64-71.
        (int reg, bool abs, bool neg) Raw(int which, int scratch)
        {
            if (which == 0) return (Gpr(_i.Bits(24, 8)), _i.Bit(73), _i.Bit(72));
            uint form = Form;
            if (form == 0) throw Fail("source form 0");
            bool wide = which == 1 ? form is 4 or 5 or 6 : form is 2 or 3 or 7;
            if (wide)
            {
                if (form is 4 or 2) { Mov32i(scratch, _i.W1); return (scratch, false, false); }
                if (form is 5 or 3) { var (bank, off) = Cbuf(); MovC(scratch, bank, off); return (scratch, _i.Bit(62), _i.Bit(63)); }
                return (Ur(_i.Bits(32, 8)), _i.Bit(62), _i.Bit(63));
            }
            bool upper = which == 2 || form is 2 or 3 or 7;
            return upper ? (Gpr(_i.Bits(64, 8)), _i.Bit(74), _i.Bit(75)) : (Gpr(_i.Bits(32, 8)), _i.Bit(62), _i.Bit(63));
        }

        // The second operand of a two-source instruction sits in the B slot, or in the C slot for forms 2, 3 and 7.
        int SecondSlot => Form is 2 or 3 or 7 ? 2 : 1;

        // Applies float abs/neg in a scratch register (bit operations, so -0.0 and NaN payloads stay exact).
        int Mat(int reg, bool abs, bool neg, int scratch)
        {
            if (!abs && !neg) return reg;
            if (abs) { Lop32i(scratch, reg, 0x7fffffff, 0); reg = scratch; }
            if (neg) { Lop32i(scratch, reg, 0x80000000, 2); reg = scratch; }
            return reg;
        }

        int Plain(int which, int scratch)
        {
            var (r, _, _) = Raw(which, scratch);
            return r;
        }

        int FloatMat(int which, int scratch)
        {
            var (r, a, n) = Raw(which, scratch);
            return Mat(r, a, n, scratch);
        }

        // A uniform guard is swapped in by Run like any other uniform predicate operand.
        void RequireUniformGuardTrue() { }

        // Uniform-datapath instructions: their guard and predicate fields name uniform predicates.
        static bool IsUniformOp(uint op) => ((op & 0x1ff) is >= 0x080 and <= 0x0bf && op != 0x886) || op == 0x9c3;   // VOTEU: Vote

        // Uniform predicates the instruction reads and writes, as masks of UP indices (UPT excluded).
        (uint inUp, uint outUp) UniformPredicates()
        {
            uint op = _i.Bits(0, 12);
            if (!IsUniformOp(op)) return (0, 0);
            uint inUp = 0, outUp = 0;
            void use(ref uint mask, uint p) { if ((p & 7) != 7) mask |= 1u << (int)(p & 7); }
            use(ref inUp, _i.Bits(12, 3));
            switch (op & 0x1ff)
            {
                case 0x08c: use(ref outUp, _i.Bits(81, 3)); use(ref outUp, _i.Bits(84, 3)); use(ref inUp, _i.Bits(87, 3)); break;   // UISETP
                case 0x09c: use(ref outUp, _i.Bits(81, 3)); use(ref outUp, _i.Bits(84, 3));                                          // UPLOP3
                            use(ref inUp, _i.Bits(87, 3)); use(ref inUp, _i.Bits(77, 3)); use(ref inUp, _i.Bits(68, 3)); break;
                case 0x090: use(ref outUp, _i.Bits(81, 3)); use(ref outUp, _i.Bits(84, 3));                                          // UIADD3
                            if (_i.Bit(74)) { use(ref inUp, _i.Bits(87, 3)); use(ref inUp, _i.Bits(77, 3)); } break;
                case 0x092: use(ref outUp, _i.Bits(81, 3)); use(ref inUp, _i.Bits(87, 3)); break;                                     // ULOP3
                case 0x087: use(ref inUp, _i.Bits(87, 3)); break;                                                                    // USEL
                case 0x091: case 0x0a4: case 0x0a5: use(ref outUp, _i.Bits(81, 3)); if (_i.Bit(74)) use(ref inUp, _i.Bits(87, 3)); break;
            }
            return (inUp, outUp);
        }

        // ---- driver ----------------------------------------------------------------------------------------------
        public List<ulong> Run(out List<(int output, int targetSource)> branches, out int[] firstOutput)
        {
            Mov32i(RUP, 0);   // uniform predicates start false
            for (_index = 0; _index < _code.Length; _index++)
            {
                _i = _code[_index];
                _firstOutput[_index] = _out.Count;
                if (_i.Bits(12, 4) == 15) continue;   // @!PT: never runs (compiler padding)
                _guard = (ulong)_i.Bits(12, 4) << 16;
                var (inUp, outUp) = UniformPredicates();
                uint m = inUp | outUp;
                if (m != 0)
                {
                    Emit(Unguarded(0x38e8000000000000UL | ((ulong)m << 20) | ((ulong)RZ << 8) | USAVE));   // P2R: save guest P
                    Emit(Unguarded(0x38f0000000000000UL | ((ulong)m << 20) | ((ulong)RUP << 8)));          // R2P: load UPs
                }
                LowerOne();
                if (_temps.Count != 0) SpillStore();
                if (m != 0)
                {
                    if (outUp != 0) Emit(Unguarded(0x38e8000000000000UL | ((ulong)outUp << 20) | ((ulong)RUP << 8) | RUP));   // P2R merge
                    Emit(Unguarded(0x38f0000000000000UL | ((ulong)m << 20) | ((ulong)USAVE << 8)));        // R2P: restore
                }
            }
            _firstOutput[_code.Length] = _out.Count;
            branches = _branches;
            firstOutput = _firstOutput;
            return _out;
        }

        void LowerOne()
        {
            uint op = _i.Bits(0, 12);
            switch (op)
            {
                // Scheduling, dependency and reconvergence hints: the translator runs each thread on its own.
                case 0x918: // NOP
                case 0x91a: // DEPBAR
                case 0x945: // BSSY
                case 0x941: // BSYNC
                case 0x942: // BREAK
                case 0x948: // WARPSYNC
                case 0x9ab: // ERRBAR: waits for earlier memory errors to be reported, which the host GPU does not do here
                    return;

                case 0x202: case 0x802: case 0xa02: case 0xc02: Mov_(op); return;
                case 0x882: case 0xc82: Umov(op); return;
                case 0xab9: Uldc(); return;
                case 0x290: case 0x890: case 0xa90:                                 // UIADD3
                case 0x2a4: case 0x4a4: case 0x8a4: case 0xaa4: case 0x2a5: case 0x8a5: // UIMAD, UIMAD.WIDE
                case 0x292: case 0x892: case 0xa92:                                 // ULOP3
                case 0x299: case 0x499: case 0x899:                                 // USHF
                case 0x291: case 0x891:                                             // ULEA
                case 0x287: case 0x887:                                             // USEL
                case 0x28c: case 0x88c: case 0xa8c:                                 // UISETP
                case 0x89c:                                                         // UPLOP3
                case 0x296: case 0x896:                                             // UPRMT
                    UniformAlu(op); return;
                case 0x81c: Plop3(); return;
                case 0xb63: Tld4(); return;
                case 0x388: case 0x984: case 0x988: SharedAccess(op); return;
                case 0x83b: Ldsm(); return;
                case 0xb1d: Bar(); return;
                case 0x992: Membar(); return;
                case 0x305: case 0x905: case 0xb05: case 0xd05: F2i(op); return;
                case 0xb82: Ldc(); return;
                case 0x381: case 0x981: Ldg(op); return;
                case 0x386: case 0x986: Stg(op); return;
                case 0xb66: Tld(); return;
                case 0xb6c: Txd(); return;

                case 0x221: case 0x421: case 0x621: case 0x821: case 0xa21: case 0xc21: case 0xe21: Fadd(); return;
                case 0x220: case 0x420: case 0x620: case 0x820: case 0xa20: case 0xc20: Fmul(); return;
                case 0x223: case 0x423: case 0x623: case 0x823: case 0xa23: case 0xc23: case 0xe23: Ffma(); return;
                case 0x209: case 0x409: case 0x609: case 0x809: case 0xa09: case 0xc09: Fmnmx(); return;
                case 0x308: case 0x908: case 0xb08: case 0xd08: Mufu(op); return;
                case 0x307: case 0xb07: Frnd(op); return;
                case 0x20b: case 0x40b: case 0x60b: case 0x80b: case 0xa0b: case 0xc0b: Setp(true); return;
                case 0x20c: case 0x40c: case 0x60c: case 0x80c: case 0xa0c: case 0xc0c: Setp(false); return;
                case 0x20a: case 0x40a: case 0x60a: case 0x80a: case 0xa0a: case 0xc0a: Fset(); return;
                case 0x207: case 0x807: case 0xa07: case 0xc07: Sel_(false); return;
                case 0x208: case 0x808: case 0xa08: case 0xc08: Sel_(true); return;
                case 0x245: case 0x845: case 0xa45: case 0xc45: I2fp(); return;

                case 0x210: case 0x410: case 0x610: case 0x810: case 0xa10: case 0xc10: Iadd3_(); return;
                case 0x224: case 0x424: case 0x624: case 0x824: case 0xa24: case 0xc24: case 0xe24: Imad(); return;
                case 0x212: case 0x812: case 0xa12: case 0xc12: Lop3(); return;
                case 0x217: case 0x817: case 0xa17: case 0xc17: Imnmx(); return;
                case 0x219: case 0x419: case 0x619: case 0x819: case 0xa19: case 0xc19: Shf(); return;
                case 0x23e: case 0x43e: case 0x83e: case 0xa3e: case 0xc3e: F2fp(); return;
                case 0x216: case 0x416: case 0x616: case 0x816: case 0xa16: case 0xc16: Prmt(); return;
                case 0x309: case 0xd09: Popc(); return;
                case 0x300: case 0xd00: Flo(); return;
                case 0x301: Brev(); return;
                case 0x240: case 0x840: Hmnmx2(); return;
                case 0x234: case 0x434: Hsetp2(); return;
                case 0x803: case 0x804: PredicateRegister(op == 0x803); return;
                case 0x59f: Sured(); return;
                case 0x593: Suatom(); return;
                case 0x3a9: AtomgCas(); return;
                case 0x3c4: Redux(); return;
                case 0x81a: Sgxt(); return;
                case 0x806: case 0x886: Vote(op == 0x886); return;
                case 0x3a1: Match(); return;
                case 0xf8c: AtomsPopcInc(); return;
                case 0x3a8: case 0x9a8: Atomg(op == 0x9a8); return;
                case 0x38c: Atoms(); return;
                case 0x98e: Red(); return;
                case 0x389: case 0x589: case 0x989: case 0xf89: Shfl(op); return;
                case 0x822: Fswzadd(); return;
                case 0x230: case 0x430: case 0x630: Half(0x5d10000000000000UL); return;               // HADD2
                case 0x232: case 0x832: case 0xa32: Half(0x5d08000000000000UL); return;               // HMUL2
                case 0x231: case 0x431: case 0x631: case 0x831: case 0xa31: Half(0x5d00000000000000UL); return;   // HFMA2
                case 0xb9b: SurfaceAccess(true); return;
                case 0xb97: SurfaceAccess(false); return;
                case 0x211: case 0x811: case 0xa11: case 0xc11: Lea(); return;

                case 0x321: case 0x322: AttributeAccess(op == 0x322); return;
                case 0x326: Ipa(); return;
                case 0x805: Cs2r(); return;
                case 0x919: case 0x9c3: S2r(op == 0x9c3); return;
                case 0x927: Isbewr(); return;
                case 0x949: Brx(false); return;
                case 0x958: Brx(true); return;
                case 0x923: Isberd(); return;
                case 0x3c2: R2ur(); return;
                case 0x225: case 0x227: case 0x625: case 0x825: case 0x827: case 0xa27: ImadWide(op); return;
                case 0x213: case 0xa13: case 0xc13: Iabs(); return;
                case 0xb6f: Txq(); return;
                case 0xb60: case 0xf60: case 0x361: Tex(op); return;
                case 0xb69: case 0xf69: case 0x36a: Tmml(op); return;
                case 0xf66: TldBindless(); return;
                case 0x370: TxqBindless(); return;
                case 0x95b: Kill(); return;
                case 0x94d: Exit(); return;
                case 0x947: Bra(); return;
                case 0x944: Call(); return;
                case 0x950: Ret(); return;
                case 0x2bd: Uflo(); return;
                case 0x2be: Ubrev(); return;
                case 0x2bf: Upopc(); return;
                case 0x883: Up2ur(); return;
                case 0x243: F2ipByte(); return;
                case 0x23c: Hmma(); return;
                case 0x23a: Movm(); return;
                case 0x306: case 0xb06: case 0xd06: I2f(op); return;
                case 0x387: case 0x987: case 0x983: LocalAccess(op != 0x983); return;
                case 0x526: IpaIndexed(); return;
                case 0xabb: UldcIndexed(); return;
            }
            throw Fail("opcode not lowered yet");
        }

        // ---- moves and uniform datapath ----------------------------------------------------------------------------
        void Mov_(uint op)
        {
            if (_i.Bits(72, 4) != 15) throw Fail("MOV lane mask");
            int d = Gpr((uint)Dst);
            switch (op)
            {
                case 0x802: Mov32i(d, _i.W1, true); return;
                case 0x202: Mov(d, Gpr(_i.Bits(32, 8))); return;
                case 0xc02: Mov(d, Ur(_i.Bits(32, 8))); return;
                default: { var (bank, off) = Cbuf(); MovC(d, bank, off, true); return; }
            }
        }

        void Umov(uint op)
        {
            RequireUniformGuardTrue();
            int d = Ur((uint)Dst);
            if (d == RZ) return;
            if (op == 0x882) Mov32i(d, _i.W1, true);
            else EmitG(0x5c98000000000000UL | (15UL << 39) | ((ulong)Ur(_i.Bits(32, 8)) << 20) | (uint)d);
        }

        // ULDC: width (bits 73-75) 4 = 32 bit, 5 = 64 bit into a uniform pair.
        void Uldc()
        {
            RequireUniformGuardTrue();
            uint width = _i.Bits(73, 3);
            int d = Ur((uint)Dst);
            if (width < 4)   // U8, S8, U16, S16: Maxwell LDC with the same width code, immediate address (any alignment)
            {
                // (this form, 0xab9, has no register index: bits 24-29 are not read, as for the 32- and 64-bit loads
                // below; compilers leave 0 there as well as 63, and the 0 cases failed. 0xabb is the indexed form.)
                int cb = Bank(_i.Bits(54, 5)), at = (int)_i.Bits(38, 16);
                if (cb >= 18 || at >= 0x8000) throw Fail($"ULDC width {width} c[{_i.Bits(54, 5)}][0x{at:x}]");
                if (d != RZ)
                    EmitG(0xef90000000000000UL | ((ulong)RZ << 8) | ((ulong)at << 20) | ((ulong)cb << 36) | ((ulong)width << 48) | (uint)d);
                return;
            }
            var (bank, off) = Cbuf();
            if (width == 4) { if (d != RZ) MovC(d, bank, off, true); return; }
            if (width == 5 && (Dst & 1) == 0) { if (d != RZ) { MovC(d, bank, off, true); MovC(d + 1, bank, off + 4, true); } return; }
            throw Fail($"ULDC width {width}");
        }

        // Uniform-datapath arithmetic (UIADD3, UIMAD, UIMAD.WIDE, ULOP3, USHF, ULEA, USEL) has the encoding of its vector
        // twin (base opcode - 0x80) with uniform registers in the register fields and bit 91 set; it is lowered as the
        // twin with the fields read as uniform registers; its predicate fields name uniform predicates, which Run has
        // swapped into the P registers of the same numbers.
        void UniformAlu(uint op)
        {
            uint twin = (op & 0x1ff) - 0x80;
            Insn saved = _i;
            _i.Lo = (_i.Lo & ~0x1ffUL) | twin;
            _i.Hi &= ~(1UL << 27);   // bit 91
            _uniform = true;
            try { LowerOne(); }
            finally { _uniform = false; _i = saved; }
        }

        // ---- floating point ----------------------------------------------------------------------------------------
        void Fadd()
        {
            if (_i.Bits(81, 10) != 0) throw Fail("FADD mode");
            var (a, aAbs, aNeg) = Raw(0, SA);
            var (b, bAbs, bNeg) = Raw(SecondSlot, SB);
            EmitG(0x5c58000000000000UL | ((ulong)b << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst)
                  | (aNeg ? 1UL << 48 : 0) | (aAbs ? 1UL << 46 : 0) | (bNeg ? 1UL << 45 : 0) | (bAbs ? 1UL << 49 : 0)
                  | ((ulong)_i.Bits(78, 2) << 39) | ((ulong)_i.Bits(80, 1) << 44) | ((ulong)_i.Bits(77, 1) << 50));
        }

        // FMUL: scale (bits 84-86) 1-3 divide by 8/4/2, 4 none, 5-7 multiply by 2/4/8; Maxwell's scale field is 0 none,
        // 1-3 divide by 2/4/8, 4-6 multiply by 8/4/2.
        static readonly uint[] FmulScale = { 0, 3, 2, 1, 0, 6, 5, 4 };
        void Fmul()
        {
            if (_i.Bit(76) || _i.Bits(81, 3) != 0 || _i.Bits(87, 4) != 0 || _i.Bits(84, 3) == 0) throw Fail("FMUL mode");
            var (a, aAbs, aNeg) = Raw(0, SA);
            var (b, bAbs, bNeg) = Raw(1, SB);
            a = Mat(a, aAbs, false, SA);
            b = Mat(b, bAbs, false, SB);
            EmitG(0x5c68000000000000UL | ((ulong)b << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst)
                  | (aNeg != bNeg ? 1UL << 48 : 0) | ((ulong)FmulScale[_i.Bits(84, 3)] << 41)
                  | ((ulong)_i.Bits(78, 2) << 39) | ((ulong)_i.Bits(80, 1) << 44) | ((ulong)_i.Bits(77, 1) << 50));
        }

        void Ffma()
        {
            if (_i.Bit(76) || _i.Bits(81, 10) != 0) throw Fail("FFMA mode");
            var (a, aAbs, aNeg) = Raw(0, SA);
            var (b, bAbs, bNeg) = Raw(1, SB);
            var (c, cAbs, cNeg) = Raw(2, SC);
            a = Mat(a, aAbs, false, SA);
            b = Mat(b, bAbs, false, SB);
            c = Mat(c, cAbs, false, SC);
            EmitG(0x5980000000000000UL | ((ulong)c << 39) | ((ulong)b << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst)
                  | (aNeg != bNeg ? 1UL << 48 : 0) | (cNeg ? 1UL << 49 : 0)
                  | ((ulong)_i.Bits(77, 1) << 50) | ((ulong)_i.Bits(78, 2) << 51) | ((ulong)_i.Bits(80, 1) << 53));
        }

        // FMNMX: Rd = p ? min : max (p in bits 87-90). .NAN (bit 81) is not lowered.
        void Fmnmx()
        {
            if (_i.Bits(74, 6) != 0 || _i.Bits(81, 6) != 0) throw Fail("FMNMX mode");
            var (a, aAbs, aNeg) = Raw(0, SA);
            var (b, bAbs, bNeg) = Raw(SecondSlot, SB);
            EmitG(0x5c60000000000000UL | ((ulong)b << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst)
                  | (aNeg ? 1UL << 48 : 0) | (aAbs ? 1UL << 46 : 0) | (bNeg ? 1UL << 45 : 0) | (bAbs ? 1UL << 49 : 0)
                  | ((ulong)_i.Bits(87, 3) << 39) | ((ulong)_i.Bits(90, 1) << 42) | ((ulong)_i.Bits(80, 1) << 44));
        }

        // MUFU: operation in bits 74-79, numbered as Maxwell's (0 COS, 1 SIN, 2 EX2, 3 LG2, 4 RCP, 5 RSQ, 8 SQRT).
        // SM70+ COS/SIN take the angle in turns (the compiler multiplies by 1/2pi first); the translator's take radians.
        // MUFU.F16 (bit 73, register form): the half in the low or, with bit 60, the high 16 bits of Rb, abs 62 and
        // neg 63; the result is a half in the low 16 bits. The upscaler's colour decode raises halves to a power this
        // way (LG2.F16, HMUL2, EX2.F16, the two lanes packed by PRMT). Lowered as F2F to F32, the F32 function and F2F
        // back to F16 (round to nearest), which writes the half with the high 16 bits clear.
        void Mufu(uint op)
        {
            uint operation = _i.Bits(74, 6);
            bool half = _i.Bit(73);
            if (operation > 8 || operation is 6 or 7 || (half && (op != 0x308 || _i.Bit(61))))
                throw Fail($"MUFU operation {operation}{(half ? ".F16" : "")}");
            int src; bool abs, neg;
            if (op == 0x908) { Mov32i(SB, _i.W1); (src, abs, neg) = (SB, false, false); }
            else if (half)
            {
                Emit(Unguarded(0x5ca8000000000000UL | (2UL << 8) | (1UL << 10) | ((ulong)Gpr(_i.Bits(32, 8)) << 20)
                               | (_i.Bit(60) ? 1UL << 41 : 0) | (_i.Bit(63) ? 1UL << 45 : 0) | (_i.Bit(62) ? 1UL << 49 : 0) | SB));   // F2F.F32.F16
                (src, abs, neg) = (SB, false, false);
            }
            else (src, abs, neg) = Raw(1, SB);
            if (operation <= 1)
            {
                src = Mat(src, abs, neg, SB);
                Fmul32i(SB, src, 0x40c90fdb);   // 2pi
                src = SB; abs = neg = false;
            }
            ulong mufu = 0x5080000000000000UL | ((ulong)operation << 20) | ((ulong)src << 8) | (abs ? 1UL << 46 : 0) | (neg ? 1UL << 48 : 0);
            if (!half) { EmitG(mufu | (uint)Gpr((uint)Dst)); return; }
            Emit(Unguarded(mufu | SB));
            EmitG(0x5ca8000000000000UL | (1UL << 8) | (2UL << 10) | ((ulong)SB << 20) | (uint)Gpr((uint)Dst));   // F2F.F16.F32
        }

        // FRND.F32: rounding (bits 78-79: nearest/floor/ceil/trunc) as Maxwell F2F.F32.F32 with integer rounding.
        void Frnd(uint op)
        {
            if (_i.Bits(75, 2) != 2 || _i.Bits(84, 2) != 2 || _i.Bits(81, 3) != 0 || _i.Bits(86, 6) != 0) throw Fail("FRND format");
            var (b, bAbs, bNeg) = Raw(1, SB);
            EmitG(0x5ca8000000000000UL | (2UL << 8) | (2UL << 10) | ((ulong)b << 20) | (uint)Gpr((uint)Dst)
                  | ((ulong)(8 + _i.Bits(78, 2)) << 39) | ((ulong)_i.Bits(80, 1) << 44) | (bNeg ? 1UL << 45 : 0) | (bAbs ? 1UL << 49 : 0));
        }

        // FSETP / ISETP: P (bits 81-83), Q (84-86), combined with Pc (87-90) by the operation in 74-75.
        void Setp(bool floating)
        {
            if (!floating && _i.Bit(72)) { SetpExtended(); return; }
            uint combine = _i.Bits(74, 2);
            if (combine > 2) throw Fail("predicate combination");
            var (a, aAbs, aNeg) = Raw(0, SA);
            var (b, bAbs, bNeg) = Raw(1, SB);
            ulong w = (floating ? 0x5bb0000000000000UL : 0x5b60000000000000UL) | _i.Bits(84, 3) | ((ulong)_i.Bits(81, 3) << 3)
                      | ((ulong)a << 8) | ((ulong)b << 20) | ((ulong)_i.Bits(87, 3) << 39) | ((ulong)_i.Bits(90, 1) << 42) | ((ulong)combine << 45);
            if (floating)
                w |= ((ulong)_i.Bits(80, 1) << 47) | ((ulong)_i.Bits(76, 4) << 48)
                     | (aNeg ? 1UL << 43 : 0) | (aAbs ? 1UL << 7 : 0) | (bNeg ? 1UL << 6 : 0) | (bAbs ? 1UL << 44 : 0);
            else
                w |= ((ulong)_i.Bits(73, 1) << 48) | ((ulong)_i.Bits(76, 3) << 49);
            EmitG(w);
        }

        // FSET.BF: Rd = 1.0 or 0.0.
        void Fset()
        {
            if (_i.Bits(64, 8) != 0 || _i.Bits(74, 2) == 3 || _i.Bits(81, 6) != 0) throw Fail("FSET output mode");
            var (a, aAbs, aNeg) = Raw(0, SA);
            var (b, bAbs, bNeg) = Raw(1, SB);
            EmitG(0x5800000000000000UL | ((ulong)b << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst)
                  | ((ulong)_i.Bits(87, 3) << 39) | ((ulong)_i.Bits(90, 1) << 42) | ((ulong)_i.Bits(74, 2) << 45)
                  | ((ulong)_i.Bits(76, 4) << 48) | (1UL << 52) | ((ulong)_i.Bits(80, 1) << 55)
                  | (aNeg ? 1UL << 43 : 0) | (bNeg ? 1UL << 53 : 0) | (aAbs ? 1UL << 54 : 0) | (bAbs ? 1UL << 44 : 0));
        }

        // SEL / FSEL: Rd = p ? a : b (p in bits 87-90). FSEL applies the float modifiers first.
        void Sel_(bool floating)
        {
            int a = floating ? FloatMat(0, SA) : Plain(0, SA);
            int b = floating ? FloatMat(1, SB) : Plain(1, SB);
            Sel(Gpr((uint)Dst), a, b, _i.Bits(87, 4), true);
        }

        // I2FP.F32.{U32,S32}: bit 74 signed, rounding in 78-79.
        void I2fp()
        {
            uint w2 = _i.W2 & ~(0x400u | 0xc000u | 0x08000000u);
            if (w2 != 0x00201000) throw Fail("I2FP format");
            int b = Plain(1, SB);
            EmitG(0x5cb8000000000000UL | (2UL << 8) | (2UL << 10) | ((ulong)_i.Bits(74, 1) << 13) | ((ulong)b << 20)
                  | ((ulong)_i.Bits(78, 2) << 39) | (uint)Gpr((uint)Dst));
        }

        // ---- integer -----------------------------------------------------------------------------------------------
        // IADD3 Rd, [Pu,] a, b, c. Negations are arithmetic (Maxwell IADD3's too). One carry out (bits 81-83) with c = RZ,
        // or .X (bit 74) adding the carry-in predicate of bits 87-90.
        void Iadd3_()
        {
            uint op = _i.Bits(0, 12);
            bool negA = _i.Bit(72), negB = op != 0x810 && _i.Bit(63) && Form is not 2 and not 3, negC = _i.Bit(75), extended = _i.Bit(74);
            uint carry1 = _i.Bits(81, 3), carry2 = _i.Bits(84, 3);
            int a = Plain(0, SA), b = Plain(1, SB), c = Plain(2, SC);
            int d = Gpr((uint)Dst);
            if (Form is 2 or 3 && (_i.Bit(63) || negC)) throw Fail("IADD3 negation with a wide third source");
            if (!extended && carry1 == 7 && carry2 == 7)
            {
                EmitG(0x5cc0000000000000UL | ((ulong)c << 39) | ((ulong)b << 20) | ((ulong)a << 8) | (uint)d
                      | (negA ? 1UL << 51 : 0) | (negB ? 1UL << 50 : 0) | (negC ? 1UL << 49 : 0));
                return;
            }
            // The 34-bit sum: low word in R244, carry count in R245 (IADD.CC / IADD.X). A negated source is ~x, plus 1
            // unless .X, whose carry-ins (bits 87-90 and 77-80) are added instead. Pu = bit 32, Pv = bit 33.
            const int lo = 244, hi = 245, one = 246;
            int inv(int r, bool neg, int t) { if (!neg) return r; Lop32i(t, r, 0xffffffff, 2); return t; }
            a = inv(a, negA, 241); b = inv(b, negB, 242); c = inv(c, negC, 243);
            void addc(int r)
            {
                Emit(Unguarded(0x5c10000000000000UL | (1UL << 47) | ((ulong)r << 20) | ((ulong)lo << 8) | lo));    // IADD.CC
                Emit(Unguarded(0x5c10000000000000UL | (1UL << 43) | ((ulong)RZ << 20) | ((ulong)hi << 8) | hi));   // IADD.X
            }
            Emit(Unguarded(0x5c10000000000000UL | (1UL << 47) | ((ulong)b << 20) | ((ulong)a << 8) | lo));
            Emit(Unguarded(0x5c10000000000000UL | (1UL << 43) | ((ulong)RZ << 20) | ((ulong)RZ << 8) | hi));
            addc(c);
            if (extended)
                foreach (uint p in new[] { _i.Bits(87, 4), _i.Bits(77, 4) })
                {
                    if (p == 15) continue;
                    Mov32i(one, 1);
                    if (p != 7) Sel(one, one, RZ, p, false);
                    addc(one);
                }
            else if (negA || negB || negC)
            {
                Mov32i(one, (uint)((negA ? 1 : 0) + (negB ? 1 : 0) + (negC ? 1 : 0)));
                addc(one);
            }
            if (d != RZ) Mov(d, lo);
            uint guard = _i.Bits(12, 3);
            foreach (int k in guard == carry1 ? new[] { 1, 0 } : new[] { 0, 1 })   // an output that is the guard goes last
            {
                uint p = k == 0 ? carry1 : carry2;
                if (p == 7) continue;
                Lop32i(one, hi, k == 0 ? 1u : 2u, 0);
                EmitG(0x5b60000000000000UL | (5UL << 49) | ((ulong)PT << 39) | ((ulong)RZ << 20) | ((ulong)one << 8) | ((ulong)p << 3) | PT);
            }
        }

        // IMAD Rd, a, b, c (low word); bit 73 signed, bit 75 negates c (~c with .X), .X (bit 74) adds the carry-in
        // predicate of bits 87-90. A carry out (bits 81-83) is not lowered; .HI/.WIDE are ImadWide.
        void Imad()
        {
            uint known = 0xffu | 1u << 9 | 1u << 10 | 1u << 11 | 7u << 17 | 0xfu << 23 | (Form is 6 or 7 ? 1u << 27 : 0u);
            bool x = _i.Bit(74);
            if ((_i.W2 & ~known) != 0 || _i.Bits(81, 3) != 7 || (!x && _i.Bits(87, 4) != 15)) throw Fail("IMAD mode");
            int a = Plain(0, SA), b = Plain(1, SB), c = Plain(2, SC);
            if (_i.Bit(75) && c != RZ)
            {
                if (x) Lop32i(SC, c, 0xffffffff, 2);                      // .X: ~c, the +1 comes as the carry
                else Iadd3(SC, RZ, c, RZ, false, negB: true);
                c = SC;
            }
            ulong signed = _i.Bit(73) ? (1UL << 48) | (1UL << 53) : 0;
            int d = Gpr((uint)Dst);
            ulong imad(int dst) => 0x5a00000000000000UL | ((ulong)c << 39) | ((ulong)b << 20) | ((ulong)a << 8) | (uint)dst | signed;
            if (!x) { EmitG(imad(d)); return; }
            Mov32i(SX, 1);
            Sel(SX, SX, RZ, _i.Bits(87, 4), false);
            Emit(Unguarded(imad(SA)));
            Iadd3(d, SA, SX, RZ, true);
        }

        // IMAD.HI Rd / IMAD.WIDE Rd:Rd+1 = a * b + (c+1:c), bit 73 signed. b: a register (bits 32-39), an immediate
        // (0x8xx) or a constant (0xa27); 0x625 takes b from bits 64-71 and adds a constant pair. The 64-bit sum uses
        // Maxwell IMUL.LO/.HI and IADD.CC/.X into R247:R248. A carry out (bits 81-83) is not lowered.
        void ImadWide(uint op)
        {
            bool wide = op is 0x225 or 0x625 or 0x825, signedMul = _i.Bit(73);
            if ((_i.W2 & ~0x000e02ffu) != 0x07800000 || _i.Bits(81, 3) != 7) throw Fail("IMAD.HI/WIDE mode");
            int a = Gpr(_i.Bits(24, 8)), b, cLo, cHi;
            if (op is 0x825 or 0x827) { Mov32i(SB, _i.W1); b = SB; }
            else if (op == 0xa27) { var (bank, off) = Cbuf(); MovC(SB, bank, off); b = SB; }
            else b = Gpr(_i.Bits(op == 0x625 ? 64 : 32, 8));
            if (op == 0x625) { var (bank, off) = Cbuf(); MovC(SC, bank, off); MovC(SX, bank, off + 4); cLo = SC; cHi = SX; }
            else
            {
                int c = Gpr(_i.Bits(64, 8));
                if (c != RZ && (c & 1) != 0) throw Fail("IMAD.WIDE odd addend pair");
                cLo = c; cHi = c == RZ ? RZ : c + 1;
            }
            int d = Gpr((uint)Dst);
            if (wide && (d == RZ || (d & 1) != 0)) throw Fail("IMAD.WIDE destination pair");
            ulong sign = signedMul ? (1UL << 40) | (1UL << 41) : 0;
            Emit(Unguarded(0x5c38000000000000UL | ((ulong)b << 20) | ((ulong)a << 8) | 247 | sign));                 // IMUL lo
            Emit(Unguarded(0x5c38000000000000UL | (1UL << 39) | ((ulong)b << 20) | ((ulong)a << 8) | 248 | sign));   // IMUL.HI
            Emit(Unguarded(0x5c10000000000000UL | (1UL << 47) | ((ulong)cLo << 20) | (247UL << 8) | 247));            // IADD.CC
            Emit(Unguarded(0x5c10000000000000UL | (1UL << 43) | ((ulong)cHi << 20) | (248UL << 8) | 248));            // IADD.X
            if (wide) { Mov(d, 247); Mov(d + 1, 248); }
            else Mov(d, 248);
        }

        // IABS Rd, b: max(b, -b), signed.
        void Iabs()
        {
            uint op = _i.Bits(0, 12);
            if ((_i.W2 & ~(op == 0xc13 ? 0x08000000u : 0u)) != 0) throw Fail("IABS mode");
            int b = Plain(1, SB);
            Iadd3(SX, RZ, b, RZ, false, negB: true);
            EmitG(0x5c20000000000000UL | ((ulong)SX << 20) | ((ulong)b << 8) | (uint)Gpr((uint)Dst) | (15UL << 39) | (1UL << 48));
        }

        // LOP3.LUT: table in bits 72-79, optional P = result != 0 (bits 81-83).
        void Lop3()
        {
            if (_i.Bit(80) || _i.Bits(87, 4) != 15) throw Fail("LOP3 predicate mode");
            int a = Plain(0, SA), b = Plain(1, SB), c = Gpr(_i.Bits(64, 8));
            uint p = _i.Bits(81, 3);
            EmitG(0x5be0000000000000UL | ((ulong)c << 39) | ((ulong)b << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst)
                  | ((ulong)_i.Bits(72, 8) << 28) | ((ulong)(p == 7 ? 0 : 3) << 36) | ((ulong)p << 48));
        }

        // IMNMX: Rd = p ? min : max (p in bits 87-90), bit 73 signed.
        void Imnmx()
        {
            int a = Plain(0, SA), b = Plain(1, SB);
            EmitG(0x5c20000000000000UL | ((ulong)b << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst)
                  | ((ulong)_i.Bits(87, 3) << 39) | ((ulong)_i.Bits(90, 1) << 42) | ((ulong)_i.Bits(73, 1) << 48));
        }

        // SHF Rd, low (A), shift (B), high (C): bits 73-74 type (0 S64, 1 U64, 2 S32, 3 U32), 75 .W (wrap), 76 right,
        // 80 .HI (the high word of the 64-bit result).
        void Shf()
        {
            if (_i.Bits(81, 10) != 0) throw Fail("SHF mode");
            uint type = _i.Bits(73, 2);
            bool wrap = _i.Bit(75), right = _i.Bit(76), hi = _i.Bit(80), wide = type <= 1, signedType = type == 0 || type == 2;
            int low = Plain(0, SA), s = Plain(1, SB), high = Plain(2, SC);
            int d = Gpr((uint)Dst);
            ulong maxShift = type == 0 ? 3UL : type == 1 ? 2UL : 0UL;
            if (!right && hi)          // high word of (high:low) << s
                EmitG(0x5bf8000000000000UL | ((ulong)high << 39) | ((ulong)s << 20) | ((ulong)low << 8) | (uint)d | (wrap ? 1UL << 50 : 0) | (maxShift << 37));
            else if (!right)           // low word: low << s
            {
                if (wrap && wide) throw Fail("SHF.L.W 64-bit low word");
                EmitG(0x5c48000000000000UL | ((ulong)s << 20) | ((ulong)low << 8) | (uint)d | (wrap ? 1UL << 39 : 0));
            }
            else if (!hi)              // low word of (high:low) >> s
                EmitG(0x5cf8000000000000UL | ((ulong)high << 39) | ((ulong)s << 20) | ((ulong)low << 8) | (uint)d | (wrap ? 1UL << 50 : 0) | (maxShift << 37));
            else                       // high word: high >> s
            {
                if (wrap && wide) throw Fail("SHF.R.W 64-bit high word");
                EmitG(0x5c28000000000000UL | ((ulong)s << 20) | ((ulong)high << 8) | (uint)d | (wrap ? 1UL << 39 : 0) | (signedType ? 1UL << 48 : 0));
            }
        }

        // LEA Rd, [Pu,] a, b, s: (a << s) + b, shift in bits 75-79, bit 63 negates b. LEA.HI (bit 80): b + high word of
        // (c:a) << s (bit 73 sign-extends a when c is RZ), .X (bit 74) adds the carry predicate of bits 87-90.
        void Lea()
        {
            uint op = _i.Bits(0, 12);
            int a = Plain(0, SA), b = Plain(1, SB), d = Gpr((uint)Dst);
            uint shift = _i.Bits(75, 5), carry = _i.Bits(81, 3);
            bool negB = op != 0x811 && _i.Bit(63);
            if (_i.Bit(72)) throw Fail("LEA negated a");
            if (!_i.Bit(80))
            {
                if (_i.Bit(74)) throw Fail("LEA.X without .HI");
                ulong iscadd(int dst, int bb) => 0x5c18000000000000UL | ((ulong)shift << 39) | ((ulong)bb << 20) | ((ulong)a << 8) | (uint)dst | (negB ? 1UL << 48 : 0);
                if (carry == 7) { EmitG(iscadd(d, b)); return; }
                if (negB) throw Fail("LEA carry out with negated b");
                // The sum goes to scratch unconditionally; only the destination and the carry are guarded. A guarded
                // copy of b made a constant-buffer base (c[0][0x110] in Ocarina's edge-AA tile list, prog 98) a phi
                // of it and the scratch register's old value, and GlobalToStorage could not find the buffer.
                Mov(SX, b, false);
                Emit(Unguarded(iscadd(SC, SX)));
                if (d != RZ) Mov(d, SC);
                IsetpLtU((int)carry, SC, SX);   // carry = sum < b
                return;
            }
            if (carry != 7) throw Fail("LEA.HI carry out");
            int high = Gpr(_i.Bits(64, 8));
            // With c = RZ the high word is a right shift of a by 32 - s (arithmetic when bit 73 sign-extends a);
            // otherwise SHF.L.HI of (c:a) by s.
            Mov32i(SX, high == RZ ? 32 - shift : shift);
            if (high == RZ)
                EmitG(0x5c28000000000000UL | ((ulong)SX << 20) | ((ulong)a << 8) | SC | (_i.Bit(73) ? 1UL << 48 : 0));
            else
                EmitG(0x5bf8000000000000UL | ((ulong)high << 39) | ((ulong)SX << 20) | ((ulong)a << 8) | SC);
            if (_i.Bit(74))
            {
                Mov32i(SX, 1);
                Sel(SX, SX, RZ, _i.Bits(87, 4), false);
                Iadd3(SC, SC, SX, RZ, true);
            }
            Iadd3(d, SC, b, RZ, true, negB);
        }

        // ---- attributes, special registers ------------------------------------------------------------------------
        // ALD / AST: address bits 40-49, components 74-75 (+1), patch 76, output 79; vertex register 32 (ALD) / 64 (AST).
        void AttributeAccess(bool store)
        {
            bool patch = _i.Bit(76), output = _i.Bit(79);
            uint address = _i.Bits(40, 10), components = _i.Bits(74, 2) + 1;
            if (_i.Bit(77) || patch || (output && store) || (address & 3) != 0 || address + components * 4 > 0x400 || _i.Bits(24, 8) != RZ)
                throw Fail("indexed/patch attribute access");
            // a[0x5c] is an Ampere per-primitive hint (shading rate); nothing reads it back.
            if (store && address == 0x5c && components == 1) return;
            int value = store ? Gpr(_i.Bits(32, 8)) : Gpr((uint)Dst);
            if (value == RZ)
            {
                if (!store) return;
                for (int k = 0; k < components; k++) Mov32i(TexOut + k, 0);
                value = TexOut;
            }
            int vertex = Gpr(store ? _i.Bits(64, 8) : _i.Bits(32, 8));
            EmitG((store ? 0xeff0000000000000UL : 0xefd8000000000000UL) | (uint)value | ((ulong)RZ << 8) | ((ulong)address << 20)
                  | (output ? 1UL << 32 : 0) | ((ulong)vertex << 39) | ((ulong)(components - 1) << 47));
        }

        // IPA: attribute (bits 64-71, in words), sample mode 76-77 (default/centroid), bit 78 flat. Maxwell IPA.PASS:
        // like Maxwell's, SM86 IPA of a perspective input returns the value times 1/w, and the NVN2 compiler multiplies
        // by w itself (IPA a[0x7c], MUFU.RCP, FMUL). Treating it as perspective-correct and adding IPA.MULTIPLY by w
        // scaled every texture coordinate by w a second time: Ocarina's floors and walls smeared along the view.
        void Ipa()
        {
            if (_i.Bits(32, 8) != RZ || _i.Bits(76, 2) > 1 || _i.Bit(79) || _i.Bits(81, 3) != 7) throw Fail("IPA mode");
            EmitG(0xe000000000000000UL | ((ulong)RZ << 8) | ((ulong)RZ << 20) | ((ulong)_i.Bits(64, 8) << 30)
                  | ((ulong)_i.Bits(76, 2) << 52) | ((ulong)_i.Bits(78, 1) << 55) | ((ulong)PT << 47) | (uint)Gpr((uint)Dst));
        }

        // CS2R Rd, SRZ: a zeroed register pair.
        void Cs2r()
        {
            if (_i.Bits(72, 8) != RZ || !_i.Bit(80)) throw Fail("CS2R special register");
            int d = Gpr((uint)Dst);
            if (d == RZ) return;
            Mov32i(d, 0, true);
            Mov32i(d + 1, 0, true);
        }

        // ISBEWR [Ra + imm], Rb: a stage buffer write of a task or mesh program (MeshInfo): Ra 24-31, Rb 32-39, the byte
        // offset 40-63, the region 75-78, bit 79 set (a write). Region 15 the vertex outputs, 0 the primitive indices
        // (bytes), 1 the primitive count, 7 the task payload. A shared memory store here.
        void Isbewr()
        {
            if (_mesh == null) throw Fail("ISBEWR outside a task or mesh program");
            uint region = _i.Bits(75, 4), offset = _i.Bits(40, 24);
            if (!_i.Bit(79) || (_i.W2 & ~0xf800u) != 0 || _i.Bits(64, 8) != 0 || offset >= 0x10000) throw Fail("ISBEWR mode");
            int a = Gpr(_i.Bits(24, 8)), data = Gpr(_i.Bits(32, 8));
            int at; uint width = 4;
            if (_mesh.Stage == 7 && region == 15) at = _mesh.Attr + (int)offset;
            else if (_mesh.Stage == 7 && region == 0) { at = _mesh.Map + (int)offset; width = 0; }
            else if (_mesh.Stage == 7 && region == 1)
            {
                if (a != RZ) throw Fail("ISBEWR primitive count at a register address");
                at = _mesh.Map;
            }
            else if (_mesh.Stage == 6 && region == 7) at = _mesh.Pay + (int)offset;
            else throw Fail($"ISBEWR region {region} in a stage {_mesh.Stage} program");
            EmitG(0xef58000000000000UL | (uint)data | ((ulong)a << 8) | ((ulong)(uint)at << 20) | ((ulong)width << 48));
        }

        // ISBERD Rd, [Ra + imm]: a stage buffer read (fields as ISBEWR's, bit 79 clear). Address 0 of region 15 (a mesh
        // program without a task stage, a task program) or of region 7 (a mesh program after a task stage) is the
        // workgroup's index; region 7 elsewhere is the task payload. With bit 79 set (the output side, as ISBEWR's) a
        // task program reads back the payload it writes (AglShader's task program stores a word and reloads it).
        void Isberd()
        {
            if (_mesh == null) throw Fail("ISBERD outside a task or mesh program");
            uint region = _i.Bits(75, 4), offset = _i.Bits(40, 24);
            bool output = _i.Bit(79);
            if ((_i.W2 & ~0xf800u) != 0 || (output && (region != 7 || _mesh.Stage != 6)) || _i.Bits(32, 8) != 0 || _i.Bits(64, 8) != 0
                || offset >= 0x10000)
                throw Fail("ISBERD mode");
            int a = Gpr(_i.Bits(24, 8)), d = Gpr((uint)Dst);
            if (d == RZ) return;
            if (output)
            {
                EmitG(0xef48000000000000UL | (uint)d | ((ulong)a << 8) | ((ulong)(uint)(_mesh.Pay + (int)offset) << 20) | (4UL << 48));
                return;
            }
            if ((region == 15 || region == 7) && a == RZ && offset == 0)
            {
                Put(0xf0c8000000000000UL | (0x25UL << 20) | (uint)d, true);   // S2R SR_CTAID.X
                return;
            }
            if (region != 7 || _mesh.Stage != 7 || !_mesh.HasTask) throw Fail($"ISBERD region {region} at a nonzero address");
            EmitG(0xef48000000000000UL | (uint)d | ((ulong)a << 8) | ((ulong)(uint)(_mesh.Pay + (int)offset) << 20) | (4UL << 48));
        }

        // S2R / S2UR: special register in bits 72-79, numbered as Maxwell's (lane id 0, thread and workgroup ids
        // 33-39, lane masks 56-60, ...). SR 132 (variable-rate shading: fragment width and height in bytes 1 and 2)
        // reads as 1x1.
        void S2r(bool uniform)
        {
            uint special = _i.Bits(72, 8);
            if (_i.W1 != 0 || _i.W2 != special << 8) throw Fail("special register mode");
            if (uniform) RequireUniformGuardTrue();
            int d = uniform ? Ur((uint)Dst) : Gpr((uint)Dst);
            if (d == RZ) return;
            if (special == 132) { Mov32i(d, 0x00010100, true); return; }
            Put(0xf0c8000000000000UL | ((ulong)special << 20) | (uint)d, true);
        }

        // R2UR: a uniform register takes the value of the first active lane. Uniform registers are per-thread here, so
        // this is a move (the compiler emits R2UR for values that are uniform, or loops until they are).
        void R2ur()
        {
            if (_i.W1 != 0 || _i.W2 != 0x000e0000) throw Fail("R2UR mode");
            int d = Ur((uint)Dst);
            if (d != RZ) Mov(d, Gpr(_i.Bits(24, 8)));
        }

        // ---- texture ---------------------------------------------------------------------------------------------
        // TEX (bound): Rd 16-23, Ra 24-31, Rb 32-39, descriptor word offset 40-53, its bank 54-58, dimension 61-63
        // (1D, 2D, 3D, CUBE, ARRAY_1D, ARRAY_2D, -, ARRAY_CUBE), second destination 64-71, mask 72-75, offsets 76,
        // depth compare 78, FP16 results 79, LOD mode 87-89 (numbered as Maxwell's), bit 90 no-dependency.
        // Operands: array index, coordinates, then LOD/bias, offsets, depth reference; up to four values are split as
        // halves over Ra and Rb (1+1, 2+1, 2+2), five or more as four in Ra and the rest in Rb. Maxwell TEX takes the
        // coordinates in Ra.. and the rest in Rb.., and writes the masked components to consecutive registers.
        static readonly int[] TexCoords = { 1, 2, 3, 3, 2, 3, 0, 4 };
        static readonly int[] TexMaxwellDim = { 0, 2, 4, 6, 1, 3, 0, 7 };
        // TEX. 0xb60 a bound descriptor; the bindless forms take the 32-bit handle (texture index in bits 0-19, sampler
        // index above) from a uniform register in bits 40-45 (0xf60) or, with bit 59 (.B) set, as one more operand
        // right after the coordinates (0x361; with bit 60 clear Ra holds all coordinates and Rb the handle and the
        // rest). Bindless becomes Maxwell TEX.B, the handle first among the meta registers (offsets bit 36, LOD mode 37):
        // the translator then indexes its texture-pool arrays with it (BindlessElimination GenerateBindlessAccess,
        // the handle being loaded from memory).
        void Tex(uint op)
        {
            bool bindless = op != 0xb60;
            uint dim = _i.Bits(61, 3), lod = _i.Bits(87, 3), mask = _i.Bits(72, 4);
            bool offsets = _i.Bit(76), dc = _i.Bit(78);
            if (dim == 6 || _i.Bit(77) || _i.Bits(81, 3) != 7 || _i.Bit(59) != (op == 0x361) || (lod > 3 && lod != 6 && lod != 7) || mask == 0)
                throw Fail("TEX mode");
            uint handle = 0;
            int handleReg = RZ;
            if (op == 0xb60)
            {
                handle = Handle(false);
                uint bank = _i.Bits(54, 5);
                if (handle > 0x1fff) throw Fail("TEX descriptor offset");
                if (!TexBank(bank)) throw Fail($"TEX descriptor bank {bank}");
            }
            else if (op == 0xf60)
            {
                if (_i.Bits(46, 13) != 0) throw Fail("bindless TEX mode");
                handleReg = Ur(_i.Bits(40, 6));
            }
            else if (_i.Bits(40, 19) != 0) throw Fail("TEX.B mode");
            bool hasLod = lod is 2 or 3 or 6 or 7;
            int coords = TexCoords[dim], handleSlot = op == 0x361 ? 1 : 0;
            int count = coords + handleSlot + (hasLod ? 1 : 0) + (offsets ? 1 : 0) + (dc ? 1 : 0);
            int inA = op == 0x361 && !_i.Bit(60) ? coords : count <= 4 ? (count + 1) / 2 : op == 0x361 ? coords : 4;
            int ra = (int)_i.Bits(24, 8), rb = (int)_i.Bits(32, 8);
            int Value(int k) => k < inA ? (ra == RZ ? RZ : Gpr((uint)(ra + k))) : (rb == RZ ? RZ : Gpr((uint)(rb + k - inA)));
            for (int k = 0; k < coords; k++) Mov(TexCoord + k, Value(k), false);
            int next = coords + handleSlot, meta = TexMeta;
            if (bindless) Mov(meta++, op == 0x361 ? Value(coords) : handleReg, false);
            if (hasLod) Mov(meta++, Value(next++), false);
            if (offsets) Mov(meta++, Value(next++), false);
            if (dc) Mov(meta++, Value(next++), false);
            if (bindless)
                EmitG(0xde80000000000000UL | TexOut | ((ulong)TexCoord << 8) | ((ulong)TexMeta << 20) | ((ulong)TexMaxwellDim[dim] << 28)
                      | ((ulong)mask << 31) | (offsets ? 1UL << 36 : 0) | ((ulong)lod << 37) | ((ulong)_i.Bits(90, 1) << 49)
                      | (dc ? 1UL << 50 : 0) | (7UL << 51));
            else
                EmitG(0xc000000000000000UL | TexOut | ((ulong)TexCoord << 8) | ((ulong)(meta == TexMeta ? RZ : TexMeta) << 20)
                      | ((ulong)TexMaxwellDim[dim] << 28) | ((ulong)mask << 31) | ((ulong)handle << 36) | ((ulong)_i.Bits(90, 1) << 49)
                      | (dc ? 1UL << 50 : 0) | (7UL << 51) | (offsets ? 1UL << 54 : 0) | ((ulong)lod << 55));
            TexResults(mask, _i.Bit(79));
        }

        // TMML (LOD query; results 8.8 fixed point as Maxwell's), 1D/2D/3D/CUBE. 0xb69 a bound descriptor; 0xf69 the
        // handle in a uniform register (bits 40-45, bit 91 set); 0x36a (TMML.B, bit 59) the handle in Rb and every
        // coordinate in Ra. Coordinates come from Ra, or from Ra and Rb as TEX's when Rb is a register. Maxwell TMML
        // (0xdf58, descriptor in 36-48) / TMML.B (0xdf60, handle in Rb), dimension in Maxwell's TexDim numbering.
        void Tmml(uint op)
        {
            bool bindless = op != 0xb69;
            uint dim = _i.Bits(61, 3), mask = _i.Bits(72, 4);
            int ra = (int)_i.Bits(24, 8), rb = (int)_i.Bits(32, 8);
            int coords = dim < 4 ? new[] { 1, 2, 3, 3 }[dim] : 0;
            if (coords == 0 || _i.Bits(76, 4) != 0 || _i.Bits(80, 10) != 0 || mask == 0 || _i.Bit(59) != (op == 0x36a))
                throw Fail("TMML mode");
            uint handle = 0;
            int handleReg = RZ;
            if (op == 0xb69)
            {
                handle = Handle(false);
                if (handle > 0x1fff || !TexBank(_i.Bits(54, 5))) throw Fail("TMML descriptor");
            }
            else if (op == 0xf69)
            {
                if (_i.Bits(46, 13) != 0 || !_i.Bit(91)) throw Fail("bindless TMML mode");
                handleReg = Ur(_i.Bits(40, 6));
            }
            else
            {
                if (_i.Bits(40, 19) != 0 || _i.Bit(91)) throw Fail("TMML.B mode");
                handleReg = Gpr((uint)rb);
            }
            int inA = rb == RZ || op == 0x36a ? coords : (coords + 1) / 2;
            for (int k = 0; k < coords; k++)
                Mov(TexCoord + k, k < inA ? (ra == RZ ? RZ : Gpr((uint)(ra + k))) : (rb == RZ ? RZ : Gpr((uint)(rb + k - inA))), false);
            if (bindless) Mov(TexMeta, handleReg, false);
            EmitG((bindless ? 0xdf60000000000000UL : 0xdf58000000000000UL) | TexOut | ((ulong)TexCoord << 8)
                  | ((ulong)(bindless ? TexMeta : RZ) << 20) | ((ulong)(dim * 2) << 28) | ((ulong)mask << 31)
                  | ((ulong)handle << 36) | ((ulong)_i.Bits(90, 1) << 49));
            TexResults(mask, false);
        }

        // TLD.LZ with the handle in a uniform register (0xf66: bits 40-45, bit 91 and bit 60 set): Maxwell TLD.B, the
        // coordinates x | y, layer, x | y or x, y | z over Ra and Rb (1D, 2D, ARRAY_2D, 3D) and the handle in Rb.
        void TldBindless()
        {
            uint dim = _i.Bits(61, 3), mask = _i.Bits(72, 4);
            bool array = dim == 5, volume = dim == 2;
            if (_i.Bits(46, 13) != 0 || !_i.Bit(91) || !_i.Bit(60) || _i.Bit(59) || _i.Bits(76, 5) != 0 || _i.Bits(81, 3) != 7
                || _i.Bits(87, 3) != 1 || (dim != 0 && dim != 1 && !array && !volume) || mask == 0)
                throw Fail("bindless TLD mode");
            int ra = (int)_i.Bits(24, 8), rb = (int)_i.Bits(32, 8);
            int handleReg = Ur(_i.Bits(40, 6));
            Mov(TexCoord, ra == RZ ? RZ : Gpr((uint)ra), false);
            if (array || volume) Mov(TexCoord + 1, ra == RZ ? RZ : Gpr((uint)(ra + 1)), false);
            if (dim != 0) Mov(array || volume ? TexCoord + 2 : TexCoord + 1, rb == RZ ? RZ : Gpr((uint)rb), false);
            Mov(TexMeta, handleReg, false);
            EmitG(0xdd00000000000000UL | TexOut | ((ulong)TexCoord << 8) | ((ulong)TexMeta << 20)
                  | ((ulong)(array ? 3u : volume ? 4u : dim == 0 ? 0u : 2u) << 28) | ((ulong)mask << 31)
                  | ((ulong)_i.Bits(90, 1) << 49) | (7UL << 51));
            TexResults(mask, false);
        }

        // TXQ.B (0x370): the handle in Ra and the level in Ra+1, as Maxwell TXQ.B (0xdf50) takes them; query 1
        // (dimensions), results as TXQ's.
        void TxqBindless()
        {
            uint mask = _i.Bits(72, 4);
            if (_i.Bits(59, 5) != 1 || mask == 0 || _i.Bits(76, 14) != 0 || _i.Bit(91) || _i.Bits(32, 27) != 0) throw Fail("TXQ.B mode");
            int ra = (int)_i.Bits(24, 8);
            Mov(TexCoord, ra == RZ ? RZ : Gpr((uint)ra), false);
            Mov(TexCoord + 1, ra == RZ ? RZ : Gpr((uint)(ra + 1)), false);
            EmitG(0xdf50000000000000UL | TexOut | ((ulong)TexCoord << 8) | (1UL << 22) | ((ulong)mask << 31) | ((ulong)_i.Bits(90, 1) << 49));
            TexResults(mask, false);
        }

        // F2I: destination signed (bit 72) and size (75-76: 8/16/32 bits), rounding 78-79 (nearest, floor, ceil,
        // trunc), FTZ 80, source size 84-85 (F16, F32). Maxwell F2I has the same rounding order.
        void F2i(uint op)
        {
            const uint known = 1u << 8 | 3u << 11 | 1u << 13 | 3u << 14 | 1u << 16 | 3u << 20;
            uint w2 = _i.W2 & ~(op == 0xd05 ? 0x08000000u : 0u);
            uint dstSize = _i.Bits(75, 2), srcSize = _i.Bits(84, 2);
            if ((w2 & ~known) != 0 || dstSize > 2 || srcSize is not 1 and not 2 || (srcSize == 1 && _i.Bits(60, 2) != 0))
                throw Fail("F2I format");
            int b; bool abs, neg;
            if (op == 0x905) { Mov32i(SB, _i.W1); (b, abs, neg) = (SB, false, false); }   // F2I of a 32-bit immediate
            else (b, abs, neg) = Raw(1, SB);
            uint fmt = dstSize | (_i.Bit(72) ? 4u : 0u);   // ISrcDstFmt: U8 0, U16 1, U32 2, +4 signed
            EmitG(0x5cb0000000000000UL | ((ulong)b << 20) | (uint)Gpr((uint)Dst) | ((ulong)(fmt & 3) << 8) | ((ulong)(fmt >> 2) << 12)
                  | ((ulong)srcSize << 10) | ((ulong)_i.Bits(78, 2) << 39) | ((ulong)_i.Bits(80, 1) << 44)
                  | (neg ? 1UL << 45 : 0) | (abs ? 1UL << 49 : 0));
        }

        // LDC Rd, c[bank][Ra + offset]: offset 38-53, bank 54-58, size 73-75 (as Maxwell's: U8 S8 U16 S16 32 64),
        // mode 78-79 (0, or 2 .IS: Ra addresses all banks).
        void Ldc()
        {
            uint width = _i.Bits(73, 3), mode = _i.Bits(78, 2);
            int bank = Bank(_i.Bits(54, 5));
            if (width > 5 || mode is not 0 and not 2 || (_i.W2 & ~0xce00u) != 0 || (_i.W1 & 0xf800003fu) != 0 || bank >= 18)
                throw Fail("LDC mode");
            int d = Gpr((uint)Dst);
            if (width == 5 && (d & 1) != 0) throw Fail("LDC.64 odd destination");
            EmitG(0xef90000000000000UL | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | ((ulong)_i.Bits(38, 16) << 20) | ((ulong)bank << 36)
                  | ((ulong)mode << 44) | ((ulong)width << 48) | (uint)d);
        }

        // Global memory address of LDG/STG: a 64-bit register pair (Ra, .E), or a uniform pair (bits 32-37 for loads,
        // 64-69 for stores) plus Ra (64-bit when bit 90 is set, else zero-extended), or with the "desc[UR][Ra.64]"
        // forms Ra alone (the uniform pair is a cache-policy descriptor). Sums go to R244:R245.
        int GlobalAddress(bool uniformForm, bool store)
        {
            int ra = (int)_i.Bits(24, 8);
            if (!uniformForm)
            {
                if (store ? (_i.W2 & ~0xe00u) != 0x0010a100 : (_i.W2 & ~0x2e00u) != 0x001e8100) throw Fail("global access mode");
                if (ra != RZ && ((ra & 1) != 0)) throw Fail("global address pair");
                return ra == RZ ? RZ : Gpr((uint)ra);
            }
            uint ur = store ? _i.Bits(64, 6) : _i.Bits(32, 6);
            bool descriptor = store ? (_i.W2 & ~(0x3fu | 0xe00u)) == 0x0c101100
                                    : (_i.W2 & ~(0x3fu | 0xe00u)) == 0x0c1e1100 || _i.W2 == 0x0c1ebd00;
            // Bit 78 is .STRONG.GPU instead of .STRONG.SM (two agl_common compute programs load their arguments and store
            // their results so): the translator's accesses have no scope, which gives the same values outside a race.
            uint plain = store ? 0x0810a100u : 0x081e8100u;
            if (!descriptor && (_i.W2 & ~(0x04000000u | 0x3fu | 0xe00u | 0x4000u | (store ? 0u : 0x2000u))) != plain) throw Fail("global access uniform mode");
            if (!store && _i.Bits(38, 2) != 0) throw Fail("global access uniform mode");
            if (descriptor)
            {
                if ((ra & 1) != 0 || ra == RZ) throw Fail("global address pair");
                return Gpr((uint)ra);
            }
            int u = Ur(ur);
            if (u != RZ && (ur & 1) != 0) throw Fail("global uniform address pair");
            if (ra == RZ) return u;
            bool wide = _i.Bit(90);
            if (wide && (ra & 1) != 0) throw Fail("global address pair");
            int r = Gpr((uint)ra);
            Emit(Unguarded(0x5c10000000000000UL | (1UL << 47) | ((ulong)r << 20) | ((ulong)u << 8) | 244));                          // IADD.CC
            Emit(Unguarded(0x5c10000000000000UL | (1UL << 43) | ((ulong)(wide ? r + 1 : RZ) << 20) | ((ulong)(u == RZ ? RZ : u + 1) << 8) | 245));   // IADD.X
            return 244;
        }

        // LDG Rd, [address + offset]: offset 40-63 (signed), size 73-75 (U8 S8 U16 S16 32 64 128, as Maxwell's).
        void Ldg(uint op)
        {
            uint size = _i.Bits(73, 3);
            int d = Gpr((uint)Dst);
            if (size > 6 || (size == 5 && (d & 1) != 0) || (size == 6 && (d & 3) != 0)) throw Fail("LDG size");
            if (op == 0x381 && _i.Bits(32, 8) != 0) throw Fail("LDG mode");
            int addr = GlobalAddress(op == 0x981, false);
            EmitG(0xeed0000000000000UL | (uint)d | ((ulong)addr << 8) | ((ulong)_i.Bits(40, 24) << 20) | (1UL << 45) | ((ulong)size << 48));
        }

        // STG [address + offset], Rdata: data 32-39, offset 40-63, size 73-75; .E (bit 72) required.
        void Stg(uint op)
        {
            uint size = _i.Bits(73, 3);
            int data = Gpr(_i.Bits(32, 8));
            if (size > 6 || !_i.Bit(72) || (data != RZ && ((size == 5 && (data & 1) != 0) || (size == 6 && (data & 3) != 0)))) throw Fail("STG size");
            if (data == RZ && size > 4)
            {
                for (int k = 0; k < (1 << (int)(size - 4)); k++) Mov32i(TexOut + k, 0);
                data = TexOut;
            }
            int addr = GlobalAddress(op == 0x986, true);
            EmitG(0xeed8000000000000UL | (uint)data | ((ulong)addr << 8) | ((ulong)_i.Bits(40, 24) << 20) | (1UL << 45) | ((ulong)size << 48));
        }

        // TLD (texel fetch, integer coordinates): fields as TEX; LOD mode 87-89 is 1 (.LZ) or 3 (.LL, the level after
        // the coordinates), offsets bit 76. Maxwell TLD takes its operands as Maxwell TEX does. Bit 78 (.MS) reads one
        // sample of a multisampled 2D texture, its index in the operand after the coordinates (agl_common's 2x resolve:
        // TLD.LZ.MS with samples 0 and 1, averaged); it is lowered for .LZ without offsets only, Maxwell's MS bit 50.
        void Tld()
        {
            uint dim = _i.Bits(61, 3), lod = _i.Bits(87, 3), mask = _i.Bits(72, 4);
            bool offsets = _i.Bit(76), ms = _i.Bit(78);
            if (dim == 6 || _i.Bit(77) || _i.Bit(79) || _i.Bits(81, 3) != 7 || _i.Bit(59) || lod is not 1 and not 3 || mask == 0
                || (ms && (dim != 1 || lod != 1 || offsets)))
                throw Fail("TLD mode");
            uint handle = Handle(false), bank = _i.Bits(54, 5);
            if (handle > 0x1fff) throw Fail("TLD descriptor offset");
            if (!TexBank(bank)) throw Fail($"TLD descriptor bank {bank}");
            int coords = TexCoords[dim];
            int count = coords + (lod == 3 ? 1 : 0) + (offsets ? 1 : 0) + (ms ? 1 : 0);
            int inA = count <= 4 ? (count + 1) / 2 : 4;
            int ra = (int)_i.Bits(24, 8), rb = (int)_i.Bits(32, 8);
            int Value(int k) => k < inA ? (ra == RZ ? RZ : Gpr((uint)(ra + k))) : (rb == RZ ? RZ : Gpr((uint)(rb + k - inA)));
            for (int k = 0; k < coords; k++) Mov(TexCoord + k, Value(k), false);
            int next = coords, meta = TexMeta;
            if (lod == 3) Mov(meta++, Value(next++), false);
            if (offsets) Mov(meta++, Value(next++), false);
            if (ms) Mov(meta++, Value(next++), false);
            EmitG(0xdc00000000000000UL | TexOut | ((ulong)TexCoord << 8) | ((ulong)(meta == TexMeta ? RZ : TexMeta) << 20)
                  | ((ulong)TexMaxwellDim[dim] << 28) | ((ulong)mask << 31) | (offsets ? 1UL << 35 : 0) | ((ulong)handle << 36)
                  | ((ulong)_i.Bits(90, 1) << 49) | (ms ? 1UL << 50 : 0) | (7UL << 51) | (lod == 3 ? 1UL << 55 : 0));
            TexResults(mask, false);
        }

        // TXQ (dimensions): level in Ra, descriptor offset 40-53 in bank 54-58 (2), mask 72-75, second destination
        // 64-71 for the third and fourth values. Maxwell TXQ with query 1 (dimension) writes them to consecutive
        // registers.
        void Txq()
        {
            uint mask = _i.Bits(72, 4), handle = Handle(false), bank = _i.Bits(54, 5);
            if (_i.Bits(59, 5) != 0 || mask == 0 || _i.Bits(76, 14) != 0 || _i.Bit(91) || handle > 0x1fff) throw Fail("TXQ mode");
            if (!TexBank(bank)) throw Fail($"TXQ descriptor bank {bank}");
            int ra = Gpr(_i.Bits(24, 8));
            EmitG(0xdf48000000000000UL | TexOut | ((ulong)ra << 8) | (1UL << 22) | ((ulong)mask << 31) | ((ulong)handle << 36) | ((ulong)_i.Bits(90, 1) << 49));
            TexResults(mask, false);
        }

        // Texture results: the components of the mask, from TexOut on, to Rd, Rd+1 and then the second destination
        // (bits 64-71) when there is one. FP16 results pack (c0, c1) into the first register and (c2, c3) into the
        // second, the first of each pair in the low half (F2F.F16 writes its half there; ISCADD merges).
        void TexResults(uint mask, bool f16)
        {
            int d = Dst, d2 = (int)_i.Bits(64, 8);
            int n = System.Numerics.BitOperations.PopCount(mask);
            if (!f16)
            {
                for (int k = 0; k < n; k++)
                {
                    int target = d2 == RZ || k < 2 ? (d == RZ ? RZ : d + k) : d2 + (k - 2);
                    if (target != RZ) Mov(Gpr((uint)target), TexOut + k);
                }
                return;
            }
            void pack(int target, int lo, int hi)
            {
                if (target == RZ) return;
                Emit(Unguarded(0x5ca8000000000000UL | (1UL << 8) | (2UL << 10) | ((ulong)lo << 20) | 246));
                if (hi == RZ) Mov32i(247, 0);
                else Emit(Unguarded(0x5ca8000000000000UL | (1UL << 8) | (2UL << 10) | ((ulong)hi << 20) | 247));
                EmitG(0x5c18000000000000UL | (16UL << 39) | (246UL << 20) | (247UL << 8) | (uint)Gpr((uint)target));
            }
            pack(d, TexOut, n > 1 ? TexOut + 1 : RZ);
            if (n > 2) pack(d2 != RZ ? d2 : d == RZ ? RZ : d + 1, TexOut + 2, n > 3 ? TexOut + 3 : RZ);
        }

        // PRMT Rd, a, selector (B slot), c: byte i of the result is byte (s & 7) of c:a, its sign repeated when s & 8;
        // the mode (bits 72-75) other than the default index mode is not lowered. Maxwell PRMT has the same meaning.
        void Prmt()
        {
            if (_i.Bits(72, 4) != 0 || _i.Bits(76, 15) != 0) throw Fail("PRMT mode");
            int a = Plain(0, SA), sel = Plain(1, SB), c = Plain(2, SC);
            EmitG(0x5bc0000000000000UL | ((ulong)c << 39) | ((ulong)sel << 20) | ((ulong)a << 8) | (uint)Gpr((uint)Dst));
        }

        // FLO Rd, b: the index of the highest set bit (bit 73 signed, bit 74 .SH: the shift amount, bit 63 of ~b).
        void Flo()
        {
            if ((_i.W2 & ~(0x600u | 0x08000000u)) != 0x000e0000) throw Fail("FLO mode");
            var (b, _, inv) = Raw(1, SB);
            EmitG(0x5c30000000000000UL | ((ulong)b << 20) | (inv ? 1UL << 40 : 0) | ((ulong)_i.Bits(74, 1) << 41)
                  | ((ulong)_i.Bits(73, 1) << 48) | (uint)Gpr((uint)Dst));
        }

        // HSETP2 P, Q, a, b: compare 76-79, FTZ 80, P/Q 81-86, combined with Pc (87-90) by bits 69-70, H_AND 71;
        // A: reg 24, neg 72, abs 73, swizzle 74-75; b: a register (swizzle 60-61, abs 62, neg 63) or an immediate half
        // pair. Maxwell HSETP2 has the same fields at its own positions.
        void Hsetp2()
        {
            uint op = _i.Bits(0, 12);
            ulong source;
            if (op == 0x234)
            {
                if ((_i.W1 & 0x0fffff00u) != 0 || _i.Bits(64, 5) != 0) throw Fail("HSETP2 mode");
                source = ((ulong)Gpr(_i.Bits(32, 8)) << 20) | ((ulong)_i.Bits(60, 2) << 28) | ((ulong)_i.Bits(62, 1) << 30) | ((ulong)_i.Bits(63, 1) << 31);
            }
            else { Mov32i(SB, _i.W1); source = (ulong)SB << 20; }
            EmitG(0x5d20000000000000UL | _i.Bits(84, 3) | ((ulong)_i.Bits(81, 3) << 3) | ((ulong)_i.Bits(80, 1) << 6)
                  | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | source | ((ulong)_i.Bits(76, 4) << 35) | ((ulong)_i.Bits(87, 3) << 39)
                  | ((ulong)_i.Bits(90, 1) << 42) | ((ulong)_i.Bits(72, 1) << 43) | ((ulong)_i.Bits(73, 1) << 44)
                  | ((ulong)_i.Bits(69, 2) << 45) | ((ulong)_i.Bits(74, 2) << 47) | ((ulong)_i.Bits(71, 1) << 49));
        }

        // P2R Rd, PR, Ra, mask / R2P PR, Ra, mask: immediate mask (bits 32-38), byte select 76-77, Ra the merge source
        // (P2R) or the value (R2P), as Maxwell's P2R/R2P with an immediate.
        void PredicateRegister(bool toRegister)
        {
            if (_i.W1 > 0x7f || (_i.W2 & ~0x3000u) != 0 || (!toRegister && _i.Bits(16, 8) != 0)) throw Fail("P2R/R2P mode");
            ulong w = (toRegister ? 0x38e8000000000000UL | (uint)Gpr((uint)Dst) : 0x38f0000000000000UL)
                      | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | ((ulong)_i.W1 << 20) | ((ulong)_i.Bits(76, 2) << 41);
            EmitG(w);
        }

        // SURED.D.op U32 (bound descriptor): data 32-39, coordinates 24-31, operation 87-89 (ADD MIN MAX INC DEC AND OR
        // XOR, as Maxwell's), dimension 61-63 (1D buffer or 2D), descriptor offset 40-53 in bank 54-58. Maxwell's SURED
        // is 1110101101011 in the top bits (0xeb58, InstTable Sured); 0xeb48 matched no instruction, so the
        // translator skipped every SURED: Ocarina's edge AA (the blend-weight pass ORs its weights into an image) never
        // wrote anything.
        void Sured()
        {
            uint dim = _i.Bits(61, 3), handle = Handle(true), bank = _i.Bits(54, 5);
            if ((_i.W2 & ~(7u << 23)) != 0x0010e000 || dim is not 1 and not 3 || _i.Bits(59, 2) != 0 || handle > 0x1fff || !TexBank(bank))
                throw Fail("SURED mode");
            EmitG(0xeb58000000000000UL | (uint)Gpr(_i.Bits(32, 8)) | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | ((ulong)_i.Bits(87, 3) << 24)
                  | ((ulong)dim << 33) | ((ulong)handle << 36));
        }

        // SUATOM.D.op U32 (bound descriptor): Rd = the image word's old value, fields as SURED's plus the destination and
        // the result predicate 81-83 (PT only). agl_technique_pfx's append counters add to word 0 of a 1D image this
        // way. Maxwell's SUATOM.D is 11101010 in the top bits with bit 54 clear (InstTable Suatom): operation 29-32,
        // dimension 33-35, descriptor 36-48, size 51-53 (U32 0).
        void Suatom()
        {
            uint dim = _i.Bits(61, 3), handle = Handle(true), bank = _i.Bits(54, 5);
            if ((_i.W2 & ~(0xfu << 23)) != 0x001ee000 || _i.Bits(87, 4) > 8 || dim > 5 || _i.Bits(59, 2) != 0 || handle > 0x1fff
                || !TexBank(bank))
                throw Fail("SUATOM mode");
            EmitG(0xea00000000000000UL | (uint)Gpr((uint)Dst) | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | ((ulong)Gpr(_i.Bits(32, 8)) << 20)
                  | ((ulong)_i.Bits(87, 4) << 29) | ((ulong)dim << 33) | ((ulong)handle << 36));
        }

        // ATOMG.E.CAS U32: Rd = [Ra.64 + offset 40-63], replaced by Rc (64-71) where it equals Rb (32-39). The foliage
        // compute programs claim hash slots with it. Maxwell ATOM.E.CAS takes the compare value in Rb and the new value
        // in Rb + 1 (xlate/patches/0037-global-compare-and-swap.patch, which also makes the translator emit it), so both
        // go to SB and SB + 1 first. Only a zero offset is lowered.
        void AtomgCas()
        {
            if ((_i.W2 & ~0xffu) != 0x001ee100 || _i.Bits(40, 24) != 0 || (_i.Bits(24, 8) & 1) != 0 || _i.Bits(24, 8) == RZ)
                throw Fail("ATOMG.CAS mode");
            Mov(SB, Gpr(_i.Bits(32, 8)), false);
            Mov(SB + 1, Gpr(_i.Bits(64, 8)), false);
            EmitG(0xeef0000000000000UL | (uint)Gpr((uint)Dst) | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | ((ulong)SB << 20) | (1UL << 48));
        }

        // REDUX URd, Ra: Ra reduced over the warp's active lanes into a uniform register. Mode (bits 78-80): SUM 3
        // (modulo 2^32), MIN 4, MAX 5 (unsigned, signed with bit 73), OR 1, XOR 2. Lowered as VOTE.ANY (the active-lane
        // mask) and one SHFL.IDX per lane; an inactive lane contributes the operation's identity.
        void Redux()
        {
            uint mode = (_i.W2 & ~0x200u) >> 14;
            bool signedOrder = _i.Bit(73);
            if (_i.W1 != 0 || (_i.W2 & ~0x200u & ~(7u << 14)) != 0 || mode is not (1 or 2 or 3 or 4 or 5)
                || (signedOrder && mode is not (4 or 5)) || _i.Bits(12, 4) != 7)
                throw Fail("REDUX mode");
            int t = Ur((uint)Dst), src = Gpr(_i.Bits(24, 8));
            if (t == RZ) return;
            const int ballot = 240, lanev = 241, keep = 242;
            bool min = mode == 4;
            Emit(Unguarded(0x50d8000000000000UL | (1UL << 48) | (7UL << 45) | ((ulong)PT << 39) | ballot));   // VOTE.ANY ballot
            Mov32i(t, min ? 0xffffffffu : 0u);
            ulong lop3(int d, int a, int b, uint lut) => Unguarded(0x5be0000000000000UL | ((ulong)RZ << 39) | ((ulong)b << 20) | ((ulong)a << 8) | (uint)d | ((ulong)lut << 28) | (7UL << 48));
            for (uint lane = 0; lane < 32; lane++)
            {
                Emit(Unguarded(0xef10000000000000UL | (7UL << 48) | (31UL << 34) | (1UL << 29) | (1UL << 28) | ((ulong)lane << 20) | ((ulong)src << 8) | lanev));
                if (signedOrder) Lop32i(lanev, lanev, 0x80000000, 2);   // signed order as unsigned
                Emit(Unguarded(0x3828000000000000UL | ((ulong)lane << 20) | ((ulong)ballot << 8) | keep));   // SHR imm
                Lop32i(keep, keep, 1, 0);
                Iadd3(keep, RZ, keep, RZ, false, negB: true);                                                   // 0 or ~0
                Emit(lop3(lanev, lanev, keep, min ? 0xF3u : 0xC0u));                                           // x | ~m, x & m
                switch (mode)
                {
                    case 4: Emit(Unguarded(0x5c20000000000000UL | ((ulong)lanev << 20) | ((ulong)t << 8) | (uint)t | ((ulong)PT << 39))); break;          // min
                    case 5: Emit(Unguarded(0x5c20000000000000UL | ((ulong)lanev << 20) | ((ulong)t << 8) | (uint)t | (15UL << 39))); break;               // max
                    case 1: Emit(lop3(t, t, lanev, 0xFC)); break;
                    case 2: Emit(lop3(t, t, lanev, 0x3C)); break;
                    default: Iadd3(t, t, lanev, RZ, false); break;
                }
            }
            if (signedOrder) Lop32i(t, t, 0x80000000, 2);
        }

        // MATCH.ALL Pd, Rd, Ra (op 0x3a1 with no mode bits; Ocarina's NVN2 compute programs test subgroupAllEqual with it
        // before an aggregated shared atomic: @!Pd per-lane ATOMS, else REDUX and one): Pd = every active lane holds Ra's
        // value, Rd = the active-lane mask then, else 0. Lowered as VOTE.ANY (the active lanes), FLO (the highest of
        // them), SHFL.IDX (that lane's value), ISETP.EQ into Pd and VOTE.ALL over Pd. Only the unguarded form with a
        // predicate destination is taken. Bit 73 is .U64: Ra:Ra+1 is compared, the high word ANDed into Pd.
        void Match()
        {
            uint pd = _i.Bits(81, 3);
            bool wide = _i.Bit(73);
            if (_i.W1 != 0 || (_i.W2 & ~(7u << 17 | 1u << 9)) != 0 || _i.Bits(12, 4) != 7 || pd == PT)
                throw Fail("MATCH mode");
            int d = Gpr((uint)Dst), src = Gpr(_i.Bits(24, 8));
            if (wide && (src == RZ || (src & 1) != 0)) throw Fail("MATCH.U64 register pair");
            const int ballot = 240, lead = 241, value = 242, valueHi = 243;
            Emit(Unguarded(0x50d8000000000000UL | (1UL << 48) | (7UL << 45) | ((ulong)PT << 39) | ballot));        // VOTE.ANY
            Emit(Unguarded(0x5c30000000000000UL | ((ulong)ballot << 20) | lead));                                    // FLO.U32
            Emit(Unguarded(0xef10000000000000UL | (7UL << 48) | (31UL << 34) | (1UL << 29) | ((ulong)lead << 20)
                           | ((ulong)src << 8) | value));                                                            // SHFL.IDX
            Emit(Unguarded(0x5b60000000000000UL | (2UL << 49) | ((ulong)PT << 39) | ((ulong)value << 20)
                           | ((ulong)src << 8) | ((ulong)pd << 3) | PT));                                            // ISETP.EQ.U32
            if (wide)
            {
                Emit(Unguarded(0xef10000000000000UL | (7UL << 48) | (31UL << 34) | (1UL << 29) | ((ulong)lead << 20)
                               | ((ulong)(src + 1) << 8) | valueHi));                                                // SHFL.IDX
                Emit(Unguarded(0x5b60000000000000UL | (2UL << 49) | ((ulong)pd << 39) | ((ulong)valueHi << 20)
                               | ((ulong)(src + 1) << 8) | ((ulong)pd << 3) | PT));                                  // ISETP.EQ.U32.AND
            }
            Emit(Unguarded(0x50d8000000000000UL | ((ulong)pd << 45) | ((ulong)pd << 39) | RZ));                     // VOTE.ALL
            if (d != RZ) Sel(d, ballot, RZ, pd, false);
        }

        // ATOMS.POPC.INC RZ, [Ra + URb + offset] (op 0xf8c, mode word 0x0d000000; the increment of Ocarina's aggregated
        // shared counters after MATCH and REDUX): memory goes up by the number of active lanes. With no result register
        // that is one ATOMS.ADD of 1 per active lane, which is what this emits; a form that wants the per-lane result (its
        // rank among the active lanes) is not taken. Bits 78-79 scale Ra as LDS / STS's stride does (SharedAccess): the
        // luminance histograms of agl_common use 3, three counters in each 16-byte bin at offsets 0, 4 and 8.
        void AtomsPopcInc()
        {
            uint offset = _i.Bits(40, 24), ur = _i.Bits(64, 6), stride = _i.Bits(78, 2);
            if ((_i.W2 & ~(0x3fu | 3u << 14)) != 0x0d000000u || _i.Bits(32, 8) != 0 || Dst != RZ || (offset & 3) != 0 || offset >= (1u << 22))
                throw Fail("ATOMS.POPC.INC mode");
            int addr = Gpr(_i.Bits(24, 8));
            if (stride != 0 && addr != RZ)
            {
                Emit(Unguarded(0x3848000000000000UL | ((ulong)(stride + 1) << 20) | ((ulong)addr << 8) | SA));   // SHL imm
                addr = SA;
            }
            if (ur != 0x3f)
            {
                Iadd3(SA, addr, Ur(ur), RZ, false);
                addr = SA;
            }
            Mov32i(SB, 1);
            EmitG(0xec00000000000000UL | RZ | ((ulong)addr << 8) | ((ulong)SB << 20) | ((ulong)(offset >> 2) << 30));   // ATOMS.ADD
        }

        // VOTE Rd, Pu, Pin: ballot into Rd (URd for VOTEU), mode 72-73 (ALL, ANY, EQ) over the vector predicate of bits
        // 87-90, result predicate 81-83 (a uniform predicate for VOTEU). Maxwell VOTE has the same fields. VOTEU's
        // output goes through Pk (k its UP index) with the guest's Pk saved around it; no uniform predicate is read.
        void Vote(bool uniform)
        {
            if (_i.W1 != 0 || _i.Bits(72, 2) > 2 || (_i.W2 & ~0x078e0300u) != 0 || (uniform && (_i.Bits(24, 8) != 0 || _i.Bits(12, 4) != 7)))
                throw Fail("VOTE mode");
            uint pout = _i.Bits(81, 3);
            int d = uniform ? Ur((uint)Dst) : Gpr((uint)Dst);
            ulong vote = 0x50d8000000000000UL | (uint)d | ((ulong)_i.Bits(87, 4) << 39) | ((ulong)pout << 45) | ((ulong)_i.Bits(72, 2) << 48);
            if (!uniform || pout == 7) { EmitG(vote); return; }
            uint m = 1u << (int)pout;
            Emit(Unguarded(0x38e8000000000000UL | ((ulong)m << 20) | ((ulong)RZ << 8) | USAVE));
            EmitG(vote);
            Emit(Unguarded(0x38e8000000000000UL | ((ulong)m << 20) | ((ulong)RUP << 8) | RUP));
            Emit(Unguarded(0x38f0000000000000UL | ((ulong)m << 20) | ((ulong)USAVE << 8)));
        }

        // ATOMG.E U32: Rd = atomic op (87-90, numbered as Maxwell ATOM's: ADD MIN MAX INC DEC AND OR XOR EXCH) of
        // [address + offset 40-63] with Rb (32-39); the address a register pair (24-31) or a uniform pair (64-69).
        void Atomg(bool uniformAddress)
        {
            uint operation = _i.Bits(87, 4);
            int offset = (int)(_i.Bits(40, 24) << 8) >> 8;
            uint w2 = _i.W2 & ~(0xfu << 23);
            int addr;
            if (uniformAddress)
            {
                if (_i.Bits(24, 8) != RZ || (w2 & ~0x3fu) != 0x081ee140 || (_i.Bits(64, 6) & 1) != 0) throw Fail("ATOMG mode");
                addr = Ur(_i.Bits(64, 6));
            }
            else
            {
                if (w2 != 0x001ee100 || (_i.Bits(24, 8) & 1) != 0) throw Fail("ATOMG mode");
                addr = Gpr(_i.Bits(24, 8));
            }
            if (operation > 8 || offset < -0x80000 || offset > 0x7ffff) throw Fail("ATOMG operation/offset");
            EmitG(0xed00000000000000UL | (uint)Gpr((uint)Dst) | ((ulong)addr << 8) | ((ulong)Gpr(_i.Bits(32, 8)) << 20)
                  | ((ulong)((uint)offset & 0xfffff) << 28) | (1UL << 48) | ((ulong)operation << 52));
        }

        // RED.E (global reduction, no result): op 87-89 (ADD MIN MAX INC DEC AND OR XOR), type 73-75 (U32, S32, F32 add),
        // data 32-39, address a register pair (24-31) or, with Ra = RZ, a uniform pair (64-69), offset 40-63.
        void Red()
        {
            uint type = _i.Bits(73, 3), operation = _i.Bits(87, 3);
            bool ur = _i.Bits(24, 8) == RZ;
            uint variable = 0x03800e00u | (ur ? 0x3fu : 0u), mode = ur ? 0x0c10e100u : 0x0010e100u;
            int offset = (int)(_i.Bits(40, 24) << 8) >> 8;
            if ((_i.W2 & ~variable) != mode || type is not (0 or 1 or 3) || (type == 3 && operation != 0)) throw Fail("RED mode");
            uint a = ur ? _i.Bits(64, 6) : _i.Bits(24, 8);
            if ((a & 1) != 0 || offset < -0x80000 || offset > 0x7ffff) throw Fail("RED address");
            int addr = ur ? Ur(a) : Gpr(a);
            EmitG(0xebf8000000000000UL | (uint)Gpr(_i.Bits(32, 8)) | ((ulong)addr << 8) | ((ulong)type << 20) | ((ulong)operation << 23)
                  | ((ulong)((uint)offset & 0xfffff) << 28) | (1UL << 48));
        }

        // SHFL Rd, [Pout,] a, lane, mask: lane an immediate (53-57; 0x989, 0xf89) or a register (32-39), the
        // clamp/segment mask an immediate (40-52; 0x589, 0xf89) or a register (64-71), mode 58-59 (IDX UP DOWN BFLY),
        // lane-valid predicate 81-83. Maxwell SHFL has the same choices.
        void Shfl(uint op)
        {
            bool laneImm = op is 0x989 or 0xf89, maskImm = op is 0x589 or 0xf89;
            if ((_i.W2 & ~(0x000e0000u | (maskImm ? 0u : 0xffu))) != 0 || _i.Bits(60, 4) != 0 || (laneImm && _i.Bits(32, 8) != 0) || (!laneImm && _i.Bits(53, 5) != 0))
                throw Fail("SHFL mode");
            ulong lane = laneImm ? ((ulong)_i.Bits(53, 5) << 20) | (1UL << 28) : (ulong)Gpr(_i.Bits(32, 8)) << 20;
            ulong mask = maskImm ? ((ulong)_i.Bits(40, 13) << 34) | (1UL << 29) : (ulong)Gpr(_i.Bits(64, 8)) << 39;
            EmitG(0xef10000000000000UL | (uint)Gpr((uint)Dst) | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | lane | mask
                  | ((ulong)_i.Bits(58, 2) << 30) | ((ulong)_i.Bits(81, 3) << 48));
        }

        // FSWZADD Rd, a, b (64-71), per-lane operation codes (bits 32-39): 0 a+b, 1 a-b, 2 b-a, 3 b. Maxwell numbers 1
        // and 2 the other way round.
        void Fswzadd()
        {
            uint swz = _i.W1;
            if (swz > 0xff || _i.W2 > 255) throw Fail("FSWZADD mode");
            uint maxwell = ((swz & 0x55) << 1) | ((swz & 0xaa) >> 1);
            EmitG(0x50f8000000000000UL | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | ((ulong)Gpr(_i.Bits(64, 8)) << 20) | ((ulong)maxwell << 28) | (uint)Gpr((uint)Dst));
        }

        // ATOMS (shared) U32/S32 (bit 73): Rd = op (87-90, as Maxwell ATOMS's) of [Ra + offset 40-63] with Rb.
        void Atoms()
        {
            uint offset = _i.Bits(40, 24), operation = _i.Bits(87, 4);
            if ((_i.W2 & ~0x07800200u) != 0 || operation > 8 || (offset & 3) != 0) throw Fail("ATOMS mode");
            EmitG(0xec00000000000000UL | (uint)Gpr((uint)Dst) | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | ((ulong)Gpr(_i.Bits(32, 8)) << 20)
                  | ((ulong)_i.Bits(73, 1) << 28) | ((ulong)(offset >> 2) << 30) | ((ulong)operation << 52));
        }

        // SGXT Rd, a, n (immediate): the low n bits of a, sign-extended with bit 73 (else zero-extended): Maxwell BFE
        // from bit 0, n bits.
        void Sgxt()
        {
            if ((_i.W2 & ~0x200u) != 0 || _i.W1 > 32) throw Fail("SGXT mode");
            EmitG(0x3800000000000000UL | ((ulong)(_i.W1 << 8) << 20) | ((ulong)_i.Bits(73, 1) << 48) | ((ulong)Gpr(_i.Bits(24, 8)) << 8) | (uint)Gpr((uint)Dst));
        }

        // BREV Rd, b (register): Maxwell SHR.BREV by 0.
        void Brev()
        {
            if (_i.W2 != 0 || _i.W1 > 255) throw Fail("BREV mode");
            EmitG(0x3828000000000000UL | ((ulong)Gpr(_i.Bits(32, 8)) << 8) | (1UL << 39) | (1UL << 40) | (uint)Gpr((uint)Dst));
        }

        // HMNMX2: per half lane, both operands widened to F32 (F2F, exact), FMNMX with the predicate of bits 87-90
        // (true: min), narrowed back and packed (lane 1 in the high half).
        void Hmnmx2()
        {
            uint op = _i.Bits(0, 12);
            if (_i.Bits(76, 11) != 0 || _i.Bit(91)) throw Fail("HMNMX2 mode");
            var a = HalfSrc(0, SA);
            if (a.swz == 1) throw Fail("HMNMX2 F32 operand");
            (int reg, uint swz, bool abs, bool neg) b = op == 0x840 ? (SB, 0u, false, false) : HalfSrc(1, SB);
            if (op == 0x840) Mov32i(SB, _i.W1);
            if (b.swz == 1) throw Fail("HMNMX2 F32 operand");
            void widen(int target, (int reg, uint swz, bool abs, bool neg) h, int lane)
            {
                bool high = h.swz == 3 || (h.swz == 0 && lane == 1);
                Emit(Unguarded(0x5ca8000000000000UL | (2UL << 8) | (1UL << 10) | ((ulong)h.reg << 20) | (high ? 1UL << 41 : 0)
                               | (h.neg ? 1UL << 45 : 0) | (h.abs ? 1UL << 49 : 0) | (uint)target));
            }
            for (int lane = 0; lane < 2; lane++)
            {
                int t = lane == 0 ? 246 : 247;
                widen(244, a, lane);
                widen(245, b, lane);
                Emit(Unguarded(0x5c60000000000000UL | (245UL << 20) | (244UL << 8) | (uint)t | ((ulong)_i.Bits(87, 3) << 39) | ((ulong)_i.Bits(90, 1) << 42)));
                Emit(Unguarded(0x5ca8000000000000UL | (1UL << 8) | (2UL << 10) | ((ulong)t << 20) | (uint)t));
            }
            EmitG(0x5c18000000000000UL | (16UL << 39) | (246UL << 20) | (247UL << 8) | (uint)Gpr((uint)Dst));
        }

        // A packed-half operand: register, swizzle (0 H1_H0, 1 F32, 2 H0_H0, 3 H1_H1, as Maxwell's), abs, neg.
        // A: reg 24, swizzle 74-75, abs 73, neg 72. B in bits 32-39 (swizzle 60-61, abs 62, neg 63) or, for forms 2/3/7,
        // 64-71 (swizzle 81-82, abs 83, neg 84). C in 64-71 (81-82, 83, 84). Immediates are a half pair as stored;
        // constants take swizzle/abs/neg from 60-63.
        (int reg, uint swz, bool abs, bool neg) HalfSrc(int which, int scratch)
        {
            if (which == 0) return (Gpr(_i.Bits(24, 8)), _i.Bits(74, 2), _i.Bit(73), _i.Bit(72));
            uint form = Form;
            bool wide = which == 1 ? form is 4 or 5 : form is 2 or 3;
            if (wide)
            {
                if (form is 4 or 2) { Mov32i(scratch, _i.W1); return (scratch, 0, false, false); }
                var (bank, off) = Cbuf(); MovC(scratch, bank, off);
                return (scratch, _i.Bits(60, 2), _i.Bit(62), _i.Bit(63));
            }
            if (form is 6 or 7) throw Fail("half operand in a uniform register");
            bool upper = which == 2 || form is 2 or 3;
            return upper ? (Gpr(_i.Bits(64, 8)), _i.Bits(81, 2), _i.Bit(83), _i.Bit(84))
                         : (Gpr(_i.Bits(32, 8)), _i.Bits(60, 2), _i.Bit(62), _i.Bit(63));
        }

        // HADD2 / HMUL2 / HFMA2 with packed-half results: saturate 77, FTZ 80, bit 76 the FMZ precision of HMUL2/HFMA2.
        // HFMA2.RELU (79) clamps each lane at zero where the predicate in bits 87-90 holds; only PT is lowered. Measured
        // on an NVIDIA GPU (tests/tensor_lowering_check.py): a negative result or -0 becomes +0, +inf stays, -inf
        // becomes 0, a NaN result stays NaN (0x7fff). The sum goes to TexOut and integer operations build the mask of
        // the half lanes to clear. Maxwell PRMT would do that in one instruction and the translator implements it
        // (xlate/patches/0021-prmt-byte-permute.patch, Instructions/InstEmitPermute.cs PrmtR); this keeps the integer
        // form, which is what was measured.
        // F32 results (78), the H0_NH1 source (86) and absolute values where Maxwell has none are not lowered. HADD2's
        // second operand is B, or C for forms 2/3.
        void Half(ulong maxwell)
        {
            uint op = _i.Bits(0, 12) & 0x1ff;
            if (op == 0x030 && _i.Bit(78))
            {
                // HADD2.F32 Rd, -RZ, b: the compiler's half-to-float conversion of one lane of b
                var a0 = HalfSrc(0, SA);
                var bh = HalfSrc(Form is 2 or 3 ? 2 : 1, SB);
                if (a0.reg != RZ || bh.swz is 0 or 1 || _i.Bit(77) || _i.Bits(81, 10) != 0) throw Fail("HADD2.F32 mode");
                EmitG(0x5ca8000000000000UL | (2UL << 8) | (1UL << 10) | ((ulong)bh.reg << 20) | (bh.swz == 3 ? 1UL << 41 : 0)
                      | (bh.neg ? 1UL << 45 : 0) | (bh.abs ? 1UL << 49 : 0) | (uint)Gpr((uint)Dst));
                return;
            }
            bool relu = op == 0x031 && _i.Bit(79);
            if (relu && (_i.Bit(77) || _i.Bits(85, 2) != 0 || _i.Bits(87, 4) != 7)) throw Fail("HFMA2.RELU mode");
            if (_i.Bit(78) || (_i.Bit(79) && !relu) || _i.Bit(91)
                || (op == 0x031 ? (relu ? _i.Bits(85, 2) : _i.Bits(85, 6)) != 0 : _i.Bits(81, 10) != 0))
                throw Fail("half arithmetic mode");
            if (op == 0x030 && _i.Bit(76)) throw Fail("HADD2 mode");
            var a = HalfSrc(0, SA);
            var b = HalfSrc(op == 0x030 && Form is 2 or 3 ? 2 : 1, SB);
            uint precision = _i.Bit(76) ? 2u : _i.Bits(80, 1);
            ulong w = maxwell | ((ulong)a.reg << 8) | ((ulong)b.reg << 20) | ((ulong)b.swz << 28) | ((ulong)a.swz << 47)
                      | ((ulong)_i.Bits(77, 1) << 32) | (uint)(relu ? TexOut : Gpr((uint)Dst));
            if (op == 0x030)
                w |= (b.abs ? 1UL << 30 : 0) | (b.neg ? 1UL << 31 : 0) | (a.neg ? 1UL << 43 : 0) | (a.abs ? 1UL << 44 : 0)
                     | ((ulong)_i.Bits(80, 1) << 39);
            else if (op == 0x032)
                w |= (b.abs ? 1UL << 30 : 0) | (a.neg != b.neg ? 1UL << 31 : 0) | (a.abs ? 1UL << 44 : 0) | ((ulong)precision << 39);
            else
            {
                var c = HalfSrc(2, SC);
                if (a.abs || b.abs || c.abs) throw Fail("HFMA2 absolute operand");
                w |= (c.neg ? 1UL << 30 : 0) | (a.neg != b.neg ? 1UL << 31 : 0) | ((ulong)c.swz << 35) | ((ulong)precision << 37)
                     | ((ulong)c.reg << 39);
            }
            if (!relu) { EmitG(w); return; }
            // RELU on both half lanes of the sum x at once: each half h becomes |h| (its sign bit cleared), or 0 when h is
            // a negative number (sign set, not NaN). |h| > 0x7c00 means NaN, so |h| + 0x3ff carries into bit 15 of its
            // half exactly for a NaN, and never into the other half (|h| + 0x3ff < 0x10000). A NaN keeps the payload the
            // host computed, with the sign cleared (the hardware's is 0x7fff). Scratch writes unguarded, Rd guarded.
            const int x = TexOut, y = TexOut + 1, t = TexOut + 2;
            Emit(Unguarded(w));                                                                         // HFMA2 x
            Lop32i(y, x, 0x7fff7fff, 0);                                                                // y = |h| per half
            Emit(Unguarded(0x1c00000000000000UL | (0x03ff03ffUL << 20) | ((ulong)y << 8) | t));          // IADD32I t = y + 0x03ff03ff
            Emit(Unguarded(0x5c40000000000000UL | (7UL << 48) | (1UL << 40) | ((ulong)t << 20) | ((ulong)x << 8) | t));   // LOP.AND t = x & ~t
            Emit(Unguarded(0x3828000000000000UL | (15UL << 20) | ((ulong)t << 8) | t));                 // SHR t >>= 15
            Lop32i(t, t, 0x00010001, 0);                                                                // bit 0 / 16: clear that half
            Emit(Unguarded(0x5c18000000000000UL | (16UL << 39) | (1UL << 48) | ((ulong)t << 20) | ((ulong)t << 8) | t));   // ISCADD t = (t << 16) - t
            // LOP.AND Rd = y & ~t (InvB bit 40), no predicate output (PT at 48)
            EmitG(0x5c40000000000000UL | (7UL << 48) | (1UL << 40) | ((ulong)t << 20) | ((ulong)y << 8) | (uint)Gpr((uint)Dst));
        }

        // MOVM.16.MT88 Rd, Ra (0x23a): an 8x8 matrix of 16-bit elements spread over the warp (lane t holds row t/4,
        // columns 2(t%4) in the low half and 2(t%4)+1 in the high half) is transposed into the same layout. Element (r, c)
        // lives in lane 4r + c/2, half c%2, so lane t takes both halves from lanes 8(t%4) + t/8 and that + 4, half
        // (t/4)%2. Maxwell has no matrix move: two SHFL.IDX over the whole warp, the half picked by a shift, ISCADD to pack.
        // Scratch is written unguarded so every lane takes part in the shuffles; only Rd is guarded.
        void Movm()
        {
            if (_i.W1 != 0 || _i.W2 != 0) throw Fail("MOVM mode");
            int src = Gpr(_i.Bits(24, 8)), d = Gpr((uint)Dst);
            int lane = TexCoord, shift = TexCoord + 1, lo = TexMeta, hi = TexMeta + 1;
            void shl(int r, int a, uint n) => Emit(Unguarded(0x3848000000000000UL | ((ulong)n << 20) | ((ulong)a << 8) | (uint)r));
            void shr(int r, int a, uint n) => Emit(Unguarded(0x3828000000000000UL | ((ulong)n << 20) | ((ulong)a << 8) | (uint)r));
            // SHFL.IDX r, value, lane register, clamp 31 (whole warp), no predicate output
            void shfl(int r, int value, int from) => Emit(Unguarded(0xef10000000000000UL | (7UL << 48) | (31UL << 34) | (1UL << 29)
                                                                    | ((ulong)from << 20) | ((ulong)value << 8) | (uint)r));
            Emit(Unguarded(0xf0c8000000000000UL | (uint)SX));                                            // S2R SX, SR_LANEID
            Lop32i(lane, SX, 3, 0);
            shl(lane, lane, 3);                                                                          // 8(t%4)
            shr(shift, SX, 3);                                                                           // t/8
            Emit(Unguarded(0x5c10000000000000UL | ((ulong)shift << 20) | ((ulong)lane << 8) | (uint)lane));   // IADD
            shfl(lo, src, lane);
            Emit(Unguarded(0x1c00000000000000UL | (4UL << 20) | ((ulong)lane << 8) | (uint)lane));      // IADD32I +4: next row
            shfl(hi, src, lane);
            shr(shift, SX, 2);
            Lop32i(shift, shift, 1, 0);
            shl(shift, shift, 4);                                                                        // 16 * ((t/4) % 2)
            Emit(Unguarded(0x5c28000000000000UL | ((ulong)shift << 20) | ((ulong)lo << 8) | (uint)lo));   // SHR by register
            Lop32i(lo, lo, 0xffff, 0);
            Emit(Unguarded(0x5c28000000000000UL | ((ulong)shift << 20) | ((ulong)hi << 8) | (uint)hi));
            EmitG(0x5c18000000000000UL | (16UL << 39) | ((ulong)lo << 20) | ((ulong)hi << 8) | (uint)d);   // ISCADD (hi << 16) + lo
        }

        // POPC Rd, b (bit 63: of ~b).
        void Popc()
        {
            if ((_i.W2 & ~0x08000000u) != 0) throw Fail("POPC mode");
            var (b, _, inv) = Raw(1, SB);
            EmitG(0x5c08000000000000UL | ((ulong)b << 20) | (inv ? 1UL << 40 : 0) | (uint)Gpr((uint)Dst));
        }

        // F2FP.PACK_AB Rd, a, b: half(a) in the high 16 bits, half(b) in the low (round to nearest). With .MERGE_C
        // (bit 78) the high half is the low 16 bits of the C operand instead.
        void F2fp()
        {
            if (_i.Bits(62, 2) != 0 || _i.Bits(73, 5) != 0 || _i.Bits(79, 2) != 0 || _i.Bits(81, 10) != 0 || _i.Bit(91) || _i.Bit(72))
                throw Fail("F2FP mode");
            bool merge = _i.Bit(78);
            int a = merge ? Plain(2, SC) : Plain(0, SA), b = Plain(1, SB);
            if (merge) Mov(247, a, false);
            else Emit(Unguarded(0x5ca8000000000000UL | (1UL << 8) | (2UL << 10) | ((ulong)a << 20) | 247));
            Emit(Unguarded(0x5ca8000000000000UL | (1UL << 8) | (2UL << 10) | ((ulong)b << 20) | 246));
            EmitG(0x5c18000000000000UL | (16UL << 39) | (246UL << 20) | (247UL << 8) | (uint)Gpr((uint)Dst));
        }

        // SUST.P / SULD.P (formatted, bound descriptor): coordinates from Ra, data (stores) at 32-39 or destination
        // (loads), channel mask 72-75, dimension 61-63 in Maxwell's surface numbering (1D, BUFFER_1D, ARRAY_1D, 2D,
        // ARRAY_2D, 3D), descriptor offset 40-53 in bank 54-58. Strong stores (0x0010a000) are taken as plain ones.
        void SurfaceAccess(bool store)
        {
            uint dim = _i.Bits(61, 3), mask = _i.Bits(72, 4), handle = Handle(true), bank = _i.Bits(54, 5);
            uint mode = _i.W2 & ~0xf00u;
            bool okMode = store ? mode is 0x00100000 or 0x0010a000 : mode == 0x001e0000;
            if (!okMode || mask == 0 || dim > 5 || _i.Bits(59, 2) != 0 || handle > 0x1fff || !TexBank(bank))
                throw Fail("surface access mode");
            int coord = Gpr(_i.Bits(24, 8));
            int data = store ? Gpr(_i.Bits(32, 8)) : Gpr((uint)Dst);
            EmitG((store ? 0xeb28000000000000UL : 0xeb08000000000000UL) | (uint)data | ((ulong)coord << 8) | ((ulong)mask << 20)
                  | ((ulong)dim << 33) | ((ulong)handle << 36));
        }

        // Descriptor offset (bits 40-53, in words) of a bound texture or image. NVN2 compute programs keep their handles
        // in one table in the driver bank: texture slot k at start + 8k, image slot j at start + 8 * (textures + j), the
        // start and counts from the control section (job "nvn2_table", runtime/gfx_shader.c). It is renumbered into the
        // NVN1 layout the translator's host binds by: textures at 0x20 + 8k, images at 0x120 + 8j.
        uint Handle(bool image)
        {
            uint h = _i.Bits(40, 14);
            if (!_compute || _table == null) return h;
            int entry = ((int)h * 4 - _table[0]) / 8;
            if ((int)h * 4 < _table[0] || ((int)h * 4 - _table[0]) % 8 != 0 || entry >= _table[1] + _table[2])
                throw Fail($"handle word {h} outside the binding table (start 0x{_table[0]:x}, {_table[1]} textures, {_table[2]} images)");
            if (image != (entry >= _table[1])) throw Fail($"handle word {h} is a {(image ? "texture" : "image")} entry");
            return image ? (uint)(0x120 + 8 * (entry - _table[1])) / 4 : (uint)(0x20 + 8 * entry) / 4;
        }

        // Bound texture descriptors: bank 2 for graphics stages, bank 0 for compute (NVN2 keeps the compute handles in
        // the driver bank). The translator leaves the bank to the host, which reads the handle table as NVN1's c2.
        bool TexBank(uint bank) => bank == 2 || (bank == 0 && _compute);

        // TLD4 (gather), 2D or ARRAY_2D: component 87-88, offset mode 76-77 (1: one offset register, 2: per-tap offsets
        // in two registers), depth compare 78, FP16 results 79, operands packed as TEX's. Maxwell TLD4: component 56-57,
        // offset mode 54-55 (same numbering), DC 50.
        void Tld4()
        {
            uint dim = _i.Bits(61, 3), mask = _i.Bits(72, 4), offMode = _i.Bits(76, 2);
            bool dc = _i.Bit(78);
            if (dim is not 1 and not 5 || offMode == 3 || _i.Bits(81, 3) != 7 || _i.Bit(59) || mask == 0 || _i.Bit(89))
                throw Fail("TLD4 mode");
            uint handle = Handle(false), bank = _i.Bits(54, 5);
            if (handle > 0x1fff || !TexBank(bank)) throw Fail($"TLD4 descriptor c[{bank}] 0x{handle:x}");
            int coords = TexCoords[dim];
            int count = coords + (int)offMode + (dc ? 1 : 0);
            int inA = count <= 4 ? (count + 1) / 2 : 4;
            int ra = (int)_i.Bits(24, 8), rb = (int)_i.Bits(32, 8);
            int Value(int k) => k < inA ? (ra == RZ ? RZ : Gpr((uint)(ra + k))) : (rb == RZ ? RZ : Gpr((uint)(rb + k - inA)));
            for (int k = 0; k < coords; k++) Mov(TexCoord + k, Value(k), false);
            int next = coords, meta = TexMeta;
            for (int k = 0; k < offMode; k++) Mov(meta++, Value(next++), false);
            if (dc) Mov(meta++, Value(next++), false);
            EmitG(0xc800000000000000UL | TexOut | ((ulong)TexCoord << 8) | ((ulong)(meta == TexMeta ? RZ : TexMeta) << 20)
                  | ((ulong)TexMaxwellDim[dim] << 28) | ((ulong)mask << 31) | ((ulong)handle << 36) | ((ulong)_i.Bits(90, 1) << 49)
                  | (dc ? 1UL << 50 : 0) | (7UL << 51) | ((ulong)offMode << 54) | ((ulong)_i.Bits(87, 2) << 56));
            TexResults(mask, _i.Bit(79));
        }

        // PLOP3.LUT Pu, Pv, a, b, c: table in bits 64-66 and 72-76, inputs a 87-90, b 77-80, c 68-71 (index | negate << 3),
        // outputs 81-83 and 84-86 (the second with the table in bits 16-23). Each input becomes 0 or ~0 (SEL), LOP3 combines
        // them and ISETP.NE sets the output. Bit 67 (c a uniform predicate) is not lowered for the vector form.
        void Plop3()
        {
            uint lut = _i.Bits(64, 3) | _i.Bits(72, 5) << 3, t1 = _i.Bits(81, 3), t2 = _i.Bits(84, 3), lut2 = _i.Bits(16, 8);
            const int ones = 240, ra = 241, rb = 242, rc = 243, r1 = 244, r2 = 245;
            Mov32i(ones, 0xffffffff);
            Sel(ra, ones, RZ, _i.Bits(87, 4), false);
            Sel(rb, ones, RZ, _i.Bits(77, 4), false);
            uint c = _i.Bits(68, 4);
            if (_i.Bit(67) && !_uniform)
            {
                // c is UPn: 0 - ((RUP >> n) & 1) gives 0 or ~0 straight from the uniform predicate state; UPT is ~0.
                if ((c & 7) == 7) Mov(rc, ones, false);
                else
                {
                    Emit(Unguarded(0x3828000000000000UL | ((ulong)(c & 7) << 20) | ((ulong)RUP << 8) | rc));   // SHR imm
                    Lop32i(rc, rc, 1, 0);
                    Iadd3(rc, RZ, rc, RZ, false, negB: true);
                }
                if ((c & 8) != 0) Lop32i(rc, rc, 0xffffffff, 2);
            }
            else Sel(rc, ones, RZ, c, false);
            ulong lop3(int dst, uint table) => Unguarded(0x5be0000000000000UL | ((ulong)rc << 39) | ((ulong)rb << 20) | ((ulong)ra << 8) | (uint)dst | ((ulong)table << 28) | (7UL << 48));
            Emit(lop3(r1, lut));
            if (t2 != 7) Emit(lop3(r2, lut2));
            ulong ne(uint p, int r) => 0x5b60000000000000UL | (5UL << 49) | ((ulong)PT << 39) | ((ulong)RZ << 20) | ((ulong)r << 8) | ((ulong)p << 3) | PT;
            if (t1 != 7) EmitG(ne(t1, r1));
            if (t2 != 7) EmitG(ne(t2, r2));
        }

        // LDS / STS: [Ra << (stride ? stride + 1 : 0) (+ a uniform register when bit 91 is set: bits 32-39 for loads,
        // 64-71 for stores) + offset 40-63], size 73-75 (as Maxwell's), stride 78-79. STS data at 32-39.
        void SharedAccess(uint op)
        {
            bool store = op != 0x984;
            uint width = _i.Bits(73, 3), stride = _i.Bits(78, 2);
            bool uniformBase = op is 0x984 or 0x988 && _i.Bit(91);
            uint ignored = uniformBase ? (op == 0x988 ? 0x080000ffu : 0x08000000u) : 0u;
            if (width > 6 || (_i.W2 & ~ignored) != (width << 9 | stride << 14) || (op == 0x984 && !uniformBase && _i.Bits(32, 8) != 0)
                || (op == 0x988 && !uniformBase))
                throw Fail("shared memory addressing mode");
            int addr = Gpr(_i.Bits(24, 8));
            if (stride != 0 && addr != RZ)
            {
                Emit(Unguarded(0x3848000000000000UL | ((ulong)(stride + 1) << 20) | ((ulong)addr << 8) | SA));   // SHL imm
                addr = SA;
            }
            if (uniformBase)
            {
                int u = Ur(_i.Bits(op == 0x988 ? 64 : 32, 8));
                if (u != RZ) { Iadd3(SA, addr, u, RZ, false); addr = SA; }
            }
            int data = store ? Gpr(_i.Bits(32, 8)) : Gpr((uint)Dst);
            EmitG((store ? 0xef58000000000000UL : 0xef48000000000000UL) | (uint)data | ((ulong)addr << 8)
                  | ((ulong)_i.Bits(40, 24) << 20) | ((ulong)width << 48));
        }

        // BAR.SYNC 0 for the whole workgroup (bit 80, .DEFER_BLOCKING, needs nothing here).
        void Bar()
        {
            if (_i.W1 != 0 || (_i.W2 & ~0x10000u) != 0 || _i.Bits(12, 4) != 7) throw Fail("BAR mode");
            Emit(Unguarded(0xf0a8000000000000UL | (1UL << 43) | (1UL << 44) | (7UL << 39)));
        }

        // MEMBAR: Maxwell MEMBAR.GL whatever the scope. A barrier at least as wide as the one asked for orders the same
        // accesses; the decoder reads MEMBAR's level from bits 8-9 (CTA 0, GL 1, SYS 2), and the SPIR-V it makes is a
        // device-scope memory barrier for GL and SYS alike.
        void Membar() => Emit(Unguarded(0xef98000000000000UL | (1UL << 8)));

        // ISETP.EX: the high-word half of a 64-bit compare. The low words' result is the predicate of bits 68-71; EQ is
        // (high equal AND low), NE (high different OR low), an ordering (high strictly ordered OR (high equal AND low)),
        // signed with bit 73. The result is combined with Pc (87-90) by the operation in 74-75 into P (81-83) and Q
        // (84-86), as ISETP does: Maxwell ISETPs into a saved scratch predicate (P6, or the highest one the instruction
        // does not name), then PSETP.
        void SetpExtended()
        {
            uint cmp = _i.Bits(76, 3), combine = _i.Bits(74, 2), low = _i.Bits(68, 4);
            if (cmp is 0 or 7 || combine == 3) throw Fail("ISETP.EX mode");
            var (a, _, _) = Raw(0, SA);
            var (b, _, _) = Raw(1, SB);
            // the scratch predicate: P6 unless the instruction names it (low result, Pc, guard or a destination)
            int t = 6;
            while (t >= 0 && (t == (int)(low & 7) || t == (int)_i.Bits(87, 3) || t == (int)_i.Bits(12, 3) ||
                              t == (int)_i.Bits(81, 3) || t == (int)_i.Bits(84, 3)))
                t--;
            if (t < 0) throw Fail("ISETP.EX: no free scratch predicate");
            Emit(Unguarded(0x38e8000000000000UL | (1UL << (20 + t)) | ((ulong)RZ << 8) | SX));   // P2R: save P6
            ulong isetp(uint c, uint input, uint bop, bool signedCmp) =>
                0x5b60000000000000UL | ((ulong)t << 3) | PT | ((ulong)a << 8) | ((ulong)b << 20) | ((ulong)(input & 7) << 39)
                | ((ulong)(input >> 3) << 42) | ((ulong)bop << 45) | (signedCmp ? 1UL << 48 : 0) | ((ulong)c << 49);
            bool sg = _i.Bit(73);
            const uint And = 0, Or = 1, Lt = 1, Eq = 2, Gt = 4, Ne = 5;
            if (cmp == Eq) EmitG(isetp(Eq, low, And, sg));
            else if (cmp == Ne) EmitG(isetp(Ne, low, Or, sg));
            else { EmitG(isetp(Eq, low, And, sg)); EmitG(isetp(cmp <= 3 ? Lt : Gt, (uint)t, Or, sg)); }
            // PSETP P, Q = (P6 AND PT) combine Pc
            EmitG(0x5090000000000000UL | _i.Bits(84, 3) | ((ulong)_i.Bits(81, 3) << 3) | ((ulong)t << 12) | ((ulong)PT << 29)
                  | ((ulong)_i.Bits(87, 3) << 39) | ((ulong)_i.Bits(90, 1) << 42) | ((ulong)combine << 45));
            Emit(Unguarded(0x38f0000000000000UL | (1UL << (20 + t)) | ((ulong)SX << 8)));        // R2P: restore P6
        }

        // UBREV URd, URb: BREV on uniform registers (bit 91 marks the uniform datapath).
        void Ubrev()
        {
            if ((_i.W2 & ~0x08000000u) != 0 || _i.W1 > 255) throw Fail("UBREV mode");
            int d = Ur((uint)Dst);
            if (d == RZ) return;
            EmitG(0x3828000000000000UL | ((ulong)Ur(_i.Bits(32, 8)) << 8) | (1UL << 39) | (1UL << 40) | (uint)d);   // SHR.BREV by 0
        }

        // UPOPC URd, URb: POPC on uniform registers, lowered through POPC (0x309) with register fields naming uniform
        // registers (as UniformAlu does).
        void Upopc()
        {
            if ((_i.W2 & ~0x08000000u) != 0 || _i.W1 > 255) throw Fail("UPOPC mode");
            Insn saved = _i;
            _i.Lo = (_i.Lo & ~0xfffUL) | 0x309;
            _i.Hi &= ~(1UL << 27);
            _uniform = true;
            try { Popc(); }
            finally { _uniform = false; _i = saved; }
        }

        // UP2UR URd, UPR, URa, mask: P2R over the uniform predicates, byte 0 only: URd = (URa & ~mask) | (UP & mask),
        // the mask in bits 32-38. RUP holds UPk in bit k.
        void Up2ur()
        {
            if (_i.W1 > 0x7f || (_i.W2 & ~0x08000000u) != 0) throw Fail("UP2UR mode");
            int d = Ur((uint)Dst), a = Ur(_i.Bits(24, 8));
            if (d == RZ) return;
            uint mask = _i.W1;
            Lop32i(SX, RUP, mask, 0);   // AND
            if (a == RZ) { Mov(d, SX); return; }
            Lop32i(SA, a, ~mask, 0);
            Iadd3(d, SA, SX, RZ, true);   // disjoint bits: the sum is the OR
        }

        // F2IP.U8.F32 Rd, Ra, Rb, RZ (opcode 0x243): two floats converted to unsigned bytes and packed into the low
        // half of Rd. Rb's byte goes in bits 0-7, Ra's byte in bits 8-15, and bits 16-31 are zero. With Ra = RZ only
        // Rb's byte is written. W1 holds only Rb (bits 32-39), and the C operand (bits 64-71) must be RZ. Rounding is
        // in bits 78-79, using the same codes as Maxwell's F2I RoundMode (bits 39-40):
        //   W2 0x0000c4ff: bits 78-79 = 3, round toward zero;
        //   W2 0x000004ff: bits 78-79 = 0, round to nearest even. The compiler uses this form for unorm values it has
        //                  already multiplied by 255.
        // Each byte goes through NaN -> 0, a float clamp to [0, 255], then F2I.U32.F32 with that rounding. The clamp
        // has to be explicit. The translator's F2I (Instructions/InstEmitConversion.cs, EmitF2I) clamps only
        // U16/S16 destinations, and IDstFmt has no U8, so a plain F2I.U8 lets values above 255 through. It also feeds
        // NaN to FMax, whose SPIR-V result is undefined.
        void F2ipByte()
        {
            if ((_i.W1 & ~0xffu) != 0 || (_i.W2 != 0x0000c4ff && _i.W2 != 0x000004ff)) throw Fail("packed float-to-byte mode");
            int a = Gpr(_i.Bits(24, 8)), b = Gpr(_i.Bits(32, 8)), d = Gpr((uint)Dst);
            if (d == RZ) return;
            uint round = _i.Bits(78, 2);

            // Convert src to an integer 0-255. Steps 1-4 work in scratch register t without a guard. Step 5 writes dst
            // and is guarded only when dst is the guest register.
            void ToByte(int src, int t, int dst, bool guarded)
            {
                // FSET.NUM.AND t = (src is not NaN) ? 0xffffffff : 0 (FComp 7 in 48-51, source predicate PT in 39-41,
                // BVal clear so the result is an integer mask)
                Emit(Unguarded(0x5800000000000000UL | (7UL << 48) | ((ulong)PT << 39) | ((ulong)src << 20) | ((ulong)src << 8) | (uint)t));
                // LOP.AND t = src & t: NaN becomes +0.0 (destination predicate PT in 48-50)
                Emit(Unguarded(0x5c40000000000000UL | ((ulong)PT << 48) | ((ulong)t << 20) | ((ulong)src << 8) | (uint)t));
                // FMNMX t = max(t, 0.0): immediate form, predicate !PT (39-42) selects the maximum
                Emit(Unguarded(0x3860000000000000UL | ((ulong)PT << 39) | (1UL << 42) | ((ulong)t << 8) | (uint)t));
                // FMNMX t = min(t, 255.0): 255.0f is 0x437f0000, whose top 20 bits 0x437f0 fit the immediate field
                Emit(Unguarded(0x3860000000000000UL | (0x437f0UL << 20) | ((ulong)PT << 39) | ((ulong)t << 8) | (uint)t));
                // F2I.U32.F32 dst = t, with the instruction's rounding (IDstFmt U32 in 8-9, SrcFmt F32 in 10-11, FTZ 44)
                Put(0x5cb0000000000000UL | (2UL << 8) | (2UL << 10) | ((ulong)round << 39) | (1UL << 44) | ((ulong)t << 20) | (uint)dst, guarded);
            }

            if (a == RZ)
            {
                // Single byte: convert straight into Rd. Steps 1-4 read b and write only SA, so this is safe when d == b.
                ToByte(b, SA, d, true);
                return;
            }
            // Both bytes go to scratch before Rd is written, so d may alias a or b.
            ToByte(b, SA, SA, false);
            ToByte(a, SB, SB, false);
            // ISCADD Rd = (SB << 8) + SA. Both values are 0-255, so the sum equals the OR of the two bytes.
            EmitG(0x5c18000000000000UL | (8UL << 39) | ((ulong)SA << 20) | ((ulong)SB << 8) | (uint)d);
        }

        // TXD (0xb6c): a sample with explicit derivatives, 2D or ARRAY_2D, bound descriptor. Fields as TEX's: Rd 16-23,
        // Ra 24-31 (x, y, then for ARRAY_2D the layer as an integer in its low 16 bits), Rb 32-39 (the four derivatives,
        // interleaved: dx.x, dy.x, dx.y, dy.y), descriptor word offset 40-53 in bank 54-58, dimension 61-63, second
        // destination 64-71, mask 72-75, bit 90 no-dependency. Offsets (76), FP16 results (79) and the bindless forms
        // (59-60) are not lowered. Bits 81-84 are set in every TXD seen (81-83: no sparse predicate).
        // Maxwell TXD (0xde00) takes the coordinates and the layer the same way in Ra.., but the translator's EmitTxd reads Rb..
        // as dPdx (2 values) then dPdy (2 values), so the derivatives are reordered on the way into TexMeta.
        // With no second destination the third and fourth channels are dropped (SM70+ register pairs), so the mask
        // handed to Maxwell keeps only its two lowest channels and TexResults writes Rd, Rd+1; otherwise TexResults
        // writes Rd, Rd+1 and then the second destination, which need not follow Rd.
        void Txd()
        {
            uint dim = _i.Bits(61, 3), mask = _i.Bits(72, 4);
            if ((_i.W2 & ~(0xfffu | 1u << 26)) != 0x001e0000 || _i.Bits(59, 2) != 0 || dim is not 1 and not 5 || mask == 0)
                throw Fail("TXD mode");
            uint handle = Handle(false), bank = _i.Bits(54, 5);
            if (handle > 0x1fff) throw Fail("TXD descriptor offset");
            if (!TexBank(bank)) throw Fail($"TXD descriptor bank {bank}");
            int d2 = (int)_i.Bits(64, 8);
            if (Dst == RZ && d2 == RZ) return;   // nothing is written, and sampling has no side effects
            if (d2 == RZ)
            {
                uint upper = mask & (mask - 1);   // drop the lowest channel...
                upper &= upper - 1;               // ...and the next: what is left has no destination
                mask ^= upper;
            }
            int ra = (int)_i.Bits(24, 8), rb = (int)_i.Bits(32, 8);
            int coords = dim == 5 ? 3 : 2;
            for (int k = 0; k < coords; k++) Mov(TexCoord + k, ra == RZ ? RZ : Gpr((uint)(ra + k)), false);
            // SM86 order dx.x, dy.x, dx.y, dy.y; the translator's Maxwell order dx.x, dx.y, dy.x, dy.y.
            int[] order = { 0, 2, 1, 3 };
            for (int k = 0; k < 4; k++) Mov(TexMeta + k, rb == RZ ? RZ : Gpr((uint)(rb + order[k])), false);
            EmitG(0xde00000000000000UL | TexOut | ((ulong)TexCoord << 8) | ((ulong)TexMeta << 20) | ((ulong)TexMaxwellDim[dim] << 28)
                  | ((ulong)mask << 31) | ((ulong)handle << 36) | ((ulong)_i.Bits(90, 1) << 49) | (7UL << 51));
            TexResults(mask, false);
        }

        // LDSM.16.M88.{1,2,4} and LDSM.16.MT88.{1,2,4} (ldmatrix): count = 1 << bits 72-73 (1, 2 or 4) 8x8 matrices of
        // 16-bit elements from shared memory into Rd..Rd+count-1 (Rd aligned to count). Lanes 8m..8m+7 hold, in Ra, the
        // byte addresses of rows 0-7 of matrix m; bit 91 adds a uniform register (bits 32-39, as in SharedAccess) and every
        // row adds the immediate offset 40-63. Without transpose (bit 78 clear) lane t receives in Rd+m the 32 bits at row
        // t/4, bytes 4(t%4)..4(t%4)+3 of matrix m. With transpose (.MT88) it receives column t/4 of row 2(t%4) in the low
        // half and of row 2(t%4)+1 in the high half.
        // Maxwell has no matrix load, so each lane fetches the row address it needs from the lane that holds it
        // (SHFL.IDX across the whole warp), adds its own column and loads with LDS: one LDS.32 per matrix, or for .MT88 two
        // LDS.U16 per matrix joined by ISCADD (high << 16) + low. Address arithmetic and the shuffles are unguarded
        // scratch writes so every lane takes part in the shuffles (the hardware needs the whole warp too); the loads and
        // the writes to Rd are guarded by the instruction's predicate.
        // Scratch: SX lane id, then the column bytes (non-transposed); TexCoord the source lane of row 0 (or of row
        // 2(t%4)), TexCoord+1 the lane of later matrices (or the column bytes for .MT88), TexCoord+2 the lane for .MT88;
        // SA the fetched row address; SC the base address when it needs a copy; TexMeta/TexMeta+1 the two halves.
        void Ldsm()
        {
            uint sizeCode = _i.Bits(72, 2);
            bool transpose = _i.Bit(78), uniformOffset = _i.Bit(91);
            // Allowed in word 2: size 72-73 (0x300), transpose 78 (0x4000), uniform offset 91 (0x08000000).
            if (sizeCode == 3 || (_i.W2 & ~0x08004300u) != 0 || (!uniformOffset && _i.Bits(32, 8) != 0))
                throw Fail("LDSM mode");
            int count = 1 << (int)sizeCode;
            if (Dst == RZ) return;   // a load into RZ has no visible effect
            if (Dst % count != 0 || Dst + count > RZ) throw Fail("LDSM destination alignment");
            var dst = new int[count];
            for (int m = 0; m < count; m++) dst[m] = Gpr((uint)(Dst + m));

            int addr = Gpr(_i.Bits(24, 8));
            if (uniformOffset)
            {
                int u = Ur(_i.Bits(32, 8));
                if (u != RZ) { Iadd3(SC, addr, u, RZ, false); addr = SC; }
            }
            // A destination that is also the base would be overwritten before the later matrices fetch their rows.
            if (addr != RZ && Array.IndexOf(dst, addr) >= 0) { Mov(SC, addr, false); addr = SC; }
            ulong offset = (ulong)_i.Bits(40, 24) << 20;

            // Unguarded Maxwell helpers: SHR/SHL by an immediate (unsigned), IADD32I, and SHFL.IDX of the base address
            // from a lane held in a register, clamp 31 with segment mask 0 (the whole warp), valid predicate to PT.
            void shr(int d, int a, int n) => Emit(Unguarded(0x3828000000000000UL | ((ulong)n << 20) | ((ulong)a << 8) | (uint)d));
            void shl(int d, int a, int n) => Emit(Unguarded(0x3848000000000000UL | ((ulong)n << 20) | ((ulong)a << 8) | (uint)d));
            void addImm(int d, int a, uint imm) => Emit(Unguarded(0x1c00000000000000UL | ((ulong)imm << 20) | ((ulong)a << 8) | (uint)d));
            void fetchRow(int d, int lane) => Emit(Unguarded(0xef10000000000000UL | (uint)d | ((ulong)addr << 8) | ((ulong)lane << 20)
                                                             | (0x1fUL << 34) | (1UL << 29) | (7UL << 48)));
            // LDS d, [a + offset], size as Maxwell's LsSize2 (2 U16, 4 B32), guarded.
            void lds(int d, int a, ulong size) => EmitG(0xef48000000000000UL | (uint)d | ((ulong)a << 8) | offset | (size << 48));

            Emit(Unguarded(0xf0c8000000000000UL | SX));   // S2R SX, SR_LANEID (0)
            if (!transpose)
            {
                shr(TexCoord, SX, 2);    // lane holding row t/4 of matrix 0
                Lop32i(SX, SX, 3, 0);    // t & 3 (LOP32I.AND)
                shl(SX, SX, 2);          // column bytes 4(t%4)
                for (int m = 0; m < count; m++)
                {
                    int lane = TexCoord;
                    if (m != 0) { addImm(TexCoord + 1, TexCoord, (uint)(8 * m)); lane = TexCoord + 1; }
                    fetchRow(SA, lane);
                    Iadd3(SA, SA, SX, RZ, false);
                    lds(dst[m], SA, 4);   // LDS.32
                }
                return;
            }
            Lop32i(TexCoord, SX, 3, 0);
            shl(TexCoord, TexCoord, 1);            // lane holding row 2(t%4) of matrix 0
            shr(TexCoord + 1, SX, 2);
            shl(TexCoord + 1, TexCoord + 1, 1);    // column bytes 2(t/4)
            for (int m = 0; m < count; m++)
            {
                for (int half = 0; half < 2; half++)
                {
                    int lane = TexCoord;
                    uint step = (uint)(8 * m + half);
                    if (step != 0) { addImm(TexCoord + 2, TexCoord, step); lane = TexCoord + 2; }
                    fetchRow(SA, lane);
                    Iadd3(SA, SA, TexCoord + 1, RZ, false);
                    lds(TexMeta + half, SA, 2);   // LDS.U16 (zero-extended)
                }
                // ISCADD Rd+m = (high << 16) + low
                EmitG(0x5c18000000000000UL | (16UL << 39) | ((ulong)TexMeta << 20) | ((ulong)(TexMeta + 1) << 8) | (uint)dst[m]);
            }
        }

        // HMMA.1688 / HMMA.16816 Rd, Ra, Rb, Rc with F16 inputs: a warp-wide matrix multiply-accumulate D(16x8) =
        // A(16xK) * B(Kx8) + C(16x8), K = 8, or 16 when bit 75 is set. Bit 76 selects F32 accumulators and results
        // (.F32: C and D are four registers each) instead of F16 ones (.F16: a register pair). Fields: D 16-23, A 24-31
        // (K/4 registers), B 32-39 (K/8 registers), C 64-71 (RZ means a zero accumulator). With g = lane / 4 and
        // q = lane % 4, each lane holds (the lower-numbered column or k in the low half of every packed register):
        //   A register 2j + r: row g + 8r, k 8j + 2q and 8j + 2q + 1   (j is 0 only when K = 8)
        //   B register j:      k 8j + 2q and 8j + 2q + 1, column g
        //   C / D, .F16:       register r holds row g + 8r, columns 2q and 2q + 1
        //   C / D, .F32:       register 2r + c holds row g + 8r, column 2q + c
        // Maxwell has no tensor instructions, so the product is expanded per lane. For each k pair (2p, 2p + 1) the lane
        // fetches the A word of its own rows from lane 4g + p % 4 and, for each of its two columns n = 2q + c, the B word
        // from lane 4n + p % 4 (SHFL.IDX), widens the halves to F32 (F2F.F32.F16, exact) and accumulates with FFMA in
        // F32 (an F16 x F16 product is exact in F32, so the step rounds once whether or not the host fuses it).
        // The .F16 form rounds to F16 once, after all K products, as the hardware does: it does not round between the
        // two eight-k halves of HMMA.16816. Measured on an NVIDIA GPU (tests/tensor_lowering_check.py compares this
        // lowering with those results): a first-half sum of 65536 that the second half cancels gives 0, which an F16
        // rounding after the first half turns into +inf, and 1 + 2^-11 + 2^-11 split over the halves gives 1 + 2^-10,
        // which that rounding turns into 1. The F16 results are then packed as F2fp in this file does (F2F.F16 writes
        // the low half and zeroes the high half, ISCADD merges the column pair).
        // The work runs unguarded because every lane must supply its fragments to the shuffles; only the result writes
        // take the instruction's guard. Scratch R239-R252 (this file's scratch range, see the header); the guest operands
        // are never there (guest and uniform registers end below R237, spill temporaries and uniform registers are R156-R235).
        void Hmma()
        {
            bool k16 = _i.Bit(75), f32 = _i.Bit(76);
            uint d = (uint)Dst, a = _i.Bits(24, 8), b = _i.Bits(32, 8), c = _i.Bits(64, 8);
            int aRegs = k16 ? 4 : 2, bRegs = k16 ? 2 : 1, cdRegs = f32 ? 4 : 2;
            // A fragment base aligned to its size and the whole fragment below RZ (RZ itself reads as zeros).
            bool fits(uint reg, int n) => reg == RZ || (reg % (uint)n == 0 && reg + (uint)n <= RZ);
            // Word 1 above B, word 2 other than C, the shape (75) and the accumulator type (76), and word 3 bits 96-104 /
            // 126-127 select modes (bit 74 another shape, HMMA.1684; other input types) that are not lowered.
            if ((_i.W1 & ~0xffu) != 0 || (_i.W2 & ~0x18ffu) != 0 || ((uint)(_i.Hi >> 32) & 0xc00001ffu) != 0
                || !fits(d, cdRegs) || !fits(a, aRegs) || !fits(b, bRegs) || !fits(c, cdRegs))
                throw Fail("HMMA mode");
            if (d == RZ) return;   // no result and no other effect

            // Register k of a fragment (RZ stays RZ); Gpr maps spilled guest registers to this instruction's temporaries.
            int part(uint reg, int k) => reg == RZ ? RZ : Gpr(reg + (uint)k);

            const int laneA = 239;   // 4g: the lane of A's row group for k pair 0
            const int laneB = 240;   // 8q: the lane holding B column 2q for k pair 0
            const int lane = 241;    // the lane of the current shuffle
            const int word = 242;    // the shuffled packed-half word
            const int aw = 243;      // 243-246: A element (row g + 8r, k 2p + h) at aw + 2r + h, F32
            const int bw = 247;      // 247-248: B element (k 2p + h, current column) at bw + h, F32
            const int acc = 249;     // 249-252: accumulator (row g + 8r, column 2q + c) at acc + 2r + c, F32

            void widen(int target, int src, bool high) =>   // F2F.F32.F16 target, src.H0 / .H1
                Emit(Unguarded(0x5ca8000000000000UL | (2UL << 8) | (1UL << 10) | ((ulong)src << 20) | (high ? 1UL << 41 : 0) | (uint)target));
            void narrow(int t) =>                          // F2F.F16.F32 t, t (round to nearest; high half zero)
                Emit(Unguarded(0x5ca8000000000000UL | (1UL << 8) | (2UL << 10) | ((ulong)t << 20) | (uint)t));
            void shuffle(int target, int src, int laneReg) =>   // SHFL.IDX PT, target, src, laneReg, 0x1f (as Redux)
                Emit(Unguarded(0xef10000000000000UL | (7UL << 48) | (31UL << 34) | (1UL << 29) | ((ulong)laneReg << 20) | ((ulong)src << 8) | (uint)target));
            int laneAt(int baseLane, uint offset)          // baseLane + offset in `lane` (IADD32I), or baseLane itself
            {
                if (offset == 0) return baseLane;
                Emit(Unguarded(0x1c00000000000000UL | ((ulong)offset << 20) | ((ulong)baseLane << 8) | lane));
                return lane;
            }

            // Accumulators from C, or zero: accumulator 2r + col is register 2r + col of an .F32 C, half col of register
            // r of an .F16 C.
            for (int k = 0; k < 4; k++)
            {
                if (c == RZ) Mov32i(acc + k, 0);
                else if (f32) Mov(acc + k, part(c, k), false);
                else widen(acc + k, part(c, k >> 1), (k & 1) == 1);
            }

            // Lane bases: 8q first (it needs the raw lane id), then 4g.
            Put(0xf0c8000000000000UL | laneA, false);                                                // S2R SR_LANEID
            Lop32i(laneB, laneA, 3, 0);                                                             // q
            Emit(Unguarded(0x3848000000000000UL | (3UL << 20) | ((ulong)laneB << 8) | laneB));       // SHL imm: 8q
            Lop32i(laneA, laneA, 0x1c, 0);                                                          // 4g

            int pairs = k16 ? 8 : 4;
            for (int p = 0; p < pairs; p++)
            {
                uint pq = (uint)(p & 3);   // the source lanes' q for this k pair
                int j = p >> 2;            // which 8-k block of the fragments
                int srcA = laneAt(laneA, pq);
                for (int r = 0; r < 2; r++)
                {
                    shuffle(word, part(a, 2 * j + r), srcA);
                    widen(aw + 2 * r, word, false);
                    widen(aw + 2 * r + 1, word, true);
                }
                for (int col = 0; col < 2; col++)
                {
                    shuffle(word, part(b, j), laneAt(laneB, 4u * (uint)col + pq));
                    widen(bw, word, false);
                    widen(bw + 1, word, true);
                    for (int r = 0; r < 2; r++)
                        for (int h = 0; h < 2; h++)
                        {
                            int t = acc + 2 * r + col;   // FFMA t = A(r, 2p + h) * B(2p + h, col) + t
                            Emit(Unguarded(0x5980000000000000UL | ((ulong)t << 39) | ((ulong)(bw + h) << 20) | ((ulong)(aw + 2 * r + h) << 8) | (uint)t));
                        }
                }
            }

            // .F32: D register 2r + col is accumulator 2r + col.
            if (f32)
            {
                for (int k = 0; k < 4; k++) Mov(part(d, k), acc + k);
                return;
            }
            // .F16: one rounding of each sum to F16, then D register r = column 2q + 1 in the high half, column 2q in the
            // low half (ISCADD hi << 16 + lo).
            for (int t = acc; t < acc + 4; t++) narrow(t);
            for (int r = 0; r < 2; r++)
                EmitG(0x5c18000000000000UL | (16UL << 39) | ((ulong)(acc + 2 * r) << 20) | ((ulong)(acc + 2 * r + 1) << 8) | (uint)part(d, r));
        }


        // UFLO URd, URb: FLO on uniform registers (bit 74 .SH, bit 63 of ~b).
        void Uflo()
        {
            if ((_i.W2 & ~(0x400u | 0x08000000u)) != 0x000e0000) throw Fail("UFLO mode");
            int d = Ur((uint)Dst);
            if (d == RZ) return;
            EmitG(0x5c30000000000000UL | ((ulong)Ur(_i.Bits(32, 8)) << 20) | (_i.Bit(63) ? 1UL << 40 : 0) | ((ulong)_i.Bits(74, 1) << 41) | (uint)d);
        }

        // I2F: source signed (bit 74) and size (84-85: 8/16/32 bits, its byte or half in 60-61), destination size
        // (75-76: F16, F32, F64), rounding 78-79. Maxwell I2F: ISrcFmt (size code in 10-11, signed in bit 13), ByteSel, DstFmt.
        void I2f(uint op)
        {
            const uint known = 1u << 10 | 3u << 11 | 3u << 14 | 3u << 20;
            uint w2 = _i.W2 & ~(op == 0xd06 ? 0x08000000u : 0u);
            uint src = _i.Bits(84, 2), dst = _i.Bits(75, 2), sel = _i.Bits(60, 2);
            if ((w2 & ~known) != 0 || src == 3 || dst == 0 || (op != 0xb06 && (_i.W1 & 0xcfffff00u) != 0)) throw Fail("I2F format");
            int b = op == 0xb06 ? Plain(1, SB) : op == 0xd06 ? Ur(_i.Bits(32, 8)) : Gpr(_i.Bits(32, 8));
            uint bytesel = src == 0 ? sel : src == 1 ? sel * 2 : 0;
            EmitG(0x5cb8000000000000UL | ((ulong)dst << 8) | ((ulong)src << 10) | (_i.Bit(74) ? 1UL << 13 : 0) | ((ulong)b << 20)
                  | ((ulong)bytesel << 41) | ((ulong)_i.Bits(78, 2) << 39) | (uint)Gpr((uint)Dst));
        }

        // STL / LDL (local memory) at [Ra + offset 40-63], size 73-75 (U8 S8 U16 S16 32 64 128); 64 and 128 bits go as
        // 32-bit words. STL data at 32-39. With bit 91 the address adds a uniform register URb (STL 0x987, LDL 0x983 in
        // that form): a store's at 64-69, a load's at 32-37 (its B field; Ocarina's mesh programs write that register just
        // before the access). Ra + URb goes to a scratch register first, or URb's register is the address when Ra is RZ.
        void LocalAccess(bool store)
        {
            uint size = _i.Bits(73, 3), allowed = store ? 0xe00u : 0x200e00u;
            bool uniform = _i.Bit(91);
            uint w2 = uniform ? _i.W2 & ~(store ? 0x0800003fu : 0x08000000u) : _i.W2;
            uint bField = _i.Bits(32, 8) & (uniform && !store ? 0xc0u : 0xffu);
            if (size > 6 || (w2 & ~allowed) != 0x00100000 || (!store && bField != 0) || (store && _i.Bits(16, 8) != 0))
                throw Fail("local memory mode");
            int addr = Gpr(_i.Bits(24, 8)), value = store ? Gpr(_i.Bits(32, 8)) : Gpr((uint)Dst);
            if (uniform)
            {
                int ub = Ur(store ? _i.Bits(64, 6) : _i.Bits(32, 6));
                if (ub != RZ && addr == RZ) addr = ub;
                else if (ub != RZ) { Iadd3(SX, addr, ub, RZ, false); addr = SX; }
            }
            uint off = _i.Bits(40, 24), words = size == 6 ? 4u : size == 5 ? 2u : 1u;
            if (!store && words > 1 && addr != RZ && addr >= value && addr < value + words) { Mov(SX, addr, false); addr = SX; }
            for (uint k = 0; k < words; k++)
                EmitG((store ? 0xef50000000000000UL : 0xef40000000000000UL) | (uint)(value == RZ ? RZ : value + (int)k) | ((ulong)addr << 8)
                      | ((ulong)((off + 4 * k) & 0xffffff) << 20) | ((ulong)(size < 4 ? size : 4) << 48));
        }

        // IPA with the attribute indexed by a uniform register (bits 24-31): Maxwell IPA.IDX.PASS with the whole byte
        // address in Ra. As Ipa: the value times 1/w, which the program multiplies by w itself.
        void IpaIndexed()
        {
            if (_i.Bits(32, 8) != RZ || (_i.Bits(76, 4) != 0 && _i.Bits(76, 4) != 4) || _i.Bits(81, 3) != 7) throw Fail("indexed IPA mode");
            int idx = Ur(_i.Bits(24, 8)), d = Gpr((uint)Dst);
            uint attr = _i.Bits(64, 8);
            if (idx == RZ)
            {
                EmitG(0xe000000000000000UL | ((ulong)RZ << 8) | ((ulong)RZ << 20) | ((ulong)attr << 30) | ((ulong)_i.Bits(78, 1) << 55) | ((ulong)PT << 47) | (uint)d);
                return;
            }
            Emit(Unguarded(0x1c00000000000000UL | ((ulong)(attr * 4) << 20) | ((ulong)idx << 8) | SX));   // IADD32I
            EmitG(0xe000000000000000UL | ((ulong)SX << 8) | ((ulong)RZ << 20) | (1UL << 38) | ((ulong)_i.Bits(78, 1) << 55)
                  | ((ulong)PT << 47) | (uint)d);
        }

        // ULDC URd, c[bank][URa + offset]: Maxwell LDC with a register index; width 73-75 as LDC's.
        void UldcIndexed()
        {
            uint width = _i.Bits(73, 3);
            int d = Ur((uint)Dst), idx = Ur(_i.Bits(24, 8)), bank = Bank(_i.Bits(54, 5));
            if (width > 5 || (_i.W2 & ~0xe00u) != 0x08000000 || bank >= 18 || _i.Bits(62, 2) != 0) throw Fail("indexed ULDC mode");
            if (d == RZ) return;
            EmitG(0xef90000000000000UL | (uint)d | ((ulong)idx << 8) | ((ulong)_i.Bits(38, 16) << 20) | ((ulong)bank << 36) | ((ulong)width << 48));
        }

        // BRX Ra / BRXU URa with the program start as base (the offset makes the target Ra itself): a switch. The
        // compiler loads Ra from a jump table in c1 of native byte addresses, LDC Ra, c[0x1][Rx + base], after
        // clamping the index (IMNMX.U32 Ry, Ry, bound; SHL Rx, Ry, 2). The translator's BRX compares the register with
        // the lowered addresses, so it cannot take native ones; each table entry becomes a compare and branch instead:
        // IADD32I.CC RZ, Ra, -target; BRA CC.EQ to the target's lowered code, the last entry also unconditionally.
        void Brx(bool uniform)
        {
            if (_i.Bits(12, 4) != 7) throw Fail("predicated BRX");
            if ((_i.W2 & ~0x3ffffu) != (uniform ? 0x0b800000u : 0x03800000u)) throw Fail("BRX mode");
            long raw = (long)(_i.W1 | ((ulong)(_i.W2 & 0x3ffff) << 32));
            if ((long)_index * 16 + 16 + ((raw << 14) >> 14) != 0) throw Fail("BRX relative to a base other than the program start");
            uint src = _i.Bits(24, 8);
            var targets = BrxTargets(src, uniform);
            int reg = uniform ? Ur(src) : Gpr(src);
            foreach (uint t in targets)
            {
                Emit(Unguarded(0x1c00000000000000UL | (1UL << 52) | ((ulong)(uint)(-(int)t) << 20) | ((ulong)reg << 8) | RZ));
                _branches.Add((_out.Count, (int)(t / 16)));
                Emit(0xe240000000000000UL | ((ulong)PT << 16) | 2);   // BRA CC.EQ
            }
            _branches.Add((_out.Count, (int)(targets[^1] / 16)));
            Emit(0xe24000000000000fUL | ((ulong)PT << 16));
        }

        // The jump table behind a BRX register: the LDC (or, for BRXU, indexed ULDC) of bank 1 that last wrote it in
        // the straight-line code before the BRX, and its entry count from the IMNMX bound of the index feeding it.
        List<uint> BrxTargets(uint src, bool uniform)
        {
            if (C1 == null || C1.Length == 0) throw Fail("BRX without the program's c1 constants");
            int Writer(int from, uint reg, bool ur)
            {
                for (int k = from; k >= 0 && k >= from - 64; k--)
                {
                    var p = _code[k];
                    uint op = p.Bits(0, 12), b = op & 0x1ff;
                    // control flow: not straight-line. BSSY, BSYNC and WARPSYNC branch nowhere and write no register: the
                    // compiler puts a BSSY for the switch's reconvergence between the IMNMX bound and the SHL.
                    if (b is >= 0x140 and <= 0x15f && op is not 0x945 and not 0x941 and not 0x948) return -1;
                    // uniform datapath: ULDC, UMOV and the U* ALU ops (base 0x080-0x0bf; UISETP/UPLOP3 write predicates)
                    bool isUr = op is 0xab9 or 0xabb or 0x882 or 0xc82 || (b is >= 0x080 and < 0x0c0 && b != 0x08c && b != 0x09c);
                    if (p.Bits(16, 8) == reg && isUr == ur) return k;
                }
                return -1;
            }
            int ldc = Writer(_index - 1, src, uniform);
            if (ldc < 0) throw Fail("BRX register not loaded from a jump table");
            var l = _code[ldc];
            uint lop = l.Bits(0, 12);
            if (!(lop == 0xb82 && !uniform || lop == 0xabb && uniform) || l.Bits(54, 5) != 1)
                throw Fail($"BRX register loaded by op {lop:x3}, not from c1");
            uint tableBase = l.Bits(38, 16);
            // the bound: the index register of the load, written by SHL x, 2 from an IMNMX.U32 with an immediate
            int count = 0;
            int shl = Writer(ldc - 1, l.Bits(24, 8), uniform);
            if (shl >= 0)
            {
                var sh = _code[shl];
                int mn = Writer(shl - 1, sh.Bits(24, 8), uniform);
                // IMNMX / UIMNMX with an immediate bound, or SEL / USEL Rx, Rx, bound under the compare that clamps it
                // (the uniform switches: UISETP.GT P0, URx, bound; USEL URx, URx, bound, P0)
                uint mop = mn >= 0 ? _code[mn].Bits(0, 12) : 0, mb = mop & 0x7f;
                if (mn >= 0 && (mb == 0x17 || mb == 0x07) && _code[mn].Bits(9, 3) == 4) count = (int)_code[mn].W1 + 1;
                // the register form, the bound in a register a MOV (UMOV) of an immediate set before it: SEL Rx, Rx, Rb
                // under ISETP Rx, Rb, or IMNMX Rx, Rx, Rb; either source may be the bound, the other is the index
                else if (mn >= 0 && (mb == 0x17 || mb == 0x07) && _code[mn].Bits(9, 3) == 1)
                    foreach (uint r in new[] { _code[mn].Bits(32, 8), _code[mn].Bits(24, 8) })
                    {
                        int mv = r == RZ ? -1 : Writer(mn - 1, r, uniform);
                        if (mv >= 0 && _code[mv].Bits(0, 12) == (uniform ? 0x882u : 0x802u) && _code[mv].Bits(12, 4) == 7)
                        {
                            count = (int)_code[mv].W1 + 1;
                            break;
                        }
                    }
            }
            if (count <= 0 || count > 1024) throw Fail("BRX jump table without a bound");
            var t = new List<uint>();
            for (int k = 0; k < count; k++)
            {
                int at = (int)tableBase + 4 * k;
                if (at + 4 > C1.Length) throw Fail("BRX jump table outside c1");
                uint v = BitConverter.ToUInt32(C1, at);
                if (v % 16 != 0 || v / 16 > _code.Length) throw Fail($"BRX jump table entry 0x{v:x}");
                if (!t.Contains(v)) t.Add(v);
            }
            return t;
        }

        // CALL.REL.NOINC (relative, the offset as BRA's) pushes no return address: it is a jump. The compiler makes a
        // call of it by putting the address after the CALL in a register first (MOV Rn, imm, which the scheduler may
        // hoist dozens of instructions, within the straight-line code before the CALL)
        // for the callee's RET.REL.NODEC Rn; such a CALL becomes Maxwell CAL and its RET Maxwell RET (the translator
        // keeps the call stack, so the link register is not written). Without that MOV it is a plain, possibly
        // predicated, jump (`@P0 CALL.NOINC` over a BRA) and becomes BRA: as CAL it made functions that call each
        // other, a recursive call graph Vulkan forbids and NVIDIA's compiler inlines until its stack overflows.
        bool SetsReturnAddress()
        {
            uint ret = (uint)(_index + 1) * 16;
            for (int k = _index - 1; k >= 0 && k >= _index - 64; k--)
            {
                var p = _code[k];
                uint op = p.Bits(0, 12);
                if ((op & 0x1ff) >= 0x140 && (op & 0x1ff) <= 0x15f) break;   // control flow: the MOV would not reach the CALL
                if (op == 0x802 && p.W1 == ret && p.Bits(12, 4) == 7) return true;
            }
            return false;
        }

        void Call()
        {
            if (_i.W2 >> 18 != 0x03c00000 >> 18) throw Fail("CALL mode");
            long raw = (long)(_i.W1 | ((ulong)(_i.W2 & 0x3ffff) << 32));
            long target = (long)_index * 16 + 16 + ((raw << 14) >> 14);
            if (target < 0 || target % 16 != 0 || target / 16 > _code.Length) throw Fail("CALL target");
            if (!SetsReturnAddress())
            {
                _branches.Add((_out.Count, (int)(target / 16)));
                Emit(0xe24000000000000fUL | ((ulong)_i.Bits(12, 4) << 16));   // BRA under the CALL's own predicate
                return;
            }
            if (_i.Bits(12, 4) != 7)
            {
                // Maxwell CAL has no predicate: skip the call unless the guard holds
                _branches.Add((_out.Count, _index + 1));
                Emit(0xe24000000000000fUL | ((ulong)(_i.Bits(12, 4) ^ 8) << 16));
            }
            _branches.Add((_out.Count, (int)(target / 16)));
            // Predicate field 0, as Maxwell CAL encodes it (it has no predicate): the translator's Decoder reads a CAL whose
            // field says PT as an unconditional branch and decodes nothing after it (Decoder.IsUnconditionalBranch).
            Emit(0xe260000000000000UL);
        }

        void Ret()
        {
            if ((_i.W2 & ~0x3ffffu) != 0x03c00000) throw Fail("RET mode");
            EmitG(0xe32000000000000fUL);
        }

        // ---- control flow ------------------------------------------------------------------------------------------
        void Kill()
        {
            if (_i.W1 != 0 || _i.W2 != 0x03800000) throw Fail("KILL mode");
            EmitG(0xe33000000000000fUL);
        }

        // EXIT, with an optional second condition Pc in bits 87-90: exits when the guard and Pc hold.
        void Exit()
        {
            if (_i.Bits(84, 2) != 0) throw Fail("conditional/special EXIT");
            uint pc = _i.Bits(87, 4);
            if (pc == 7) { EmitG(0xe30000000000000fUL); return; }
            if (_i.Bits(12, 4) != 7)
            {
                _branches.Add((_out.Count, _index + 1));
                Emit(0xe24000000000000fUL | ((ulong)(_i.Bits(12, 4) ^ 8) << 16));   // skip unless the guard holds
            }
            Emit(0xe30000000000000fUL | ((ulong)pc << 16));
        }

        // BRA: byte offset from the next instruction in bits 34-81 (sign-extended), mode in bits 32-33 (.DIV 2: taken
        // only on a diverged warp, never here; .CONV 3: taken on a converged warp, always here), an extra condition
        // predicate in bits 87-90.
        void Bra()
        {
            uint mode = _i.Bits(32, 2);
            if (mode <= 1 && _i.Bit(91)) throw Fail("BRA uniform condition");
            if (mode == 2) return;
            long raw = (long)((_i.W1 & ~3u) | ((ulong)(_i.W2 & 0x3ffff) << 32));
            long rel = (raw << 14) >> 14;
            long target = (long)_index * 16 + 16 + rel;
            if (target < 0 || target % 16 != 0 || target / 16 > _code.Length) throw Fail("BRA target");
            ulong pred = _guard;
            uint extra = _i.Bits(87, 4);
            if (extra != 7)
            {
                if (_i.Bits(12, 4) != 7)
                {
                    // both conditions: skip the branch unless the guard holds
                    _branches.Add((_out.Count, _index + 1));
                    Emit(0xe24000000000000fUL | ((ulong)(_i.Bits(12, 4) ^ 8) << 16));
                }
                pred = (ulong)extra << 16;
            }
            _branches.Add((_out.Count, (int)(target / 16)));
            Emit(0xe24000000000000fUL | pred);
        }
    }

    // code: the NVN2 program as stored (graphics: 0x80-byte header then instructions; compute: instructions only), size its
    // byte length from the control section (+0x38). Returns the Maxwell program in the translator's input layout.
    // mesh: a task or mesh program (graphics false): its 0x80-byte header is read for the output map and dropped, and the
    // program is lowered as a compute program.
    public static byte[] Lower(byte[] code, int size, bool graphics, int[] table = null, MeshInfo mesh = null) =>
        Lower(code, size, graphics, out _, table, mesh);

    // c1: the program's c1 constants (BRX jump tables); set before Lower by the caller.
    [ThreadStatic] public static byte[] C1Bytes;

    // Diagnostics only (Program.cs LowerFile, the --lower mode): the register UR0 was mapped to by the last Lowerer
    // built on this thread (Lowerer.UrBase; SpillFrom in spill mode). Lowering never reads it.
    [ThreadStatic] public static int LastUrBase;

    // localBytes: the local memory size the lowered program needs when that changed (spill mode), else 0. Graphics
    // programs carry it in their header already; compute and task/mesh programs need it from the translator's host
    // (job "local_mem").
    // computeLocal: a compute program's own local memory in bytes, where its spill area starts (0: the translator's
    // compute default of 0x1000).
    public static byte[] Lower(byte[] code, int size, bool graphics, out int localBytes, int[] table = null, MeshInfo mesh = null,
                               int computeLocal = 0)
    {
        localBytes = 0;
        int header = graphics || mesh != null ? 0x80 : 0;
        if (size <= header || size > code.Length || (size - header) % 16 != 0)
            throw new LoweringException($"sm86 program size 0x{size:x} (file 0x{code.Length:x})");
        var insns = new Insn[(size - header) / 16];
        for (int k = 0; k < insns.Length; k++)
            insns[k] = new Insn { Lo = BitConverter.ToUInt64(code, header + k * 16), Hi = BitConverter.ToUInt64(code, header + k * 16 + 8) };
        byte[] sph = null;
        if (graphics && ((BitConverter.ToUInt32(code, 0) >> 10) & 15) == 5) { sph = new byte[0x50]; Buffer.BlockCopy(code, 0, sph, 0, 0x50); }
        if (mesh != null) { var h = new byte[0x80]; Buffer.BlockCopy(code, 0, h, 0, 0x80); mesh.Layout(h); }
        List<ulong> lowered;
        List<(int output, int targetSource)> branches;
        int[] first;
        int spillAt = -1;
        try { lowered = new Lowerer(insns, sph, !graphics, table, mesh) { C1 = C1Bytes }.Run(out branches, out first); }
        catch (RegisterPressureException)
        {
            // Local memory after the program's own: the header's sizes (translator: low + high + CRS / 32), or for a
            // compute program its own size (computeLocal), the translator's compute default (0x1000) when not given.
            spillAt = computeLocal > 0 ? computeLocal : 0x1000;
            if (header != 0)
                spillAt = ((int)(BitConverter.ToUInt32(code, 4) & 0xffffff) + (int)(BitConverter.ToUInt32(code, 8) & 0xffffff)
                           + (int)(BitConverter.ToUInt32(code, 12) & 0xffffff) / 32 + 15) & ~15;
            try
            {
                lowered = new Lowerer(insns, sph, !graphics, table, mesh, spillAt, Spill) { C1 = C1Bytes }.Run(out branches, out first);
                localBytes = spillAt + Spill.SpillBytes;
            }
            catch (UrPoolException)
            {
                lowered = new Lowerer(insns, sph, !graphics, table, mesh, spillAt, SpillWide) { C1 = C1Bytes }.Run(out branches, out first);
                localBytes = spillAt + SpillWide.SpillBytes;
            }
        }

        int start = graphics ? 0x50 : 0;
        var address = new int[lowered.Count + 1];
        int pos = start;
        for (int k = 0; k <= lowered.Count; k++)
        {
            if (((pos - start) & 0x1f) == 0) pos += 8;
            address[k] = pos;
            pos += 8;
        }
        foreach (var (output, targetSource) in branches)
        {
            long rel = address[first[targetSource]] - (address[output] + 8);
            if (rel < -0x800000 || rel > 0x7fffff) throw new LoweringException("sm86 lowered branch distance");
            lowered[output] |= ((ulong)rel & 0xffffff) << 20;
        }
        var result = new byte[pos + 16];
        if (graphics) Buffer.BlockCopy(code, 0, result, 0, 0x50);
        if (graphics && spillAt >= 0)   // the header's low local memory size covers the spill area too
            BitConverter.GetBytes((BitConverter.ToUInt32(result, 4) & 0xff000000u) | (uint)localBytes).CopyTo(result, 4);
        for (int k = 0; k < lowered.Count; k++) BitConverter.GetBytes(lowered[k]).CopyTo(result, address[k]);
        // a closing EXIT after the last instruction, so a branch to the end lands somewhere
        BitConverter.GetBytes(0xe30000000007000fUL).CopyTo(result, address[lowered.Count]);
        return result;
    }
}
