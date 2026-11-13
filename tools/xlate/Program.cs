// NX2NAVI runtime shader translator front end. Copyright (c) 2026 jhaizhou (MIT; see THIRD_PARTY_NOTICES.md).
// SPIR-V for Vulkan is the only output (no MSL, no vertex-counter lowering, no near-is-minus-w rewrite); depth clip control
// is reported as supported and non-constant texture offsets as unsupported.
// Protocol (stdout carries replies only; diagnostics go to stderr):
//   nx2navi_xlate --serve              one request per stdin line: {"job": {...}, "out": "<dir>"}; one reply line per request:
//                                   "ok <key>" or "fail <key> <message>"; a malformed line gets "fail ? <message>" and the loop continues.
//   nx2navi_xlate <jobs.json> <outdir> batch mode: a JSON array of jobs, prints a summary, exit 1 if any failed.
//   nx2navi_xlate --lower <file>       lowers SM86 instructions given as hex words and prints the Maxwell words (LowerFile).
//   job: { "key", "stage": "vs|ps|cs", "code": path (graphics = 0x50-byte SPH + instructions, compute = instructions), "c1": hex,
//          "sm86": bool + "size": bytes (an NVN2 program, lowered by AmpereLowering.cs), "nvn2_table": [start, textures,
//          images] (NVN2 compute binding table), "lowered_out": path, "mesh": {"stage": 6|7, "threads", "maxv", "maxp",
//          "topo", "payload", "task"} (stage "ts" / "ms": an NVN2 task or mesh program, AmpereLowering.MeshInfo),
//          "tex": {slot: type}, "img": {slot: {type, fmt}}, "attr": {loc: float|sint|uint}, "fout": {idx: float|sint|uint},
//          "local": [x, y, z], "shared": bytes, "debug": bool, "reduced": bool, "softfloat64": bool, "zero_locals": bool,
//          "sbuf_align": int, "subgroup": int }
//   Output: <out>/<key>.spv and <out>/<key>.res.json (bindings, queried slots, notes).
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class SwshXlate
{
    const int TexBase = 0x168, SampBase = 0x568;   // NVN 分离贴图 / 采样器句柄在 c[2] 的字节基址（每槽 8 字节；dm/gfx/tools/maxwell_refs.py 全量实测） | byte base of NVN separate texture / sampler handles in c[2] (8 bytes per slot; full measurement in dm/gfx/tools/maxwell_refs.py)

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1; public readonly List<string> Notes = new(); public readonly SortedSet<int> TexQueried = new(), ImgQueried = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); }
        // c1 常数段（着色器包控制段前 0x100 字节）：间接跳转 BRX 的跳转表在这里（LDC c[1]）；不提供时翻译器解不出跳转目标、控制流出错（粒子着色器死循环）
        // c1 constant segment (the 0x100 bytes before the control segment in the shader package): the jump table of indirect jump BRX lives here (LDC c[1]); without it the translator cannot resolve jump targets and control flow goes wrong (particle shaders loop forever)
        // The translator's own warnings (a global memory access it could not map to a storage buffer, an invalid
        // sampler type, ...) go to res.json "notes" with ours; IGpuAccessor's default dropped them.
        public void Log(string message) => Notes.Add(message);
        public uint ConstantBuffer1Read(int offset) { if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset); Notes.Add($"c1 读越界 +{offset:X}（c1 长 {_c1.Length}）"); return 0; }
        // 全部资源放进第 0 组，各占不同绑定号（spirv-cross 出成一个参数缓冲）；常量段号 index 的缓冲由翻译器给 binding = index+1 的惯例不依赖
        // All resources go into set 0, each with a distinct binding number (spirv-cross emits them as one argument buffer); the translator's convention of giving the buffer of constant segment number index binding = index+1 is not relied on
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        // A pool array (bindless) takes one binding number: the runtime moves it to its own set (vk_pipeline.c
        // spirv_pool_rebind), and 32768 skipped numbers pushed the bindings after it past the runtime's 16-bit field.
        static bool PoolArray(int count) => count == TexturePoolLength || count == SamplerPoolLength;
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += PoolArray(count) ? 1 : count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += PoolArray(count) ? 1 : count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        // Bindless handles loaded from memory index the whole texture and sampler pools (the translator's BindlessElimination
        // GenerateBindlessAccess): arrays of TexturePoolLength textures (handle bits 0-19) and SamplerPoolLength samplers
        // (bits 20-31; NVN sampler ids are below 4096). res.json marks them "pool": 1 (textures) / 2 (samplers); the
        // runtime binds them from its global pool set (vk_exec_draw.c, texture and sampler pools).
        // TexturePoolLength: the runtime's VK_POOL_TEX (vk.h), past Ocarina's pool of 33068 descriptors.
        public const int TexturePoolLength = 40960, SamplerPoolLength = 4096;
        public int QuerySamplerArrayLengthFromPool() => SamplerPoolLength;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => TexturePoolLength;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) TexQueried.Add(slot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (t == null) { Notes.Add($"贴图句柄 {handle:X}（常量段 {cbufSlot}，槽 {slot}）无 PC 类型，按 2D"); return SamplerType.Texture2D; }
            if (t.EndsWith("_depth")) t = t.Substring(0, t.Length - 6);   // depth-format texture: same sampler type here; the host binds a depth view
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        // Image handles: NVN1 c[2] 0x120 + 8 * slot (NVN2 compute handles are renumbered there by AmpereLowering.Handle);
        // TexSlot knows only the separate texture range, so image formats were never answered (an R32UI view of an
        // R11G11B10F target was declared float, and the raw words the shader packs were converted on store).
        int ImgSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= 0x120 && bytes < TexBase && (bytes - 0x120) % 8 == 0 ? (bytes - 0x120) / 8 : -1;
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = ImgSlot(handle, cbufSlot);
            if (slot < 0) slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) ImgQueried.Add(slot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"图像句柄 {handle:X} 槽 {slot} 无 PC 格式，按 R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        // "local_mem": local memory bytes of a compute program (the lowering's spill area adds to it); else the translator's 0x1000.
        public int QueryComputeLocalMemorySize() => _job["local_mem"] is JsonNode n ? (int)n : 0x1000;
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;   // 配对 PC 的 groupshared 声明之和 | sum of the paired PC shader's groupshared declarations
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            // Vulkan: VK_EXT_depth_clip_control handles NEAR_IS_MINUS_W, so the shader is never rewritten ("depth_minus_w" in a job is ignored).
            const bool depthMinusW = false;
            // "early_fragment_tests": the fragment shader's NVN control byte +0x718 (the driver's early-Z-force, the
            // translator's EarlyZForce); the SPIR-V then declares EarlyFragmentTests.
            bool earlyZ = _job["early_fragment_tests"] is JsonValue ez && ez.TryGetValue<bool>(out bool eb) && eb;
            return new GpuGraphicsState(earlyZ, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, depthMinusW, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* 不做 Y 取反 | no Y flip */, true /* 原点左上 | origin upper-left */, false);
        }
        public bool QueryHostSupportsDepthClipControl() => true;
        // Vulkan: the ConstOffset / ConstOffsets image operands the translator emits must be constants. A TLD4.PTP whose offsets
        // come from registers (agl_technique_pfx compute b940aa86, dfe7662f) otherwise becomes an OpConstantComposite over
        // computed ids, which spirv-val rejects; false makes TexturePass emulate only non-constant offsets.
        public bool QueryHostSupportsNonConstantTextureOffset() => false;
        // "softfloat64": true lowers FP64 operations to FP32 (the translator's DoubleToFloat pass), for a host without shaderFloat64.
        public bool QueryHostSupportsShaderFloat64() => _job["softfloat64"] is not JsonValue v || !(v.TryGetValue<bool>(out bool b) && b);
        // The runtime binds the device address table (runtime/vk_gpa.c) when the device has buffer device addresses:
        // global accesses the translator finds no storage buffer for go through it (patch 0030).
        public bool QueryHostSupportsBufferDeviceAddress() => _job["bda"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        public bool QueryHostSupportsTextureShadowLod() => true;
        // Sub-texel precision bits of a host whose gathers round differently from Maxwell (AMD, Intel): the translator then
        // biases gather coordinates so the same texels are picked (TexturePass.InsertCoordGatherBias). 0: no bias.
        public int QueryHostGatherBiasPrecision() => _job["gather_bias"] is JsonNode n ? (int)n : 0;
        // 作业 reduced = true：放宽精度——翻译器只给强制精确的运算打 NoContraction（缺省全部打：保持 Maxwell 分开舍入的逐位结果，代价是 MSL 里大量禁优化的 spvFMul 等）
        // Job reduced = true: relaxed precision - the translator marks NoContraction only on operations that must be exact (the default marks all: keeps the bit-exact results of Maxwell's separate rounding, at the cost of many optimization-disabled spvFMul etc. in the MSL)
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // Host answers for GPUs other than the RX 9070 (runtime/vk_caps.c); absent from the job they keep the translator's defaults.
        // "sbuf_align": n: the host binds each storage buffer at its guest address rounded down to n (minStorageBufferOffsetAlignment
        // above 4), so storage offsets count from there (the translator's unaligned path; the multi-target path uses n too).
        public bool QueryHasUnalignedStorageBuffer() => _job["sbuf_align"] is JsonNode;
        public int QueryHostStorageBufferOffsetAlignment() => _job["sbuf_align"] is JsonNode n ? (int)n : 16;
        // "subgroup": n: lanes per subgroup when the host cannot pin 32 (AMD GCN/Vega run 64): ballots and votes span n lanes.
        public int QueryHostSubgroupSize() => _job["subgroup"] is JsonNode n ? (int)n : 32;
        // "mvprev": true: the vertex shader also writes last frame's position, computed from slots bank + 15 in place of
        // the user banks c3-c16 (patch 0015, runtime/vk_mvprev.c); res.json's "mvprev" names the output location (-1:
        // none) and "mvprev_banks" the banks the position depends on (bit n: cn).
        public bool QueryMotionPrevious() => _job["mvprev"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        // "zero_locals": true: function-local variables start at 0 (patch 0016, runtime/vk_caps.c decides per driver).
        public bool QueryZeroLocals() => _job["zero_locals"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
        public int MotionPreviousLocation = -1, MotionPreviousBanks;
        public void ReportMotionPrevious(int location, int banks) => (MotionPreviousLocation, MotionPreviousBanks) = (location, banks);
    }


    internal static string Translate(JsonObject job, string outDir)   // also called by the --service mode (Service.cs)
    {
        string key = (string)job["key"];
            try
            {
                byte[] code = File.ReadAllBytes((string)job["code"]);
                AmpereLowering.MeshInfo mesh = null;
                // "sm86": true: the code is an NVN2 (Switch 2) program as stored, "size" bytes long (control section +0x38);
                // it is lowered to Maxwell first (AmpereLowering.cs). "lowered_out": a path that receives the Maxwell code.
                if (job["sm86"] is JsonValue sm && sm.TryGetValue<bool>(out bool isSm86) && isSm86)
                {
                    int size = job["size"] is JsonNode sz ? (int)sz : code.Length;
                    AmpereLowering.C1Bytes = job["c1"] is JsonNode c1h ? Convert.FromHexString((string)c1h) : null;
                    int[] table = job["nvn2_table"] is JsonArray t && t.Count == 3 ? new[] { (int)t[0], (int)t[1], (int)t[2] } : null;
                    if (job["mesh"] is JsonObject mj)
                    {
                        // A task or mesh program: lowered and translated as a compute program whose shared memory holds
                        // the stage buffer, then rewritten into a TaskEXT / MeshEXT module (MeshSpirv.cs).
                        mesh = new AmpereLowering.MeshInfo
                        {
                            Stage = (int)mj["stage"], Threads = (int)mj["threads"], MaxVertices = (int)mj["maxv"],
                            MaxPrimitives = (int)mj["maxp"], Topology = (int)mj["topo"], PayloadBytes = (int)mj["payload"],
                            HasTask = mj["task"] is JsonValue tv && tv.TryGetValue<bool>(out bool tb) && tb,
                        };
                        // The size field counts 256-byte units, and some task programs and the mesh programs they feed
                        // carry 0 there while the payload is used (the task writes the mesh count at +4, the mesh reads
                        // ISBERD region 7 at 0x10 and 0x14): a payload under 256 bytes rounds down. One unit covers it.
                        if ((mesh.Stage == 6 || mesh.HasTask) && mesh.PayloadBytes == 0) mesh.PayloadBytes = 256;
                        // The size field counts the program's own data, which starts after the buffer's header (the mesh
                        // count at +4, the words at 0x10 and 0x14): an Ocarina foliage task with a size of 3072 writes
                        // three 1024-byte arrays from 0x18, up to byte 3096. A payload cut at 3072 bytes lost the last
                        // entries of the third array, and its mesh programs read past the end of their buffer there, a
                        // different value on every run: a few leaves of each dense patch moved between the depth pre-pass
                        // and the colour pass and left white specks. The header comes on top, rounded to 16 bytes.
                        if (mesh.Stage == 6 || mesh.HasTask) mesh.PayloadBytes += 0x20;
                        code = AmpereLowering.Lower(code, size, false, out int meshLocal, null, mesh);
                        if (meshLocal != 0) job["local_mem"] = meshLocal;
                        job["local"] = new JsonArray(mesh.Threads, 1, 1);
                        job["shared"] = mesh.SharedBytes;
                    }
                    else
                    {
                        // A compute program's own local memory: "local_mem" from its control section, at least the
                        // translator's default of 0x1000 bytes; the lowering's spill area goes after it.
                        bool cs = (string)job["stage"] == "cs";
                        int own = cs ? (Math.Max(job["local_mem"] is JsonNode lm ? (int)lm : 0, 0x1000) + 15) & ~15 : 0;
                        code = AmpereLowering.Lower(code, size, !cs, out int local, table, null, own);
                        if (local != 0) job["local_mem"] = local;
                        else if (cs) job["local_mem"] = own;
                    }
                    if (job["lowered_out"] is JsonNode lo) File.WriteAllBytes((string)lo, code);
                }
                var acc = new Accessor(code, job);
                // 作业 debug = false：关 DebugMode（DebugMode 会跳过 AST 优化器，产物满是临时变量；不改语义）
                // Job debug = false: turn off DebugMode (DebugMode skips the AST optimizer and the output is full of temporaries; semantics unchanged)
                var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] is "cs" or "ts" or "ms" ? TranslationFlags.Compute : 0);
                var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
                ShaderProgram p = ctx.Translate();
                // "glsl_out": a path that receives the same program as GLSL, for reading (diagnosis only).
                if (job["glsl_out"] is JsonNode go)
                {
                    var gctx = Translator.CreateContext(0, new Accessor(code, job), new TranslationOptions(TargetLanguage.Glsl, TargetApi.OpenGL, flags));
                    File.WriteAllText((string)go, gctx.Translate().Code);
                }
                byte[] spirv = p.BinaryCode;
                // A recursive call graph (subroutines the translator could not tell apart) is refused: Vulkan forbids it
                // and NVIDIA's compiler overflows its stack on it (MeshSpirv.CallCycle).
                if (MeshSpirv.CallCycle(spirv) is long cyc && cyc >= 0) throw new Exception($"recursive call graph (function %{cyc})");
                if (mesh != null)
                {
                    if (job["compute_out"] is JsonNode co) File.WriteAllBytes((string)co, spirv);
                    spirv = MeshSpirv.Convert(spirv, mesh);
                }
                File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), spirv);
                var res = new JsonObject
                {
                    ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                    ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                    ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                    ["tex_queried"] = new JsonArray(acc.TexQueried.Select(x => (JsonNode)x).ToArray()), ["img_queried"] = new JsonArray(acc.ImgQueried.Select(x => (JsonNode)x).ToArray()),
                };
                if (acc.QueryMotionPrevious()) (res["mvprev"], res["mvprev_banks"]) = (acc.MotionPreviousLocation, acc.MotionPreviousBanks);
                if (mesh != null)
                    res["mesh"] = new JsonObject { ["lines"] = mesh.Lines.Count, ["shared"] = mesh.SharedBytes,
                        ["outputs"] = new JsonArray(MeshSpirv.GenericLocations(mesh).Select(x => (JsonNode)x).ToArray()) };
                foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
                foreach (var b in p.Info.SBuffers)
                {
                    // A buffer whose address is a pointer stored in another storage buffer (patch 0022, GlobalToStorage
                    // PointerSlotFlag): cb_slot 0x20 | the slot of the outer buffer's address, cb_offset its word offset in
                    // bits 0-9 and the pointer's byte offset in the outer buffer / 8 in bits 10-15. Written as the outer
                    // buffer's cb_slot / cb_offset plus "ptr", the byte offset.
                    bool pointer = (b.SbCbSlot & 0x20) != 0;
                    var o = new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = pointer ? b.SbCbSlot & 0x1f : b.SbCbSlot,
                        ["cb_offset"] = pointer ? b.SbCbOffset & 0x3ff : b.SbCbOffset, ["flags"] = (int)b.Flags };
                    if (pointer) o["ptr"] = (b.SbCbOffset >> 10) * 8;
                    if (b.SbCbSlot == 0x40) o["table"] = true;   // the device address table (patch 0030, ResourceManager.DeviceAddressTableCbSlot)
                    res["sbufs"].AsArray().Add((JsonNode)o);
                }
                foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                    foreach (var t in list) res[name].AsArray().Add((JsonNode)new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags,
                        ["pool"] = t.ArrayLength == Accessor.TexturePoolLength ? 1 : t.ArrayLength == Accessor.SamplerPoolLength ? 2 : 0 });
                File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                return null;
            }
            catch (Exception e) { if (Environment.GetEnvironmentVariable("DMSH_STACK") != null) Console.Error.WriteLine(e.StackTrace); return $"{e.GetType().Name}: {e.Message.Split('\n')[0]}"; }
    }

    // --lower <file>: runs AmpereLowering on a few SM86 instructions, for executing single instructions on the CPU
    // (tests/maxwell_warp_emu.py). Each line of the file is one instruction as four hex 32-bit words "w0 w1 w2 w3"
    // (w0 = bits 0-31, optional 0x prefix; blank lines and '#' comments skipped). They are lowered as a compute program
    // (AmpereLowering.Lower with graphics false), with an SM86 EXIT appended unless the last instruction is an
    // unpredicated EXIT. Prints the lowered Maxwell instruction words in program order, one 16-digit hex word per line:
    // the scheduling words Lower puts first in every 32 bytes are left out, the closing EXIT Lower appends is kept. Then
    // "urbase N" (the register UR0 maps to, AmpereLowering.LastUrBase) and, in spill mode only, "local N" (the local
    // memory bytes the program needs). Errors go to stderr with exit code 1.
    static int LowerFile(string path)
    {
        try
        {
            var bytes = new List<byte>();
            uint[] last = null;
            foreach (string raw in File.ReadAllLines(path))
            {
                string line = raw.Split('#')[0].Trim();
                if (line.Length == 0) continue;
                string[] f = line.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                if (f.Length != 4) throw new FormatException($"expected four hex words: \"{line}\"");
                last = f.Select(s => Convert.ToUInt32(s.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? s[2..] : s, 16)).ToArray();
                foreach (uint w in last) bytes.AddRange(BitConverter.GetBytes(w));
            }
            // SM86 EXIT: opcode 0x94d under PT (bits 12-15 = 7), second condition PT (bits 87-90 = 7); AmpereLowering Exit.
            bool exits = last != null && (last[0] & 0xffff) == 0x794d && ((last[2] >> 23) & 15) == 7 && ((last[2] >> 20) & 3) == 0;
            if (!exits) foreach (uint w in new uint[] { 0x0000794d, 0, 0x03800000, 0x000fea00 }) bytes.AddRange(BitConverter.GetBytes(w));
            byte[] code = bytes.ToArray();
            byte[] lowered = AmpereLowering.Lower(code, code.Length, false, out int local);
            // Compute layout (AmpereLowering.Lower): a scheduling word at every 32-byte boundary, instructions in the other
            // slots, zero padding after the closing EXIT. Instruction words are never 0, so the last nonzero one ends it.
            var words = new List<ulong>();
            int end = 0;
            for (int a = 0; a + 8 <= lowered.Length; a += 8)
            {
                if ((a & 0x1f) == 0) continue;
                ulong w = BitConverter.ToUInt64(lowered, a);
                words.Add(w);
                if (w != 0) end = words.Count;
            }
            foreach (ulong w in words.Take(end)) Console.WriteLine(w.ToString("x16"));
            Console.WriteLine($"urbase {AmpereLowering.LastUrBase}");
            if (local != 0) Console.WriteLine($"local {local}");
            return 0;
        }
        catch (Exception e) { Console.Error.WriteLine($"{e.GetType().Name}: {e.Message}"); return 1; }
    }

    static int Main(string[] args)
    {
        if (args.Length == 2 && args[0] == "--lower") return LowerFile(args[1]);
        if (args.Length == 1 && args[0] == "--serve")
        {
            // One request per line: {"job": {...}, "out": "<dir>"}; one reply line "ok <key>" / "fail <key> <message>".
            string line;
            while ((line = Console.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                string key = "?", err;
                try
                {
                    var req = JsonNode.Parse(line).AsObject(); var job = req["job"].AsObject(); string o = (string)req["out"];
                    key = (string)job["key"];
                    Directory.CreateDirectory(o);
                    err = Translate(job, o);
                }
                catch (Exception e) { err = $"{e.GetType().Name}: {e.Message.Split((char)10)[0]}"; }
                Console.WriteLine(err == null ? $"ok {key}" : $"fail {key} {err}");
                Console.Out.Flush();
            }
            return 0;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string err = Translate(job, outDir);
            if (err == null) ok++; else { bad++; Console.Error.WriteLine($"【红】{(string)job["key"]}：{err}"); }
        }
        Console.WriteLine($"翻译 {ok} 成功，{bad} 失败");
        return bad == 0 ? 0 : 1;
    }
}
