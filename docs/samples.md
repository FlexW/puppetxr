# Sample apps under test

What runs, what does not and why, and every change we make to a sample. Mechanics of the
tests (build, driver, references): `docs/testing.md`, Sample apps. Keep this file current
whenever a sample, a patch or an extension it needs changes.

## Status legend

| status | meaning |
|---|---|
| tested | runs in the `samples` ctest with screenshot references |
| runs | runs on puppetxr, no test yet |
| blocked | needs a puppetxr feature, listed under "missing" |
| n/a | cannot run on Linux or Windows |

The missing column lists what the sample asks for at `xrCreateInstance` and puppetxr lacks.

## Meta OpenXR SDK

`meta-quest/Meta-OpenXR-SDK` v85 (`bbed2f20e38a5df7113630771c83cb8279e4fc26`),
patch `tests/samples/meta_openxr_sdk/linux.patch`, built with clang.

| sample | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| XrInput | tested | `meta_xrinput` | | start, right trigger held; 4 `xrCreateActionSpace` calls with null actions fail as the spec says (app bug, harmless) |
| XrColorSpaceFB | tested | `meta_xrcolorspacefb` | | |
| XrColocationDiscovery | tested | `meta_xrcolocationdiscovery` | | still only; advertising and discovery not yet scripted |
| XrDynamicObjects | tested | `meta_xrdynamicobjects` | | living_room, a keyboard prop from `PUPPETXR_PROPS` tilted towards the head; the sample tracks it and cuts a passthrough window around it (`tracked`), then `prop move` shifts it and the window follows (`moved`) |
| XrOverlayKeyboard | tested | `meta_xroverlaykeyboard` | | panels show, no keyboard |
| XrVirtualKeyboard | tested | `meta_xrvirtualkeyboard` | | the keyboard within reach (DIRECT); the head looks at it, the right ray presses shift, h, i (held, `pressed`) and space, `vkb type` adds the rest; the text box shows "Hi puppetxr" and "1.0" |
| XrPassthroughOcclusion | tested | `meta_xrpassthroughocclusion` | | living_room; start and head moved aside |
| XrSceneSharing | tested | `meta_xrscenesharing` | | living_room, still |
| XrSpatialAnchor | tested | `meta_xrspatialanchor` | | living_room; A places an anchor |
| XrPassthrough | tested | `meta_xrpassthrough` | | living_room; all six modes of its cycle, 40 frames each instead of 400 (`XRPASSTHROUGH_FRAMES_PER_MODE`), each shot in the middle (basic, dynamic ramp, green ramp with yellow edges, masked, projected quad on the left grip, stopped); all but the ramp are bit-identical to the 400-frame run |
| XrSceneModel | tested | `meta_xrscenemodel` | | living_room; all entities, then the room mesh view (left thumbstick twice). Horizontal lines every 64 pixels in both views, on llvmpipe's tile rows (open issue below) |
| XrControllers | tested | `meta_xrcontrollers` | | binds only the 1.1 profile names; the right ray clicks a 1 s vibration, a 1 s PCM wave and the parametric curve, `puppetxrctl haptic right` checks each |
| XrBodyFaceEyeSocial | blocked | | `XR_FB_body_tracking`, `XR_FB_face_tracking2`, `XR_FB_eye_tracking_social` | renders, then crashes at exit |
| XrHandsFB | tested | `meta_xrhandsfb` | | both hands tracked; open, right pinch, right fist; the generated mesh with the sample's joint axes |
| XrHandDataSource | tested | `meta_xrhanddatasource`, `meta_xrhanddatasource_models` | | hands as above in its room; about one frame a second on one llvmpipe thread, so scripts step 10 frames at a time. `_models`: controllers held, the right ray clicks "Render Controllers", the sample draws the XR_FB_render_model GLBs in the conforming hands |
| XrHandTrackingWideMotionMode | tested | `meta_xrhandtrackingwidemotionmode` | | `PUPPETXR_HAND_TRACKING_FOV=40`: right hand moved out of the cone, then the left hand pinches "Stop Rendering Wide Motion Mode Hands" and only the red left hand remains |
| XrHandsAndControllers | tested | `meta_xrhandsandcontrollers` | | hands as above; the put down controllers show at their detached poses and the detached pose actions are active; needed `XR_META_simultaneous_hands_and_controllers` |
| XrMicrogestures | tested | `meta_xrmicrogestures` | | all five right hand microgestures via `puppetxrctl input`; the panels count one event each |
| XrCompositor_NativeActivity | n/a | | | Android NativeActivity only |
| XrSpaceWarp | n/a | | | Android NativeActivity only |

### What linux.patch changes

Build:
- `Samples/3rdParty/CMakeLists.txt`: KTX, zlib and the OpenXR loader built from source on every
  non-Android platform (the Windows path); KTX static, without tests, tools and SSE, and
  without warnings (`-w`): it builds with `-Werror`, which every new clang warning breaks.
  `cmake/samples.cmake` passes `CMAKE_POLICY_VERSION_MINIMUM=3.5`, since KTX's CMake 2.8.12
  minimum fails on CMake 4.
- `Samples/SampleXrFramework/CMakeLists.txt`: a Linux branch defining `XR_OS_LINUX`,
  `XR_USE_PLATFORM_EGL`, `XR_USE_GRAPHICS_API_OPENGL_ES`, linking EGL and GLESv2, and
  force-including `<cstdint>`, `<cstddef>`, `<climits>`, `<cstring>`, `<algorithm>`,
  `<unistd.h>` (the sources rely on transitive includes of the Android and MSVC toolchains); the same for `samplecommon_gl`.
- `Samples/XrSamples/*/CMakeLists.txt`: the Windows `elseif(WIN32)` branch is the non-Android
  `else()`, so every sample becomes an executable with its assets next to it.

Platform (framework):
- `OVR_LogUtils.h`: Linux logs to stderr through the Windows macros instead of folly.
- `Render/Egl.c`, `Egl.h`: the Android EGL path on Linux, without
  `EGL_ANDROID_native_fence_sync`; configs need only `EGL_PBUFFER_BIT` (no window: puppetxr's
  mirror window shows the eyes).
