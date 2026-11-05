# Shaders

One source per stage in `shaders/`, written in GLSL with the macros of `shaders/shader.h`, so that
the same file builds for every render backend. The approach is bgfx's: GLSL is the base
language, the HLSL prelude maps GLSL names, and declarations go through macros.

| Backend | Define | Compiled | Status |
|---|---|---|---|
| Vulkan | `R_SHADER_VULKAN` | `glslc -std=450core` at build time, SPIR-V embedded (`<file>.spv.h`) | used |
| OpenGL 3.3 core | `R_SHADER_GL` | by the driver at pipeline creation (`render_gl.cpp`) | used |
| OpenGL ES 3.0 | `R_SHADER_GLES` | by the driver at pipeline creation (`render_gl.cpp`) | used |
| D3D11, D3D12 | `R_SHADER_HLSL` | shader model 5.0 DXBC at build time on Windows (`puppetxr_hlsl`, `D3DCompile`), embedded | used |
| Metal | none | `spirv-cross --msl` from the Vulkan SPIR-V at build time on macOS, MSL source embedded, compiled at the first pipeline that needs it | used |

The stage is `R_STAGE_VERTEX` or `R_STAGE_FRAGMENT`, from the file extension. Files have no
`#version`: glslc gets `-std`, and GL backends prepend `#version` and the defines.

## D3D bytecode

On Windows `cmake/shaders.cmake` builds `tools/hlsl_compile.cpp` as `puppetxr_hlsl` and runs it on
the single-source text (includes inlined) with `vs_5_0` or `ps_5_0`, warnings as errors. It
calls `D3DCompile` of `d3dcompiler_47.dll`, the compiler `fxc` wraps, which every Windows since
10 has. Its preprocessor rejects a function-like macro passed by name (`R_PUSH_BLOCK(COMP_PUSH_FIELDS)`),
so the C compiler preprocesses the text first (`<build>/shaders/<file>.hlsl`). fxc also warns
on `pow` of a base it cannot prove non-negative; clamp the base with `max`. The DXBC lands in `R_ShaderCode::dxbc`; D3D12 runs shader model 5.0 bytecode as well,
so both backends share it. Elsewhere `dxbc` is null. Cross builds run the tool through
`CMAKE_CROSSCOMPILING_EMULATOR` (Wine); Wine's compiler has neither `atan` nor `atan2`, so the
`mingw-debug` preset passes `R_HLSL_NO_ATAN2` (`PUPPETXR_HLSL_DEFINES`) and `shader.h` substitutes
a polynomial for tests under Wine only.

`blit.vert` and `blit.frag` draw scaled copies for backends without a blit command (D3D, Metal);
they are built for every backend like the others.

## Metal source

On macOS `cmake/shaders.cmake` translates each SPIR-V file with `spirv-cross --msl --msl-version
20300 --msl-decoration-binding`, vertex shaders with `--flip-vert-y` as well: Metal puts NDC
y = +1 at row 0, where Vulkan puts y = -1. The MSL lands in `R_ShaderCode::msl` (entry point
`main0`; null elsewhere). Only Xcode has the `metal` compiler, so `render_metal.mm` compiles the
source with `newLibraryWithSource` when a pipeline first needs it, once per shader, with fast
math off (determinism). The translation fixes the interface: the push block is buffer 0 of both
stages, texture and sampler N binding N, and the backend puts the vertex buffer at index 30.

## Files

- `shaders/shader.h`: the prelude, one section per language.
- `shaders/composite.h`: the compositor's layer shader, included by `composite.frag` and, with
  `COMP_DEPTH` defined, `composite_depth.frag` (writes the layer depth for
  `XR_FB_composition_layer_depth_test`); `composite_resolve.frag` converts a projection view's
  depth into the compositor's. Including files declare the textures, so the embedded names match
  the shader (`cmake/embed.cmake` reads them from the file itself).
- `shaders/blocks.h`: every push block as a field X-macro, plus `COMP_FLAG_*`. The shaders
  expand it with `R_PUSH_BLOCK`; C++ (`comp.cpp`, `ui.cpp`, tests) gets `COMP_Push`,
  `COMP_PassthroughPush`, `COMP_DepthPush`, `UI_Push`. One definition, no copies to keep in sync.
- `cmake/shaders.cmake`: `puppetxr_add_shaders`; `cmake/shader_text.cmake` writes the
  single-source GL text (includes inlined, line continuations joined).

## Declarations

