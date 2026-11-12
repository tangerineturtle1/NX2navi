# Paired water coordinates and absorption-distance guard

Some views of the Hyrule Castle moat and Castle Town canal place water rendering
over solid geometry as a rectangle. A second defect can amplify that rendering
into an extreme red beam. These corrections address two separate calculations;
they do not establish the original translator/compiler/driver fault.
Related investigation: https://github.com/tangerineturtle1/NX2navi/issues/398.
The earlier fragment-coordinate/offset workaround is superseded by the paired
coordinate representation below.

## Coordinate correction

The vertex output carries undivided clip X, Y and the actual output W. The
fragment input uses perspective interpolation instead of NoPerspective and
reconstructs X/W and Y/W before the original half-scale and Y inversion. The
fragment's subsequent material and sampling calculations remain intact. There
are no empirical offsets or object/scene coordinates.

Both stages change together, only for an exact supported pair and when both
private replacement files exist. Separate pre-raster and fragment library
variants keep the correction from being reused with other shader pairs.

## Independent red-amplification guard

The identified absorption material stores a scalar distance used in attenuation.
A negative distance can turn attenuation into amplification. The recipe inserts
`NMax(distance, 0)` immediately before that exact store. It does not inspect or
suppress red color, alter valid positive distances, or change unrelated effects.
`NMax` also selects zero when only the distance operand is NaN.

This is a defensive bound on an invalid input. The reason the original distance
becomes invalid remains unresolved; this must not be described as a proven
translator or driver root-cause fix. It is independent of the UV representation
and can activate when the paired coordinate correction is not selected.

## Exact translation profiles

| Profile | Vertex | Fragment | Fragment set 1 | Material | Material set 1 |
| --- | --- | --- | --- | --- | --- |
| AMD | f6d753852c47b0de | a39c3d09241441fa | 3d3a316ab4da3bba | e73dfc0f4c556df8 | abfccea256824a78 |
| NVIDIA | 8d83e3705d790fed | 3c20cb98dbaeb257 | 216bd323c6b48ee2 | 061318799d885bcf | b3cec6d0d78ac25a |

These are FNV64 identities of translated modules, not hashes of a game EXE.
Unknown modules retain their original calculation. The runtime requires
independent stage layouts. Its graphics-library variant byte distinguishes
original, paired-coordinate and absorption-guard pipelines.

The vertex replacement stands for the translation itself: the outputs an install's
vertex output union adds are added to the replacement as to any other module, so the
pair holds whatever other fragment shaders that install has met with the vertex
shader. A pair blended with dual-source factors keeps its original modules.
The translations of game 1.1.1 are the same under the 0.9.0 translator as under
0.8.0's on NVIDIA (RTX 5090), so the profiles above still apply there.

## In the runtime, on by default

The runtime makes both corrections itself (`runtime/spirv_water.c`, called from
`runtime/vk_pipeline.c` `water_projective_pair`): when a pipeline's shaders are one of
the translations above, it edits their SPIR-V in memory, with the steps of
`tools/projective_recipe.py` in the same order, and builds the pipeline from the
result. No file is read or written and nothing derived from the game is shipped.
`water_projective = 0` in `nx2navi.ini` builds the translations as they are. The log
names each corrected module the first time it is built ("vk_pipeline: water: ...");
a module that is not of the shape an edit expects is built as it is, with a line
that says so. A coordinate pair is corrected in both stages or in neither.

On the NVIDIA profile the runtime's three modules are byte for byte the ones the
recipe writes (RTX 5090, game 1.1.1, 0.9.0's translator). The AMD profile runs the
same code on AMD's translations, which were not at hand to compare.
`python tests/water_projective_check.py` compares the two on made-up modules.

## The recipe as a tool

`tools/projective_recipe.py` makes the same modules as files from locally held
inputs, for comparing or inspecting them:

```sh
python tools/projective_recipe.py --vertex <local-vertex.spv> --fragment <local-fragment.spv> --material <local-material.spv> --output build/water-projective
```

The material argument is optional for a coordinate-only comparison. The recipe
rejects unknown inputs, mixed profiles and existing output files. It writes
replacement modules named by the original vertex or set-1 fragment identities,
plus a manifest. Validate a generated module with
`spirv-val --target-env vulkan1.2 <module.spv>`. The runtime does not read them.

## Validation and limits

The equivalent embedded corrections were visually tested on 0.7.1 and 0.8.0,
game 1.1.1, RX 9070 XT / driver 26.10.1, in repeated short visits to the moat
and market canal. No timed endurance run was recorded. The reporter also
reports successful AMD testing beyond the recorded system.

A separate tester has now reported the combined native patch runs cleanly on
NVIDIA without observed breakage. The exact NVIDIA model, driver, duration and
complete scene coverage were not recorded in that report. This is reported
cross-vendor validation of the equivalent embedded correction, not proof that
every GPU, scene, or the source file-loading integration has been gameplay-tested.
The source integration is compilation-checked. Its generated corrections match
the six AMD/NVIDIA modules used by the native test patch byte for byte and pass
SPIR-V validation. The private native patcher is a distribution/test aid and is
not part of this PR.

Recorded fragment invocations with UV error above 0.001:

| View | Original | Paired correction |
| --- | ---: | ---: |
| Castle moat | 4,999 / 18,065 | 0 / 20,400 |
| Market canal | 7,346 / 72,720 | 0 / 72,033 |

These are instrumented fragment invocations, not unique visible pixels. Views
were revisited manually and are not pixel-identical. The coordinate error
measurements concern the paired correction, not the absorption guard.

Sewer coverage, other translator outputs, drivers, resolutions and water scenes
remain open validation work. A brief sewer red flash was reported but was not
captured or conclusively attributed to these exact shaders. Do not infer that
all red flashes, the separate full-screen flash bug, or unrelated shadow defects
are fixed by this change. The texture-owner correction is separate (merged #535).

Only authored patch logic, diagnostic identities and documentation are committed.
Generated modules, disassembly, caches, executables and GPU captures stay private
under ignored build directories and must not be uploaded.