- `XrApp.cpp`, `XrApp.h`: GLES headers, `XR_MNDX_egl_enable` requested,
  `XrGraphicsBindingEGLMNDX`, a `LinuxMainLoopContext` that quits on SIGINT and SIGTERM, `Run()`.
- `OVR_FileSys.cpp`: `apk://` maps to the executable's directory (`/proc/self/exe`), as on Windows.
- `OVR_MappedFile.cpp`: the POSIX implementation on Linux.
- `Render/Framebuffer.h` and each sample's platform header (`xr_helper.h`, `XrHelper.h`,
  `SimpleXrInput.h`, `*Xr.h`, `*Gl.h`): an `XR_OS_LINUX` include branch for EGL and GLES 3.

Standalone samples (XrPassthrough, XrPassthroughOcclusion, XrSceneModel, XrSceneSharing,
XrSpatialAnchor):
- `*Xr.cpp`: pbuffer-only EGL configs, `XR_MNDX_egl_enable` requested, `XrGraphicsBindingEGLMNDX`.
- `*Gl.cpp`: `eglGetProcAddress` on Linux.
- XrPassthrough.cpp: `XRPASSTHROUGH_FRAMES_PER_MODE` overrides the 400 frames per mode, so the
  test reaches all six modes in 220 frames instead of 2200 on llvmpipe.

