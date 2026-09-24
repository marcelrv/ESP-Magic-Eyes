# Sequences

Play-mode sequence JSON files, per the architecture plan §5 and §9. They
are data-driven (parsed at runtime by `PlayModeManager`, not baked into
firmware) so new sequences can be added/edited without a rebuild.

- `greeting.json` — the one-shot "greeting" play mode's scripted
  wake-up/look-at-viewer/surprise/settle sequence (Phase 4). See the
  file's own `_comment` field for the keyframe schema, or
  `src/motion/playmode_manager.cpp`'s loader.

Gestures (`blink`, `wink_left`, `surprise`, ...) are intentionally NOT
JSON files — they're small, fixed, hand-authored C++ data tables in
`src/motion/gesture_engine.cpp` (plan §5: "hand-authored C++ data tables,
not loaded from JSON"). Only play-mode *sequences* are JSON-driven.

`curious`/`idle`/`sleep` play modes are code-parameterized (amplitude and
timing constants in `src/motion/playmode_manager.cpp`), not JSON-driven —
see PROGRESS.md's Phase 4 notes for why this was judged an acceptable
scope simplification for this phase.
