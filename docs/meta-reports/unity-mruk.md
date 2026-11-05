# Issues found running Meta's MRUK Unity samples on a non-Meta OpenXR runtime

Found while running `oculus-samples/Unity-MRUtilityKitSample` on puppetxr, an OpenXR simulator
runtime (https://github.com/FlexW/puppetxr), as automated tests.

Setup:
- Unity-MRUtilityKitSample at commit `8b6daeb6e592e30faa9d2c2ab423a1e3d5bb1136` ("Release for updated SDK 207.0.0"), unmodified
- Unity 6000.0.66f2, Windows 64-bit player built in batch mode, D3D11
- Meta XR Core SDK / OVRPlugin 1.207.0, MR Utility Kit 207.0.0, Interaction SDK 207.0.0, Unity OpenXR plugin with the Meta XR feature
- Windows 11, OpenXR loader 1.1.61 (shipped with the player)

Issues 1 to 4 make the samples misbehave or crash on any conformant runtime that is not
Meta's. 5 to 8 are robustness problems. 9 and 10 are about the sample repository. The last
section lists things seen once that I could not pin down.

## 1. MRUK and the XrSceneModel sample read semantic labels with a trailing NUL

Severity: high. On a runtime that follows the OpenXR string convention, MRUK recognizes no
semantic label at all.

- MRUK's native code ("MRUK Shared") reads labels with the two-call idiom:
  - a size query;
  - then a buffer of exactly `bufferCountOutput` chars;
  - then a string built from `std::string(buffer, bufferCountOutput)`.
- Meta's OpenXR SDK sample XrSceneModel does the same (`SceneModelXr.cpp`, `GetSemanticLabels`):
  `return std::string(labels.buffer, labels.bufferCountOutput);`
- OpenXR's convention for string two-call buffers counts the terminating null in
  `*CountOutput` (as `xrPathToString` does). A runtime that follows it returns 6 for "FLOOR".
  Every label MRUK builds then ends in `\0`, and the log shows each of them as unrecognized:
  ```
  MRUK Shared: Unrecognized label: FLOOR
  MRUK Shared: Unrecognized label: CEILING
  MRUK Shared: Unrecognized label: WALL_FACE
  MRUK Shared: Unrecognized label: GLOBAL_MESH
  ...
  ```
- **Consequences seen in the samples:**
  - MRUKBase, BouncingBall and SpaceMap: EffectMesh draws the global mesh with the
    SceneMeshDefault wall effect. The mesh has no UVs, so the effect's edge stroke covers
    every pixel and the view turns into opaque cyan.
  - SpaceMap draws no terrain.
  - DestructibleMesh creates no destructible mesh.
  - FindMultiSpawn spawns nothing.
  - VirtualHome builds no furniture.
- With the count reported without the terminator, every sample behaves as intended. So Meta's
  runtime presumably counts without it, which differs from the convention above.
- **Ask:** please confirm which count `xrGetSpaceSemanticLabelsFB` is specified to return. If
  the count includes the terminator, build the string from `strnlen(buffer, count)` in MRUK and
  in the SDK samples; that works with both conventions.

## 2. OVRPlugin refuses runtimes by name, then crashes Unity

Severity: high. The samples cannot run at all on a conformant non-Meta runtime.

- OVRPlugin checks the runtime name from `xrGetInstanceProperties`:
  `[OVRPlugin][INFO] Support Non-Oculus runtime: NO`
- With any other name, MetaXRFeature logs:
  `[MetaXRFeature] OnInstanceCreate returned an error. If you are using Quest Link, please verify if it's started.`
- Every later OVRPlugin `xrGetInstanceProcAddr` lookup then fails with `ErrorHandleInvalid`.
- UnityPlayer.dll then crashes with an access violation (0xC0000005) instead of reporting an
  error and continuing without OVRPlugin.
- Reporting the runtime name "Oculus" makes everything work, which shows no Meta-only
  behaviour is needed for these samples.
- **Ask:**
  - never crash when OVRPlugin declines a runtime;
  - ideally run on any runtime that offers the extensions the app needs, or make the
    "support non-Oculus runtime" switch available to apps.

## 3. The UI ray is the grip pose turned 60 degrees down, not the OpenXR aim pose

Severity: medium. The pointer points somewhere else on runtimes whose grip-to-aim offset
differs from Quest's.

- The start scene's `OVRInputModule.rayTransform` is the `OVRCameraRig` right hand anchor. Its
  pose comes from OVRPlugin and is the OpenXR grip pose turned 60 degrees down about its X
  axis, with its origin at the grip.
- Measured: with the grip pitched up 60 degrees and placed at the eye, the cursor lands at the
  centre of the view.
- OpenXR defines a separate aim pose (`/input/aim/pose`) for pointing. A runtime's aim pose
  can differ from Quest's grip-plus-60-degrees, and then the UI ray points away from where the
  controller points.
- **Ask:** cast UI rays from the aim pose, or from the aim pose the runtime reports, rather
  than from a fixed offset of the grip.

## 4. A frame is submitted with an app space destroyed during it, and never recovers

Severity: medium. Unity's OpenXR plugin and OVRManager, in the MRUKBase scene, whose
OVRCameraRig uses the Stage tracking origin.

- When the scene loads, the app space changes within one frame:
  1. Create STAGE space A.
  2. Destroy the previous app space.
  3. Create STAGE space B.
  4. Destroy A.
  5. End the frame with a projection layer whose `space` is A, which was just destroyed.
- A runtime that validates handles answers `XR_ERROR_HANDLE_INVALID`:
  `[XR] [Error] xrEndFrame: XR_ERROR_HANDLE_INVALID`
- After that the app never recovers:
  - every frame logs a Unity OpenXR diagnostic report ("Uncaught Exception");
  - every frame changes the app space again (`[MetaXRFeature] OnAppSpaceChange` once per frame);
  - every frame fails the same way.
  - The display freezes on the last good frame.
- Quest evidently accepts the destroyed handle, so nothing shows there.
- **Ask:**
  - never submit a layer with a space destroyed earlier in the frame;
  - recover after a single failed `xrEndFrame` rather than failing every frame.

## 5. OVRManager sets the Stage tracking origin again every frame

Severity: low (cost and log noise, no visible fault once issue 4 is tolerated).

- In MRUKBase, `OVRManager.Update` compares `OVRPlugin.GetTrackingOriginType()` with its
  configured Stage origin.
- On a non-Meta runtime `GetTrackingOriginType()` never returns Stage. So every frame it calls
  the `trackingOriginType` setter again, which calls `TrySetTrackingOriginMode(Floor)` and
  `OpenXRSettings.RefreshRecenterSpace()`.
- Unity then recreates its STAGE app space every frame: one `OnAppSpaceChange` per frame, over
  500 in a short run.
- The runtime offers STAGE with bounds. The start scene, which uses FloorLevel (LOCAL_FLOOR),
  does not do this.
- The likely cause shows in the player log. OVRPlugin matches the new app space to a tracking
  origin by locating it against its own spaces, with a time that is not valid. A runtime that
  validates the time rejects both calls, and OVRPlugin gives up:
  ```
  [OVRPlugin][ERROR] OpenXR error: XR_ERROR_TIME_INVALID, cmd xrLocateSpace(appSpace, m_xrStationarySpace, xrTime, &spaceLocation)
  [OVRPlugin][ERROR] OpenXR error: XR_ERROR_TIME_INVALID, cmd xrLocateSpace(appSpace, m_xrLocalFloorSpace, xrTime, &spaceLocation)
  [OVRPlugin][ERROR] CompositorOpenXR::SetExternalXrAppSpace: Unable to match appSpace to a TrackingOrigin
  ```
- **Ask:**
  - locate with a valid time, such as the last predicted display time;
  - have `GetTrackingOriginType()` report what was set, or compare against the space type
    actually in use.
- **Confirmed:** the time is 0. The spec says `XR_ERROR_TIME_INVALID` for time 0, and the
  OpenXR CTS checks that (`test_xrLocateSpace`, `test_xrLocateViews`). When the runtime treats 0
  as "now", OVRPlugin matches the app space, and Unity changes it 3 times in MRUKBase instead of
  once per frame.
- OVRPlugin passes time 0 in more calls:
  - `xrLocateViews` in `LocateEyeViewsInternal(ToXrTime(GetTimeInSeconds()), ...)`
    (CompositorOpenXR.cpp:6611 and :12507), on the Oculus XR plugin at start;
  - `xrCreateSpatialAnchorFB` (`[MetaXRFeature]: ErrorTimeInvalid xrCreateSpatialAnchorFB`). The
    Shared Spatial Anchors sample's anchors fail to create ("ColoDiscoAnchor: FAILED TO CREATE!"),
    and so does MRUK's world-lock anchor.
- **Ask:** pass the frame's predicted display time (or `xrConvert*ToTimeKHR` of now), never 0.

## 6. OVRPlugin crashes in its JSON reader when several players start at once

Severity: medium (crash). Seen while starting 4 players of the same app within a second or two.

- Exit code 0xC0000409, an uncaught C++ exception ending the process.
- The player log stops at `OVRTelemetry.cpp:57] Telemetry_Init: calling startSession ...`;
  `startSession completed` never follows.
- Crash stack of the faulting thread, from the dump:
  ```
  KERNELBASE!RaiseException
  OVRPlugin!Json::operator<<
  OVRPlugin!Json::FastWriter::~FastWriter
  OVRPlugin!Json::operator>>
  OVRPlugin!Json::operator>>
  OVRPlugin!Json::operator<<
  kernel32!BaseThreadInitThunk
  ```
- Starting the players one at a time avoids it, so it looks like a race on a file the players
  share, read with an exception-throwing JSON parser.
- **Ask:** catch parse errors there, and make the shared file safe for concurrent processes.
  A crash dump is available.

## 7. The player starts an adb server that inherits its handles

Severity: low.

- The Windows player starts an adb server. It was the Meta Quest Developer Hub's `adb.exe`
  when that was installed, otherwise the Android SDK's from `PATH`. Its command line is
  `adb -L tcp:5037 fork-server server --reply-fd ...`.
- The server inherits the player's handles, including its stdout and its `-logFile`. It keeps
  them open after the player exits, so the log files cannot be deleted or reused until the adb
  server is killed.
- **Ask:**
  - start adb only when looking for a device is actually needed (Link);
  - start it without handle inheritance.

## 8. Missing-script and serialization layout warnings in the player

Severity: low (log noise, maybe a packaging problem).

- A fresh batch build of the sample logs these at every start:
  ```
  The referenced script on this Behaviour (Game Object '<null>') is missing!
  A scripted object (probably UnityEngine.XR.OpenXR.Features.Meta.ARRaycastFeature?) has a different serialization layout when loading. (Read 60 bytes but expected 244 bytes)
  ```
- The same appears for ARMeshFeature, OpenXRLifeCycleFeature, ARCameraFeature,
  BoundaryVisibilityFeature, ARPlaneFeature, ARBoundingBoxFeature, AROcclusionFeature,
  ColocationDiscoveryFeature, ARAnchorFeature, DisplayUtilitiesFeature and ARSessionFeature.
- The project's OpenXR feature assets seem not to match the package versions it resolves.

## 8b. The Depth API cannot work in a Windows player that uses the OpenXR loader

Severity: medium. Occlusion in the samples, such as PassthroughRelighting, never turns on in a
Windows build.

- `EnvironmentDepthManager.CreateProvider` offers two providers:
  - The OpenXR provider (`DepthProviderOpenXR`) is compiled only under
    `OPEN_XR_META_2_1_OR_NEWER && (UNITY_ANDROID || UNITY_EDITOR)`.
  - The OVRPlugin provider (`DepthProvider`) is used only with the Oculus XR plugin's loader.
- So a Windows player that uses the OpenXR loader, as this sample's Standalone settings do, has
  no provider. It logs:
  ```
  EnvironmentDepth is disabled. Please enable XR provider in 'Project Settings / XR Plug-in Management'.
  Environment Depth is not supported. Please check EnvironmentDepthManager.IsSupported before enabling EnvironmentDepthManager.
  ```
- In addition, the Meta XR feature's extension list (`MetaXRFeature`) has no
  `XR_META_environment_depth` on any platform:
  `[OVRPlugin][INFO] Unavailable OpenXR extension: XR_META_environment_depth`
- Unity's AR features of `com.unity.xr.meta-openxr` (occlusion, planes, meshes, anchors, bounding
  boxes) do not load in the Windows player. That is the "different serialization layout" warnings
  of issue 8.