App fixes:
- XrPassthroughOcclusionGl.cpp: shaders are `#version 310 es` on Linux; they use
  `layout(binding)` on a sampler, which ES 3.0 allows only with
  `GL_ARB_shading_language_420pack` (Quest drivers have it, Mesa's GLES does not).
- XrPassthroughOcclusion.cpp: the depth swapchain is destroyed before its provider. It is a
  child of the provider, so destroying the provider destroys it, and the app aborted on the
  `XR_ERROR_HANDLE_INVALID` that followed.
- XrDynamicObjects main.cpp: the passthrough layer gets `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`.
  The app leaves `flags` 0, which the spec forbids (`flags` is not optional in the registry, so
  it must not be 0); Quest accepts it, puppetxr fails `xrEndFrame` with
  `XR_ERROR_VALIDATION_FAILURE`. Seen once puppetxr offered `XR_FB_triangle_mesh`, which turns the
  sample's passthrough on.

## Godot XR demos

`godotengine/godot-demo-projects` tag 4.7-6ad6167 (`6ad6167e0577fe3622c18546138f456b107ce93c`),
the demos released with Godot 4.7, on the official Godot 4.7.2 Linux build (SHA-256 pinned in
`cmake/samples.cmake`). Patch `tests/samples/godot_demos/quit_on_stop.patch`: each demo's
`start_vr.gd` quits when its session stops, so `puppetxrctl exit` ends a run. Every demo is a test
per renderer: `godot_<demo>_gl` (compatibility, OpenGL 3.3 over GLX) and `godot_<demo>_vk`
(mobile, Vulkan). All request OpenXR 1.0.

| demo | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| openxr_composition_layers | tested | `godot_composition_layers_*` | | cylinder and quad layers; the right pointer rests on "Press this button", the trigger presses it (`pressed`) |
| openxr_hand_tracking_demo | tested | `godot_hand_tracking_demo_*` | | controllers, then both hands tracked (`hands`) and the right pinching (`pinch`); hand meshes follow `XR_EXT_hand_tracking` |
| openxr_character_centric_movement | tested | `godot_character_centric_movement_*` | | the right thumbstick walks the player forward; the shot waits until the player stopped (`walked`): Godot on MoltenVK now and then draws a frame with the uniforms of the frame before, which only a moving view shows |
| openxr_origin_centric_movement | tested | `godot_origin_centric_movement_*` | | as above |
| openxr_passthrough | tested | `godot_passthrough_*` | | living_room; A toggles between the scene and passthrough (`toggled`) |
| openxr_spectator_view | tested | `godot_spectator_view_*` | | still; its spectator viewport renders into the scene |
| openxr_render_models | tested | `godot_render_models_*` | | Godot draws both controllers from `XR_EXT_render_model`; the right one is held up (`model`), then the trigger and A press its nodes in (`pressed`: the button sinks) |
| openxr_binding_modifier_demo | tested | `godot_binding_modifier_demo_*` | | the head leans over the two panels; both triggers ramp up and down in steps of 0.05 and the panels show where the click changed (left 0.50/0.45, right 0.65/0.30: the demo's analog thresholds), the right stick up and the left stick right check the dpad boxes (`thresholds_dpad`) |

References made in `tools/samples/run.sh` (docs/testing.md). Godot 4.7 creates OpenXR 1.1
instances, so `grip_surface/pose` needs no `XR_KHR_maintenance1`.

## Godot OpenXR vendors samples

`GodotVR/godot_openxr_vendors` 5.1.0-stable (`7afcd397e6206d9751b82100559ff39a93a8f59e`) with
the released addon zip of that tag (SHA-256 pinned in `cmake/samples.cmake`), on the same Godot
4.7.2. Unpatched: the build copies the addon and the repository's `demo/addons/common` into each
sample as the repository's own build does, then imports it (`tests/samples/godot_vendors/import.cmake`).
The driver passes `-- --puppetxr-automated-tests`, for Meta's XR Simulator CI: the samples then quit
when their session stops and stop their animations. Each runs on the renderer its project picks:
`vendors_<sample>_gl` (compatibility over GLX) or `_vk` (mobile over Vulkan).

| sample | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| demo | tested | `vendors_demo_gl` | | controllers from `XR_FB_render_model` (`controllers`), then both hands from `XR_FB_hand_tracking_mesh` (`hands`) |
| meta-scene-sample | tested | `vendors_meta_scene_gl` | | living_room; `OpenXRFbSceneManager` puts a labelled box or plane on every room entity (`scene`), Y turns passthrough off (`virtual`). The plugin creates its environment depth provider on Android only, yet draws the occlusion mesh, which then hides all virtual content; B turns it off first |
| meta-passthrough-sample | tested | `vendors_meta_passthrough_gl` | | living_room; the right ray clicks Mono Map, `XR_FB_passthrough`'s mono to RGBA filter (`mono_map`) |
| meta-hand-tracking-sample | tested | `vendors_meta_hand_tracking_gl` | | hands tracked (`hands`), the right index pinching lights its bar and pinch signal (`pinch`) |
| meta-composition-layers-sample | tested | `vendors_meta_composition_layers_gl` | | still; cylinder, quad and equirect layers |
| meta-space-warp-sample | tested | `vendors_meta_space_warp_gl` | | A turns `XR_FB_space_warp` off, the panel says so (`disabled`) |
| performance-metrics-sample | tested | `vendors_performance_metrics_vk` | | living_room; the HUD lists the `XR_META_performance_metrics` counters (`metrics`) |
| body-tracking-sample | tested | `vendors_body_tracking_gl` | `XR_FB_body_tracking`, `XR_META_body_tracking_full_body` | still; the avatar stands in T-pose |
| hybrid-app-sample | not tested | | | Android hybrid app mode; its model turns forever, so it never holds still |
| androidxr-*, magic-leap-* | not tested | | `XR_ANDROID_*`, `XR_ML_*` | other vendors' extensions |

`XR_FB_render_model`: the plugin asks for glTF subset 2 only, so puppetxr offers its controller
models at subsets 1 and 2 (docs/openxr.md).

## Unity MRUK samples

`oculus-samples/Unity-MRUtilityKitSample` at the 207.0.0 release (`8b6daeb6e592e30faa9d2c2ab423a1e3d5bb1136`;
Unity 6000.0.66f2, MRUK, Meta XR Core and Interaction SDK 207, Unity OpenXR), unpatched but for
one added script (`PuppetxrFixedStep.cs`: fixed game time and random seed), built
into a Windows player that draws with D3D11 (docs/testing.md, Unity samples). OVRPlugin runs on
puppetxr only as `Oculus`, which puppetxr reports when OVRPlugin is loaded (docs/config.md, Known modules). MRUK loads `living_room` from puppetxr's scene: a room
anchor and 20 scene anchors, discovery result 0. Each test
starts at the start menu, clicks its scene's button with the right controller's ray (aimed a third
of a button below its centre, where the UI cursor lands on it) and waits until the menu is gone, as
scenes load in wall-clock time.

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| start menu | tested | `unity_mruk_menu` | | the scene list over passthrough |
| MRUKBase | tested | `unity_mruk_mrukbase` | | the room's effect mesh over passthrough: outlined walls, floor grid, the TV and its stand in boxes |
| KeyboardTracker | tested | `unity_mruk_keyboardtracker` | | a keyboard prop tilted towards the head (`XR_META_dynamic_object_keyboard`): the log reports the keyboard and the sample draws its model there, over a virtual sky |
| BouncingBall | tested | `unity_mruk_bouncingball` | | the effect mesh; no ball thrown yet |
| DestructibleMesh | tested | `unity_mruk_destructiblemesh` | | the destructible mesh drawn red over the room's surfaces (`scene`), then the trigger destroys the TV's segment and its wireframe goes (`destroyed`). The sample casts along the right controller's forward axis, its grip, not the aim ray; B removes the whole mesh and A adds it again. It needs a Quest 3 or 3S, which the default preset is |
| EnvironmentPanelPlacement | tested | `unity_mruk_environmentpanelplacement` | | a panel placed on the TV by the environment raycast |
| FindFloorZone | tested | `unity_mruk_findfloorzone` | | the instructions with a looping video |
| FindMultiSpawn | tested | `unity_mruk_findmultispawn` | | spawned objects in the room that move about, and the instructions |
| NavMesh | tested | `unity_mruk_navmesh` | | the instructions |
| QRCodeDetection | tested | `unity_mruk_qrcodedetection` | | a QR prop facing the head (`XR_EXT_spatial_marker_tracking`): the sample decodes its text, draws its axes and outline, and shows it tracked |
| PassthroughRelighting | tested | `unity_mruk_passthroughrelighting` | | Oppy from above, his glow and shadow relighting the floor (`oppy`), drawn over the coffee table from low behind it (`behind_table`, no Depth API on this loader) and from above (`over_table`), walked forward and right with the left stick (`walked`, `walked_right`) |
| SpaceMap | tested | `unity_mruk_spacemap` | | the space map's textured terrain under the effect mesh |
| VirtualHome | tested | `unity_mruk_virtualhome` | | the virtual room built on the room's anchors, with furniture and its instructions |

The same sample on Unity's Oculus XR plugin (`unity_mruk_oculus`, docs/testing.md): every scene
above is tested there as `unity_mruk_oculus_<scene>` and looks as on OpenXR, except

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| DestructibleMesh | tested | `unity_mruk_oculus_destructiblemesh` | | OVRPlugin reads the trigger on its own time, so the test holds it 30 frames |
| PassthroughRelighting | tested | `unity_mruk_oculus_passthroughrelighting` | | Oppy relit (`oppy`); from low behind the coffee table the Depth API hides him behind it (`behind_table`), from above he shows (`over_table`); no walk, OVRPlugin reads the stick on its own time |

## Unity Shared Spatial Anchors sample

`oculus-samples/Unity-SharedSpatialAnchors` at the 207.0.0 release (`71b653caac743b29942a2b4f942320b7b8847972`;
Unity 6000.0.66f2, Meta XR Core and Platform SDK 207, Unity OpenXR), built with Mono, runs as
`Oculus` with `accept_time_zero`, as puppetxr sets for OVRPlugin. `tests/samples/unity_ssa/sample_tests.patch` enables the
colocation buttons when Meta's platform service does not start, as the sample does when its login
fails.

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| Colocation & Group Sharing | tested | `unity_ssa_colocation`, `unity_ssa_oculus_colocation` (Oculus XR plugin, the same patch) | | two players: the host advertises (`XR_META_colocation_discovery`), places an anchor and shares it with its group; the peer discovers and joins the group, which loads the anchor (`XR_META_spatial_entity_group_sharing`); both show it (`host_anchor`, `peer_anchor`) |
| Basic Sharing | not tested | | | shares through Photon, which needs an app id and the network |

On this loader OVRPlugin needed from puppetxr its non-registry Touch Plus and Pro binding paths and
32-bit float depth with stencil swapchains (docs/openxr.md).

What the samples needed from puppetxr: semantic label counts without the terminator (with it MRUK
recognized no label, drew the global mesh with its wall effect over the whole view and built
none of the spawn, destruction or virtual room content); the runtime name; `puppetxrctl point` with `grip -60`, as
OVRPlugin's UI ray is the grip turned 60 degrees down and clicks on the right trigger's release
over a button; and layers naming a space destroyed during the frame (docs/openxr.md, Frame
loop), as MRUKBase's Stage origin makes Unity replace its app space every frame (TODO step 12).

## Unreal MRUK sample

`oculus-samples/Unreal-MRUtilityKitSample` at `5.8.0-v207` and the commit after it
(`3abad9230d4c8194c11713e2779915c9b9d415de`), on Meta's Unreal fork (`oculus-5.8.0-release-1.207.0`),
run uncooked by the editor on Unreal's OpenXR plugin, as `puppetxr` (no OVRPlugin; MRUK's shared library
is a known module, docs/config.md). MRUK loads `living_room` and the start level dresses it in its own
walls, tiled floor and dark ceiling. Each map test starts at the start level and opens its map with
`-ExecCmds=open`, as the menu's Maps tab does: the start level begins the persistent passthrough that
DistanceMap, Navmesh, RandomPlacement and SceneDecoration keep using; opened on their own they draw
over no passthrough, so black. `tests/samples/unreal_mruk/sample_tests.patch` binds the pawn's menu
timer weakly (`BindWeakLambda`): it fired 0.3 s after `BeginPlay` into a pawn that a level change in
that time had destroyed, and crashed; it also adds the console command `puppetxr.VRPreview` to the
sample's editor module, which starts a VR Preview as the toolbar's button does, for which the editor
has no command or script API.

`unreal_mruk_vr_preview` runs the editor as a user does, a window, real time, not paused: a startup
script of the Python plugin (`UNREAL_EDITOR_SCRIPT`) starts and stops the preview and quits when
the test leaves a file of that name. The editor comes to the front as with a click on its toolbar, the
preview starts and puppetxr's window must come in front with the dressed room; the preview stops and
the window must go; a second preview, closed through puppetxr's window (Unreal ends the preview when
the runtime exits the session), the same, and a third, which a closed window used to end at once;
then the editor quits, with no failed OpenXR call in its log.

