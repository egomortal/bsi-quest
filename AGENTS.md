# Working on BSI-Quest with an AI assistant

Using an AI coding assistant is fine. Most of this project was written with one. This file is for two readers:

- **You:** copy the text in the box below into your assistant at the start of the session. Use it word for word; the
  rules in it are the ones a pull request is checked against.
- **The assistant:** tools that read `AGENTS.md` automatically (Codex, Cursor, Claude Code and others) should treat
  everything under "Rules for the assistant" as binding for work in this repository.

## Text to give your assistant (copy exactly)

```
You are working in the BSI-Quest repository: BioShock Infinite (PC, Unreal Engine 3, 32-bit x86) statically recompiled
to ARM64 and running natively on Meta Quest 3 in stereo VR, through our own runtime, DXVK 3.1.1 and the Turnip driver.

Goal: full PC Ultra settings in VR at a permanent 36 fps (27.8 ms per frame) on Quest 3.

Read first: README.md, CONTRIBUTING.md, CHANGELOG.md (including "Measured and rejected"), PIPELINE.md.

Hard rules. Do not break them, and tell me if my request would break one:
1. Same picture. A change must not alter the image. It is either bit-identical by construction, or it ships with a
   CHECK mode that compares old and new output on the device and counts mismatches. Never claim "visually identical".
2. Never use foveated rendering, lower eye resolution, SpaceWarp or other frame generation, TAA, or removal of an
   effect to gain speed. The floating HUD stays.
3. Every behaviour change goes behind an environment switch named BSI_B1_<NAME> (read from /sdcard/BSI/env.txt) with
   a stated default. The off value must reproduce the previous behaviour exactly.
4. Never add game content to the repository: no game files, no translated game code (gen*/, *.o, *.so, *.apk), no
   Ghidra exports, no dumps or screenshots of the game. Never weaken the installer's check for a real Steam copy.
5. Never copy code from GPL projects or from Unreal Engine source. Never push DXVK or Mesa trees; our changes to them
   live as patch files in patches/.
6. Do not invent results. If you did not build or measure something, say "not built" or "not measured". Frame times
   count only when measured on a Quest 3 at the standard spots (Fair, Rooftops, Gondola, Cab ride).
7. Before proposing an optimisation, check CHANGELOG.md "Measured and rejected". Do not re-propose a listed idea
   unless you have a new reason, and say what the new reason is.
8. Keep the change small and in the existing style. Do not reformat or rename code you are not changing.

Where things are: kit/src_b1n/linux/rhi_b1_*.{cpp,inc,h} = native renderer; kit/src_vr/linux = stereo VR and UI;
runtime/linux = Win32 layer, COM bridge, audio, input; translator/ = x86-to-C translator; install/ = installer and
report script; lab/ = automated headset test runner; tools/ = analysis tools.

When you finish, write the pull request description with exactly these headings:
What changed / Switch and default / Why the picture is the same / Measured (or "not built, please measure") /
Files touched / AI assistance (name the assistant and say which parts it wrote).
```

## Rules for the assistant

1. The eight hard rules in the box above apply to every change in this repository.
2. Work goes to this repository as a pull request against `main`. Do not suggest publishing a fork or separate builds.
3. You usually cannot build the app: it needs translated game objects that are not in the repository. Say so plainly
   instead of guessing at build or test output, and mark the pull request "not built, please measure".
4. Do not ask the user for, or handle, game files, the translated code, signing keys or account credentials.
5. Commit messages: one line saying what changed, then the switch name and default if there is one. Add a
   `Co-Authored-By:` line naming the assistant.
6. If a request conflicts with these rules, stop and tell the user which rule and why.

## Wording to use in commits and pull requests

- "exact" or "bit-identical" only when it is identical by construction or a CHECK mode reported zero mismatches.
  Give the number of pixels or bytes compared.
- "measured" only with the spot, the build and the numbers: `Fair 40.7 -> 39.2 ms (b143 vs b144, Quest 3)`.
- "not built" / "not measured" when that is the case. No "should be faster", no "looks the same".
- "AI assistance: <assistant name>, wrote <which parts>" in every pull request where one was used.
