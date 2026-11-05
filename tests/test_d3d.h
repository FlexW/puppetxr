#pragma once

// Plays the role of a D3D11 or D3D12 app for tests: a device on WARP, Windows' software
// rasterizer, which every Windows has, else on the default adapter, and for D3D12 a direct queue.
// d3d11.dll, d3d12.dll and dxgi.dll are loaded at runtime as the app would have them loaded.
// Without D3D in the build, create fails and the tests skip.

#include "render.h"

#if PUPPETXR_HAVE_D3D
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
// windows.h defines them empty, for 16-bit pointers, and rpcndr.h small as char; the tests call
// zm::near and name variables small
#undef near
#undef far
#undef small
#endif

struct TestD3DApp
{
    b8    d3d12  = {};
    void* device = {}; // ID3D11Device* or ID3D12Device*
    void* queue  = {}; // ID3D12CommandQueue*

#if PUPPETXR_HAVE_D3D
    typedef HRESULT (WINAPI* CreateD3D11) (IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels, UINT count, UINT version, ID3D11Device** device, D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context);
    typedef HRESULT (WINAPI* CreateD3D12) (IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** device);
    typedef HRESULT (WINAPI* CreateFactory) (REFIID riid, void** factory);

    b8 create_d3d11 ()
    {
        const CreateD3D11       make   = (CreateD3D11)os_library_symbol(os_library_open("d3d11.dll"), "D3D11CreateDevice");
        const D3D_FEATURE_LEVEL level  = D3D_FEATURE_LEVEL_11_0;
        ID3D11Device*           result = nullptr;
        if (!make)
        {
            return false;
        }
        if (FAILED(make(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &result, nullptr, nullptr)) &&
            FAILED(make(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &result, nullptr, nullptr)))
        {
            return false;
        }
        device = result;
        return true;
    }

    b8 create_d3d12 ()
    {
        const CreateD3D12   make    = (CreateD3D12)os_library_symbol(os_library_open("d3d12.dll"), "D3D12CreateDevice");
        const CreateFactory factory = (CreateFactory)os_library_symbol(os_library_open("dxgi.dll"), "CreateDXGIFactory1");
        if (!make)
        {
            return false;
        }
        IDXGIFactory4* dxgi = nullptr;
        IDXGIAdapter*  warp = nullptr;
        if (factory && SUCCEEDED(factory(__uuidof(IDXGIFactory4), (void**)&dxgi)))
        {
            if (FAILED(dxgi->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void**)&warp)))
            {
                warp = nullptr;
            }
            dxgi->Release();
        }
        ID3D12Device* result = nullptr;
        if (!warp || FAILED(make(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&result)))
        {
            result = nullptr;
            if (FAILED(make(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&result)))
            {
                result = nullptr;
            }
        }
        if (warp)
        {
            warp->Release();
        }
        if (!result)
        {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC desc       = {};
        desc.Type                           = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue*      result_queue = nullptr;
        if (FAILED(result->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), (void**)&result_queue)))
        {
            result->Release();
            return false;
        }
        device = result;
        queue  = result_queue;
        return true;
    }
