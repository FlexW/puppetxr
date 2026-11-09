# Changelog

Changes users notice, newest release first. Every such change adds a line under Unreleased;
`tools/release.sh` turns that section into the release, and the release notes are taken from
it (docs/building.md, Releasing). Breaking changes of the C API, the recording format or the
config start with **Breaking:**.

## Unreleased

- Body tracking: the skeleton's hierarchy is a Quest 3's: the hand's wrist hangs from the lower arm and the palm from the wrist (both hung from the wrist twist), and the root's parent is -1 in the full body joint set too (was `XR_FULL_BODY_JOINT_NONE_META`). A hand wrist rotation is now relative to the lower arm.
- Body tracking: an untracked hand bends its fingers by their joints' rotations (`body joint right_hand_index_proximal rot ...`); before, hand joint rotations were stored but had no effect.

## 0.1.0 - 2026-11-07

- Body tracking: `body_tracking.skeleton_change_period_ms` (or `PUPPETXR_SKELETON_CHANGE_PERIOD_MS`) makes the skeleton change count rise once per period with the same shape, as some runtimes do; off by default.
- Recordings are format 2: the header holds the skeleton change period. Format 1 still reads.

- `xrQuerySpacesFB` refuses a component filter without `XrSpaceStorageLocationFilterInfoFB`, as a Quest 3 does.
- The sources build with MSVC again.
- Linux: closing the mirror window no longer can end the app with X's BadWindow error.
- First public release.