- **Ask:** support the Depth API with the OpenXR loader on Windows: request the extension and
  use OVRPlugin's depth path. Link supports the Depth API.

## 9. Sample repository: TextMesh Pro resources are git-ignored

Severity: medium for anyone building from a clean clone without opening the editor.

- `Assets/TextMesh Pro` is ignored by the repository; the editor offers to import it on first
  use.
- A clean clone built in batch mode (`-batchmode -buildWindows64Player`) has a start menu with
  no text. Its buttons cannot be clicked either.
- Importing `TMP Essential Resources.unitypackage` from the uGUI package before the build fixes
  it.
- **Ask:** commit the TMP essentials, or document the import step for command-line builds.

## 10. Sample: some scenes animate on the wall clock

Not a bug, but it gets in the way of automated tests.

- FindFloorZone's and FindMultiSpawn's instruction videos and the spawned objects move on real
  time.
- FindMultiSpawn spawns a random number of objects, so screenshots differ from run to run even
  when frames are stepped one by one.
- A switch for deterministic playback would help automated tests, e.g. honouring
  `Time.captureDeltaTime` or a fixed seed.

## 11. OVRPlugin passes the group filter as the query's filter

Severity: low (a question of reading the spec; it fails on a runtime that follows the registry).

- `OVRSpatialAnchor.LoadUnboundSharedAnchorsAsync(groupUuid)` calls `xrQuerySpacesFB` with
  `XrSpaceGroupUuidFilterInfoMETA` as `XrSpaceQueryInfoFB::filter`.