#endif

    b8 create (b8 use_d3d12)
    {
        d3d12 = use_d3d12;
#if PUPPETXR_HAVE_D3D
        return d3d12 ? create_d3d12() : create_d3d11();
#else
        return false;
#endif
    }

    void destroy ()
    {
#if PUPPETXR_HAVE_D3D
        if (queue)
        {
            ((IUnknown*)queue)->Release();
        }
        if (device)
        {
            ((IUnknown*)device)->Release();
        }
#endif
        device = nullptr;
        queue  = nullptr;
    }

    // The resource a swapchain image of the session is: its size, layers and DXGI format
    b8 describe (u64 texture, u32& out_width, u32& out_height, u32& out_layers, u32& out_format) const
    {
#if PUPPETXR_HAVE_D3D
        if (!texture)
        {
            return false;
        }
        if (d3d12)
        {
            const D3D12_RESOURCE_DESC desc = ((ID3D12Resource*)(uintptr_t)texture)->GetDesc();
            out_width  = (u32)desc.Width;
            out_height = desc.Height;
            out_layers = desc.DepthOrArraySize;
            out_format = (u32)desc.Format;
            return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        ((ID3D11Texture2D*)(uintptr_t)texture)->GetDesc(&desc);
        out_width  = desc.Width;
        out_height = desc.Height;
        out_layers = desc.ArraySize;
        out_format = (u32)desc.Format;
        return true;
#else
        (void)texture;
        out_width = out_height = out_layers = out_format = 0;
        return false;
#endif
    }

    // Whether compute shaders may write the resource: XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT
    b8 allows_unordered_access (u64 texture) const
    {
#if PUPPETXR_HAVE_D3D
        if (!texture)
        {
            return false;
        }
        if (d3d12)
        {
            return (((ID3D12Resource*)(uintptr_t)texture)->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        }
        D3D11_TEXTURE2D_DESC desc = {};
        ((ID3D11Texture2D*)(uintptr_t)texture)->GetDesc(&desc);
        return (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
#else
        (void)texture;
        return false;
#endif
    }

    // Renders as an app does into an acquired swapchain image: `top` over the upper half of every
    // layer, `bottom` over the lower half, through views of `view_format`. D3D's image origin is
    // the top-left corner, as Vulkan's.
    b8 fill_halves (u64 texture, u32 width, u32 height, u32 layers, u32 view_format, const f32 top[4], const f32 bottom[4]) const
    {
#if PUPPETXR_HAVE_D3D
        if (!d3d12)
        {
            // Each half is a cleared texture copied in place: D3D11 clears whole views only
            ID3D11Device*        d3d11   = (ID3D11Device*)device;
            ID3D11DeviceContext* context = nullptr;
            d3d11->GetImmediateContext(&context);
            D3D11_TEXTURE2D_DESC desc = {};
            ((ID3D11Texture2D*)(uintptr_t)texture)->GetDesc(&desc);
            desc.Height    = height / 2;
            desc.ArraySize = 1;
            desc.MipLevels = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            desc.MiscFlags = 0;
            b8 done = true;
            for (u32 half = 0; half < 2 && done; half++)
            {
                ID3D11Texture2D*        part = nullptr;
                ID3D11RenderTargetView* view = nullptr;
                D3D11_RENDER_TARGET_VIEW_DESC view_desc = {};
                view_desc.Format        = (DXGI_FORMAT)view_format;
                view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                done = SUCCEEDED(d3d11->CreateTexture2D(&desc, nullptr, &part)) && SUCCEEDED(d3d11->CreateRenderTargetView(part, &view_desc, &view));
                if (done)
                {
                    context->ClearRenderTargetView(view, half == 0 ? top : bottom);
                    for (u32 layer = 0; layer < layers; layer++)
                    {
                        context->CopySubresourceRegion((ID3D11Resource*)(uintptr_t)texture, layer, 0, half * (height / 2), 0, part, 0, nullptr);
                    }
                }
                if (view)
                {
                    view->Release();
                }
                if (part)
                {
                    part->Release();
                }
            }
            (void)width;
            context->Flush();
            context->Release();
            return done;
        }
        const D3D12_RECT           halves[2]    = { { 0, 0, (LONG)width, (LONG)height / 2 }, { 0, (LONG)height / 2, (LONG)width, (LONG)height } };
        ID3D12Device*              d3d12_device = (ID3D12Device*)device;
        ID3D12CommandAllocator*    allocator    = nullptr;
        ID3D12GraphicsCommandList* list         = nullptr;
        ID3D12DescriptorHeap*      heap         = nullptr;
        ID3D12Fence*               fence        = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc    = {};
        heap_desc.Type                          = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors                = 1;
        b8 done = SUCCEEDED(d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&allocator)) &&
                  SUCCEEDED(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&list)) &&
                  SUCCEEDED(d3d12_device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), (void**)&heap)) &&
                  SUCCEEDED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
        for (u32 layer = 0; layer < layers && done; layer++)
        {
            // One descriptor: the clears read it when they are recorded
            D3D12_RENDER_TARGET_VIEW_DESC desc  = {};
            desc.Format                         = (DXGI_FORMAT)view_format;
            desc.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.FirstArraySlice = layer;
            desc.Texture2DArray.ArraySize       = 1;
            const D3D12_CPU_DESCRIPTOR_HANDLE view = heap->GetCPUDescriptorHandleForHeapStart();
            d3d12_device->CreateRenderTargetView((ID3D12Resource*)(uintptr_t)texture, &desc, view);
            list->ClearRenderTargetView(view, top, 1, &halves[0]);
            list->ClearRenderTargetView(view, bottom, 1, &halves[1]);
        }
        if (done && SUCCEEDED(list->Close()))
        {
            ID3D12CommandList* lists[] = { list };
            ((ID3D12CommandQueue*)queue)->ExecuteCommandLists(1, lists);
            ((ID3D12CommandQueue*)queue)->Signal(fence, 1);
            fence->SetEventOnCompletion(1, nullptr);
        }
        if (fence)
        {
            fence->Release();
        }
        if (heap)
        {
            heap->Release();
        }
        if (list)
        {
            list->Release();
        }
        if (allocator)
        {
            allocator->Release();
        }
        return done;
