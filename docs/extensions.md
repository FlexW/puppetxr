# OpenXR extensions

Every extension of the OpenXR 1.1.65 registry (`xr.xml`, 261 published) and whether
puppetxr implements it. Update a row when its extension lands, together with
`OXR_EXTENSIONS` in `src/oxr_internal.h`. Regenerate the list itself only when the
OpenXR SDK pin moves and new extensions appear.

Status: `done` advertised and tested, `partial` advertised with gaps listed in the
notes, `no` not advertised, `n/a` needs a platform or service puppetxr does not run on.

Effort, for a simulator that has to fake the hardware behind the extension:

| effort | meaning |
|---|---|
| S | advertise it, validate its structs, accept or report a fixed value; hours |
| M | new entry points or compositor work with simple simulated behaviour; days |
| L | a new simulated subsystem (hands, skeletons, passthrough, anchors, models); a week or more |
| XL | a simulated world model with discovery, snapshots and async futures; weeks |
| platform | Windows, macOS, Android or a vendor service; see TODO step 10 |

Interaction profile extensions are S because `src/oxr_profiles.inc` already validates
their bindings; their input stays unbound until a `SIM_Controller` emulates the device.
Extensions promoted to core 1.1 are covered by the core work as well.

Meta's FB and META extensions follow Horizon OS where it differs from the spec text, for behaviour
seen on a headset; the layer's doc records the difference. Core OpenXR and ratified extensions
follow the spec.

Totals: 99 done, 1 partial, 147 no, 16 n/a; effort S 105, M 88, L 43, XL 8, platform 19.

## KHR