- The registry declares the struct `structextends="XrSpaceQueryInfoBaseHeaderFB"`, i.e. chained
  to the query info. The extension's text says "in the query filters". A runtime that follows
  the registry rejects the filter type, and the app sees `Failure_InvalidParameter(-1001)`.
- **Ask:** settle in the spec which one is meant. If both are meant, say so and fix the
  registry's `structextends`.

## 12. Sample: colocation buttons need Meta's platform service, though the sample does not

Severity: low (sample only).

- In Unity-SharedSpatialAnchors' Colocation & Group Sharing scene, `ColoDiscoMan` disables its
  buttons until `Users.GetLoggedInUser` answers.
- With `m_RequireLoggedInUser = false` it enables them when the login *fails*, but not when
  `Oculus.Platform.Core.Initialize` throws (no app id, a PC without the Meta app). Then the
  buttons stay disabled for good, with no hint why.
- Group sharing needs no platform user.
- **Ask:** enable the buttons in that `catch` too, as the login failure path does. It is a
  three-line fix.

## Seen once, not pinned down

- Once in about 15 full runs, a player hung after its shutdown log
  (`Input System module state changed to: Shutdown.`) and never exited.
- I have no stacks for it yet. The test harness now saves the stacks of all threads when this
  happens, so the cause will be known if it recurs.