The menu is clicked with the right controller as the Interaction SDK's ray does on Quest: it casts
along the aim pose (`IsdkXRUtils.h`, 0.7 cm behind it) and hides the ray while the hand is at the eye,
so `unreal_mruk_click` points from half a metre ahead (`puppetxrctl point ... ahead 0.5`). The SDK
hovers a widget some frames after the hand arrives and selects only while hovering, so the click
waits 30 frames before the trigger and moves the hand off the panel after it. The tabs take clicks
on their icons, not their labels.

| map | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| start level | tested | `unreal_mruk_menu` | | the menu in the dressed room with the room's name |
| core demos | tested | `unreal_mruk_core_demos` | | each entry of the menu's drop-down picked with the controller ray; the panel describes it |
| Maps tab | tested | `unreal_mruk_maps_tab` | | the tab, its map list and Load map open Guardian |
| VR Preview | tested | `unreal_mruk_vr_preview` | | the editor's VR Preview three times: stopped, closed through puppetxr's window, stopped again; checked, not compared |
| DestructibleMesh | tested | `unreal_mruk_destructiblemesh` | | segments in random colours over a sky; checked for being lit and the menu, no reference. The walls break into segments, the floor is the reserved one; the trigger, aimed left of the TV, destroys a segment, checked against how much the scene changes by itself. Meta's `AVRPawn` sets `InputComponent` in its constructor, so Unreal never binds the pawn's input events (`IA_Select`, the menu button) on any runtime; the sample's patch leaves it unset |
| DistanceMap | tested | `unreal_mruk_distancemap` | | the distance field in rainbow colours on the floor |
| EnvironmentRaycast | tested | `unreal_mruk_environmentraycast` | | the raycast surfaces as checkers |
| Guardian | tested | `unreal_mruk_guardian` | | the menu over the dressed room; no boundary drawn yet |
| KeyboardTracker | tested | `unreal_mruk_keyboardtracker` | | the keyboard prop (`PUPPETXR_PROPS`) tracked, drawn as a passthrough cut-out, black over the sky |
| Navmesh | tested | `unreal_mruk_navmesh` | | the navmesh controls; Oppy moves on his own, so checked like DestructibleMesh |
| PassthroughCameraAccess | tested | `unreal_mruk_passthroughcameraaccess` | `XR_METAX1_passthrough_camera_data` | the left camera's image of the room on a screen in front of the head, which follows the camera's pose at the image's capture time |
| PTRL | tested | `unreal_mruk_ptrl` | | the passthrough relighting panel |
| QRCodeTracker | tested | `unreal_mruk_qrcodetracker` | | the QR code prop (`PUPPETXR_PROPS`) tracked, its marker and axes drawn over it |
| RandomPlacement | tested | `unreal_mruk_randomplacement` | | the placement panel, a gizmo at a random place; checked like DestructibleMesh |
| SceneDecoration | tested | `unreal_mruk_scenedecoration` | | grass and rocks on the floor and the walls from its PCG graph |

What the sample needed from puppetxr: `xrWaitFrame` blocking while an older frame is still rendered,
early discovery for MRUK's shared library loaded after the instance, frames read back in
`xrEndFrame` for screenshots (docs/openxr.md, Frame loop) and an `puppetxrctl shell` that waits for and
follows the app across its two instances.

