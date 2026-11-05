# Issues found running Meta's Unreal samples on a non-Meta OpenXR runtime

Found while running Meta's Unreal samples on puppetxr, an OpenXR simulator runtime
(https://github.com/FlexW/puppetxr), as automated tests.

Setup:
- Meta's Unreal fork, branch `oculus-5.8` (`oculus-5.8.0-release-1.207.0`), built from source;
  samples run uncooked by the editor (`UnrealEditor.exe <uproject> -game -vr`), Windows 11
- Meta XR plugin v207 (v208 for the Interaction SDK sample), on Unreal's OpenXR plugin unless
  noted
- Samples: Unreal-PassthroughSample `abe38260a14f5ecde1dc9482896a2ca6ec413b0c`,
  Unreal-InteractionSDK-Sample `de94c3674d48f40ab61fd11dc30449ebf5243f61`,
  Unreal-MRUtilityKitSample `3abad9230d4c8194c11713e2779915c9b9d415de` (the commit after `5.8.0-v207`),
  Unreal-Movement `8d7ede9e9864a0f0940097386dcaa9eb86df06a6` (the commit after `5.8.0-v207`)

The inverted-alpha sRGB bug and OVRPlugin's fragment density map request on Vulkan are in a
separate report.

## 1. Meta XR plugin: passthrough colour LUT destroyed through OVRPlugin at exit (crash)

Severity: medium (crash at every exit). Unreal-PassthroughSample, level PTColorLut, on Unreal's
OpenXR plugin (no OVRPlugin loaded).

- At exit the process ends with `EXCEPTION_ACCESS_VIOLATION 0x0000000000000000` (exit code 3).
- Stack, from the minidump:
  ```
  OculusXRPassthrough!UOculusXRPassthroughColorLut::DestroyLutObject
  OculusXRPassthrough!UOculusXRPassthroughColorLut::RemoveReference
  OculusXRPassthrough!UOculusXRPassthroughLayerBase::BeginDestroy
  CoreUObject!UObject::ConditionalBeginDestroy
  CoreUObject!UnhashUnreachableObjects
  CoreUObject!IncrementalPurgeGarbage
  CoreUObject!PurgeAllUObjectsOnExit
  CoreUObject!StaticExit
  UnrealEditor!FEngineLoop::AppPreExit
  ```
- `OculusXRPassthroughColorLut.cpp`, `DestroyLutObject`: it picks the API with
  `OculusXR::IsOpenXRSystem()`. During `PurgeAllUObjectsOnExit` the XR system is already gone,
  so that returns false and the code calls
  `FOculusXRHMDModule::GetPluginWrapper().DestroyPassthroughColorLut(Handle)`. The OVRPlugin
  wrapper was never loaded in this project, so the call goes through a null function pointer.
- The LUT handle was created through `XRPassthrough::xrCreatePassthroughColorLutMETA`, so it
  belongs to the OpenXR path either way.
- **Ask:**
  - remember which API created the handle and destroy it through that one;
  - skip the destroy when the session, and with it every LUT, is already gone.
  - Also check the other `IsOpenXRSystem()` branches that can run during exit.

## 2. Interaction SDK: UIsdkHandGrabPose asks for the editor world in a game (crash)

Severity: medium (the level cannot run). Unreal-InteractionSDK-Sample, level TransformerExamples,
run as a game (`-game`) from the editor binary.

- Opening the level crashes:
  ```
  UnrealEd!UEditorEngine::GetEditorWorldContext
  OculusInteraction!UIsdkHandGrabPose::InitializeDebugVisual
  Engine!USCS_Node::ExecuteNodeOnActor
  ```
- `InitializeDebugVisual` runs from the construction script and calls
  `GEditor->GetEditorWorldContext()`. Under `-game` (and in a packaged build) there is no editor
  world context.
- **Ask:** build the debug visual only `if (GIsEditor && !IsRunningGame())` or with the
  actor's own world, `GetWorld()`, instead of the editor's.

## 3. Unreal mobile renderer: assert on a post-process material at exit

Severity: low (after the session ends, but a crash dialog and a non-zero exit code). Seen with
Unreal-InteractionSDK-Sample on D3D12 with the mobile renderer (`-FeatureLevelES31`), in most
levels.

- At exit:
  ```
  Assertion failed: MaterialProxy [File:Engine\Source\Runtime\Renderer\Private\PostProcess\PostProcessMaterial.cpp] [Line: 427]
  Renderer!GetMaterialInfo                     PostProcessMaterial.cpp:427
  Renderer!AddPostProcessMaterialPass          PostProcessMaterial.cpp:751
  Renderer!AddPostProcessMaterialChain         PostProcessMaterial.cpp:1319
  Renderer!AddMobilePostProcessingPasses       PostProcessing.cpp:2953 / 3541
  Renderer!FMobileSceneRenderer::Render        MobileShadingRenderer.cpp:1988
  Renderer!RenderViewFamily_RenderThread
  ```
- The log just before it: `Engine exit requested (reason: EngineExit() was called; note: exit was
  already requested)`, and `GetCurrentInteractionProfile ... failed because session is null!`.
- The render thread still draws a view family whose post-process material has lost its render
  proxy, as the material is torn down at exit. This might be the engine or the fork (a blendable
  released before the last frame is flushed), not the sample.
- **Ask:** flush rendering before releasing post-process materials at exit, or skip a blendable
  whose `MaterialProxy` is null instead of asserting.

## 4. MRUK sample: menu timer fires into a destroyed pawn (crash)

Severity: medium (crash when a level is changed right after start, e.g. from the Maps tab or with
`-ExecCmds=open <map>`).

- `Source/MRUtilityKitSample/Private/DemoVRPawn.cpp`, `ADemoVRPawn::BeginPlay` sets a 0.3 s
  timer with `Timer.BindLambda([this]() { PlaceMenu(); })`.
- A level change within those 0.3 s destroys the pawn. The timer still fires and `PlaceMenu`
  runs on a freed object.
- **Fix** (a one-line change, which the tests carry as a patch):
  ```diff
  -	Timer.BindLambda([this]() {
  +	Timer.BindWeakLambda(this, [this]() {
  ```
  Alternatively, clear `MenuDelayTimerHandle` in `EndPlay`.

## 5. Meta XR plugin: `xrGetRecommendedLayerResolutionMETA` asked for an older frame (ensure)

Severity: low (an ensure in the log every few frames, dynamic resolution gets no answer).

- `OculusXRLayerExtensionPlugin.cpp`, `FLayerExtensionPlugin::UpdateCompositionLayers_RHIThread`
  (line 481) passes `PredictedDisplayTime_RHIThread`. That is the game thread's
  `OpenXRHMD->GetDisplayTime()` (line 161) handed to the RHI thread. By the time the RHI thread
  asks, the game thread has already called `xrWaitFrame` for a newer frame, so the time
  belongs to an older frame than the last waited one.
- A runtime that enforces the extension's time rule answers `XR_ERROR_TIME_INVALID`:
  ```
  Ensure condition failed: (XRES_Result = xrGetRecommendedLayerResolutionMETA.GetValue()(Session, &ResolutionRecommendationGetInfo, &ResolutionRecommendation), ((XRES_Result) >= 0))
  XR call xrGetRecommendedLayerResolutionMETA ... failed with result: XR_ERROR_TIME_INVALID
  OculusXRHMD!FLayerExtensionPlugin::UpdatePixelDensity::<lambda>
  OculusXRHMD!FLayerExtensionPlugin::UpdateCompositionLayers_RHIThread  OculusXRLayerExtensionPlugin.cpp:512
  OpenXRHMD!FOpenXRHMD::OnFinishRendering
  ```
- **Ask:**
  - Confirm which display times the extension accepts. Is any time of a frame not yet ended
    valid, or only the latest waited frame's, as a strict reading gives?
  - Pass the newest waited frame's time, or treat `XR_ERROR_TIME_INVALID` as "no
    recommendation", without an ensure.

## 6. MRUK: room discovery before the session runs, never retried

Severity: medium on runtimes that follow the spec strictly (no room at all). Unity and Unreal MRUK.

- MRUK starts `xrDiscoverSpacesMETA` before the session reaches `XR_SESSION_STATE_READY` and
  `xrBeginSession`. In Unity this happens in the first scene's `Awake`; in Unreal, at start.
- The spec allows a runtime to answer `XR_ERROR_SESSION_NOT_RUNNING` there. MRUK then never
  retries, so the scene stays empty for the whole run.
- Unreal also logs, on the start level:
  ```
  LogOculusXRAnchors: RequestId 1 not found in AnchorDiscoveryBindings, skipping xrRetrieveSpaceDiscoveryResultsMETA
  LogMRUK: MRUK Shared: Anchor search complete with 19 anchors
  LogOculusXRAnchors: Error: Couldn't find binding for anchors discovery! Request: 1
  ```
  So the discovery's completion is matched against a binding that does not exist yet.
- **Ask:**
  - start discovery once the session runs (`XR_SESSION_STATE_FOCUSED` or after
    `xrBeginSession`);
  - or retry after `XR_ERROR_SESSION_NOT_RUNNING`;
  - register the discovery binding before the request is sent.

## 7. Vulkan: parts of the scene drawn wrong (black lit meshes, noise on a character)

Severity: medium (the same levels are right on D3D11 and D3D12). The scenes repeat identically
with a warm shader cache, so this is not the default material shown while shaders compile.

- Unreal-InteractionSDK-Sample, PokeExamples, `-vulkan -FeatureLevelES31`:
  - the table and all furniture come out black;
  - the sky, UI panels and controllers are right.
- Unreal-PassthroughSample, PTStartup, `-vulkan` (desktop renderer): Oppy's head has a grey-white
  noisy patch. Mobile renderer and D3D are right.
- The Khronos validation layer (`-vulkandebug`) reports errors in Unreal's own commands on the
  mobile renderer, in every run:
  - `VUID-vkCmdDrawIndexed-viewMask-06178` and `VUID-vkCmdDrawIndexedIndirect-viewMask-06178` (multiview);
  - `VUID-vkCmdDispatch-viewType-07752` and `VUID-vkCmdDrawIndexedIndirect-viewType-07752`: a 2D
    image view bound where the shader declares an arrayed image (binding #17);
  - `VUID-vkCmdPipelineBarrier-shaderTileImageColorReadAccess-08718`.
- These were seen with Unreal-OcclusionSample and may explain the two scenes above. RenderDoc
  1.x crashes inside `FVulkanCommandBuffer::BeginDynamicRendering` (`vkCmdBeginRendering`), so I
  have no frame capture.
- The runtime composites each eye's image as a whole, the same way for D3D and Vulkan. A fault
  limited to some meshes is inside the image the app submits.

## 8. Vulkan: Unreal keeps running without drawing after a lost device

- In a run of Unreal-OcclusionSample on Vulkan, the device was lost (`vkQueueSubmit ...
  VK_ERROR_DEVICE_LOST`, VulkanQueue.cpp:486).
- In that run Unreal never exited; it kept running without drawing until it was killed.
- In another run it exited through `FVulkanDynamicRHI.TerminateOnGPUCrash`, as expected.
- The device loss itself was the runtime's fault and is fixed there. The hang after it is
  Unreal's.

## 9. Meta XR plugin: an uninitialized space user passed to xrShareSpacesFB

Severity: medium (sharing with a player whose id is not known yet fails for everyone).

- `OculusXRAnchorsXR.cpp`, `FAnchorsXR::ShareAnchors`: each user id becomes an `XrSpaceUserFB`
  through `Algo::Transform` and `CreateSpaceUser`. For id 0 (a player whose platform id has not
  arrived yet, as in the Shared Anchors sample right after a player joins), `CreateSpaceUser` returns
  `XR_ERROR_VALIDATION_FAILURE` before writing its output, and the uninitialized handle is still
  put into `XrSpaceShareInfoFB::users`:
  ```
  xrCreateSpaceUserFB(..., userId=1) -> XR_SUCCESS {user=0xd00000000000001}
  xrShareSpacesFB(..., userCount=2, users=[0xd00000000000001, 0xd7fa2c00]) -> XR_ERROR_HANDLE_INVALID
  ```
  The share then fails for every user, including those with valid ids. `DestroySpaceUser` is also
  called on the garbage handle afterwards.
- **Fix:** skip ids whose `CreateSpaceUser` failed (and do not destroy them), or zero-initialize
  `out` and drop null handles before the call.

## 10. Meta XR plugin: crash on an empty visibility mask

Severity: medium on runtimes without a lens mask (the app cannot start). Unreal-OcclusionSample
(OVRPlugin path) on a runtime whose `xrGetVisibilityMaskKHR` returns zero counts.

- The spec allows it: "If a view mask for the specified view isn't available, the returned vertex
  and index counts must be 0."
- `OculusXRHMD.cpp`, `BuildOcclusionMesh`: it creates the vertex and index buffers with the counts
  `GetViewportStencil` returned, without checking for 0:
  ```
  Fatal error: Attempt to create zero-sized buffer 'FOculusXRHMD_Occlusion_Vertices', owner 'None', usage 0x4001, stride 0.
  OculusXRHMD!OculusXRHMD::BuildOcclusionMesh
  OculusXRHMD!OculusXRHMD::FOculusXRHMD::SetupOcclusionMeshes::<lambda_1>
  ```
- **Fix:** return (leave the mesh empty) when `VertexCount` or `IndexCount` is 0.

## 11. Unreal-MRUtilityKitSample: the pawns' input events are never bound

Severity: high for the sample (DestructibleMesh's trigger does nothing; the menu button's C++
binding is dead too), on every runtime.

- `Source/MRUtilityKitSample/Private/VRPawn.cpp`, the constructor:
  ```
  EnhancedInputComponent = CreateDefaultSubobject<UEnhancedInputComponent>("EnhancedInputComponent");
  InputComponent = EnhancedInputComponent;
  ```
- `APawn::PawnClientRestart` (Engine `Pawn.cpp`) creates the player input component, calls
  `SetupPlayerInputComponent` and binds the Blueprint input events
  (`UInputDelegateBinding::BindInputDelegatesWithSubojects`) only `if (InputComponent == nullptr)`.
  With the constructor's component in place none of it runs: `ADemoVRPawn::SetupPlayerInputComponent`
  never binds `IA_ShowMenu`, and `BP_DestructibleMeshPawn`'s `IA_Select` event never fires,
  although Enhanced Input triggers the action (`showdebug enhancedinput`) while the right trigger is
  held. Calling the event's chain (trace, `ComponentHasTag`, `RemoveGlobalMeshSegment`) by hand
  removes the segment.
- **Fix:** drop `InputComponent = EnhancedInputComponent;` (set the project's default input
  component class to `UEnhancedInputComponent`, as it is), or bind in `BeginPlay`. With the line
  removed the engine makes `PawnInputComponent0`, and the trigger destroys the aimed-at segment.
- The Interaction SDK's sample pawn has the same constructor (`OculusInteractionSamples`,
  `IsdkSamplePawnClass.cpp`). Nothing is lost there today, as neither it nor `IsdkSamplePawn`
  binds input, but any input event a project adds to a pawn derived from it never fires.

## 12. Unreal-Movement: the commit for 5.8.0-v207 lacks the prebuilt utility library

Severity: high for the sample at that commit (the editor target does not link).

- `5.8.0-v207` (`92f21c0e770f3153162edb4aba3f913d51038da7`) has
  `Plugins/MetaMovementSDK_Utility/Source/MetaMovementSDK_Utility.Build.cs` link
  `Binaries/Win64/MetaMovementSDK_Utility.lib`, which `.gitignore` (`/Plugins/**/Binaries`) keeps out:
  ```
  LINK : fatal error LNK1181: cannot open input file '...\Plugins\MetaMovementSDK_Utility\Binaries\Win64\MetaMovementSDK_Utility.lib'
  ```
- The next commit on `main` (`8d7ede9e9864a0f0940097386dcaa9eb86df06a6`, "Add prebuilt libs")
  adds the libraries and a `.gitignore` exception; the sample builds from there.
- **Ask:** tag the commit that builds, or note it in the README.

## 13. Meta XR plugin: OculusXRMovement asserts without an XR system (crash)

Severity: medium (any map with a body tracking component crashes the game without a headset or
with `-nohmd`, where it could report body tracking as unsupported).

- `OculusXRMovement.cpp`, `OculusXRMovement::GetOculusXRMovementFunctionsImpl` (line 189)
  `check(MovementFunctionsImpl)`: neither the OVR HMD nor an OpenXR XR system exists, so no
  implementation is made.
  ```
  Assertion failed: MovementFunctionsImpl [File:...\OculusXRMovement\Private\OculusXRMovement.cpp] [Line: 189]
  OculusXRMovement!OculusXRMovement::GetOculusXRMovementFunctionsImpl
  OculusXRMovement!OculusXRMovement::IsBodyTrackingSupported
  OculusXRMovement!UOculusXRBodyTrackingComponent::BeginPlay
  ```
- Steps: Unreal-Movement, `UnrealEditor.exe MovementSample.uproject -game -nohmd`.
- **Fix:** return false from the `Is*Supported` queries (and fail the others) when there is no
  implementation.

## 14. OVRPlugin: `xrCreateFoveationProfileFB` called before the session exists

Severity: low (an error in the log at startup; the profile is not made).

- OVRPlugin 1.207.0, `CompositorOpenXR.cpp` line 11435, right after "Supported Performance
  Metrics", calls `xrCreateFoveationProfileFB` with `session = XR_NULL_HANDLE`, before
  `xrCreateSession`. The spec's answer is `XR_ERROR_HANDLE_INVALID`:
  ```
  [OVRPlugin][ERROR] OpenXR error: XR_ERROR_HANDLE_INVALID, cmd xrCreateFoveationProfileFB(m_xrSession, &profileCreateInfo, &profile)
  ```
- Seen with Unreal-Movement (the Meta XR plugin v207 on its OpenXR path); an API trace shows the
  call with the null handle before the instance's `xrCreateSession`.
- **Fix:** create the profile once the session exists.

## 15. Unreal: synchronous shader compiling asserts on uncached global shaders (crash)

Severity: medium for automated runs (the game cannot start on a cold cache with
`bAllowAsynchronousShaderCompiling=False`).

- With `-ini:Engine:[DevOptions.Shaders]:bAllowAsynchronousShaderCompiling=False` and global
  shaders not in the DDC (Unreal-Movement: `r.ForwardShading`, `vr.InstancedStereo`), startup fails:
  ```
  Assertion failed: Results->NumPendingJobs.GetValue() == 0 [File:...\ShaderCompiler\ShaderCompiler.cpp] [Line: 1949]
  Engine!FShaderCompilingManager::BlockOnShaderMapCompletion
  Engine!FShaderCompilingManager::FinishCompilation
  Engine!VerifyGlobalShaders
  Engine!CompileGlobalShaderMap
  UnrealEditor!FEngineLoop::PreInitPreStartupScreen
  ```
  The synchronous branch of `BlockOnShaderMapCompletion` loops until no worker is active, then
  finds jobs still pending. The same start with asynchronous compiling compiles them, after which
  the synchronous start works.
- Steps: Unreal-Movement on a fresh DDC, `UnrealEditor.exe MovementSample.uproject -game -vr
  -ini:Engine:[DevOptions.Shaders]:bAllowAsynchronousShaderCompiling=False`.
- This is the engine's, not the sample's; the report is for Meta's fork, where it was seen.