| extension | status | effort | notes |
|---|---|---|---|
| `XR_KHR_binding_modification` | done | S | carries the dpad and analog threshold modifications; docs/openxr.md "Binding modifiers" |
| `XR_KHR_composition_layer_color_scale_bias` | done | S | applied to the unpremultiplied color, negative scale clamped to 0 |
| `XR_KHR_composition_layer_depth` | done | S | depth infos validated; the compositor uses them for XR_FB_composition_layer_depth_test |
| `XR_KHR_convert_timespec_time` | done | S | XrTime and CLOCK_MONOTONIC related at the last frame; on Windows the UTC wall clock, as the CTS reads it there and MRUK converts passthrough camera timestamps with it |
| `XR_KHR_extended_result_name_lengths` | no | S | longer result names |
| `XR_KHR_extended_struct_name_lengths` | no | S | longer structure type names |
| `XR_KHR_generic_controller` | no | S | interaction profile only |
| `XR_KHR_locate_spaces` | done | S | core in 1.1 |
| `XR_KHR_swapchain_usage_input_attachment_bit` | no | S | one more usage flag |
| `XR_KHR_vulkan_swapchain_format_list` | no | S | format list on swapchain images |
| `XR_KHR_composition_layer_cube` | done | M | faceCount 6 swapchains, sampled like a samplerCube in the layer's frame; the CTS has no visual test, gtests cover it |
| `XR_KHR_composition_layer_cylinder` | done | M | ray cast inside the cylinder, finite and infinite radius; the CTS has no visual test, gtests cover it |
| `XR_KHR_composition_layer_equirect` | done | M | ray cast inside the sphere; scale is the covered fraction, bias offsets texture coordinates |
| `XR_KHR_composition_layer_equirect2` | done | M | ray cast inside the sphere |
| `XR_KHR_maintenance1` | no | M | core in 1.1; 1.1 behaviour for 1.0 apps: grip_surface, extra results |
| `XR_KHR_visibility_mask` | done | M | lens mask with cut corners (`views.mask_radius`); hidden, visible and line loop; without the lens mask the whole view is visible (the view's rectangle, a hidden mesh without area) |
| `XR_KHR_vulkan_enable` | done | M |  |
| `XR_KHR_vulkan_enable2` | done | M |  |
| `XR_KHR_opengl_enable` | done | L | Xlib (GLX), Win32 (WGL) and EGL (`XR_MNDX_egl_enable`) bindings; GL 3.3 to 4.6; no cube arrays; mirror window (`docs/openxr.md`) |
| `XR_KHR_opengl_es_enable` | done | L | through the EGL binding of `XR_MNDX_egl_enable`; GLES 3.0 to 3.2; Android binding with Android |
| `XR_KHR_D3D11_enable` | done | platform | Windows builds; feature level 11_0; typeless swapchain textures; deferred contexts (`docs/compositor.md`) |
| `XR_KHR_D3D12_enable` | done | platform | Windows builds; feature level 11_0; typeless swapchain resources in `RENDER_TARGET` / `DEPTH_WRITE` |
| `XR_KHR_android_create_instance` | n/a | platform | Android |
| `XR_KHR_android_surface_swapchain` | n/a | platform | Android |
| `XR_KHR_android_thread_settings` | n/a | platform | Android |
| `XR_KHR_loader_init` | n/a | platform | only used on Android |
| `XR_KHR_loader_init_android` | n/a | platform | Android |
| `XR_KHR_metal_enable` | done | platform | macOS builds; the system default device; swapchain textures are 2D, 2D array or cube by their layers; also offered as `XR_KHRX2_metal_enable`, the provisional name Unity's OpenXR plugin asks for (`src/oxr_metal.cpp`, `docs/compositor.md`) |
| `XR_KHR_win32_convert_performance_counter_time` | done | platform | Windows; XrTime and QueryPerformanceCounter related at the last frame |

## EXT

| extension | status | effort | notes |
|---|---|---|---|
| `XR_EXT_composition_layer_inverted_alpha` | done | S | alpha inverted before blending; the flag is invalid without the extension |
| `XR_EXT_enumerate_swapchain_formats2` | no | S | swapchain formats per view configuration |
| `XR_EXT_hand_joints_motion_range` | done | S | conforming curls the fingers of a hand holding its controller around it |
| `XR_EXT_hand_tracking_data_source` | done | S | unobstructed is a tracked hand, controller is the hand around a held controller |
| `XR_EXT_hp_mixed_reality_controller` | no | S | core in 1.1; interaction profile only |
| `XR_EXT_interaction_profile_battery_state_display` | no | S | fixed battery state per device |
| `XR_EXT_loader_init_properties` | no | S | accept loader properties |
| `XR_EXT_local_floor` | done | S | core in 1.1 |
| `XR_EXT_palm_pose` | no | S | core in 1.1; palm_ext/pose and grip_surface/pose in the simulated hand |
| `XR_EXT_performance_settings` | no | S | accept levels, never send notifications |
| `XR_EXT_samsung_odyssey_controller` | no | S | core in 1.1; interaction profile only |
| `XR_EXT_spatial_plane_semantic_label2` | no | S | on EXT_spatial_plane_tracking |
| `XR_EXT_stationary_reference_space` | done | S | the LOCAL origin at sim creation, never recentered; one constant generation id (no tracking loss); `docs/openxr.md` |
| `XR_EXT_thermal_query` | no | S | fixed nominal thermal state |
| `XR_EXT_user_presence` | no | S | presence events, toggled from the UI and the control API |
| `XR_EXT_uuid` | no | S | core in 1.1; types only |
| `XR_EXT_view_configuration_depth_range` | no | S | fixed depth range |
| `XR_EXT_view_configuration_views_change` | no | S | event struct |
| `XR_EXT_active_action_set_priority` | no | M | priority masking of shared inputs in xrSyncActions |
| `XR_EXT_app_consumed_swapchain` | no | M | swapchains the app reads back |
| `XR_EXT_conformance_automation` | done | M | drives the simulated controllers, takes over from keyboard and mouse; runs the interactive CTS input tests unattended |
| `XR_EXT_debug_utils` | no | M | messengers, object names, labels; the loader already handles part of it |
| `XR_EXT_dpad_binding` | partial | M | dpad directions on thumbsticks with center region, wedges and sticky latching; trackpad dpads (`dpad_center`, force) are unused as no emulated controller has a trackpad; only `XrHapticVibration` plays |
| `XR_EXT_eye_gaze_interaction` | done | M | `/interaction_profiles/ext/eye_gaze_interaction` on `/user/eyes_ext`: the gaze pose between the simulated eyes, along their gaze (the `eyes` profile turns it with the mouse); active while the head is tracked; `XrEyeGazeSampleTimeEXT` on the gaze action space's location (`docs/face-tracking.md`) |
| `XR_EXT_frame_synthesis` | no | M | accept motion vector layers, compose without synthesis |
| `XR_EXT_future` | done | M | futures are ready as soon as they exist, scoped to their session |
| `XR_EXT_hand_interaction` | done | M | `ext/hand_interaction_ext` on tracked hands (pinch and grasp from trigger and squeeze); pinch, poke and palm poses on the controller profiles too |
| `XR_EXT_haptic_parametric` | done | M | amplitude and frequency curves sampled at sim time, streams of frames; also the `XR_EXTX1_haptic_parametric` preview; transients ignored; a rejected last frame still ends its stream (the CTS relies on it) |
| `XR_EXT_interaction_render_model` | done | M | a model per hand holding a controller with bound actions, listed at `xrSyncActions` with a change event; new ids for another controller; subaction and top-level paths of its hand; docs/openxr.md "Render models" |
| `XR_EXT_spatial_anchor` | no | M | on EXT_spatial_entity |
| `XR_EXT_spatial_container_self_rendering` | no | M | on EXT_spatial_container |
| `XR_EXT_spatial_entity_archetypes` | no | M | on EXT_spatial_entity |
| `XR_EXT_spatial_persistence` | no | M | persist entity ids in a local store |
| `XR_EXT_spatial_persistence_operations` | no | M | on EXT_spatial_persistence |
| `XR_EXT_hand_tracking` | done | L | 26 joints per hand from a procedural skeleton: grip pose, pinch from trigger, grab from squeeze; velocities from the hand's |
| `XR_EXT_plane_detection` | no | L | deprecated by XR_EXT_spatial_plane_tracking |
| `XR_EXT_render_model` | done | L | the procedural controllers of `XR_FB_render_model` as GLB without glTF animations, UUID a hash of the bytes; animatable `trigger` and `buttons` nodes follow the inputs; spaces at the grip; no glTF extensions needed |
| `XR_EXT_spatial_container` | no | L | containers of spatial entities |
| `XR_EXT_spatial_image_tracking` | no | L | images placed in the world model |
| `XR_EXT_spatial_marker_tracking` | done | L | QR codes only, from QR props (docs/props.md), with the fixed size and static features; Micro QR, ArUco and AprilTag are unsupported capabilities; docs/openxr.md "Spatial entity contexts" |
| `XR_EXT_spatial_mesh_tracking` | no | L | meshes of the world model |
| `XR_EXT_spatial_object_tracking` | no | L | objects of the world model |
| `XR_EXT_spatial_plane_tracking` | no | L | planes of the world model |
| `XR_EXT_spatial_entity` | done | XL | contexts, discovery and update snapshots, entities from ids, string and uint8 buffers, discovery recommendations; the only capability comes from XR_EXT_spatial_marker_tracking; docs/openxr.md "Spatial entity contexts" |
| `XR_EXT_aosp_hand_tracking_permission` | n/a | platform | Android permission model |
| `XR_EXT_win32_appcontainer_compatible` | n/a | platform | Windows |

## EXTX

| extension | status | effort | notes |
|---|---|---|---|
| `XR_EXTX_overlay` | no | L | provisional; overlay sessions next to a main session |

## ALMALENCE

| extension | status | effort | notes |
|---|---|---|---|
| `XR_ALMALENCE_digital_lens_control` | no | S | lens control request, accept and ignore |

## ANDROID

| extension | status | effort | notes |
|---|---|---|---|
| `XR_ANDROID_mouse_interaction` | no | S | interaction profile only |
| `XR_ANDROID_passthrough_camera_state` | no | S | report a fixed camera state |
| `XR_ANDROID_recommended_resolution` | no | S | event with the recommended resolution |
| `XR_ANDROID_spatial_anchor_space` | no | S | on EXT_spatial_anchor |
| `XR_ANDROID_spatial_component_subsumed_by` | no | S | on EXT_spatial_plane_tracking |
| `XR_ANDROID_spatial_discovery_bounds` | no | S | on EXT_spatial_entity |
| `XR_ANDROID_spatial_entity_bound_anchor` | no | S | on EXT_spatial_anchor |
| `XR_ANDROID_unbounded_reference_space` | no | S | deprecated by XR_EXT_stationary_reference_space |
| `XR_ANDROID_anchor_sharing_export` | no | M | export anchors for sharing; needs EXT_spatial_anchor |
| `XR_ANDROID_eye_tracking` | no | M | eye poses from the simulated gaze |
| `XR_ANDROID_face_tracking` | no | M | blend weights from a neutral or scripted face |
| `XR_ANDROID_google_cloud_auth` | no | M | cloud service; only a local stub is possible |
| `XR_ANDROID_light_estimation` | no | M | light from the simulated environment config |
| `XR_ANDROID_performance_metrics` | no | M | counters from simulator frame timing |
| `XR_ANDROID_spatial_discovery_raycast` | no | M | raycasts against the simulated world model |
| `XR_ANDROID_spatial_object_tracking` | no | M | objects in the simulated world model |
| `XR_ANDROID_composition_layer_passthrough_mesh` | no | L | passthrough through an app mesh; needs simulated passthrough |
| `XR_ANDROID_device_anchor_persistence` | no | L | deprecated by XR_EXT_spatial_persistence, XR_EXT_spatial_persistence_operations |
| `XR_ANDROID_geospatial_anchor` | no | L | on ANDROID_geospatial |
| `XR_ANDROID_raycast` | no | L | deprecated by XR_ANDROID_spatial_discovery_raycast |
| `XR_ANDROID_trackables` | no | L | deprecated by XR_EXT_spatial_entity, XR_EXT_spatial_plane_tracking, XR_ANDROID_spatial_component_subsumed_by, XR_EXT_spatial_anchor, XR_ANDROID_spatial_entity_bound_anchor, XR_ANDROID_spatial_anchor_space |
| `XR_ANDROID_trackables_image` | no | L | deprecated by XR_EXT_spatial_image_tracking |
| `XR_ANDROID_trackables_marker` | no | L | deprecated by XR_EXT_spatial_marker_tracking |
| `XR_ANDROID_trackables_object` | no | L | deprecated by XR_ANDROID_spatial_object_tracking |
| `XR_ANDROID_trackables_qr_code` | no | L | deprecated by XR_EXT_spatial_marker_tracking |
| `XR_ANDROID_geospatial` | no | XL | simulated geospatial pose, futures; needs a configured earth location |
| `XR_ANDROID_scene_meshing` | no | XL | deprecated by XR_EXT_spatial_mesh_tracking |

## BD

| extension | status | effort | notes |
|---|---|---|---|
| `XR_BD_controller_interaction` | no | S | core in 1.1; interaction profiles only |
| `XR_BD_future_progress` | no | S | on EXT_future |
| `XR_BD_ultra_controller_interaction` | no | S | interaction profile only |
| `XR_BD_body_tracking_auxiliary_metrics` | no | M | on BD_body_tracking |
| `XR_BD_dynamic_object_keyboard` | no | M | on BD_dynamic_object_tracking |
| `XR_BD_dynamic_object_mouse` | no | M | on BD_dynamic_object_tracking |
| `XR_BD_facial_simulation` | no | M | blend weights from a neutral or scripted face |
| `XR_BD_motion_tracker` | no | M | profile plus simulated motion trackers |
| `XR_BD_spatial_anchor_sharing` | no | M | single machine only |
| `XR_BD_spatial_light_estimation` | no | M | on BD_spatial_sensing |
| `XR_BD_spatial_plane` | no | M | planes of the world model |
| `XR_BD_spatial_scene` | no | M | scene capture of the world model |
| `XR_BD_body_tracking` | no | L | simulated skeleton driven by head and hands |
| `XR_BD_dynamic_object_tracking` | no | L | tracked objects in the world model |
| `XR_BD_spatial_anchor` | no | L | on BD_spatial_sensing |
| `XR_BD_spatial_mesh` | no | L | meshes of the world model |
| `XR_BD_camera_image` | no | XL | camera frames rendered from the simulated environment |
| `XR_BD_spatial_audio_rendering` | no | XL | an audio renderer; out of scope for a visual simulator |
| `XR_BD_spatial_sensing` | no | XL | base framework: providers, snapshots, futures over a world model |

## EPIC

| extension | status | effort | notes |
|---|---|---|---|
| `XR_EPIC_view_configuration_fov` | no | S | chained fov on view configuration views |

## FB

| extension | status | effort | notes |
|---|---|---|---|
| `XR_FB_color_space` | done | S | Rec.709 native, all spaces accepted and reported in `status`, no conversion |
| `XR_FB_composition_layer_image_layout` | done | S | vertically flipped layers |
| `XR_FB_composition_layer_secure_content` | done | S | the mirror (headset) shows the layer; screenshots leave EXCLUDE layers out and draw REPLACE layers black |
| `XR_FB_composition_layer_settings` | done | S | supersampling and sharpening flags validated, no effect |
| `XR_FB_display_refresh_rate` | done | S | `clock.refresh_rates`, a request sets the display period; fixed while recording or replaying |
| `XR_FB_foveation_configuration` | done | S | levels validated and kept, no visual effect |
| `XR_FB_hand_tracking_aim` | done | S | aim pose, index pinch strength and flag, dominant hand, menu and system gestures |
| `XR_FB_hand_tracking_capsules` | done | S | one capsule per bone, joint radii |
| `XR_FB_haptic_amplitude_envelope` | done | S | samples kept per hand and output (resampled to 4000), the current amplitude follows sim time; UI, `puppetxrctl haptic` |
| `XR_FB_passthrough_keyboard_hands` | done | S | keyboard hands layers and intensities accepted; no tracked keyboard, so nothing is drawn |
| `XR_FB_scene_capture` | done | S | completes successfully at once, no capture UI |
| `XR_FB_spatial_entity_container` | done | S | rooms contain every entity of the loaded room |
| `XR_FB_spatial_entity_storage_batch` | done | S | `xrSaveSpaceListFB`, local and cloud (`docs/anchors.md`) |
| `XR_FB_spatial_entity_user` | done | S | user handles holding an id; the simulated user is `anchors.user_id` (`docs/anchors.md`) |
| `XR_FB_swapchain_update_state` | done | S | `xrUpdateSwapchainFB` and `xrGetSwapchainStateFB` for the Vulkan sampler state |
| `XR_FB_swapchain_update_state_vulkan` | done | S | sampler state stored and returned; the compositor uses the mag filter |
| `XR_FB_touch_controller_pro` | done | S | `meta_touch_pro` controller: curl, slide, thumbrest and stylus force, trigger and thumb haptics |
| `XR_FB_touch_controller_proximity` | done | S | trigger and thumb proximity on every Touch, implied by touch with `auto_touch` |
| `XR_FB_composition_layer_alpha_blend` | done | M | the four factors on Vulkan, GL, GLES, D3D11, D3D12; destination alpha starts at 0; only OPAQUE environment blend with it; `docs/compositor.md` |
| `XR_FB_composition_layer_depth_test` | done | M | all eight compare ops and the depth mask on Vulkan, GL, GLES, D3D11, D3D12; projection depth from KHR_composition_layer_depth, quads, cylinders, equirects and cubes their own; `docs/compositor.md`; CTS ProjectionDepth compared in cts_composition |
| `XR_FB_eye_tracking_social` | done | M | a gaze pose per eye from the simulated gaze: ahead, turned by the controls or converging on a point (`docs/face-tracking.md`) |
| `XR_FB_face_tracking` | done | M | the first 63 weights of face tracking 2 |
| `XR_FB_face_tracking2` | done | M | 70 weights of the simulated face: presets or set weights, eye blendshapes following the gaze, blinks every 4 s; confidences; visual or audio source (`docs/face-tracking.md`) |
| `XR_FB_foveation` | done | M | foveation profiles applied to swapchains, no visual effect; the fragment density map flag is accepted without one; `docs/openxr.md` |
| `XR_FB_foveation_vulkan` | no | M | fragment density map images; needs VK_EXT_fragment_density_map on the app's device, which puppetxr would have to enable and lavapipe lacks |
| `XR_FB_haptic_pcm` | done | M | buffers resampled to 2000 Hz into the envelope arrays, append, samples consumed; `puppetxrctl haptic` |
| `XR_FB_keyboard_tracking` | done | M | the oldest keyboard prop (docs/props.md) is the local, connected system keyboard: id, 30 x 1.8 x 11.2 cm, a keyboard space at its box centre that follows it; draw it with `/model_fb/keyboard/local`; docs/openxr.md "Tracked keyboards" |
| `XR_FB_space_warp` | done | M | deprecated by XR_EXT_frame_synthesis; infos validated, no frame extrapolation (the compositor ignores motion vectors and depth); CTS passes |
| `XR_FB_spatial_entity_query` | done | M | the loaded room and the local anchor store; `XrSpaceStorageLocationFilterInfoFB` picks the cloud and is required with a component filter, as on Horizon OS (`docs/anchors.md`) |
| `XR_FB_spatial_entity_sharing` | done | M | shares through the simulated cloud directory, so between processes and machines that share it (`docs/anchors.md`) |
| `XR_FB_spatial_entity_storage` | done | M | per app and user JSON store, anchors kept in room space (`docs/anchors.md`) |
| `XR_FB_triangle_mesh` | done | M | app-built meshes, shown by projected passthrough layers |
| `XR_FB_body_tracking` | done | L | 70 joints of a posed skeleton in Meta's joint axes: it follows the head, its arms reach for the controllers by IK; shape, pose and tracking set by the tests (`docs/body-tracking.md`) |
| `XR_FB_hand_tracking_mesh` | done | L | generated low-poly mesh of tapered prisms per bone, rigidly skinned; scale is always 1; the wrist's parent is `XR_HAND_JOINT_MAX_ENUM_EXT`, no joint, as OVRPlugin's skeletons expect of the root |
| `XR_FB_passthrough` | done | L | the room's visual mesh with styles; projected layers show it on `XR_FB_triangle_mesh` geometry |
| `XR_FB_render_model` | done | L | `/model_fb/controller/left` and `right`: procedural glTF 2.0 binary per simulated controller type (handle, head, trigger, buttons, the ring of Touch), in grip space, glTF subsets 1 and 2, no textures; `/model_fb/keyboard/local` and `remote`: a procedural compact keyboard around its bounding box centre; `/model_meta/keyboard/virtual` with XR_META_virtual_keyboard |
| `XR_FB_scene` | done | L | labels, bounds, boundaries and room layout of the loaded room |
| `XR_FB_spatial_entity` | done | L | anchors and component status; anchors are storable with a storage extension, sharable with a sharing extension |
| `XR_FB_android_surface_swapchain_create` | n/a | platform | Android |
| `XR_FB_swapchain_update_state_android_surface` | n/a | platform | Android |
| `XR_FB_swapchain_update_state_opengl_es` | n/a | platform | needs KHR_opengl_es_enable |

## HTC

| extension | status | effort | notes |
|---|---|---|---|
| `XR_HTC_foveation` | no | S | no visual effect |
| `XR_HTC_vive_cosmos_controller_interaction` | no | S | core in 1.1; interaction profile only |
| `XR_HTC_vive_focus3_controller_interaction` | no | S | core in 1.1; interaction profile only |
| `XR_HTC_anchor` | no | M | named anchors |
| `XR_HTC_facial_tracking` | no | M | blend weights from a neutral or scripted face |
| `XR_HTC_hand_interaction` | no | M | needs simulated hands |
| `XR_HTC_vive_wrist_tracker_interaction` | no | M | profile plus simulated wrist trackers |
| `XR_HTC_body_tracking` | no | L | simulated skeleton driven by head and hands |
| `XR_HTC_passthrough` | no | L | passthrough from a configured environment |

## HTCX

| extension | status | effort | notes |
|---|---|---|---|
| `XR_HTCX_vive_tracker_interaction` | no | M | provisional; profile plus simulated trackers |

## HUAWEI

| extension | status | effort | notes |
|---|---|---|---|
| `XR_HUAWEI_controller_interaction` | no | S | interaction profile only |

## LOGITECH

| extension | status | effort | notes |
|---|---|---|---|
| `XR_LOGITECH_mx_ink_stylus_interaction` | no | S | interaction profile only |

## META

| extension | status | effort | notes |
|---|---|---|---|
| `XR_META_automatic_layer_filter` | done | S | the automatic bit with at least one filter bit, validated, no effect |
| `XR_META_body_tracking_calibration` | done | S | a suggested height (0.5 to 3 m) reshapes the simulated body and raises the skeleton change count; reset goes back to the configured height; status always valid |
| `XR_META_body_tracking_fidelity` | done | S | per tracker: high (default) moves the fingers with the simulated hands, low keeps them open; reported in the status |
| `XR_META_boundary_visibility` | done | S | suppression only while a reconstruction passthrough layer runs, reverts with an event when it stops; `status` reports it, nothing is drawn |
| `XR_META_face_tracking_visemes` | done | S | 15 visemes of the simulated face, chained to face tracking 2's weights |
| `XR_META_hand_tracking_frequency_hint` | done | S | validated and kept per session; the simulated hands update every frame |
| `XR_META_hand_tracking_unextrapolated_poses` | done | S | the live hand state, captured at the predicted display time |
| `XR_META_hand_tracking_wide_motion_mode2` | done | S | every tracker keeps hands outside the hand tracking cone, valid but not tracked; also the preview `XR_META_hand_tracking_wide_motion_mode` (`XrHandTrackingWideMotionModeInfoMETA` per tracker) |
| `XR_META_headset_id` | done | S | one fixed id for the simulated headset |
| `XR_META_local_dimming` | done | S | validated and reported in `status`, no visual effect |
| `XR_META_passthrough_layer_resumed_event` | done | S | one event at the first frame a resumed or running-created layer is ready |
| `XR_META_passthrough_preferences` | done | S | `passthrough.default_active` in the config |
| `XR_META_recommended_layer_resolution` | done | S | the submitted sub-image size, always valid; older frames' times for Meta's Unreal plugin (`accept_older_frame_time`) |
| `XR_META_spatial_entity_group_sharing` | done | S | `XrSpaceGroupUuidFilterInfoMETA` queries (chained or as the filter) load every anchor of a group (`docs/anchors.md`) |
| `XR_META_spatial_entity_semantic_label` | done | S | defines `XrSemanticLabelMETA`, used by `XR_META_spatial_entity_room_mesh` |
| `XR_META_tile_properties_hint` | done | S | validated and dropped |
| `XR_META_touch_controller_plus` | done | S | `meta_touch_plus` controller, the default: curl, slide, trigger force |
| `XR_META_vulkan_swapchain_create_info` | done | S | extra create and usage flags on swapchain images, `MUTABLE_FORMAT` and plain usages; others `XR_ERROR_FEATURE_UNSUPPORTED` |
| `XR_META_body_tracking_full_body` | done | M | 84 joints: the legs keep the feet on the floor by IK, crouching when the head goes down |
| `XR_META_colocation_discovery` | done | M | advertisements are lock files in the simulated cloud directory, polled every 18 frames (`docs/anchors.md`) |
| `XR_META_detached_controllers` | done | M | while a hand is tracked its controller lies where it was put down and serves its controller profile under `/user/detached_controller_meta/<side>`: poses there, inputs zero, haptics play |
| `XR_META_foveation_eye_tracked` | done | M | eye tracked profiles accepted; the foveation centers are the middle of each view, as the simulated user looks straight ahead |
| `XR_META_hand_tracking_microgestures` | done | M | swipe and tap inputs of `ext/hand_interaction_ext`, one frame pulses from the stick and primary slots of a tracked hand; the thumb animates |
| `XR_META_passthrough_color_lut` | done | M | RGB and RGBA LUTs up to 64^3, trilinear in sRGB, single and interpolated maps, destroyed LUTs kept while in use; `docs/compositor.md` |
| `XR_META_performance_metrics` | done | M | app frame times, dropped frames and space warp mode from the simulated frame loop, deterministic; CTS passes; `docs/openxr.md` |
| `XR_META_simultaneous_hands_and_controllers` | done | S | every hand switches between controller and hand tracking alone anyway, so resume and pause only validate; a put down controller is tracked through `XR_META_detached_controllers` |
| `XR_META_spatial_entity_discovery` | done | M | discovery over the loaded room |
| `XR_META_spatial_entity_mesh` | done | M | the global mesh of the loaded room |
| `XR_META_spatial_entity_persistence` | done | M | the same store as FB storage; discovery finds saved anchors (`docs/anchors.md`) |
| `XR_META_spatial_entity_room_mesh` | done | M | the room's `room_mesh`, generated by the Blender exporter from the shell and the door and window frames |
| `XR_META_spatial_entity_sharing` | done | M | group recipients through the simulated cloud (`docs/anchors.md`) |
| `XR_META_environment_depth` | done | L | depth of the room's visual mesh from the eyes, infinite projection; no hands to remove |
| `XR_META_environment_raycast` | done | L | raycasts against the global mesh, limited to what the eyes see |
| `XR_META_dynamic_object_tracker` | done | M | preview (values written out, Meta's SDK has the only header): trackers whose create and class results arrive as events; tracked props found by `XR_FB_spatial_entity_query` and `XR_META_spatial_entity_discovery` with the `DYNAMIC_OBJECT_DATA` component; docs/openxr.md "Dynamic objects" |
| `XR_META_dynamic_object_keyboard` | done | S | preview: keyboard props (docs/props.md) as tracked keyboards, located, with 2D and 3D bounds |
| `XR_META_virtual_keyboard` | done | XL | procedural labelled keyboard model with key press animations and a label atlas (`/model_meta/keyboard/virtual`, subset 2), ray and direct input with poke limiting, FAR/DIRECT/CUSTOM places, typing from the simulator (UI, `puppetxrctl vkb type`, C API); docs/openxr.md "Virtual keyboard" |

## ML

| extension | status | effort | notes |
|---|---|---|---|
| `XR_ML_frame_end_info` | no | S | focus distance and flags, ignored |
| `XR_ML_global_dimmer` | no | S | dimmer value, shown in the mirror |
| `XR_ML_ml2_controller_interaction` | no | S | core in 1.1; interaction profile only |
| `XR_ML_system_notifications` | no | S | accepted, ignored |
| `XR_ML_user_calibration` | no | S | fixed calibration events |
| `XR_ML_view_configuration_depth_range_change` | no | S | on EXT_view_configuration_depth_range |
| `XR_ML_facial_expression` | no | M | blend weights from a neutral or scripted face |
| `XR_ML_spatial_anchors` | no | M | anchors with futures |
| `XR_ML_localization_map` | no | L | maps and localization events over the configured room |
| `XR_ML_marker_understanding` | no | L | markers placed in the world model |
| `XR_ML_spatial_anchors_storage` | no | L | local anchor store with queries |
| `XR_ML_world_mesh_detection` | no | L | meshes of the world model |
| `XR_ML_compat` | n/a | platform | Magic Leap coordinate frame interop |

## MND

| extension | status | effort | notes |
|---|---|---|---|
| `XR_MND_headless` | done | S |  |
| `XR_MND_swapchain_usage_input_attachment_bit` | no | S | deprecated by XR_KHR_swapchain_usage_input_attachment_bit |

## MNDX

| extension | status | effort | notes |
|---|---|---|---|
| `XR_MNDX_force_feedback_curl` | no | S | provisional; on EXT_hand_tracking, shown in the UI |
| `XR_MNDX_egl_enable` | done | L | provisional; GL or GLES by the context's client type |

## MSFT

| extension | status | effort | notes |
|---|---|---|---|
| `XR_MSFT_composition_layer_reprojection` | no | S | reprojection mode hints, ignored |
| `XR_MSFT_hand_interaction` | no | S | promoted to XR_EXT_hand_interaction; interaction profile; superseded by EXT_hand_interaction |
| `XR_MSFT_unbounded_reference_space` | no | S | one more reference space |
| `XR_MSFT_first_person_observer` | no | M | on MSFT_secondary_view_configuration |
| `XR_MSFT_scene_marker` | no | M | on MSFT_scene_understanding |
| `XR_MSFT_scene_understanding_serialization` | no | M | on MSFT_scene_understanding |
| `XR_MSFT_secondary_view_configuration` | no | M | extra views composited separately |
| `XR_MSFT_spatial_anchor` | no | M | anchors |
| `XR_MSFT_spatial_anchor_persistence` | no | M | local anchor store |
| `XR_MSFT_spatial_graph_bridge` | no | M | fake spatial graph node ids |
| `XR_MSFT_controller_model` | no | L | glTF controller models; needs freely licensed assets |
| `XR_MSFT_hand_tracking_mesh` | no | L | needs a freely licensed hand mesh |
| `XR_MSFT_scene_understanding` | no | XL | scene objects, planes and meshes over a world model |
| `XR_MSFT_holographic_window_attachment` | n/a | platform | Windows holographic windows |
| `XR_MSFT_perception_anchor_interop` | n/a | platform | Windows perception APIs |

## OCULUS

| extension | status | effort | notes |
|---|---|---|---|
| `XR_OCULUS_external_camera` | no | M | a configured external camera |
| `XR_OCULUS_android_session_state_enable` | n/a | platform | Android |
| `XR_OCULUS_audio_device_guid` | done | S | Windows only: the PC's default output and input endpoint ids (`os_default_audio_device`), empty without a device. OVRManager asks every frame and logged a warning with a stack trace each time it was missing |

## OPPO

| extension | status | effort | notes |
|---|---|---|---|
| `XR_OPPO_controller_interaction` | no | S | interaction profile only |

## QCOM

| extension | status | effort | notes |
|---|---|---|---|
| `XR_QCOM_tracking_optimization_settings` | no | S | hints, ignored |
| `XR_QCOM_hand_tracking_gesture` | no | M | deprecated by XR_EXT_hand_interaction |

## SONY

| extension | status | effort | notes |
|---|---|---|---|
| `XR_SONY_hdr_metadata` | no | S | on SONY_swapchain_color_space |
| `XR_SONY_swapchain_color_space` | no | M | color space conversion in the compositor |

## ULTRALEAP

| extension | status | effort | notes |
|---|---|---|---|
| `XR_ULTRALEAP_hand_tracking_forearm` | no | S | on EXT_hand_tracking |

## VALVE

| extension | status | effort | notes |
|---|---|---|---|
| `XR_VALVE_analog_threshold` | done | S | per-binding on/off thresholds with hysteresis and haptics (`XrHapticVibration`), in `XrBindingModificationsKHR` or chained directly |

## VARJO

| extension | status | effort | notes |
|---|---|---|---|
| `XR_VARJO_environment_depth_estimation` | no | S | toggle, no effect without passthrough |
| `XR_VARJO_view_offset` | no | S | eye offset, ignored |
| `XR_VARJO_xr4_controller_interaction` | no | S | interaction profile only |
| `XR_VARJO_composition_layer_depth_test` | no | M | depth testing between layers; needs KHR_composition_layer_depth |
| `XR_VARJO_foveated_rendering` | no | M | on VARJO_quad_views |
| `XR_VARJO_quad_views` | no | M | core in 1.1; four-view configuration in the compositor |
| `XR_VARJO_marker_tracking` | no | L | markers placed in the world model |

## YVR

| extension | status | effort | notes |
|---|---|---|---|
| `XR_YVR_controller_interaction` | no | S | interaction profile only |

## Meta SDK previews

Extensions of the Meta OpenXR SDK (`meta-quest/Meta-OpenXR-SDK` v85,
`OpenXR/meta_openxr_preview/`) that the registry does not have under that name. They are not in
the totals above.

| extension | status | effort | notes |
|---|---|---|---|
| `XR_EXTX1_event_channel` | done | M | spec version 3: create, destroy, poll, select with a timeout, a default channel, a session target in `XrSessionCreateInfo`; semantics the header leaves open in `docs/openxr.md`; no sample uses it |
| `XR_EXTX1_haptic_parametric` | done | M | preview of `XR_EXT_haptic_parametric`, same implementation |
| `XR_EXTX2_stationary_reference_space` | done | S | preview of `XR_EXT_stationary_reference_space`, same implementation; its function is `xrGetStationaryReferenceSpaceIdEXTX2`; XrSceneModel draws its axes when present |
| `XR_META_hand_tracking_wide_motion_mode` | done | S | preview of `XR_META_hand_tracking_wide_motion_mode2`, same implementation; XrHandTrackingWideMotionMode uses it |

## Unpublished

Extensions that neither the registry nor the Meta OpenXR SDK publishes: their names, and for most
their functions, appear only in the binaries and scripts of Meta's Unity packages
(`com.meta.xr.sdk.core` and `com.meta.xr.mrutilitykit` 207.0.1, from Meta's package registry).
There is no header, so the parameters, struct layouts and enum values are unknown; implementing
one needs a header from Meta or the layouts read from the binaries. Not in the totals above.

| extension | status | effort | notes |
|---|---|---|---|
| `XR_METAX1_passthrough_camera_data` | done | L | the passthrough camera images; MRUK's `PassthroughCameraAccess` uses it on Windows with Horizon Link or the Meta XR Simulator v85+. Layouts read from MRUK's shared library (`docs/openxr.md`, Passthrough cameras): two cameras, RGBA8 images of the room as passthrough shows it at 60 Hz, stamped with their capture time. `xrEnumeratePassthroughCameraSupportedFormatsMETAX1` is not offered: MRUK never calls it and its parameters are unknown |
| `XR_METAX1_room_entity_info` | no | S | `xrGetSpaceRoomLabelMETAX1`: a room's label, used by MRUK; the room format has names |
| `XR_METAX1_spatial_entity_marker` | no | M | marker trackers on FB spatial entities: `xrCreateMarkerTrackerAsyncMETAX1`, `...CompleteMETAX1`, `xrDestroyMarkerTrackerMETAX1`, `xrGetSpaceMarkerPayloadMETAX1`; the QR props of `XR_EXT_spatial_marker_tracking` could back it |
| `XR_META_spatial_entity_marker` | no | M | the same name without the X1, in the core SDK's plugin; likely its next version |
| `XR_META_raycast` | no | M | named in the core SDK's plugin, no function names; perhaps a successor of `XR_META_environment_raycast` |
| `XR_METAX2_debug_utils_region_profiling` | no | S | profiling regions in the core SDK's plugin, also as `XR_METAX3_`; accept and ignore, or show in the UI |
| `XR_METAX1_simulator_compositor_output_capture` | no | M | start, stop, capture the simulator's composited output (`xr*SimulatorCompositorOutputCaptureMETAX1`); what Meta's XR Operator layer uses to see the Meta XR Simulator, as `puppetxrctl screenshot` does |
| `XR_METAX2_simulator_head_pose` | no | M | the simulator's head pose for the XR Operator layer, with an interaction profile `/interaction_profiles/meta/head_metax2`; as `puppetxrctl pose head` |
| `XR_METAX1_agentic_external_tool` | no | n/a | `xrAgenticRegisterExternalToolMETAX1`, provided by Meta's XR Operator API layer (`XR_APILAYER_METAX_operator`), not by a runtime; puppetxr would run under that layer if it offered the two simulator extensions above |
| `XR_FB_common_events` | no | S | named in the core SDK's plugin, no function names |
| `XR_FB_eye_tracking` | no | S | requested by the core SDK's OpenXR feature; an older name next to `XR_FB_eye_tracking_social` |
| `XR_FB_hand_tracking_confidence` | no | S | requested by the core SDK's OpenXR feature; joint confidence for the FB hand extensions |
| `XR_OCULUS_common_reference_spaces` | no | S | requested by the core SDK's OpenXR feature; reference spaces of the Oculus runtime |
