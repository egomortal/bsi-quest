# Contributing

## Send it here, not to a fork

Bring fixes and improvements to this repository. Please do not release your own fork or your own builds as a separate
version. Reasons:

- Players should have one place to get the app and one answer to "which build do I install".
- A fix in a fork does not reach anyone using the main release, and the next change here may break it.
- Performance numbers only mean something when everyone measures the same tree at the same test spots.

A change that is accepted here ships to everyone in the next experimental release, with your name on the commit.

How:

1. **Small fix:** open a pull request against `main`.
2. **Bigger change or new idea:** open an issue first and say what you want to try. Many ideas have already been
   measured and rejected; they are listed in `CHANGELOG.md` under "Measured and rejected".
3. **Regular contributors** are given push access, so they can push to a branch in this repository and open the pull
   request from there. `main` changes only through pull requests.

Working on a private fork while you develop is normal (that is how GitHub pull requests work). The request is about
not *publishing* a competing version.

## The rules every change must meet

1. **Same picture.** A change must not alter what the player sees. Either it is bit-identical by construction, or it
   comes with a check mode that compares the old and new result on the headset and counts mismatches (see the `=2`
   / `CHECK` modes of the existing switches, for example `BSI_B1_FXAASPLIT=2` or `BSI_B1_SUNNL=5`).
2. **No quality shortcuts.** No foveated rendering, no lower eye resolution, no frame generation (SpaceWarp), no TAA,
   no dropped effects. The floating HUD stays.
3. **A switch for every change.** New behaviour sits behind an environment switch (`BSI_B1_<NAME>`, read from
   `/sdcard/BSI/env.txt`) with a documented default, and the off value must give exactly the previous behaviour. This
   is how a regression is found in minutes.
4. **Measured.** State the frame time before and after at the standard spots (Fair, Rooftops, Gondola, Cab ride), on a
   Quest 3, same build otherwise. "Feels faster" is not a result. A change that is exact but gives nothing is not merged.
5. **No game content in commits.** No game files, no translated game code (`gen*/`, objects, `.so`, APK), no Ghidra
   exports, no dumps or captures of the game. The `.gitignore` blocks the common ones; check your diff anyway.
6. **No DRM bypass and no piracy help.** The installer's check for a real Steam copy stays.
7. **Licences.** Do not paste code from GPL projects or from Unreal Engine source. Ideas are fine, code is not.

## What you can work on without a full build

Building the app needs the translated game objects, and the build-from-your-own-copy route (`PIPELINE.md`) still has
gaps. Until that is closed, these need no build:

- The installer and the report script (`install/`), the lab runner (`lab/`), the tools (`tools/`).
- The build pipeline itself (`PIPELINE.md` lists the gaps; closing them is the most useful contribution right now).
- The translator (`translator/`): it has its own tests (`difftest.py`, `run_diff.py`).
- Documentation.

For renderer, VR or runtime changes (`kit/`, `runtime/`): send the pull request with your reasoning and, if you could
not build it, say so. We build it, run it in the test lab on the headset and post the numbers on the pull request.

## Pull request checklist

- What it changes, in one or two sentences, and why.
- The switch name, its default, and what the off value does.
- How "same picture" is shown (identical by construction, or the check mode and its result).
- Frame times before and after, or "not built, please measure".
- Whether an AI assistant wrote or helped write it (see `AGENTS.md`). That is fine; we just want to know.

## Reporting problems without writing code

Play reports are the most useful thing a player can send: see `REPORTS.md`.
