# OculusXR Unreal plugin: virtual content near black on Windows with r.PostProcessing.PropagateAlpha

Affects: OculusXRHMD (OVRPlugin backend), Windows, an sRGB eye layer, `r.PostProcessing.PropagateAlpha=True`
(set by Meta's Occlusion, Passthrough, MRUK and Shared Anchors samples). Seen with the mobile
renderer (`-FeatureLevelES31`) on D3D11. The same Occlusion sample on Vulkan (`-vulkan -FeatureLevelES31`)
shows correct colours: the `InvAlphaTexture` path is D3D only.

## Symptom

Everything rendered into the eye buffer (meshes, controllers, Oppy) reaches the compositor
nearly black: a grey of 0.353 (sRGB) arrives as 0.102, i.e. it is decoded from sRGB twice.

## Cause

`OculusXRHMD_Layer.cpp`, `FLayer::Initialize_RenderThread` (`#if PLATFORM_WINDOWS`, `bPropagateAlpha`):
`InvAlphaTexture` is created with `TexCreate_ShaderResource | TexCreate_RenderTargetable` and the
eye swapchain's `ColorFormat`, but without the swapchain's `TexCreate_SRGB`
(`ColorTexCreateFlags` has it when `bNeedsSRGBFlag`).

`InvertTextureAlpha_RenderThread` then
1. draws the eye texture into `InvAlphaTexture` (`CopyTexture_RenderThread`, `bInvertAlpha`): the
   source is read through an sRGB SRV (decoded to linear), the destination written through a
   UNORM RTV (stored linear, not re-encoded);
2. copies `InvAlphaTexture` back into the eye texture with `CopyTexture` (raw bytes).

The eye texture now holds linear values in an sRGB texture; the compositor decodes them again.

RenderDoc (D3D11), one pixel of a lit cube:

| step | resource / view | value |
|---|---|---|
| after tonemap | eye texture, `B8G8R8A8_SRGB` RTV | 0.353 |
| invert-alpha draw | reads eye texture through `B8G8R8A8_SRGB` SRV, writes `InvAlphaTexture` through `B8G8R8A8_UNORM` RTV | 0.102 |
| `CopySubresourceRegion` back | eye texture | bytes of 0.102, read as sRGB |

## Fix

Create `InvAlphaTexture` with the swapchain's sRGB flag, e.g.
`InvTextureCreateFlags = TexCreate_ShaderResource | TexCreate_RenderTargetable | (ColorTexCreateFlags & TexCreate_SRGB)`.

## No workaround: PropagateAlpha off inverts the alpha

With `-dpcvars=r.PostProcessing.PropagateAlpha=0` there is no `InvAlphaTexture` and the colours are
right, but the layer is submitted with `ovrpLayerSubmitFlag_InverseAlpha`, which OVRPlugin does not
pass on: the projection layer reaches `xrEndFrame` with `layerFlags` 0x7 (chromatic aberration,
source alpha, unpremultiplied) and no `XR_COMPOSITION_LAYER_INVERTED_ALPHA_BIT_EXT`, and the
instance does not enable `XR_EXT_composition_layer_inverted_alpha` although the runtime offers it.
The mobile renderer's alpha (0 where content is) is then read as opacity: passthrough shows only
where virtual content is, black everywhere else.

---

# OVRPlugin on Vulkan: fragment density map requested without XR_FB_foveation_vulkan, endless retry on failure

Affects: OVRPlugin's OpenXR compositor (`CompositorOpenXR_Vulkan.cpp`), Unreal with `-vulkan`, a
runtime that offers `XR_FB_foveation` but not `XR_FB_foveation_vulkan`.

1. Every eye colour swapchain is created with `XrSwapchainCreateInfoFoveationFB.flags =
   XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB`, although the instance did not enable
   `XR_FB_foveation_vulkan` (the log says "Unavailable OpenXR extension: XR_FB_foveation_vulkan"),
   the only way to receive the map. Trace:
   `xrCreateSwapchain(createInfo={next={XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META, next={XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB, flags=0x2}}, usageFlags=0x39, format=43, 640x672, arraySize=2})`.
   The flag should only be set when `XR_FB_foveation_vulkan` is enabled.
2. When the runtime answers `XR_ERROR_FEATURE_UNSUPPORTED` (allowed by the spec), OVRPlugin goes on
   with a null swapchain (`xrEnumerateSwapchainImages` on `XR_NULL_HANDLE`, "Failed to create
   Oculus layer texture") and then destroys and recreates its XrInstance in a loop: 17860 instances
   in an hour, a 400 MB log, the app never starts. It should fail once with a clear error.