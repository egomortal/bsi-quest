# GTAO (ambient occlusion) for BSI-Quest

`gtao.comp` replaces the game's HDAO main pass with ground-truth ambient occlusion (based on Intel XeGTAO, MIT licence;
the notice is in the file header). It is our own code; it reads the game's inputs and writes the same output format, and
the game's own blur and composite passes run after it (on the headset the renderer swaps in its own exact blur).

What players get: `install/config/ao/gtao_match/` holds the compiled `match` preset (closest to the game's HDAO). The
installer copies it to `/sdcard/BSI/ao/gtao_match/`, and `ao_sel.txt` = 3 selects it. The `.txt` file keys the shader
to the game's HDAO main pass (`variant any` = by the game shader's name, the same for every Steam copy). Without the
folder, or with `ao_sel.txt` = 0, the game's own HDAO runs. The app log shows `[ao] selector 3 (gtao_match)` and
`[b1ao] cs.3ed0bfa...: replacement in use` when it is active.

Rebuilding the `.spv`: `mk_gtao.py <dxvk shader dump dir> <out dir> match KEY=VAL ...` with the values in
`params_fit_match_real.txt` (needs glslangValidator, spirv-opt, spirv-val). The dump only supplies the game shader's
hash for the variant line. **Not rebuilt here:** the shipped `.spv` (md5 41af3f5137e88279ad87809df4cf1a08) is the file
that has run on the test headset since build 89; a rebuild from this source has not been compared byte for byte yet.

Not included on purpose: the faster exact HDAO blurs (`k32id`, `t16id`) and their generator. They are made from the
game's own shader code and cannot be published.
