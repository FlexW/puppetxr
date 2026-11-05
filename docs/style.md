# Style

C-style C++20. The language is used for designated initializers, `enum class`,
`constexpr`, a handful of templates in `base.h`, and nothing else from the modern
feature set. No classes, no inheritance, no exceptions, no RTTI, no STL.

## Types

```cpp
typedef uint8_t  u8;   typedef int8_t  i8;   typedef float  f32;
typedef uint16_t u16;  typedef int16_t i16;  typedef double f64;
typedef uint32_t u32;  typedef int32_t i32;  typedef size_t usize;
typedef uint64_t u64;  typedef int64_t i64;  typedef bool   b8;
```

Always these, never `int`, `float`, `bool`, `unsigned`.

## Naming

| Kind | Form | Example |
|---|---|---|
| Module type | `PREFIX_PascalCase` | `R_Device`, `OXR_Session`, `SIM_Clock` |
| Base/math type | `PascalCase` | `Array`, `Handle`, `Vec3`, `Quat` |
| Public function | `prefix_verb_noun` | `r_create_image`, `sim_locate_device`, `oxr_verify_handle` |
| File-local function | `static`, plain `snake_case` | `static b8 create_pipelines (COMP_Compositor* comp)` |
| File-local type | in an anonymous `namespace { }` in the `.cpp` (internal linkage); opaque types declared in a header stay outside | `namespace { struct COMP_Push { ... }; }` |
| Variable, member, parameter | `snake_case` | `layers_count`, `predicted_display_time` |
| Count of an array | `name_count` | `views_count` |
| Output parameter | `out_name`, by reference | `R_ImageHandle& out_image` |
| Create info struct | `PREFIX_CreateXInfo` or `PREFIX_XInfo`, parameter `info` | `R_CreateImageInfo` |
| Enum | `enum class`, PascalCase values | `R_LoadOp::Clear` |
| Flag enum | `enum class X : u32 { A = 1 << 0 }`, stored as `u32` with a comment naming the enum | `u32 usage_flags; // R_ImageUsageFlags` |
| Constant | `#define PREFIX_UPPER_CASE` | `OXR_MAX_SWAPCHAIN_IMAGES` |
| Macro | `UPPER_CASE` | `ASSERT`, `LOG_ERROR`, `ARRAY_SIZE` |
| File | `snake_case.h/.cpp` | `render_vulkan.cpp` |

Exception: `defer` is lowercase.

## Formatting

- Allman braces everywhere, including single statements. An `if`, `else`, `for` or
  `while` body is never on the same line as its keyword and never without braces:

```cpp
if (keys_down[(u32)config.key_move_forward])
{
    move.z -= 1.0f;
}
```

- In a `switch`, the statements of a `case` start on their own line below the label,
  including a lone `return` or `break`:

```cpp
switch (kind)
{
case SIM_HandPose::Grip:
    return grip;
case SIM_HandPose::Aim:
    return zm::pose_mul(grip, config.aim_offset);
}
```

- 4 spaces, no tabs. No line length limit: a long line is preferred over a wrapped one
  because wrapped lines make the code look irregular. When a line gets hard to read,
  pull a sub-expression into a named local instead of wrapping.
- Pointer binds to the type: `R_Device* device`.
- Declaration blocks in headers and member lists are column-aligned; definitions
  and declarations have a space before `(`, calls do not:

```cpp
R_ImageHandle r_create_image  (R_Device* device, const R_CreateImageInfo& info);
void          r_destroy_image (R_Device* device, R_ImageHandle image);

R_ImageHandle r_create_image (R_Device* device, const R_CreateImageInfo& info)
{
    ASSERT(device);
    ...
}
```

- Assignment runs are aligned on `=`.
- Designated initializers for every struct literal; locals zeroed with `= {}`.
- Every local is initialized where it is declared, and every struct field has a
  default member initializer, `= {}` unless another value is meant.
- Zero is the default: fields are documented as "0 means 1" rather than given
  explicit defaults.
- Const correct: every local, reference and pointee that is not written is `const`,
  parameters included, west const (`const R_Device* device`). Handle lookups that only
  read use `OXR_GET_CONST_*`. A signature fixed by the OpenXR spec or a callback type
  keeps its exact form with a `NOLINT` naming why.

## File layout

A header: `#pragma once`, includes of lower layers, forward declarations of opaque
structs, `using X_Handle = Handle<X>;`, enums, POD structs, the declaration block.
The real struct definitions live in the `.cpp` (opaque to other layers).

A `.cpp`, in this order, each part under a banner:

```cpp
//
// Types
//

//
// Statics
//

//
// Public API
//
```

Sub-banners inside a part (`// Swapchains`) are encouraged in large files; grep for
the function name finds the one definition.

Include order: own header, project headers of lower layers, blank line, third-party
headers, platform `#if` blocks. Third-party headers never appear in our headers.

## Dispatch

No virtual functions, no inheritance. When the set of cases is roughly known, use a
tagged struct (an enum field plus a `union` or plain fields) and a `switch`, as the
compositor does with `COMP_LayerType`. When behaviour is truly dynamic, use function
pointers (a struct of function pointers for a backend, one pointer for a callback).

## Errors

- Create functions return `nullptr` or `NULL_HANDLE`; the cause is logged with
  `LOG_ERROR` at the failure site.
- Queries return `b8` and fill `out_` parameters.
- `ASSERT(cond)` for preconditions, `ASSERT_MSG(cond, fmt, ...)` for API misuse,
  `FAIL_MSG(fmt, ...)` for unreachable states. All three log, then break into the
  debugger when one is attached. Compiled out when `PUPPETXR_VALIDATION=OFF`.
- OpenXR entry points never assert on app input; they return the `XrResult` the
  spec names. `ASSERT` there is only for our own invariants.
- Cleanup on failure uses `defer` with a `success` flag:

```cpp
b8 success = false;
defer {
    if (!success)
    {
        destroy_thing(thing);
    }
};
```

## Memory

- `os_memory_alloc` (zero-filled), `os_memory_realloc`, `os_memory_release`. No
  `new`/`delete`, no `malloc` directly.
- Top-level objects are one zeroed allocation filled field by field.
- Fixed arrays with `#define` maxima inside structs; `Array<T>` only for sizes we
  must not assume. Both are allocated at create time, never per frame.
- Element types of containers are trivially copyable (they are moved with `memmove`).

## Comments

Linux kernel rules. A comment states what and why, never how. A `NOLINT` always names
the check and the reason (`// NOLINT(misc-const-correctness): the signature of main`). Write one only for:
a non-obvious invariant, a spec requirement (cite the OpenXR section), a workaround,
or an algorithm that is hard to follow from the code. Never restate the code, never
narrate steps, never add a doc header to a function. Full sentences, no trailing
period required.

## Tests

gtest, one `tests/tests_<layer>.cpp` per layer, test names `TEST(Layer, what_it_checks)`.
Tests link the object library and call the public API; OpenXR tests call the `xr*`
functions obtained from `xrGetInstanceProcAddr`, never the loader. Every bug fix adds
the test that would have caught it.
