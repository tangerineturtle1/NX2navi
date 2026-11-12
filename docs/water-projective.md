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

## Private local preparation

Use locally held inputs from the same supported translator profile:

```sh
python tools/projective_recipe.py --vertex <local-vertex.spv> --fragment <local-fragment.spv> --material <local-material.spv> --output build/water-projective
```

The material argument is optional for a coordinate-only comparison. The recipe
rejects unknown inputs, mixed profiles and existing output files. It writes
replacement modules named by the original vertex or set-1 fragment identities,
plus a manifest. Validate every generated module with
`spirv-val --target-env vulkan1.2 <module.spv>` before integration.

Rebuild using the documented UCRT64 workflow. Set `NX2NAVI_SPV_OVERRIDE` to the
absolute output directory in the test process, then restart. Default runtime
behavior is unchanged. Use separate pipeline caches for comparisons and disable
pipeline-binary reuse and boot prebuilding during the experiment. Unset the
override and restart to disable it. Do not change the directory during a run.

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