## Unreal Passthrough sample

`oculus-samples/Unreal-PassthroughSample` (`abe38260a14f5ecde1dc9482896a2ca6ec413b0c`, UE 5.8, Meta XR
plugin v207), Blueprint only, on Meta's Unreal fork on Unreal's OpenXR plugin. Each level is opened
directly (`-ExecCmds=open`) and drawn by the mobile renderer it targets (`-FeatureLevelES31`): its
masks write only alpha (translucent materials with `bWriteOnlyAlpha`), which the desktop renderer
ignores and draws black. Unreal submits the passthrough layer below a projection layer with
source, unpremultiplied and inverted alpha (`XR_EXT_composition_layer_inverted_alpha`).

| level | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| PTStartup | tested | `unreal_passthrough_startup` | | Oppy over passthrough (`level`); the left menu button opens the navigation menu on the left hand (`menu`) |
| PTBackground | tested | `unreal_passthrough_background` | | virtual objects over passthrough |
| PTStyles | tested | `unreal_passthrough_styles` | | the styled passthrough (grayscale-to-colour map) and its widget (`level`); the colour map curves picked with the controller ray: a colour curve (`curve_colour`), grayscale (`curve_gray`), none (`curve_none`); the edge colour's alpha slider dragged to 0.42 draws white edges (`edges`). The pawn's ray is the Interaction SDK's (`unreal_mruk_click`); sliders take a drag, not a click |
| PTColorMap | tested | `unreal_passthrough_colormap` | | the 256 entry colour map and its editor widget |
| PTColorLut | tested | `unreal_passthrough_colorlut` | `XR_META_passthrough_color_lut` | the LUT widget at weight 0. Meta's plugin destroys the LUTs at exit after the HMD went, through OVRPlugin's wrapper, which this project never loaded: a null call and exit code 3 after the test, accepted |
| PTColorScaleAndOffset | tested | `unreal_passthrough_colorscaleandoffset` | | the animated colour scale and offset (green at the shot) and its widget |
| PTMasked | tested | `unreal_passthrough_masked` | | a circle on the right controller shows passthrough through the virtual room (`right`); A moves it to the left controller (`left`), where it shows no passthrough yet (TODO.md) |
| PTMaskedBrush | tested | `unreal_passthrough_maskedbrush` | | the level, then a stroke painted with the right trigger held while the controller sweeps, which shows passthrough (`stroke`); the level Blueprint's `IMC_Brushes` (priority 3) wins the trigger over the pawn's `IMC_IsdkHand` (1) |
| PTLayerResumedEvent | tested | `unreal_passthrough_layerresumedevent` | `XR_META_passthrough_layer_resumed_event` | black at the start; Show Passthrough clicked with the controller ray creates a layer and fades passthrough in (`shown`), Hide fades it out (`hidden`) |

## Unreal Occlusion sample

`oculus-samples/Unreal-OcclusionSample` (`e5e0f5865b9c6fae5883c08d0e15eb038c190893`), Blueprint only,
on OVRPlugin's OpenXR backend (`XrApi=OVRPluginOpenXR`), mobile renderer. Its lights are static and
the repository has no built lighting, so `cmake/samples.cmake` bakes the map with Lightmass after
checkout. Two quads show the environment depth (`XR_META_environment_depth`) in colour.

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| modes | run, not compared | `unreal_occlusion_modes` | | the room, the depth quads; every virtual object (cubes, Oppy, the UI, the controllers) is near black from a bug in Meta's plugin (below). The modes are a combo box on a poke panel (`IsdkInteractableWidget`), not A. The panel stands low and tilted in front of the player (stage about (0, 0.8, -0.4) m); a controller pushed there pokes it (hover, then select), picking a mode is not scripted yet |
| hands | run, not compared | `unreal_occlusion_hands` | | tracked hands; needed puppetxr's hand mesh to skin only to joints of Meta's skeleton (OVRPlugin crashed building it) |
| look around | run, not compared | `unreal_occlusion_explore` | | the four directions and the floor, for where things are |

Meta plugin bug (OculusXRHMD, Windows): with `r.PostProcessing.PropagateAlpha=True`, which this
sample and the Passthrough, MRUK and Shared Anchors samples set, every pixel of the eye buffer is
decoded from sRGB twice and virtual content arrives near black (a grey of 0.353 as 0.102).
`FLayer::Initialize_RenderThread` (`OculusXRHMD_Layer.cpp`) creates `InvAlphaTexture` without the
eye swapchain's `TexCreate_SRGB`; `InvertTextureAlpha_RenderThread` draws the eye texture through
an sRGB view into it through a UNORM view, then copies the bytes back. Found with RenderDoc
(`-AttachRenderDoc`, `renderdoc.CaptureFrame`): the environment depth, `DepthViewProj`, the
min/max texture and the occlusion result (alpha) are right. Fix: give `InvAlphaTexture` the
swapchain's `TexCreate_SRGB`. Turning `PropagateAlpha` off is no workaround: OVRPlugin then
submits the inverted alpha without `XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT` (it does not
enable `XR_EXT_composition_layer_inverted_alpha`, which puppetxr offers), so passthrough shows only
where virtual content is.

## Unreal Shared Anchors sample