| Macro | Meaning |
|---|---|
| `R_PUSH_BLOCK(FIELDS)` | the push block `push`; Vulkan push constants, GL std140 block `R_Push` (bound by name), HLSL `cbuffer R_Push : register(b0)` |
| `R_TEXTURE_2D(name, slot)` | a 2D texture with sampler at binding `slot` |
| `R_TEXTURE_LAYERED(name, slot)` | a texture of any swapchain: 2D array on Vulkan and HLSL; on GL `sampler2D`, `sampler2DArray` or `samplerCube` by `R_SAMPLER_2D`, default, `R_SAMPLER_CUBE` |
| `R_TEXTURE_ARRAY(name, slot)` | a 2D array texture on every backend, whatever the variant; for images of two or more layers made by the renderer (GL makes one layer a 2D texture) |
| `R_VERTEX_INPUTS(ATTRIBUTES)` | vertex attributes `X(type, name, location)`; always present, `R_NO_FIELDS` for none |
| `R_VARYINGS(VARYINGS)` | the same list in both stages; GL matches by name, so names are unique (`v_` prefix, attributes `a_`) |
| `R_VERTEX_MAIN_BEGIN` / `_END`, `R_VERTEX_RETURN` | vertex entry point and early return |
| `R_FRAGMENT_MAIN_BEGIN` / `_END` | fragment entry point writing `R_COLOR` |
| `R_FRAGMENT_DEPTH_MAIN_BEGIN` / `_END` | fragment entry point without colour output |
| `R_FRAGMENT_COLOR_DEPTH_MAIN_BEGIN` / `_END` | fragment entry point writing `R_COLOR` and `R_DEPTH` (`gl_FragDepth`, `SV_Depth`); every path that does not discard writes both |
| `R_IN(a)`, `R_VAR(v)`, `R_POSITION`, `R_VERTEX_ID` | attribute, varying, clip position, vertex index |
| `R_FRAG_COORD` | the fragment's window position and depth (`gl_FragCoord`, `SV_Position`); its xy addresses the same texel of a target in every backend |

Sampling: `r_sample(t, uv)`, `r_sample_layer(t, uv, layer)`, `r_sample_layer_lod(t, uv, layer, lod)`,
and unfiltered `r_fetch_array(t, xy, layer)` of an `R_TEXTURE_ARRAY` (`texelFetch`, `Load`).
`r_pack_unorm24(value)` and `r_unpack_unorm24(rgba)` carry a [0, 1] value through an RGBA8 UNORM
target exactly, at 24 bits.
The GL cube variant turns `layer` (the face) and `uv` back into a direction (`r_cube_direction`),
so the compositor's own cube face selection works unchanged; GL filters across face edges
where Vulkan clamps.

## Rules of the dialect

- No single-scalar vector constructors: `r_splat2/3/4(x)` (HLSL rejects `float3(x)`).
- Matrix times vector is `mul(m, v)`. Matrices from columns are `r_mat3_cols(c0, c1, c2)`;
  never a matrix constructor from scalars (GLSL fills columns, HLSL rows).
- Two-argument arctangent is `atan2(y, x)`.
- No array constructors or initializer lists; use a ternary chain or bit math.
- No global constants; a value shared with C++ is a `#define` in `blocks.h` (`16u` is valid in both).
- GLSL names that HLSL lacks are mapped in the prelude (`mix`, `fract`, `dFdx`, `lessThan`, ...);
  anything else needs a macro there first.
- Helpers may read `push`, textures and varyings: HLSL keeps inputs and outputs in statics.
- Avoid identifiers that are keywords in HLSL or MSL (`sample`, `point`, `line`, `vertex`,
  `fragment`, `texture`, `linear`).
- Comments inside macros are `/* */`; a `//` would swallow the line continuation.

## Push block layout

Fields are `F(type, name)` or `A(type, name, count)`. Arrays are of `vec4` or `mat4`, no field
crosses a 16-byte boundary and every block is a multiple of 16 bytes (`static_assert` in
`blocks.h`). Then Vulkan's push constant layout, std140 and HLSL constant buffer packing all
equal the C layout; `RenderBlockTest.push_blocks_have_the_std140_layout` pins the offsets. At most
`R_MAX_PUSH_CONSTANTS` (128) bytes.

## Clip space

Every backend gets Vulkan's conventions: clip depth [0, 1], NDC y = -1 at row 0 of the target.
GL also maps NDC y = -1 and texture coordinate t = 0 to row 0, so offscreen targets have the
same memory layout and screenshots compare across backends. D3D puts NDC y = +1 at row 0, so
the HLSL vertex entry point negates y after the shader body; texture coordinates already match.
GL without `glClipControl` remaps depth in the vertex entry point (`R_CLIP_ZERO_TO_ONE` defined by
the backend skips it).

## Validation

When `glslangValidator` and `dxc` are found, the build compiles every file as GL 330, GLSL ES 300
(fragment shaders in all three sampler variants) and HLSL (`dxc -spirv`, the front end; the DXIL
back end needs the validator library that Linux builds often lack). A failure fails the build.
Only a `dxc` that compiles to SPIR-V counts: the one of the Windows SDK, often first on `PATH` in
a developer shell, has no SPIR-V back end, so the search skips it and prefers the Vulkan SDK's.
Expanded GL sources are in `build/<preset>/shaders/variants/` for inspection.
