# Render and compositor

## Render layer (`src/render.h`, `src/render.cpp`, `src/render_vulkan.cpp`, `src/render_gl.cpp`, `src/render_d3d11.cpp`, `src/render_d3d12.cpp`, `src/render_metal.mm`)

`render.h` is backend neutral. `R_DeviceCreateInfo::backend` picks the backend; `render.cpp`
holds the format table, `r_max_mip_levels` and the public `r_*` functions, which forward through
the backend's `R_BackendFns` table (`render_internal.h`, shared by `render*.cpp` only). Every
backend's device struct starts with `R_Device` (the table and `R_Backend`); its resource structs
are its own (`R_VkImage`, ...) in `SparseArray<R_VkImage, R_Image>`, so handles stay the
public `R_ImageHandle`.

Backend-neutral rules of the API:

- Shaders are `R_ShaderCode`, one per stage, generated from `shaders/` (`docs/shaders.md`); a
  backend rejects a pipeline whose code has nothing for it.
- Formats cross the API as `R_Format`; `r_native_format` / `r_format_from_native` convert to
  the enum of the backend's API (`VkFormat`, GL internal format, `DXGI_FORMAT`), for the
  swapchain formats of the binding.
- `r_backend_available` says which backends the build has: D3D11 and D3D12 only on Windows
  (`PUPPETXR_HAVE_D3D`, set by CMake); the OpenXR layer offers their extensions accordingly.
- Blits with scaling are a draw on backends without a blit command (D3D): `shaders/blit.*`.
- After writing mapped memory, `r_flush_buffer` before recording commands that read it
  (a no-op on Vulkan, whose host-visible memory is coherent).
- Commands are recorded between begin and end within one call of the app on one thread, so a
  backend without command buffers may execute them as they are recorded; layouts are explicit
  and ignored where the API does not track them.
- Window swapchains present to an `R_WindowTarget` (a `VkSurfaceKHR` on Vulkan, the native
  window on GL and D3D, and its HDC on WGL).

### Vulkan (`src/render_vulkan.cpp`)

The device wraps the app's `VkInstance`, `VkPhysicalDevice`, `VkDevice` and one queue.
It owns nothing of the app's; it creates its own command pools, fences, descriptor
pools, samplers and resources on that device. Function pointers come from volk: the
instance table through the app's `vkGetInstanceProcAddr` when the app provided one
(`XR_KHR_vulkan_enable2`), otherwise from the system loader; device functions through
a per-device `VolkDeviceTable`.

Resources are generational handles in `SparseArray`s (`R_ImageHandle`, ...). Every
image gets a 2D-array view over all layers for sampling and one 2D view per layer
for attachments, so a single pipeline samples both plain and array images. Memory is
one `VkDeviceMemory` per image or buffer; there are few objects, so no allocator.

Pipelines blend premultiplied or straight alpha (`blend_premultiplied`), or with four
`R_BlendFactor`s when `blend_custom` is set (`r_pipeline_blend_factors`, every backend).

Render passes have a colour attachment, a depth attachment or both (`depth_format`); depth
clears to 1 and pipelines with `depth_test` compare less-or-equal, or always with
`depth_compare = R_CompareOp::Always`; `no_color_write` leaves the colour attachment as it
is. Framebuffers take one layer of each attachment image. Pipelines bind up to
`R_MAX_TEXTURES` combined image samplers at bindings 0.. (`textures_count`,
`r_cmd_bind_textures`); `r_cmd_clear_depth` clears depth inside a pass to a given value.
`r_read_image` reads depth formats too.

Frames: `r_begin_frame` / `r_end_frame` record and submit one command buffer with a
fence, two in flight. `r_begin_immediate` / `r_end_immediate` submit and wait, used for
uploads, layout transitions at creation and readback. Descriptor sets are allocated
from the frame's pool and the pool is reset when the frame slot is reused.

Ordering with the app: everything is submitted to the app's queue from the thread that
calls `xrEndFrame`. Barriers at the start and end of the composite command buffer
(`ALL_COMMANDS` to `ALL_COMMANDS`) order our reads of the swapchain images after the
app's writes and before the app's next writes; queue submission order does the rest.
An app that submits from another thread while calling `xrEndFrame` would race on the
queue, which `VkQueue` forbids; engines serialize this themselves.

### OpenGL and OpenGL ES (`src/render_gl.cpp`)

One backend for GL 3.3 core (`R_Backend::OpenGL`) and GLES 3.0 (`OpenGLES`), without GL or EGL
headers: types and constants are declared in the file, entry points come from the binding's
`getProcAddress` (`R_GLDeviceInfo`) or the process (`os_library_symbol`). Platforms: EGL, GLX
and WGL. WGL contexts come from `wglCreateContextAttribsARB` of opengl32.dll, pixel format and
swap functions from gdi32.dll; `wglGetProcAddress` answers null (or 1, 2, 3, -1) for GL 1.1,
which opengl32.dll exports itself. The device's context is current with the app's HDC; the
mirror window gets the app's pixel format (`match_pixel_format`), set once per window. No GLES
on WGL.

- Own context: the device creates a context in the app's share group (`eglCreateContext` with the
  app's context as `share_context`), surfaceless or on a 1x1 pbuffer of the app's config. The
  app's context state is never touched.
- `enter`/`leave` wrap every call that issues GL: at the outermost level they save the app's
  current context and drawables, make the device's current, and restore the app's afterwards
  (also the bound EGL API). Nested calls (inside a frame) do nothing.
- Ordering with the app: a fence in the context that is left, `glWaitSync` in the one that is
  entered (GL 4.6 appendix D.3.1), so the app's rendering into swapchain textures lands before
  the compositor samples them and the compositor's work before the app's next commands.