`oculus-samples/Unreal-SharedAnchorsSample` (`56a2f986ebfa54383486ac6e3b206041f0d15a14`), C++ (its editor
target is built), mobile renderer. `tests/samples/unreal_shared_anchors/sample_tests.patch` makes the
bundled Meta platform plugin's `Platform_GetLoggedInUserID` return `-puppetxr-user-id=<n>`, which the
tests pass as each player's `PUPPETXR_USER_ID`: the sample shares with its players' Meta accounts,
and the tests exercise puppetxr's sharing with users, not Meta's platform service. The patch also adds
the `puppetxr.VRPreview` console command, as in the MRUK sample.

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| lobby | tested | `unreal_shared_anchors_lobby` | | passthrough, the menu on the right controller and its ray |
| session and anchor | tested | `unreal_shared_anchors_session` | `XR_FB_spatial_entity` | the menu held up before the eyes (`lobby`); the trigger picks Create Session, which opens the shared scene and its menu with the anchor preview (`session`); Create Anchor makes one (`xrCreateSpatialAnchorFB`, `Success`, `anchor`) that stays where it was made when the hand leaves (`placed`). The thumbstick moves the selection (`input right thumbstick 0 -1`) |
| sharing between two players | tested | `unreal_shared_anchors_sharing` | `XR_FB_spatial_entity_sharing`, `XR_FB_spatial_entity_user`, `XR_META_spatial_entity_persistence` | a host (user 1) and a peer (user 2): the host creates a session, the peer finds and joins it (Unreal's Null subsystem over LAN); the host creates an anchor and selects it with Select Anchor (the right controller's forward axis, `point ... grip 0`, on the anchor, then the trigger), saves it (A, `xrSaveSpacesMETA`) and shares it (Share Anchors(s), the second entry once saved: `xrShareSpacesFB` with space users 1 and 2); the peer queries it (`xrQuerySpacesFB`) and shows it (`loaded`). Both run freely, as Unreal's sessions and replication time out in wall-clock time: the shots are not compared, the API trace decides (the peer's space user, the share's result, the host's anchor uuid in the peer's query results) |

## Unreal Interaction SDK sample

`oculus-samples/Unreal-InteractionSDK-Sample` (`de94c3674d48f40ab61fd11dc30449ebf5243f61`, Meta XR plugin
v208), C++, on D3D12 and the mobile renderer (the desktop one lights differently from run to run).
Unreal fails an assert in a mobile post-process material pass at exit, after the shots, which the
tests accept (`exit_codes`). Each level is opened directly; the tests look in the four directions
and down at the level's table, after 600 frames: the levels light their fog over the first
seconds, as far as the load took in wall-clock time. Settled, runs differ by up to 3 levels in the
fog and a few small widgets (a torch, a card's icon) animate in wall-clock time, so the tests
allow tolerance 3 on up to 2000 pixels.

| level | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| StartLevel | tested | `unreal_isdk_startlevel` | | the lobby and its table |
| PokeExamples | tested | `unreal_isdk_pokeexamples`, `unreal_isdk_poke` | | the poke buttons; the right controller pushed along the eye ray onto the big red button pushes its cap into the base (`pressed`), from where it springs back (`released`); the script counts the cap's red pixels, under a quarter of `before` while pressed and back after. The label under it reads "Large Button" throughout; at 640x672 the mobile renderer draws it as a few dots. The poke point is the controller's, not the ray's: aim at a target large enough |
| RayExamples | tested | `unreal_isdk_rayexamples` | | the ray panels |
| DistanceGrabExamples | tested | `unreal_isdk_distancegrabexamples` | | the map and the objects to grab |
| ThrowExamples | tested | `unreal_isdk_throwexamples` | | the targets and the objects to throw |
| GazeExamples | tested | `unreal_isdk_gazeexamples` | | the gaze panels |
| HandPoseStudio | tested | `unreal_isdk_handposestudio` | | the posed hands and the studio's panels |
| TransformerExamples | not tested | | | the plugin's hand grab pose asks for the editor's world under `-game` and crashes |

Unreal's render thread asks `xrGetRecommendedLayerResolutionMETA` for the frame it submits, older than
the frame the game thread already waited for; puppetxr answers `XR_ERROR_TIME_INVALID` as the extension
says, which Unreal logs as an ensure (TODO.md).

## Unreal Movement sample

`oculus-samples/Unreal-Movement` (`8d7ede9e9864a0f0940097386dcaa9eb86df06a6`, the commit after
`5.8.0-v207`, which lacks the prebuilt `MetaMovementSDK_Utility` library it links), C++, on D3D12
and the desktop forward renderer its settings ask for (`r.ForwardShading`, instanced stereo). It
tracks the upper body at low fidelity, face and eyes. OVRPlugin's session instance is named "Unreal Engine" (its probe
instance before it "MovementSample"). The tests run `DisableAllScreenMessages` first, as the
engine prints its shader progress and stereo mode into the frame. The build runs the game once
after the editor target (`WARM`, on headless puppetxr): the tests compile shaders synchronously, and
on global shaders not cached yet that fails an assert in the engine's shader compiler; Meta's
movement module asserts without an XR system, so `-nohmd` does not do
(`docs/meta-reports/unreal-samples.md`). The sample's twist component logs an invalid bone name
every frame.

| map | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| MAP_HighFidelity | tested | `unreal_movement_body` | | the mirrored character retargets puppetxr's upper body standing with the controllers (`standing`), with a T-posed left arm and the right controller raised (`posed`), and with a 1.4 times longer right upper arm, which reaches higher (`longer_arm`) (`docs/body-tracking.md`) |
| MAP_Aura | tested | `unreal_movement_face` | | Aura shows puppetxr's face neutral without blinks (`neutral`), smiling (`smile`), the eyes turned left (`look_left`, only the eye band differs from `neutral`), shut with the jaw open (`closed_open`) (`docs/face-tracking.md`) |
| MAP_HighFidelity_AnimBlueprint, MAP_RetargetMannequinAnimBlueprint | not tested | | | the same tracking through animation blueprints |

## Unity Movement SDK