#else
        (void)texture;
        (void)width;
        (void)height;
        (void)layers;
        (void)view_format;
        (void)top;
        (void)bottom;
        return false;
#endif
    }

    // Clears array layer i of an acquired depth swapchain image to depths[i], as an app's depth
    // pass would leave it; D32_FLOAT views of the typeless texture
    b8 fill_depth (u64 texture, u32 layers, const f32* depths) const
    {
#if PUPPETXR_HAVE_D3D
        if (!d3d12)
        {
            ID3D11Device*        d3d11   = (ID3D11Device*)device;
            ID3D11DeviceContext* context = nullptr;
            d3d11->GetImmediateContext(&context);
            b8 done = true;
            for (u32 layer = 0; layer < layers && done; layer++)
            {
                D3D11_DEPTH_STENCIL_VIEW_DESC desc  = {};
                desc.Format                         = DXGI_FORMAT_D32_FLOAT;
                desc.ViewDimension                  = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                desc.Texture2DArray.FirstArraySlice = layer;
                desc.Texture2DArray.ArraySize       = 1;
                ID3D11DepthStencilView* view        = nullptr;
                done = SUCCEEDED(d3d11->CreateDepthStencilView((ID3D11Resource*)(uintptr_t)texture, &desc, &view));
                if (done)
                {
                    context->ClearDepthStencilView(view, D3D11_CLEAR_DEPTH, depths[layer], 0);
                    view->Release();
                }
            }
            context->Flush();
            context->Release();
            return done;
        }
        ID3D12Device*              d3d12_device = (ID3D12Device*)device;
        ID3D12CommandAllocator*    allocator    = nullptr;
        ID3D12GraphicsCommandList* list         = nullptr;
        ID3D12DescriptorHeap*      heap         = nullptr;
        ID3D12Fence*               fence        = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc    = {};
        heap_desc.Type                          = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heap_desc.NumDescriptors                = 1;
        b8 done = SUCCEEDED(d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&allocator)) &&
                  SUCCEEDED(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&list)) &&
                  SUCCEEDED(d3d12_device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), (void**)&heap)) &&
                  SUCCEEDED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
        for (u32 layer = 0; layer < layers && done; layer++)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC desc  = {};
            desc.Format                         = DXGI_FORMAT_D32_FLOAT;
            desc.ViewDimension                  = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.FirstArraySlice = layer;
            desc.Texture2DArray.ArraySize       = 1;
            const D3D12_CPU_DESCRIPTOR_HANDLE view = heap->GetCPUDescriptorHandleForHeapStart();
            d3d12_device->CreateDepthStencilView((ID3D12Resource*)(uintptr_t)texture, &desc, view);
            list->ClearDepthStencilView(view, D3D12_CLEAR_FLAG_DEPTH, depths[layer], 0, 0, nullptr);
        }
        if (done && SUCCEEDED(list->Close()))
        {
            ID3D12CommandList* lists[] = { list };
            ((ID3D12CommandQueue*)queue)->ExecuteCommandLists(1, lists);
            ((ID3D12CommandQueue*)queue)->Signal(fence, 1);
            fence->SetEventOnCompletion(1, nullptr);
        }
        if (fence)
        {
            fence->Release();
        }
        if (heap)
        {
            heap->Release();
        }
        if (list)
        {
            list->Release();
        }
        if (allocator)
        {
            allocator->Release();
        }
        return done;
#else
        (void)texture;
        (void)layers;
        (void)depths;
        return false;