- Commands execute as they are recorded; `queue_access_count` counts frames and immediates.
  Layout transitions and initial layouts do nothing.
- Images: `GL_TEXTURE_2D` for one layer, `GL_TEXTURE_2D_ARRAY` for more, `GL_TEXTURE_CUBE_MAP`
  for cube images (6 layers; cube arrays are refused). Immutable storage when the context has
  it. No BGRA formats.
- Render passes are FBOs; load ops are `glClearBufferfv` at begin. Scissor is always enabled as
  in Vulkan; clears inside a pass and blits disable or narrow it.
- Pipelines: one program per sampler variant of texture 0 for `Type2DArray` pipelines (2D, 2D
  array, cube; `docs/shaders.md`), picked at draw time from the bound texture's target. The push
  block is one uniform buffer at binding 0, updated with `glBufferSubData` per push. Samplers bind
  by name (`R_ShaderCode::textures`).
- Vertex attributes are pointed at the bound buffer at draw time, with indexed draws' vertex
  offset folded into the pointers (GLES 3.0 has no base vertex).
- Buffers: host-visible ones keep a CPU copy (`r_get_buffer_mapped_data`); `r_flush_buffer`
  uploads the range. Transfer-only buffers have no GL object.
- Depth range [0, 1] through `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`, else the vertex
  entry point remaps it. Targets have Vulkan's memory layout (row 0 at NDC y = -1), so readbacks
  and screenshots match the Vulkan backend.
- sRGB: `GL_FRAMEBUFFER_SRGB` on desktop GL, so sRGB targets encode on write as in Vulkan.
- Readback uses `glReadPixels`; GLES 3.0 cannot read depth (`r_read_image` returns false).
  `D24_UNORM_S8_UINT` reads back as GL packs it (depth in the high 24 bits).
- Validation: every outermost `leave` drains `glGetError` into `LOG_ERROR("GL error ...")` and
  an assert; the tests count those lines (`tests/test_gl.h`).
- Window presentation: the frame draws into an offscreen `R8G8B8A8_SRGB` image of the window's
  size (`r_window_acquire`); `r_window_present` makes puppetxr's context current with the window's
  drawable, blits the image to its default framebuffer with the rows flipped (sRGB writes off, so
  the encoded values are copied; GLES windows are created sRGB through `EGL_KHR_gl_colorspace`),
  swaps, and goes back to the context's own drawable. Swap interval 0, so the mirror never paces
  the app.
  - GLX: `glXCreateWindow` on the app's display and config over the window's X11 id; the window
    must have the app's visual (`oxr.cpp` makes it so). An Xlib binding without `glxFBConfig`
    or `visualid` (Godot's compatibility renderer, against the spec) gets the config whose id
    `glXQueryContext` reports for the app's context, found among `glXGetFBConfigs` (Mesa's
    `glXChooseFBConfig` filters an id by drawable type), and its visual from
    `r_gl_x11_visual`; a warning is logged. X errors are trapped (`XSetErrorHandler`,
    `XSync`) because Xlib's default handler exits; a failure leaves the mirror blank.
  - EGL: `eglCreateWindowSurface` with the X11 window, or a `wl_egl_window` of the `wl_surface`
    (`libwayland-egl.so.1`, loaded at runtime) on Wayland. Only works when the app's EGL display
    is on the same platform as the window; otherwise the mirror stays blank.

### D3D11 (`src/render_d3d11.cpp`)

The device wraps the app's `ID3D11Device` (feature level 11_0 or later) and its immediate
context; it takes a reference on both. No D3D headers outside the two D3D files; `dxgi.dll` is
loaded at runtime.

- Commands: each command buffer slot records into a deferred context; `end_*` finishes the
  command list and runs it with `ExecuteCommandList(list, TRUE)`, which restores the app's
  immediate context state. So the immediate context is touched only where render.h allows
  queue access. A device created `SINGLETHREADED` has no deferred contexts: commands then go
  to the immediate context inside an `ID3DDeviceContextState` of our own, swapped in at begin
  and out at end (D3D11.1).
- Images are typeless (`R_DxgiFormat::resource`) so apps view swapchain images as sRGB or
  linear; views use the typed format. Every image has shader views (`Texture2D` for one
  layer, `Texture2DArray` over all) and colour images are render targets, for clears and
  blits. Depth is typeless too (`R32_TYPELESS`, ...) with `D32_FLOAT` etc. as depth view and
  `R32_FLOAT` etc. as shader view.
- Host-visible buffers live in host memory; `r_flush_buffer` marks them, and binding uploads
  them with `Map(WRITE_DISCARD)` on the recording context. Push constants are one dynamic
  constant buffer at `b0`, mapped per push.
- `r_cmd_clear_depth` with a rectangle is a draw of the blit vertex shader with a viewport of
  depth range [1, 1] and depth writes always (D3D11 clears whole views only).
- Readback copies into a staging texture, read after the command list ran.
- Mirror window: a flip-model `IDXGISwapChain1` (`FLIP_DISCARD`, else `FLIP_SEQUENTIAL`) on the
  HWND, `B8G8R8A8_UNORM` with sRGB render target views, presented with sync interval 0.
  `MakeWindowAssociation` keeps DXGI from Alt+Enter. Destroying it flushes the immediate
  context, since a window gets a new flip-model swapchain only once the old one is gone.

### D3D12 (`src/render_d3d12.cpp`)

The device wraps the app's `ID3D12Device` and its direct `ID3D12CommandQueue`.

