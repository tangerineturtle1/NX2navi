// Turns the compute SPIR-V the translator made from a lowered NVN2 task or mesh program (AmpereLowering.MeshInfo) into a
// TaskEXT or MeshEXT module (SPV_EXT_mesh_shader, VK_EXT_mesh_shader).
//
// The translator's main function is kept as it is and called from a new entry function:
//   mesh: [after a task stage: the task payload copied into the shared memory payload area, barrier] main; barrier;
//         SetMeshOutputsEXT(vertex limit, primitive count from shared memory); every thread copies vertices
//         i, i + threads, ... and primitives likewise from the shared memory stage buffer into the outputs: position
//         and point size built-ins, generic vector g at location g, the primitive's vertex indices.
//   task: main; barrier; the payload area copied out to the TaskPayloadWorkgroupEXT variable; EmitMeshTasksEXT with
//         the mesh workgroup count the program wrote.
// An EXIT in main returns to the entry function, so threads that leave early still reach the barrier. SPV_EXT_mesh_shader
// needs SPIR-V 1.4, where BufferBlock is gone: the translator's storage buffers (Uniform + BufferBlock, SPIR-V 1.3) move
// to the StorageBuffer storage class with Block, and the entry point lists every global variable.
//
// Opcode and enum numbers are the SPIR-V 1.6 specification's and SPV_EXT_mesh_shader's.
using System;
using System.Collections.Generic;
using System.Linq;

static class MeshSpirv
{
    // Generic vectors the mesh program writes (output locations).
    public static IEnumerable<int> GenericLocations(AmpereLowering.MeshInfo m)
    {
        var seen = new SortedSet<int>();
        foreach (int a in m.Lines) if (a >= 0x80) seen.Add((a - 0x80) / 16);
        return seen;
    }

    const int OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15,
        OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
        OpTypeVector = 23, OpTypeArray = 28, OpTypePointer = 32, OpTypeFunction = 33, OpConstant = 43,
        OpFunction = 54, OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62,
        OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67, OpInBoundsPtrAccessChain = 70,
        OpDecorate = 71, OpCompositeConstruct = 80, OpIAdd = 128, OpIMul = 132, OpULessThan = 176,
        OpShiftRightLogical = 194, OpBitwiseAnd = 199, OpBitcast = 124, OpControlBarrier = 224, OpPhi = 245,
        OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpReturn = 253,
        OpEmitMeshTasksEXT = 5294, OpSetMeshOutputsEXT = 5295;
    const int ScUniform = 2, ScOutput = 3, ScWorkgroup = 4, ScInput = 1, ScFunction = 7, ScStorageBuffer = 12, ScTaskPayload = 5402;
    const int DecBlock = 2, DecBufferBlock = 3, DecBuiltIn = 11, DecLocation = 30;
    const int BiPosition = 0, BiPointSize = 1, BiLocalInvocationIndex = 29,
        BiPrimitivePointIndices = 5294, BiPrimitiveLineIndices = 5295, BiPrimitiveTriangleIndices = 5296;
    const int CapMeshShading = 5283, ModelTask = 5364, ModelMesh = 5365, ModelGLCompute = 5;
    const int ModeOutputVertices = 26, ModeOutputPoints = 27, ModeOutputLines = 5269, ModeOutputPrimitives = 5270,
        ModeOutputTriangles = 5298;
    const int GlslUMin = 38;

    sealed class Insn
    {
        public int Op;
        public List<uint> W;   // operands, without the opcode word
        public Insn(int op, IEnumerable<uint> w) { Op = op; W = w.ToList(); }
    }