#endif
    }

    // Reads array layer `layer` of a 16-bit texture (an environment depth image) as an app sampling
    // it would see it, rows from the top, into out_texels of width * height values
    b8 read_r16 (u64 texture, u32 layer, u32 width, u32 height, u16* out_texels) const
    {
#if PUPPETXR_HAVE_D3D
        if (!d3d12)
        {
            ID3D11Device*        d3d11   = (ID3D11Device*)device;
            ID3D11DeviceContext* context = nullptr;
            d3d11->GetImmediateContext(&context);
            D3D11_TEXTURE2D_DESC desc = {};
            ((ID3D11Texture2D*)(uintptr_t)texture)->GetDesc(&desc);
            desc.ArraySize      = 1;
            desc.MipLevels      = 1;
            desc.Usage          = D3D11_USAGE_STAGING;
            desc.BindFlags      = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags      = 0;
            ID3D11Texture2D* staging = nullptr;
            b8 done = SUCCEEDED(d3d11->CreateTexture2D(&desc, nullptr, &staging));
            if (done)
            {
                context->CopySubresourceRegion(staging, 0, 0, 0, 0, (ID3D11Resource*)(uintptr_t)texture, layer, nullptr);
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                done = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
                for (u32 y = 0; done && y < height; y++)
                {
                    memory_copy(out_texels + (usize)y * width, (const u8*)mapped.pData + (usize)y * mapped.RowPitch, (usize)width * 2);
                }
                if (done)
                {
                    context->Unmap(staging, 0);
                }
                staging->Release();
            }
            context->Release();
            return done;
        }
        ID3D12Device*                      d3d12_device = (ID3D12Device*)device;
        ID3D12Resource*                    source       = (ID3D12Resource*)(uintptr_t)texture;
        const D3D12_RESOURCE_DESC          source_desc  = source->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint    = {};
        UINT64                             total        = 0;
        d3d12_device->GetCopyableFootprints(&source_desc, layer, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES heap   = {};
        heap.Type                    = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC   buffer = {};
        buffer.Dimension             = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width                 = total;
        buffer.Height                = 1;
        buffer.DepthOrArraySize      = 1;
        buffer.MipLevels             = 1;
        buffer.SampleDesc.Count      = 1;
        buffer.Layout                = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        footprint.Offset             = 0;
        ID3D12Resource*            readback  = nullptr;
        ID3D12CommandAllocator*    allocator = nullptr;
        ID3D12GraphicsCommandList* list      = nullptr;
        ID3D12Fence*               fence     = nullptr;
        b8 done = SUCCEEDED(d3d12_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&readback)) &&
                  SUCCEEDED(d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&allocator)) &&
                  SUCCEEDED(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&list)) &&
                  SUCCEEDED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&fence));
        if (done)
        {
            // Environment depth images are left readable by shaders
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = source;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION to   = {};
            to.pResource                     = readback;
            to.Type                          = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint               = footprint;
            D3D12_TEXTURE_COPY_LOCATION from = {};
            from.pResource                   = source;
            from.Type                        = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex            = layer;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            list->ResourceBarrier(1, &barrier);
            done = SUCCEEDED(list->Close());
        }
        if (done)
        {
            ID3D12CommandList* lists[] = { list };
            ((ID3D12CommandQueue*)queue)->ExecuteCommandLists(1, lists);
            ((ID3D12CommandQueue*)queue)->Signal(fence, 1);
            fence->SetEventOnCompletion(1, nullptr);
            void* mapped = nullptr;
            done = SUCCEEDED(readback->Map(0, nullptr, &mapped));
            for (u32 y = 0; done && y < height; y++)
            {
                memory_copy(out_texels + (usize)y * width, (const u8*)mapped + (usize)y * footprint.Footprint.RowPitch, (usize)width * 2);
            }
            if (done)
            {
                readback->Unmap(0, nullptr);
            }
        }
        for (IUnknown* object : { (IUnknown*)fence, (IUnknown*)list, (IUnknown*)allocator, (IUnknown*)readback })
        {
            if (object)
            {
                object->Release();
            }
        }
        return done;
#else
        (void)texture;
        (void)layer;
        (void)width;
        (void)height;
        (void)out_texels;
        return false;
#endif
    }

    R_DeviceCreateInfo info () const
    {
        if (d3d12)
        {
            return { .backend = R_Backend::D3D12, .d3d12 = { .device = device, .queue = queue } };
        }
        return { .backend = R_Backend::D3D11, .d3d11 = { .device = device } };
    }
};