- Slots (frames, then the immediate buffer) each have an allocator, a command list, a fence
  value and a 1024-descriptor range of the shader-visible heap; a slot is reused once its
  fence passed (`SetEventOnCompletion` with a null event blocks, so no Win32 event).
- Root signature: 32 root constants at `b0` (push constants), a table of two SRVs (`t0`, `t1`)
  and a table of two samplers (point at 0, linear at 2 of a 4-entry sampler heap).
  `D3D12SerializeRootSignature` comes from `d3d12.dll` at runtime.
- Image layouts are resource states tracked per image in recording order, which is the order
  the app's queue runs them. Images are created in the state of their initial layout; render
  passes move attachments to `RENDER_TARGET` / `DEPTH_WRITE` at begin and to the final layout
  at end, as Vulkan render passes do. Swapchain images stay in `RENDER_TARGET` (colour) or
  `DEPTH_WRITE` (depth) between frames, as `XR_KHR_D3D12_enable` requires.
- CPU descriptor heaps (views, render targets, depth) with free lists; the views of a texture
  binding are copied into the slot's shader-visible range.
- Host-visible vertex, index and uniform buffers live in upload heaps, mapped; staging buffers
  in host memory. Copies go through transient upload or readback buffers with D3D12's row
  pitch, released once the slot's fence passed.
- Pipelines bake the render pass formats; blit pipelines are made per target format.
- Mirror window: a flip-model `IDXGISwapChain3` on the app's queue with two buffers, imported
  as images in `PRESENT` state.

### Metal (`src/render_metal.mm`)

The device wraps the app's `id<MTLCommandQueue>` (`R_MetalDeviceInfo`); the device is the
queue's. The only Objective-C++ file, with manual reference counting: every object kept in a
struct is retained, and calls that make autoreleased objects run in a pool of their own, since
the app's thread may have none. Apple's headers come first with `Handle` and `Point` renamed
and `os_retain`/`os_release` undefined, which clash with puppetxr's names.

- Images are private `MTLTexture`s: 2D for one layer, 2D array for more, cube or cube array for
  cube images, as apps expect swapchain textures. A 2D or cube texture bound to a layered
  sampler gets a 2D array view, made once. Every size and mip count is checked first: Metal's
  validation aborts the process on a descriptor it refuses.
- Host-visible buffers are shared (Apple GPUs share memory), so flushes are free; the others
  private and never mapped.
- Slots (frames, then the immediate buffer) each record one `MTLCommandBuffer` of the app's
  queue; a slot is reused once its last buffer completed. Commits keep the queue's order, and
  Metal tracks hazards on the app's textures.
- Render state (pipeline, push constants, textures, viewport, scissor, vertex buffer) is kept
  and applied at each draw. Push constants are `setVertexBytes` and `setFragmentBytes` at 0.
- No layouts; `r_cmd_clear_depth` is a draw of the blit vertex shader without fragments through
  a viewport of depth range [depth, depth]; clears and blits are render passes of their own.
  Internal pipelines are made per pass format.
- Readback blits into a shared buffer; depth with stencil comes out as its depth.
- Formats: no 24-bit depth (Apple GPUs lack it), so `D24_UNORM_S8_UINT` is not offered.
- Mirror window: the window's `CAMetalLayer` (`os_window_metal_layer`, the view's layer, made on
  first use and kept with the window), `BGRA8Unorm_sRGB`, `displaySyncEnabled = NO`
  so it never waits for the display. `r_end_frame_present` presents the drawable with its
  command buffer. A session presents to any target `window_target_set` accepts (Vulkan surface,
  native window, Metal layer); `OxrMetalRealWindowTest.frames_composite_and_present_to_the_mirror_window`
  checks the window's swapchain and a present, not only the composited pixels.

## Swapchains (`src/oxr.cpp`)