    static List<uint> Str(string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s + "\0");
        var w = new List<uint>();
        for (int i = 0; i < bytes.Length; i += 4)
        {
            uint v = 0;
            for (int k = 0; k < 4 && i + k < bytes.Length; k++) v |= (uint)bytes[i + k] << (8 * k);
            w.Add(v);
        }
        return w;
    }

    // The first function that reaches itself through OpFunctionCall, or -1. Vulkan SPIR-V forbids recursion
    // (VUID-StandaloneSpirv-None-04634); NVIDIA's compiler inlines such a cycle until its thread's stack overflows.
    public static long CallCycle(byte[] spirv)
    {
        var w = new uint[spirv.Length / 4];
        Buffer.BlockCopy(spirv, 0, w, 0, w.Length * 4);
        var calls = new Dictionary<uint, List<uint>>();
        uint fn = 0;
        for (int p = 5; p < w.Length;)
        {
            int n = (int)(w[p] >> 16), op = (int)(w[p] & 0xffff);
            if (n == 0) break;
            if (op == OpFunction) fn = w[p + 2];
            else if (op == OpFunctionCall) { if (!calls.TryGetValue(fn, out var l)) calls[fn] = l = new(); l.Add(w[p + 3]); }
            p += n;
        }
        var state = new Dictionary<uint, int>();   // 1 on the path, 2 done
        long found = -1;
        bool Visit(uint f)
        {
            state[f] = 1;
            if (calls.TryGetValue(f, out var l))
                foreach (uint g in l)
                {
                    if (state.TryGetValue(g, out int st) ? st == 1 : Visit(g)) { if (found < 0) found = g; return true; }
                }
            state[f] = 2;
            return false;
        }
        foreach (uint f in calls.Keys.ToList()) if (!state.ContainsKey(f) && Visit(f)) break;
        return found;
    }

    public static byte[] Convert(byte[] spirv, AmpereLowering.MeshInfo m)
    {
        if (spirv.Length < 20 || BitConverter.ToUInt32(spirv, 0) != 0x07230203) throw new Exception("mesh: not a SPIR-V module");
        var words = new uint[spirv.Length / 4];
        Buffer.BlockCopy(spirv, 0, words, 0, words.Length * 4);
        uint bound = words[3];
        var ins = new List<Insn>();
        for (int p = 5; p < words.Length;)
        {
            int n = (int)(words[p] >> 16), op = (int)(words[p] & 0xffff);
            if (n == 0 || p + n > words.Length) throw new Exception("mesh: malformed SPIR-V");
            ins.Add(new Insn(op, new ArraySegment<uint>(words, p + 1, n - 1)));
            p += n;
        }
        uint Id() => bound++;

        // ---- storage buffers: Uniform + BufferBlock -> StorageBuffer + Block
        var bufferBlocks = new HashSet<uint>();
        foreach (var i in ins)
            if (i.Op == OpDecorate && i.W[1] == DecBufferBlock) { bufferBlocks.Add(i.W[0]); i.W[1] = DecBlock; }
        var pointee = new Dictionary<uint, (uint sc, uint type)>();   // pointer type -> storage class, pointee
        var ssboPointerTypes = new HashSet<uint>();
        foreach (var i in ins)
            if (i.Op == OpTypePointer)
            {
                if (i.W[1] == ScUniform && bufferBlocks.Contains(i.W[2])) { i.W[1] = ScStorageBuffer; ssboPointerTypes.Add(i.W[0]); }
                pointee[i.W[0]] = (i.W[1], i.W[2]);
            }
        var ssboPointers = new HashSet<uint>();
        var newTypes = new List<Insn>();   // inserted before the first function
        var sbPointerOf = new Dictionary<uint, uint>();   // pointee -> StorageBuffer pointer type
        uint StoragePointer(uint type)
        {
            if (sbPointerOf.TryGetValue(type, out uint t)) return t;
            foreach (var (pt, (sc, pe)) in pointee) if (sc == ScStorageBuffer && pe == type) { sbPointerOf[type] = pt; return pt; }
            t = Id();
            newTypes.Add(new Insn(OpTypePointer, new[] { t, (uint)ScStorageBuffer, type }));
            pointee[t] = (ScStorageBuffer, type);
            sbPointerOf[type] = t;
            return t;
        }
        foreach (var i in ins)
        {
            if (i.Op == OpVariable && ssboPointerTypes.Contains(i.W[0])) { i.W[2] = ScStorageBuffer; ssboPointers.Add(i.W[1]); }
            else if (i.Op is OpAccessChain or OpInBoundsAccessChain or OpPtrAccessChain or OpInBoundsPtrAccessChain && ssboPointers.Contains(i.W[2]))
            {
                var (sc, pe) = pointee[i.W[0]];
                if (sc != ScStorageBuffer) i.W[0] = StoragePointer(pe);
                ssboPointers.Add(i.W[1]);
            }
        }

        // ---- existing types and ids
        uint Find(Func<Insn, bool> f) { var i = ins.FirstOrDefault(f); return i == null ? 0 : i.W[0]; }
        int firstFunction = ins.FindIndex(i => i.Op == OpFunction);
        var globals = new List<Insn>();   // new types, constants, variables, inserted before the first function
        uint Ty(int op, params uint[] operands)
        {
            uint t = Find(i => i.Op == op && i.W.Skip(1).SequenceEqual(operands));
            if (t == 0) t = newTypes.Concat(globals).FirstOrDefault(i => i.Op == op && i.W.Skip(1).SequenceEqual(operands))?.W[0] ?? 0;
            if (t != 0) return t;
            t = Id();
            globals.Add(new Insn(op, new[] { t }.Concat(operands)));
            return t;
        }
        uint tVoid = Ty(OpTypeVoid), tBool = Ty(OpTypeBool), tUint = Ty(OpTypeInt, 32, 0), tFloat = Ty(OpTypeFloat, 32);
        uint tFnVoid = Ty(OpTypeFunction, tVoid);
        var constants = new Dictionary<uint, uint>();
        uint C(uint v)
        {
            if (constants.TryGetValue(v, out uint id)) return id;
            id = Id();
            globals.Add(new Insn(OpConstant, new[] { tUint, id, v }));
            constants[v] = id;
            return id;
        }
        uint Ptr(int sc, uint type) { uint t = Id(); globals.Add(new Insn(OpTypePointer, new[] { t, (uint)sc, type })); return t; }
        uint Arr(uint elem, uint n) { uint t = Id(); globals.Add(new Insn(OpTypeArray, new[] { t, elem, C(n) })); return t; }
        var decorations = new List<Insn>();
        var interfaceVars = new List<uint>();
        uint Var(uint ptrType, int sc, int builtin = -1, int location = -1)
        {
            uint v = Id();
            globals.Add(new Insn(OpVariable, new[] { ptrType, v, (uint)sc }));
            if (builtin >= 0) decorations.Add(new Insn(OpDecorate, new[] { v, (uint)DecBuiltIn, (uint)builtin }));
            if (location >= 0) decorations.Add(new Insn(OpDecorate, new[] { v, (uint)DecLocation, (uint)location }));
            return v;
        }

        var shared = ins.Where(i => i.Op == OpVariable && i.W[2] == ScWorkgroup).ToList();
        if (shared.Count != 1) throw new Exception($"mesh: {shared.Count} workgroup variables (one shared memory array expected)");
        uint sharedVar = shared[0].W[1];
        uint pSharedUint = Ptr(ScWorkgroup, tUint);
        var entry = ins.Single(i => i.Op == OpEntryPoint);
        if (entry.W[0] != ModelGLCompute) throw new Exception("mesh: the translator's entry point is not a compute one");
        uint mainFn = entry.W[1];
        uint glsl = Find(i => i.Op == OpExtInstImport && Str("GLSL.std.450").SequenceEqual(i.W.Skip(1)));
        var preamble = new List<Insn>();   // capability, extension, import: inserted after the last capability
        if (glsl == 0) { glsl = Id(); preamble.Add(new Insn(OpExtInstImport, new[] { glsl }.Concat(Str("GLSL.std.450")))); }

        uint pInputUint = Ptr(ScInput, tUint);
        uint lii = Var(pInputUint, ScInput, BiLocalInvocationIndex);
        int payloadWords = m.PayloadBytes / 4;
        uint payloadVar = 0, pPayloadUint = 0;
        if (m.Stage == 6 || m.HasTask)
        {
            if (payloadWords == 0) throw new Exception("mesh: a task payload of size 0");
            payloadVar = Var(Ptr(ScTaskPayload, Arr(tUint, (uint)payloadWords)), ScTaskPayload);
            pPayloadUint = Ptr(ScTaskPayload, tUint);
        }

        // ---- the entry function body
        var body = new List<Insn>();
        uint Emit(int op, uint type, params uint[] operands) { uint r = Id(); body.Add(new Insn(op, new[] { type, r }.Concat(operands))); return r; }
        void Do(int op, params uint[] operands) => body.Add(new Insn(op, operands));
        uint Label() { uint l = Id(); Do(OpLabel, l); return l; }
        uint Add(uint a, uint b) => Emit(OpIAdd, tUint, a, b);
        uint SharedWord(uint index) => Emit(OpLoad, tUint, Emit(OpAccessChain, pSharedUint, sharedVar, index));
        void Barrier() => Do(OpControlBarrier, C(2), C(2), C(0x108));   // Workgroup, Workgroup, AcquireRelease | WorkgroupMemory
        uint UMin(uint a, uint b) => Emit(OpExtInst, tUint, glsl, GlslUMin, a, b);
        // if (a < b) { then() }
        void If(uint a, uint b, Action then)
        {
            uint c = Emit(OpULessThan, tBool, a, b), t = Id(), merge = Id();
            Do(OpSelectionMerge, merge, 0);
            Do(OpBranchConditional, c, t, merge);
            Do(OpLabel, t);
            then();
            Do(OpBranch, merge);
            Do(OpLabel, merge);
        }
        // Words i, i + threads, ... below words of shared memory from word sharedAt <-> the payload variable.
        void CopyPayload(uint tid, bool toShared)
        {
            int threads = Math.Max(1, m.Threads);
            for (int k = 0; k * threads < payloadWords; k++)
            {
                uint idx = k == 0 ? tid : Add(tid, C((uint)(k * threads)));
                void copy()
                {
                    uint pp = Emit(OpAccessChain, pPayloadUint, payloadVar, idx);
                    uint sp = Emit(OpAccessChain, pSharedUint, sharedVar, Add(idx, C((uint)(m.Pay / 4))));
                    if (toShared) Do(OpStore, sp, Emit(OpLoad, tUint, pp));
                    else Do(OpStore, pp, Emit(OpLoad, tUint, sp));
                }
                if ((k + 1) * threads > payloadWords) If(idx, C((uint)payloadWords), copy);
                else copy();
            }
        }

        // Shared memory words [0, n) set to 0, word i by thread i mod threads: a loop, as a large stage buffer would
        // otherwise unroll into hundreds of stores. `from` is the label of the block the loop is entered from.
        void ZeroShared(uint from, uint tid, uint n)
        {
            uint header = Id(), loopBody = Id(), cont = Id(), merge = Id(), i = Id(), next = Id();
            Do(OpBranch, header);
            Do(OpLabel, header);
            body.Add(new Insn(OpPhi, new[] { tUint, i, tid, from, next, cont }));
            uint more = Emit(OpULessThan, tBool, i, C(n));
            Do(OpLoopMerge, merge, cont, 0);   // the merge instruction comes right before the block's branch
            Do(OpBranchConditional, more, loopBody, merge);
            Do(OpLabel, loopBody);
            Do(OpStore, Emit(OpAccessChain, pSharedUint, sharedVar, i), C(0));
            Do(OpBranch, cont);
            Do(OpLabel, cont);
            body.Add(new Insn(OpIAdd, new[] { tUint, next, i, C((uint)Math.Max(1, m.Threads)) }));
            Do(OpBranch, header);
            Do(OpLabel, merge);
        }

        uint entryFn = Id();
        body.Add(new Insn(OpFunction, new[] { tVoid, entryFn, 0u, tFnVoid }));
        uint entryLabel = Label();
        uint tid = Emit(OpLoad, tUint, lii);
        if (m.Stage == 6)
        {
            // The mesh count starts at 0: a task that culls everything never writes it, and the word then held what the
            // workgroup's shared memory last had, up to tens of thousands of mesh groups and a lost device.
            If(tid, C(1), () => Do(OpStore, Emit(OpAccessChain, pSharedUint, sharedVar, C((uint)(m.Pay + 4) / 4)), C(0)));
        }
        else
        {
            // The whole stage buffer below the payload starts at 0: the vertex lines, the primitive count and the vertex
            // indices. Vulkan leaves workgroup memory undefined, and a mesh program need not write every vertex it has
            // room for, nor every index byte, nor its count when it emits nothing; the copy below reads all of them, so
            // whatever an earlier workgroup left there became vertices and primitives (on AMD, long thin triangles
            // across the foliage's shadow map). Now an unwritten vertex reads as zeros and an unwritten index as vertex 0.
            ZeroShared(entryLabel, tid, (uint)m.Pay / 4);
            if (m.HasTask) CopyPayload(tid, true);
        }
        Barrier();
        Emit(OpFunctionCall, tVoid, mainFn);
        Barrier();
        var modes = new List<Insn>();
        if (m.Stage == 6)
        {
            // one mesh group per task thread at most: a garbage count cannot launch more than the task could fill
            uint count = UMin(SharedWord(C((uint)(m.Pay + 4) / 4)), C((uint)Math.Max(1, m.Threads)));
            CopyPayload(tid, false);
            Do(OpEmitMeshTasksEXT, count, C(1), C(1), payloadVar);
        }
        else
        {
            int maxV = m.MaxVertices, maxP = m.MaxPrimitives, ipp = m.IndicesPerPrimitive, threads = Math.Max(1, m.Threads);
            if (maxV <= 0 || maxP <= 0) throw new Exception("mesh: no vertex or primitive limit");
            // outputs
            uint tV4 = Ty(OpTypeVector, tFloat, 4);
            uint pOutFloat = Ptr(ScOutput, tFloat);
            uint fZero = Id(), fOne = Id();
            globals.Add(new Insn(OpConstant, new[] { tFloat, fZero, 0u }));
            globals.Add(new Insn(OpConstant, new[] { tFloat, fOne, 0x3f800000u }));
            uint posVar = 0, sizeVar = 0;
            var generic = new Dictionary<int, uint>();
            foreach (int a in m.Lines)
            {
                if (a >= 0x70 && a < 0x80 && posVar == 0) posVar = Var(Ptr(ScOutput, Arr(tV4, (uint)maxV)), ScOutput, BiPosition);
                else if (a == 0x6c && sizeVar == 0) sizeVar = Var(Ptr(ScOutput, Arr(tFloat, (uint)maxV)), ScOutput, BiPointSize);
                else if (a >= 0x80 && !generic.ContainsKey((a - 0x80) / 16))
                    generic[(a - 0x80) / 16] = Var(Ptr(ScOutput, Arr(tV4, (uint)maxV)), ScOutput, -1, (a - 0x80) / 16);
            }
            uint tIdx = ipp == 1 ? tUint : Ty(OpTypeVector, tUint, (uint)ipp);
            int idxBuiltin = ipp == 1 ? BiPrimitivePointIndices : ipp == 2 ? BiPrimitiveLineIndices : BiPrimitiveTriangleIndices;
            uint idxVar = Var(Ptr(ScOutput, Arr(tIdx, (uint)maxP)), ScOutput, idxBuiltin);
            uint pOutIdx = Ptr(ScOutput, tIdx);

            uint count = UMin(SharedWord(C((uint)m.Map / 4)), C((uint)maxP));
            Do(OpSetMeshOutputsEXT, C((uint)maxV), count);
            int lines = m.Lines.Count;
            for (int k = 0; k * threads < maxV; k++)
            {
                uint v = k == 0 ? tid : Add(tid, C((uint)(k * threads)));
                If(v, C((uint)maxV), () =>
                {
                    // word ((v >> 5) * lines + line) * 32 + (v & 31) of the vertex area
                    uint group = Emit(OpShiftRightLogical, tUint, v, C(5)), lane = Emit(OpBitwiseAnd, tUint, v, C(31));
                    uint at = Add(Add(Emit(OpIMul, tUint, group, C((uint)(lines * 32))), lane), C((uint)m.Attr / 4));
                    for (int l = 0; l < lines; l++)
                    {
                        int a = m.Lines[l];
                        uint var = 0, comp = uint.MaxValue;
                        if (a >= 0x70 && a < 0x80) (var, comp) = (posVar, (uint)(a - 0x70) / 4);
                        else if (a == 0x6c) var = sizeVar;
                        else if (a >= 0x80) (var, comp) = (generic[(a - 0x80) / 16], (uint)((a - 0x80) % 16) / 4);
                        if (var == 0) continue;   // layer, viewport index: per primitive in Vulkan, not written
                        uint value = Emit(OpBitcast, tFloat, SharedWord(Add(at, C((uint)(l * 32)))));
                        uint ptr = comp == uint.MaxValue ? Emit(OpAccessChain, pOutFloat, var, v) : Emit(OpAccessChain, pOutFloat, var, v, C(comp));
                        Do(OpStore, ptr, value);
                    }
                    // A component of an output vector that the program's output map leaves out reads as Maxwell gives an
                    // unwritten one, (0, 0, 0, 1), instead of whatever the output held.
                    void Defaults(uint var, int first)
                    {
                        for (int k = 0; k < 4; k++)
                            if (!m.Lines.Contains(first + 4 * k))
                                Do(OpStore, Emit(OpAccessChain, pOutFloat, var, v, C((uint)k)), k == 3 ? fOne : fZero);
                    }
                    if (posVar != 0) Defaults(posVar, 0x70);
                    foreach (var (g, var) in generic) Defaults(var, 0x80 + 16 * g);
                });
            }
            for (int k = 0; k * threads < maxP; k++)
            {
                uint p = k == 0 ? tid : Add(tid, C((uint)(k * threads)));
                If(p, count, () =>
                {
                    // byte 4 + ipp * p + j of the primitive area
                    uint first = Add(Emit(OpIMul, tUint, p, C((uint)ipp)), C((uint)m.Map + 4));
                    var idx = new uint[ipp];
                    for (int j = 0; j < ipp; j++)
                    {
                        uint b = j == 0 ? first : Add(first, C((uint)j));
                        uint word = SharedWord(Emit(OpShiftRightLogical, tUint, b, C(2)));
                        uint shift = Emit(OpIMul, tUint, Emit(OpBitwiseAnd, tUint, b, C(3)), C(8));
                        uint value = Emit(OpBitwiseAnd, tUint, Emit(OpShiftRightLogical, tUint, word, shift), C(0xff));
                        idx[j] = UMin(value, C((uint)maxV - 1));
                    }
                    uint composite = ipp == 1 ? idx[0] : Emit(OpCompositeConstruct, tIdx, idx);
                    Do(OpStore, Emit(OpAccessChain, pOutIdx, idxVar, p), composite);
                });
            }
            Do(OpReturn);
            modes.Add(new Insn(OpExecutionMode, new[] { entryFn, (uint)ModeOutputVertices, (uint)maxV }));
            modes.Add(new Insn(OpExecutionMode, new[] { entryFn, (uint)ModeOutputPrimitives, (uint)maxP }));
            modes.Add(new Insn(OpExecutionMode, new[] { entryFn, (uint)(ipp == 1 ? ModeOutputPoints : ipp == 2 ? ModeOutputLines : ModeOutputTriangles) }));
        }
        body.Add(new Insn(OpFunctionEnd, Array.Empty<uint>()));

        // ---- reassemble
        foreach (var i in ins.Where(i => i.Op == OpExecutionMode && i.W[0] == mainFn)) i.W[0] = entryFn;
        preamble.Insert(0, new Insn(OpExtension, Str("SPV_EXT_mesh_shader")));
        preamble.Insert(0, new Insn(OpCapability, new[] { (uint)CapMeshShading }));
        int lastCap = ins.FindLastIndex(i => i.Op == OpCapability);
        // the entry point: the new model and function, every global variable as its interface (SPIR-V 1.4)
        var name = Str("main");
        var globalVars = ins.Take(firstFunction).Where(i => i.Op == OpVariable && i.W[2] != ScFunction).Select(i => i.W[1])
                            .Concat(globals.Where(i => i.Op == OpVariable).Select(i => i.W[1])).ToList();
        entry.W = new List<uint> { (uint)(m.Stage == 6 ? ModelTask : ModelMesh), entryFn }.Concat(name).Concat(globalVars).ToList();
        int lastMode = ins.FindLastIndex(i => i.Op == OpExecutionMode || i.Op == OpEntryPoint);
        int firstType = ins.FindIndex(i => i.Op >= OpTypeVoid && i.Op <= 39 || i.Op == OpConstant || i.Op == OpVariable);
        var result = new List<Insn>();
        for (int k = 0; k < ins.Count; k++)
        {
            if (k == firstType) result.AddRange(decorations);
            if (k == firstFunction) { result.AddRange(newTypes); result.AddRange(globals); }
            result.Add(ins[k]);
            if (k == lastCap) result.AddRange(preamble);
            if (k == lastMode) result.AddRange(modes);
        }
        result.AddRange(body);

        var output = new List<uint> { 0x07230203, 0x00010400, words[2], bound, 0 };
        foreach (var i in result) { output.Add((uint)((i.W.Count + 1) << 16 | i.Op)); output.AddRange(i.W); }
        var bytes = new byte[output.Count * 4];
        Buffer.BlockCopy(output.ToArray(), 0, bytes, 0, bytes.Length);
        return bytes;
    }
}