`oculus-samples/Unity-Movement` v207.0.0 (`77274f138974087ba2909bab2723fc6ff851338b`), the
`com.meta.xr.sdk.movement` package, in the Starter Samples' project (below) for its Core SDK. The
build clones the package into the project's `Packages` folder (`EMBED` in `cmake/samples.cmake`):
Unity's own git fails on macOS, its x86_64 helper finding no `xcrun`. The patch
(`tests/samples/unity_movement/sample_tests.patch`) asks for full body tracking at high fidelity
in `OculusRuntimeSettings`, declares body, face and eye tracking in `OculusProjectConfig`, enables
Unity's Eye Gaze Interaction feature, adds the Interaction SDK (`com.meta.xr.sdk.interaction.ovr`)
the scene menu needs, and leaves the package's Body Tracking and Face Tracking Samples' scenes the
only ones of the player. `<package>/.` in `PACKAGE_SAMPLES` imports the files at the root of the
package's `Samples~` too, the menu's scripts and assembly. Once the menu ran, the player takes up to
a minute to quit: the Interaction SDK's native telemetry thread sleeps out its interval before
its join at exit (`docs/meta-reports/unity-movement.md`). The Windows player (D3D11) compares
against `references/`, the macOS one against `references_macos/`. In both desktop players the face
scene's head shows dark patches on its skin, larger on Windows; they are the same with every face
and eye tracking extension hidden (`PUPPETXR_DISABLE_EXTENSIONS`), so they come from the sample's skin
material in a desktop player, not from the tracked face.

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| MovementBody | tested | `unity_movement_body` | | the mirror avatar retargets puppetxr's body standing with the controllers (`standing`), with a T-posed left arm and the right controller raised (`posed`), every bone connected, and with a 1.4 times longer right upper arm (`longer_arm`, proportions no headset reports) (`docs/body-tracking.md`) |
| scene menu | tested | `unity_movement_gaze` | | gaze and pinch (`UIGazeAndPinch`, `XR_EXT_eye_gaze_interaction`): the gaze turned onto the Face button (`aim`), a pinch of the tracked right hand loads the face scene (`face_scene`) |
| MovementFace | tested | `unity_movement_face` | | opened with `-puppetxr-scene`: the characters show puppetxr's face neutral without blinks (`neutral`), smiling (`smile`), the eyes turned left (`look_left`), shut with the jaw open (`closed_open`) (`docs/face-tracking.md`) |

## Unity Starter Samples

`oculus-samples/Unity-StarterSamples` (`a2ba45d7206dcf21a08af7a0ee9bf7768689579f`, Unity 6000.0.66f2,
Meta XR Core SDK 207, Unity OpenXR), with the Core SDK's own Sample Scenes and Haptics sample
built in (`PACKAGE_SAMPLES`, docs/testing.md). Each scene is opened directly with `-puppetxr-scene`
(`tests/samples/unity/PuppetxrFixedStep.cs`); the tests look in the four directions and down, or
play the scene's interaction.

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| StartScene | tested | `unity_starter_startscene` | | the scene list |
| Passthrough | tested | `unity_starter_passthrough` | | passthrough with its selective and projected shapes |
| EnableDisablePassthrough | tested | `unity_starter_enabledisablepassthrough` | | the virtual sky, passthrough off |
| EnableDisablePassthrough toggled | tested | `unity_starter_passthrough_toggle` | `XR_META_passthrough_layer_resumed_event` | A pressed three times: the first asks for the sky it already shows (the scene starts as if it had just switched to passthrough, `first_press`), the second turns the layer on and fades the sky away once the resumed event arrives (`on`), the third fades it back (`off`) |
| OVROverlay | tested | `unity_starter_ovroverlay` | | compositor quad layers and their mode menu |
| OVROverlay modes | tested | `unity_starter_ovroverlay_modes` | | the menu clicked with the controller ray: the panels as compositor quads (`ovroverlay`), drawn by the app (`application`), none (`none`), and Simulate Level Load's field of cubes (`level_load`) |
| OVROverlayCanvas | tested | `unity_starter_ovroverlaycanvas` | | canvases as overlay quads; some labels read mirrored, to be checked against a device (TODO.md) |
| OVROverlayCanvas_Text | tested | `unity_starter_ovroverlaycanvas_text` | | text charts as overlays |
| OVROverlayCanvas_RenderOnChange | tested | `unity_starter_ovroverlaycanvas_renderonchange` | | a canvas redrawn only on change |
| SpatialAnchor | tested | `unity_starter_spatialanchor` | | passthrough and the anchor menu |
| SpatialAnchor placed | tested | `unity_starter_anchor` | `XR_FB_spatial_entity` | the menu on the right controller (`menu`); the trigger picks Enter Create Mode and shows the placement gizmo (`create_mode`); A places an anchor (`xrCreateSpatialAnchorFB`) that stays with its label when the hand leaves (`anchor`) |
| SpatialAnchor menu | tested | `unity_starter_anchor_menu` | `XR_META_spatial_entity_persistence`, `XR_META_spatial_entity_discovery` | after placing, the trigger returns to select mode; the ray on the anchor highlights it (`hover`), A opens its menu (`selected`); Save stores it (`xrSaveSpacesMETA`, the store holds one anchor, `saved`), Hide removes it (`hidden`), the main menu's Load Anchors brings it back from the store (`xrDiscoverSpacesMETA`, `loaded`), Erase removes it from the store (`xrEraseSpacesMETA`, empty, `erased`) |
| CustomHands | tested | `unity_starter_customhands` | | the custom hand models |
| CustomControllers | tested | `unity_starter_customcontrollers` | | the custom controller models |
| Locomotion | tested | `unity_starter_locomotion`, `unity_starter_teleport` | | the locomotion course; the right stick pushed forward shows the teleport arc (`aiming`), released it moves the player past the fence (`teleported`) |
| TouchProSample | tested | `unity_starter_touchprosample` | | the Touch Pro panels |
| HandsInteractionTrainScene | tested | `unity_starter_handsinteractiontrainscene` | | the train set and its buttons, the request to put the controllers down |
| HandMicrogestures (Core SDK) | tested | `unity_starter_microgestures` | `XR_META_hand_tracking_microgestures` | tracked hands ahead (`hands`); the right thumb swipes left and forward (`right`), the left one right and taps (`left`): each lights its arrow or select icon and is named under its hand |
| HandTest (Core SDK) | tested | `unity_starter_hands` | `XR_FB_hand_tracking_mesh` | Meta's hand meshes on tracked hands, open (`open`) and the right one pinching (`pinch`) |
| HandTrackingWideMotionMode (Core SDK) | tested | `unity_starter_handtrackingwidemotionmode` | | the Wide Motion Mode toggle |
| SimultaneousHandsAndControllers (Core SDK) | tested | `unity_starter_simultaneous` | `XR_META_simultaneous_hands_and_controllers` | Enable clicked with the ray, then the left hand tracked while the right controller stays: the panel names `RTouch, LHand` |
| ControllerDrivenHandPoses (Core SDK) | tested | `unity_starter_controller_hands` | `XR_EXT_hand_tracking_data_source` | hands drawn around the controllers, conforming to them (`conforming`), natural (`natural`), off (`off`) |
| ControllerModels (Core SDK) | tested | `unity_starter_controllermodels` | | the controller model room |
| HapticsSample (Core SDK) | tested | `unity_starter_haptics` | | Simple, PCM and Parametric Haptics each play on both controllers (`puppetxrctl haptic`); Get Parametric Properties shows what they report (`properties`) |
| Stereo180Video | not tested | | | black: off Android `MoviePlayerSample` draws the video (1920x1080 H.264, Media Foundation on D3D11, "Playing Unity VideoPlayer") on a mesh in its own projection layer instead of an equirect `OVROverlay`. The app submits that one opaque layer and every pixel of it is 0 from the first frame on, in every direction and after seconds of wall time: Unity draws nothing there |

