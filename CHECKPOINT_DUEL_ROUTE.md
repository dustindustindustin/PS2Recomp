# Duel-route development checkpoint

October 27, 2026. User requested preserving and pushing current public work
because their usage budget was nearly exhausted. This branch is an unfinished
checkpoint, not a completed gameplay milestone or promotion of experiments.

The companion game repository is
https://github.com/dustindustindustin/DuelistsOfTheRosesRecomp
(branch `codex/duel-route-checkpoint-20261027`). Its
`docs/DUEL_LOOP_VALIDATION.md` is the canonical active handoff.

This snapshot preserves existing runtime, GS, VU, audio, diagnostics, and test
work, including startup input capture in ps2_pad/ps2_runtime and its keyboard
regression fixture. Startup capture queues Start safely for host movie skipping
while keeping guest input neutral, with a release gate afterward. Manual startup
and intermittent black map-to-duel entry remain unresolved. No provider change
is implied by this checkpoint; preserve diagnostic/reference fallbacks.

Final focused verification: built `ps2x_pad_keyboard_regression` in
`.codex-build-tests`, then its exact CTest selection passed1/1 in0.11s. The
companion optimized game runner built and its four selected noninteractive tests
passed. The entire runtime test suite and existing rendering/performance
experiments were not revalidated for this checkpoint. Generated code, binaries,
private saves/assets, traces, and local build trees are excluded.