A swapchain is 3 images created by the runtime on the app's device, colour ones in
`COLOR_ATTACHMENT_OPTIMAL`, depth ones in `DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, which is
the layout the OpenXR Vulkan binding requires at acquire and release. Acquire hands
out indices round robin, `acquired` is the FIFO of images the app holds, and release
pops the oldest and remembers it as `last_released`, the image the compositor shows.
Supported: formats in `oxr_swapchain_formats`, `arraySize` up to `R_MAX_IMAGE_LAYERS`
(256, Vulkan's guaranteed minimum), any `mipCount` up to a full chain (more is
`XR_ERROR_VALIDATION_FAILURE`), one sample, `faceCount` 1 or 6. A cube swapchain (6) has square faces
and `arraySize * 6` layers, face `f` of element `i` at layer `i * 6 + f` in Vulkan order (+X, -X, +Y,
-Y, +Z, -Z), created `CUBE_COMPATIBLE` so apps can make cube views; it only feeds cube layers, and
every other layer and depth info takes `faceCount` 1 (`XR_ERROR_VALIDATION_FAILURE`). The compositor samples level 0
only. Only the first `R_MAX_LAYER_VIEWS` layers of an image get attachment views, because the
runtime never renders into app swapchains.

## Compositor (`src/comp.h`, `src/comp.cpp`)

`comp_compose` clears an `R8G8B8A8_SRGB` target at the native eye resolution and composites
the layers in submission order for the selected eye.

- The target is one eye wide for `COMP_ViewMode::Left`/`Right` and two eyes wide for `Both`;
  it is recreated when the mode changes. With `Both`, eye `i` is drawn into horizontal slot
  `i` with its own viewport.
- Screenshots are this target, so they are at native resolution regardless of the window.
- `XR_FB_space_warp` motion vectors and depth never reach the compositor: frames are not
  extrapolated, every frame is the app's (`docs/openxr.md`).
- The clear is black, the initial composite colour of spec 10.6.2 (with alpha 1 so that
  screenshots are opaque).
- The lens mask (`views.lens_mask`, off by default; `views.mask_radius`, the hidden area of
  `XR_KHR_visibility_mask`) is drawn black over each eye after its layers (`COMP_FLAG_LENS_MASK` of `composite.frag`: outside a
  circle of that radius in the eye's NDC), so the corners show what the wearer sees whatever the
  app drew there. Screenshots and the sample references have it.

The compositor does no 3D math. `xrEndFrame` samples the simulator at
`XrFrameEndInfo::displayTime`, takes both display eyes (`sim_sample_view`) and turns each
layer into one `COMP_View` per eye: the swapchain sub-rectangle plus the clip-space positions
of its four corners. `composite.vert` draws that quad (two triangles; push constants `COMP_Push`: corners or rays,
source rect in UV, array layer, flags). Perspective-correct interpolation of the UV makes
this exact:

- Projection views (`place_projection_view`) are reprojected for rotation only. The view's
  fov corners are directions in the app's view frame. They are rotated into the display
  eye and projected, so the image sits at infinity. When the app rendered with the pose and
  fov from `xrLocateViews`, this is a 1:1 blit. A view rendered from another orientation
  lands where it belongs.
- Quads (`place_quad`) are rectangles of `size` at `pose` in their space, transformed into the
  eye and projected. The back face is skipped (spec 10.6.3: only the +Z side is visible), and
  so are eyes excluded by `eyeVisibility`. The GPU clips what is behind the eye.
- Cylinders and equirect spheres (`COMP_LayerType::Cylinder`, `Sphere`) are not flat, so they
  are ray cast. `place_curved` gives each eye `COMP_View::rays`: a pixel's ray in the layer's
  frame is linear in its NDC, and the eye position rides in the w components. The vertex shader
  covers the viewport; the fragment shader intersects the ray with the surface and keeps the
  far hit, the inside, the only visible side (spec 12.7.1, 12.9.1). Radius 0 is infinite: the
  ray direction alone picks the point. `COMP_Layer::uv_map` turns angle and height or latitude
  into texture coordinates; outside [0, 1] the pixel is discarded. Sampling is at level 0 with
  `textureLod`, since the mapping has seams.
- Cube layers (`COMP_LayerType::Cube`) use the same rays with the eye at the origin. The shader
  picks face and coordinates as a samplerCube does (Vulkan cube map face selection) and samples
  the 2D array view at `array_layer + face`, so no cube view or second pipeline is needed. Faces
  are clamped at their edges instead of filtered across them.
- A layer whose space cannot be located at the display time is not drawn.
- Filtering is linear, or nearest when the swapchain's `XR_FB_swapchain_update_state_vulkan`
  mag filter is nearest (`COMP_View::nearest`).
- Secure layers (`COMP_Layer::secure`, `XR_FB_composition_layer_secure_content`): the target,
  which the mirror window shows, has every layer, since the mirror stands in for the headset.
  A frame with a secure layer is composed a second time into `capture`, made at the first such
  frame (`ensure_capture`): `Exclude` layers are left out, `Replace` layers drawn opaque black
  (`COMP_FLAG_REPLACE`). `comp_read_target` and `comp_save_png`, which screenshots use, read
  `capture` after such a frame; `comp_read_mirror` always reads the target.
- Snapshots (`comp_set_snapshots`, `comp_snapshot`): the readers answer from a copy that
  `comp_snapshot` took off the GPU instead of reading it themselves, for callers that may not use
  the app's queue or immediate context: every reader while the control server is on (docs/openxr.md, Queue access). The copy holds what
  screenshots read and, after a frame with secure layers, the mirror after it; it is made for the
  target's size and dropped when the target is remade.
- `fovMutable` is reported true: views of any fov are placed by their own angles. A flipped fov
  (left > right or down > up) is a mirrored image of the same region and is drawn mirrored;
  only an empty fov or an angle of 90 degrees or more is skipped (`fov_valid`).
- Regression coverage of what the CTS composition tests show: `cts_composition`
  (`docs/testing.md`).

Blending follows spec 10.6.2 with one premultiplied pipeline (`ONE`, `ONE_MINUS_SRC_ALPHA`):

- Without `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` the shader sets alpha to 1.
- With `XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT` it uses 1 - alpha, before premultiplying.
- With `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT` it multiplies rgb by alpha.
- `COMP_Layer::color_transform` (`XR_KHR_composition_layer_color_scale_bias`) applies
  `color * max(scale, 0) + bias` to the unpremultiplied color, then premultiplies again.
- `COMP_Layer::flip_y` (`XR_FB_composition_layer_image_layout`) negates the source rect height.
- `COMP_Layer::blend_custom` (`XR_FB_composition_layer_alpha_blend`) replaces the pipeline's
  factors with the layer's `blend_factors` (source and target colour, source and target alpha,
  `R_BlendFactor`). The shader then keeps the texture's alpha whatever the layer flags say,
  still inverts it, never premultiplies and applies a colour transform to the colour as it is.
  In a frame with such a layer the target clears to alpha 0 (spec: destination alpha starts at
  0); `comp_read_target` and `comp_read_mirror` return alpha 255, as the display shows none.
- Layer pipelines besides the default one are made when first used and cached by
  `COMP_PipelineKey` (depth test, compare op, depth write, blend factors), at most
  `COMP_MAX_PIPELINES`; `comp_compose` makes them before recording, `draw_layers` only looks
  them up.
- Push block flags: `COMP_FLAG_SOURCE_ALPHA`, `UNPREMULTIPLIED`, `INVERTED_ALPHA`,
  `COLOR_TRANSFORM`, `CUSTOM_BLEND`. Blocks and flags are defined once in `shaders/blocks.h` for the shaders
  and C++ (`docs/shaders.md`); `tests_render.cpp` pins the layout.
- sRGB sources are decoded by the sampler and re-encoded by the target, so blending happens
  in linear; UNORM sources are treated as linear.

Depth composition (`XR_FB_composition_layer_depth_test`, depth from
`XR_KHR_composition_layer_depth`):

- The compositor's depth is `1 - COMP_DEPTH_NEAR / view depth` (`shaders/blocks.h`, 0.01 m): an
  infinite far plane at 1, which the render pass clears to; nearer than 1 cm clamps to 0.
- Quads are rasterized at that depth (`eye_clip` in `src/oxr.cpp`: clip z = w - near), so the
  GPU clips the part of a quad nearer than 1 cm. Projection views stay at infinity (z = w) and are
  never clipped. Cylinders and spheres write the depth of their ray hit (every ray has view depth
  1, so the hit distance is the view depth), cube layers and infinite radii depth 1.
- A projection view's depth (`COMP_DepthView`) is affine in its inverse view depth, as the
  compositor's is: `validate_depth_info` derives the map `map[0] + map[1] * value` from
  minDepth, maxDepth, nearZ and farZ (reversed and infinite included). `resolve_depth` draws each
  depth-tested projection layer's views, at the place their colour lands, into an array layer of
  `resolved` (RGBA8 UNORM, `r_pack_unorm24`, as GLES 3.0 cannot render float colours) with
  `composite_resolve.frag`; the source depth images go from `DepthStencilAttachment` to
  `ShaderReadOnly` and back around the frame.
- Depth-tested layers draw with `composite_depth.frag` through `depth_pipelines[compare op][depth
  write]`, made when first used: depth test with the layer's `XrCompareOpFB`, writes with its
  `depthMask`; the shader writes the resolved depth (`COMP_FLAG_DEPTH_RESOLVED`, at
  `R_FRAG_COORD`), the curved or cube depth, or the rasterized one. Layers without the struct
  draw as before, neither tested nor writing.
- The depth is the layer's own view depth, the same as the display eye's when the app rendered
  with the poses of `xrLocateViews`; rotation-only reprojection does not correct it.
- Spec: a depth test on a layer without depth is ignored: a projection layer without depth
  infos, or a view of one without its own, draws untested. Passthrough layers are never tested,
  and since their room depth is not composition depth, the depth is far again after each.

There is no lens distortion; the mirror view is meant for inspection, not for a headset.

Passthrough (`COMP_LayerType::Passthrough`, `XR_FB_passthrough` and the `ALPHA_BLEND`
environment blend mode):

- `comp_set_room` uploads the room's visual mesh (host-visible vertex and index buffers) and
  albedo atlas (`R8G8B8A8_SRGB`, mid gray for untextured rooms). `xrEndFrame` calls it when
  `OXR_Session::room_uploaded` differs from `world_generation`.
- A passthrough layer carries one `clip_from_room` matrix per eye (`place_passthrough`: eye
  pose and fov at the display time, near 0.05 m, far 1000 m) and a style. `passthrough.vert`/
  `.frag` draw the mesh unlit with depth testing into the target's `D32_SFLOAT` depth
  attachment, which is cleared per passthrough layer so layers never see each other.
- Styles: opacity (alpha, honoured only with source alpha, like any layer), edge colour
  (screen-space luminance gradient), and one of mono to RGBA, mono to mono (a 256x1 LUT
  image per layer slot, `luts`, re-uploaded when its hash changes) or brightness, contrast
  and saturation in CIELAB per the extension's formulas. Map colours are sRGB-encoded.
- `XR_META_passthrough_color_lut` (`COMP_ColorMap::Lut`, `InterpolatedLut`): the style's 3D
  LUTs go into a 512x1024 atlas per layer slot (`color_lut_atlases`, made when a slot first
  uses one, layout in `shaders/blocks.h`), bound in place of the 256x1 map and uploaded again
  when `color_lut_key` (generations and resolutions) changes. `passthrough.frag` interpolates
  trilinearly between texel centres, so the sampler's filter does not matter, and blends in
  sRGB: `(1 - w) * C + w * lut[C]`, or `(1 - w) * source[C] + w * target[C]`; source and target
  may differ in resolution. Without an atlas (out of memory) the imagery shows unmapped.
- The result is premultiplied and blended like other layers, so app layers after it draw
  over the room and layers before it are hidden.
- `ALPHA_BLEND` frames put a default-styled passthrough layer below all app layers.
- Props (`comp_set_props`, `docs/props.md`) draw with the same pipeline after the room, from
  their own frame-slotted buffers and atlas with nearest filtering, so QR codes stay readable.
- Projected layers (`COMP_Layer::projected`, `XR_FB_triangle_mesh` geometry instances) show the
  room only on the app's meshes, as the cameras' images projected from the eye onto them look:
  depth is cleared to 0, which blocks the room, then `mask_pipeline` (`environment_depth`
  shaders, compare always, no colour writes) draws each `COMP_Geometry` with clip z = w, which
  opens depth 1 where it covers, then the room draws as usual. `upload_geometry` copies the
  frame's meshes into one of `COMP_GEOMETRY_SLOTS` host-visible buffer pairs, made with the
  first projected layer; a slot is reused only after every frame that read it finished. More
  than `COMP_MAX_GEOMETRY` meshes, `COMP_GEOMETRY_VERTICES` vertices or `COMP_GEOMETRY_INDICES`
  indices per frame are left out with a warning.

Environment depth (`COMP_DepthTarget`, `XR_META_environment_depth`): D16 images of two
layers with `Sampled | DepthStencilAttachment | TransferSrc` usage and one framebuffer per
layer, rendered by a depth-only pass (`environment_depth.vert`, an empty fragment shader)
whose attachment starts and ends in `ShaderReadOnly`. `comp_render_depth` submits on the
app's queue from inside `xrAcquireEnvironmentDepthImageMETA`, which the extension allows;
queue order puts it before the app's sampling.

Passthrough cameras (`COMP_CameraTarget`, `XR_METAX1_passthrough_camera_data`): a colour and a
depth image in the compositor's formats with a framebuffer of `render_pass`.
`comp_render_camera` draws the room and props with `draw_passthrough` and a neutral style from
one clip-from-room matrix, waits for the GPU and reads the image back (`read_opaque`), so it runs
in `xrEndFrame` like the snapshots.

Workaround: `composite.vert` selects the corner with constant indices. Dynamically indexing
the push constant array draws nothing on RADV (Mesa, NAVI22).

Screenshots: `comp_read_target` reads the target back (`r_read_image`),
`comp_save_png` encodes with `stb_image_write`. `xrEndFrame` writes
`<screenshot_dir>/puppetxr_frame_<n>.png` when `screenshot_at_frame` matches and requests
session exit at `exit_at_frame` (`docs/config.md`). The accessors `oxr_read_frame` and
`oxr_save_screenshot` in `oxr.h` expose the same for tests and the control API.

## Mirror window and UI

`xrCreateInstance` opens the window (`os_create_window`) unless headless, hidden until the
first `xrEndFrame` (`os_window_show`; engines take seconds from the instance to their first
frame, and probe instances never draw), which shows it before presenting and makes the window
swapchain again at the shown, maximized size, so the first frame is not stretched, maximized
unless `window.maximized` is false (`os_window_maximize`; the GLX re-creation keeps the state), and raised in
front of the app's own windows, as an editor's play window (Windows allows it while the process is in front, as an
editor is when its play button is clicked). Destroying the last session hides the window again: an editor keeps its
instance between play sessions (Unreal's VR Preview), and the next session's first frame shows it. On Windows and
Linux the window and its video driver live on a thread of the platform layer (`docs/architecture.md`); window calls
go there through `os_run_on_window_thread`. The
Vulkan surface is created as soon as the app's `VkInstance` is known
(`oxr_vk_ensure_surface`, from `xrCreateVulkanInstanceKHR` or
`xrGetVulkanGraphicsDevice(2)KHR`), and the physical device that can present to it is
preferred. The runtime adds `VK_KHR_surface` plus the video driver's surface extension to
the app's instance and `VK_KHR_swapchain` to its device: appended inside the
`XR_KHR_vulkan_enable2` create calls, listed by `xrGetVulkan*ExtensionsKHR` for
`XR_KHR_vulkan_enable`.

Per session, `r_create_window_swapchain` wraps a `VkSwapchainKHR` on that surface if
the session's queue family can present (otherwise the session composites offscreen
only and logs a warning). `present_frame` in `oxr.cpp` runs after `comp_compose` in
`xrEndFrame`: acquire a window image, clear it to black, blit the composite target into the largest
rectangle that fits without upscaling (black bars when the window is larger than the
eye resolution), draw the ImGui panels built last (`ui_render`), submit waiting on the acquire semaphore and signalling the
present semaphore, present. A failed acquire or a resize event recreates the window
swapchain on the next frame.

### One flow on every platform: frames and the window tick

Frames never call the window: an engine may end frames on a render thread while its main thread,
the window thread on macOS, blocks until that thread is done (Unity's editor and players), and
every window call from the frame waited out `OS_MAIN_THREAD_TIMEOUT_MS`. So:

- A frame composites, presents the mirror and records the UI frame built last (`ui_render`), all
  on the app's queue. A hidden window gets no frame.
- `xrEndFrame` then runs `window_tick`: at once where the frame ended on the window thread
  (`os_is_window_thread`), else posted to it (`os_post_to_window_thread`, one at a time:
  `window_tick_pending`).
- The tick pumps the window's events before taking the runtime lock (the pump runs the app's run
  loop, where an editor's tick may wait for a frame that holds the lock), then shows the window
  after its first frame (the next frame is drawn at the shown size), applies relative mouse mode
  (`window_relative_pending`), `os_window_keep_cursor`, caches the window's pixel size
  (`window_pixel_width`, `window_pixel_height`), and builds the next UI frame (`ui_build`, whose
  window calls then run without waiting) and applies its requests.
- The frame reads only caches: a click is checked against the cached size, a resize event carries
  its pixel size, and capturing the mouse only marks relative mode pending.
- Windows and Linux have a window thread of their own (`HAVE_WINDOW_THREAD` in `platform.cpp`),
  macOS the app's main thread. The window thread sleeps until the window system has events or a
  call, a post or a quit wakes it (`os_sys_wake_signal`: a pipe on Linux, an event on Windows).
- A posted tick on the window thread waits for the runtime lock, whose holder may wait for a
  window call there (`xrDestroySession` shutting the panels down): the window thread waiting in
  `os_mutex_lock` runs the waiting call (`call_ticket`: the window thread's loop or its mutex
  wait runs it, whichever claims it first). `xrDestroyInstance` shuts the platform down after
  the lock, as the window thread quits only after a tick waiting for it.
- `os_push_*_event` puts a synthetic event straight into the queue for the next poll.

The mirror window never paces the app (each rule came from a profile):
1. It never waits for the display: Vulkan presents with MAILBOX, else IMMEDIATE, else FIFO (with
   FIFO MoltenVK waits in `vkQueueSubmit` for a drawable, and Godot on a 60 Hz display ran at 53
   instead of 100 frames a second); GL swaps with interval 0, D3D with sync interval 0, Metal with
   `displaySyncEnabled = NO`.
2. It never waits for the GPU: while the window's last present is still on the GPU
   (`r_window_busy`: Vulkan the fence of the frame slot that presented, as long as no later frame
   was submitted in that slot, which waited for that fence first; Metal the presenting command
   buffer's status; null for GL and D3D, which never wait) the frame skips the mirror. The slot's
   fence alone is not enough: the compositor's next frames reuse the slot, and with frames coming
   faster than the GPU finished them the window stayed busy and never presented again.
3. It presents at most at the display's refresh rate (`os_window_refresh_rate`, cached in
   `window_refresh_hz` by session creation and the tick): the window server holds a presented
   drawable until its next composite, and faster presents held all three and the acquire waited.
4. Showing a hidden maximized window takes milliseconds: on macOS the window gets the screen's
   visible frame without `-[NSWindow zoom:]`'s 240 ms animation, and `os_window_maximized` asks
   Cocoa. A Wayland show waits for the compositor's first configure instead (`docs/architecture.md`).

Tests: `OxrVulkanRealWindowTest.*` (60 frames in under 300 ms, presents at most at the display
rate, busy only while on the GPU, not kept busy by the frames after a present),
`Platform.showing_a_maximized_window_does_not_wait_for_an_animation`.
Tests on real windows take the ctest lock `real_window` (`tests/CMakeLists.txt`): side by side
they stole each other's keyboard focus.

Tests: `OxrThreadWindowedTest.*` run frames on a thread of their own while the test's main thread
waits for it without serving; a frame takes under 50 ms and input reaches it. Where the window
thread is a thread of its own, `OxrTest::frame` waits for the tick after each frame (`frame_only` does not).

Events are taken in `xrWaitFrame` (`pump_window_events`, `os_poll_queued_events`: what is queued,
never a pump), 64 a frame; the rest wait for the next poll, so none is lost after a stall of the app. The window thread's queue (512) drops key repeats
and merges mouse moves once half full, so a stall with a held key or a moving mouse cannot push
out a key release. Keys go to `sim_control_key` unless ImGui is editing text.

Mouse capture (`capture_mouse`): a left click into the view takes the mouse, in relative mode,
and is not a trigger press; a click outside the client area (the focus click on the title bar)
takes nothing. A click on a panel or while a popup is open stays the UI's
(`ui_wants_click` looks at the click's own position, as a click may come in the frame of the move
that brought the mouse there). While captured, mouse buttons go to `sim_control_key`, motion to
`sim_control_mouse_motion`, and the UI gets no mouse or key event (`ui_set_captured`, the raw
event hook); the press of the click that took it is dropped from ImGui's queue, as its release
never reaches the panels. Escape, showing the panels or the loss of focus gives the mouse back and
releases its buttons in the simulator; uncaptured, mouse buttons and motion never reach the simulator. Other windows of the
process clip the cursor too (Unreal to its game window while that is in front, lifting it again
later), so each window tick `os_window_keep_cursor` puts the window's clip back while it holds the mouse
and frees the cursor while the window is in front without (`os_cursor_clip_action`; Windows). Closing the
window or the UI's quit menu item requests session exit (`xrWaitFrame`, also when nothing can be
presented); destroying that session clears the request, so an editor's next session runs.
`controls.ui` (Tab) and F1 are always taken by the window unless a text field is
being edited; Enter and Escape close the welcome overlay while it shows, before Escape gives the
mouse back.

`ui.cpp` is the only file including ImGui. It feeds ImGui the platform layer's raw events
(`ui_handle_raw_event`), sets the cursor shape, clipboard and input method position through the
platform layer, and renders on `render.h` (`imgui.vert`/`imgui.frag`, per-frame host-visible
vertex and index buffers that grow on demand, textures created and updated through
ImGui's `ImTextureData` requests). The style (`apply_style`) is a neutral charcoal with
one steel-blue accent at three brightnesses (`UI_ACCENT*`), 4 px rounding on windows and
3 px on controls, and the scalable default font at 14 px. Panels are edge-anchored and fixed width
(`begin_panel`/`end_panel`), each auto-fits its height up to the window bottom and
cannot be moved or resized:

- Simulator (left): collapsing sections Session (frame and time, profile,
  keyboard and mouse on/off, clock, reset, reset hands, recenter), Display (mirror view,
  screenshot, eye size and rate), Recording (record, frame count and path, replay
  progress and divergences), Controllers (type) and Room (room, stage origin, anchor
  count, wipe buttons; collapsed at start).
- Devices (right): a tab per device with active, pose, the hands' source (controller
  or tracked hand), the inputs that hand has (`sim_hand_has_input`: a Touch's X, Y and menu on
  the left, A, B and system on the right) and haptic amplitudes. Buttons are pressed while held,
  floats and sticks are sliders; `UiTest.every_controller_input_of_the_devices_panel_reaches_the_simulator`
  clicks each. The selected tab follows the control
  mode whenever that changes (body selects Head). A Body tab edits body tracking
  (`docs/body-tracking.md`): a front and a side view of the skeleton in the body's frame, height,
  pose preset, which limbs are posed, a joint's rotation, tracked and confidence, reset, and the
  bone scales (left and right linked unless unlinked). Its `draw skeleton` toggle draws the
  joints over each eye the mirror shows, projected with the simulated views (`sim_get_view`),
  behind the panels; screenshots stay without them. Right side joints are blue, untracked ones
  amber. `UiTest.body_tab_shapes_poses_and_draws_the_body`. A Face tab edits face and eye tracking
(`docs/face-tracking.md`): preset, blinks, the gaze or a point to look at, confidences, the
expressions (but the eye blendshapes, which follow the gaze) and the visemes, reset.
`UiTest.face_tab_sets_expression_blinks_and_gaze`. The device tabs are named Head, Left and
Right, short enough for all five to show.
- Virtual keyboard (right, below Devices) while the app has one: a text field typed into
  the shown keyboard at the next frame, or with "live typing" each key as it is
  pressed, plus backspace and enter.
- Props (right, below): add, edit and remove props (`docs/props.md`).
- Log (bottom).

UI tests click the panels as a person does: ImGui's test engine hooks (`IMGUI_ENABLE_TEST_ENGINE`,
implemented in `ui.cpp`) record each labelled item's rectangle while probing (`ui_set_probe`),
`ui_find_item` returns its centre and `os_push_mouse_event` sends mouse events into the
window (`UiClicker` in `tests/tests_ui.cpp`; `puppetxrctl ui click` in a running app). ImGui's
combos report no label, so the panels make them through `combo` and `label_item`. Each UI frame
also takes the system cursor over the window (`os_window_cursor`, Windows), as the editor of an
engine may keep the mouse's moves from it; a window that got pushed events skips that, so a real
cursor resting over it cannot move a pushed click onto a panel.

`controls.ui`, Tab by default, shows and hides them (the Simulator panel's hint names the key);
they start hidden. A main menu bar (visible with the UI)
has a Simulator menu (screenshot, recenter, start or stop recording, stop replay,
quit app), a View menu that toggles each panel and a Help menu (welcome overlay, show
welcome at start). Disabled widgets explain why in a tooltip. While a replay drives
the simulator, device editing is disabled. A REC or REPLAY badge in the top-right
corner stays visible even when the panels are hidden. Recordings started from the UI
go to `<screenshot_dir>/puppetxr_recording_<frame>.xrrec`. The Log panel spans the bottom and shows the log history
(`os_log_history`, the last 512 lines): our own `LOG_*` calls and, while the window is
open, the app's stdout and stderr, which `os_capture_output_begin` routes through a
pipe and tees back to the terminal (`os_capture_output_poll` runs once per frame).
Checkboxes filter the two sources; new lines auto-scroll. Button inputs are momentary: pressed while the mouse button is held on them. Vertex colours are decoded from
sRGB to linear in `imgui.vert` when the target is an sRGB format, otherwise the sRGB
backbuffer would encode them a second time and wash them out. The panels edit sim
state directly: device poses, inputs (only those the selected controller type has),
clock mode, controls on/off, controller type (disabled while recording or replaying),
screenshot, recenter, quit.

### Welcome overlay

`welcome_overlay` in `ui.cpp` draws a full-window dark layer (alpha 0.96, blended in linear
space on sRGB targets: a bright scene stays faintly visible and the text readable) with the
window keys, move and look keys, controller buttons and where to get help. Key names
come from the config through `os_key_label`, so rebindings and keyboard layouts show
correctly. It takes no input, so the simulator stays drivable while it shows, and it
is raised above the panels every frame. Two columns from 952 px of window width, one below;
the body is clipped so the footer with the closing keys always shows.

- Shown at instance creation when there is a window, `show_welcome` is on
  (`PUPPETXR_SHOW_WELCOME=0` turns it off) and `state.json` has no `welcome_dismissed`
  (`docs/config.md`). Drawn whether or not the panels are hidden.
- F1 toggles it at any time; Enter closes it for good, Escape for this run only
  (`welcome_key_locked` in `oxr.cpp`). Help > Show welcome at start flips the
  remembered choice.
- `oxr_welcome_key` and `oxr_welcome_shown` (`oxr.h`) drive it from tests.

### Performance tab

The Simulator panel has two tabs: Simulator (the session, display, recording, controllers and room
sections) and Performance (`performance_tab` in `ui.cpp`), which shows the app's frame rate, mean
and slowest frame time, puppetxr's share and the mirror window's presents a second, over the last
second of frames (`ui_frame_stats`), and an ImPlot graph of the last `UI_FRAME_TIMES` (300) frames,
the newest on the right, with the display's budget (1000 / `refresh_rate_hz` ms) as a line.

- `time_frame` in `oxr.cpp`, deferred to the end of every committed `xrEndFrame` (headless too),
  keeps the time since the previous one and how long the runtime lock was held in it (`oxr_lock`,
  `oxr_unlock`; frame pacing and the frame gate wait unlocked, so they do not count) in the ring
  `OXR_Runtime::frame_times` (`UI_FrameTimes`, `ui.h`). The lock is exclusive, so puppetxr's share
  never exceeds the frame. A new session starts a new series.
- `present_frame` counts the mirror's presents; `time_frame` turns them into presents a second
  once a second.
- Wall clock only, never fed to the simulation, so determinism holds (`docs/determinism.md`).
- Tests: `OxrTest.frames_are_timed_for_the_performance_tab`, `UiFrameStats.cover_the_last_second_of_frames`,
  `UiTest.performance_tab_shows_the_frame_rate_and_the_frame_times` (`PUPPETXR_TEST_DUMP_DIR` writes
  the rendered tab), `OxrMetalRealWindowTest.frames_composite_and_present_to_the_mirror_window`
  (presents counted).

## Headless

Without a window nothing changes in the compositing path: the target is offscreen
anyway, only the blit, the UI and the event pump are skipped.