## Unity Depth API sample

`oculus-samples/Unity-DepthAPI` (`82a8f3f6f38e0833a08dee768a7f016a3ca5c86d`), project `DepthAPI-URP`,
Unity 6000.3.10f1 (`PUPPETXR_UNITY_6000_3`), on the Oculus XR plugin, the only Windows path with the
Depth API. Each scene opened with `-puppetxr-scene`.

| scene | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| OcclusionToggler | tested | `unity_depthapi_occlusiontoggler` | `XR_META_environment_depth` | the objects and the occluded UI over passthrough |
| OcclusionToggler modes | tested | `unity_depthapi_occlusionmodes` | `XR_META_environment_depth` | the room moved 2 m towards the TV wall (`PUPPETXR_ORIGIN_OFFSET`) so that the wall stands in front of the objects; A held 30 frames cycles soft occlusion (`mode0`, the wall hides them), none (`mode1`, they show) and hard occlusion (`mode2`, hidden again); the UI panel in front stays |
| SceneAPIPlacement | tested | `unity_depthapi_sceneapiplacement`, `unity_depthapi_posters` | | the placement instructions and the poster; the right controller's ray (`RightControllerAnchor`, the aim pose) on the wall moves the poster's preview (`preview`), the trigger places a poster (`placed`), B takes it down (`cleared`) |
| HandsRemoval | tested | `unity_depthapi_handsremoval` | | the hands removal instructions |
| HandsRemoval with hands | tested | `unity_depthapi_hands` | `XR_META_hand_tracking_microgestures` | tracked hands before the eyes drawn as the virtual hand mask (`hands`); a right thumb tap switches occlusion from soft to none (`occlusion`), a left one the hands' style to none (`style`) |
| DepthMask | tested | `unity_depthapi_depthmask`, `unity_depthapi_mask` | | the mask's yellow volume; B masks the room's walls out of the depth (the landscape behind them shows, `masked`), the stick held right raises the mask bias from 0.100 to 0.233 (`bias`), B again ends the mask (`off`) |

Meta's Passthrough Camera API samples for Unity (`Unity-PassthroughCameraApiSamples`, MRUK 85) are
not tested: their player starts no OpenXR session, and MRUK 85's camera path is Android only
(`CameraGetLatestImage is only supported on Android`).

## Core apps

On the loader of OpenXR-SDK-Source release-1.1.63 (`2b99fec95e9cdf352c1a98e9cb23bf4def1cf8e6`),
built with its hello_xr and installed to `build/<preset>/samples/openxr_sdk_prefix`; Monado's
demos build against that install.

| app | status | test | requested, missing in puppetxr | notes |
|---|---|---|---|---|
| hello_xr (OpenXR-SDK-Source) | tested | `hello_xr_vulkan2`, `hello_xr_vulkan` | | still; cubes at its reference spaces and the grips. Its OpenGL window needs XFree86-VidMode, which Xvfb lacks; it quits when stdin ends, so the driver keeps it open |
| openxr-simple-example (Monado, `bf920800ff156424e2710e3ca63f84be121e62cb`) | tested | `simple_example_gl` | | OpenGL over GLX through SDL2 in the Xvfb display; a block per grip, the right trigger makes the right controller vibrate (checked through `haptic right`) |
| xrgears (Monado, `51ef6c779c8d3134d8df9eca1294779e61c8243f`) | tested | `xrgears_vk` | | still; gears in a projection layer with depth over an equirect2 sky, two quad layers. With `-1` (`XR_KHR_vulkan_enable`) it asks for external fd device extensions lavapipe lacks |

Patch `tests/samples/xrgears/sample_tests.patch`: the gears turn with the predicted display time
instead of `CLOCK_MONOTONIC`, so frames repeat. Its `PI` of 3.142 puts the sky's
`centralHorizontalAngle` past 2 pi and its latitudes past pi / 2; puppetxr clamps them and logs a
warning once (docs/openxr.md, curved layers).

## Open issues

- References are made with `--update` in the image of `tools/samples/run.sh` (Mesa 26.2.4 llvmpipe).
- With llvmpipe rasterizer threads, 64-pixel tiles of XrSceneModel come out stale, differently
  each run; the tests run with `LP_NUM_THREADS=0` (TODO step 12). XrSceneModel then still shows
  lines on every 64th row (llvmpipe's tile height), possibly the same fault. The crashes once
  blamed on threads came from llvmpipe's shader disk cache, which the tests disable.
- Godot's mobile renderer once started without its directional light in a few runs with Mesa's
  shader disk cache on; not seen since it is disabled (TODO step 12).
