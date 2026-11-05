#include "oxr_internal.h"

#include <stdint.h>

// The OpenXR API trace (config "trace", PUPPETXR_TRACE, docs/openxr.md): xrGetInstanceProcAddr hands
// out wrappers that log each call with its arguments, its result and what it returned. The
// wrappers and the printers they use are generated from the registry by tools/gen_trace.py.

//
// Types
//

#define TRACE_MAX_DEPTH    6
#define TRACE_MAX_ELEMENTS 128
#define TRACE_MAX_STRING   256

namespace
{

struct TraceOut
{
    char  text[16384]; // "trace: " and the call
    usize length;
    b8    truncated;
};

} // namespace

//
// Statics
//

static void trace_text (TraceOut& t, const char* s)
{
    const usize left = sizeof(t.text) - 1 - t.length;
    const usize n    = cstr_length(s);
    if (n > left)
    {
        t.truncated = true;
    }
    const usize copied = n < left ? n : left;
    memory_copy(t.text + t.length, s, copied);
    t.length += copied;
    t.text[t.length] = 0;
}

static void trace_integer (TraceOut& t, i64 value, b8 is_unsigned)
{
    char buffer[32];
    if (is_unsigned)
    {
        cstr_format(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
    }
    else
    {
        cstr_format(buffer, sizeof(buffer), "%lld", (long long)value);
    }
    trace_text(t, buffer);
}

static void trace_float (TraceOut& t, f64 value)
{
    char buffer[48];
    cstr_format(buffer, sizeof(buffer), "%g", value);
    trace_text(t, buffer);
}

// After an array cut at TRACE_MAX_ELEMENTS: how many it left out
static void trace_more (TraceOut& t, u64 count)
{
    if (count > TRACE_MAX_ELEMENTS)
    {
        char buffer[48];
        cstr_format(buffer, sizeof(buffer), ", ... %llu more", (unsigned long long)(count - TRACE_MAX_ELEMENTS));
        trace_text(t, buffer);
    }
}

static void trace_hex (TraceOut& t, u64 value)
{
    char buffer[32];
    cstr_format(buffer, sizeof(buffer), "0x%llx", (unsigned long long)value);
    trace_text(t, buffer);
}

static void trace_handle (TraceOut& t, u64 value)
{
    if (value == 0)
    {
        trace_text(t, "XR_NULL_HANDLE");
        return;
    }
    trace_hex(t, value);
}

// Quoted, at most TRACE_MAX_STRING characters and `limit` bytes, control characters escaped
static void trace_string_n (TraceOut& t, const char* s, usize limit)
{
    if (!s)
    {
        trace_text(t, "NULL");
        return;
    }
    char  buffer[TRACE_MAX_STRING * 4 + 8];
    usize n = 0;
    buffer[n++] = '"';
    usize i = 0;
    for (; i < limit && s[i] && i < TRACE_MAX_STRING; i++)
    {
        const char c = s[i];
        if (c == '"' || c == '\\')
        {
            buffer[n++] = '\\';
            buffer[n++] = c;
        }
        else if ((u8)c < 0x20)
        {
            n += cstr_format(buffer + n, sizeof(buffer) - n, "\\x%02x", (u32)(u8)c);
        }
        else
        {
            buffer[n++] = c;
        }
    }
    if (i == TRACE_MAX_STRING && i < limit && s[i])
    {
        buffer[n++] = '.';
        buffer[n++] = '.';
        buffer[n++] = '.';
    }
    buffer[n++] = '"';
    buffer[n]   = 0;
    trace_text(t, buffer);
}

static void trace_string (TraceOut& t, const char* s)
{
    trace_string_n(t, s, (usize)-1);
}

static void trace_path (TraceOut& t, XrPath path)
{
    char name[XR_MAX_PATH_LENGTH];
    if (path != XR_NULL_PATH && oxr_trace_path_name(path, name, sizeof(name)))
    {
        trace_text(t, name);
        return;
    }
    trace_integer(t, (i64)path, true);
}

// As apps print them: 8-4-4-4-12 hex digits
static void trace_uuid (TraceOut& t, const u8 (&data)[XR_UUID_SIZE])
{
    char buffer[40];
    usize n = 0;
    for (u32 i = 0; i < XR_UUID_SIZE; i++)
    {
        n += cstr_format(buffer + n, sizeof(buffer) - n, (i == 4 || i == 6 || i == 8 || i == 10) ? "-%02x" : "%02x", (u32)data[i]);
    }
    trace_text(t, buffer);
}

static void trace_end (TraceOut& t)
{
    if (t.truncated)
    {
        memory_copy(t.text + t.length - 4, " ...", 4);
    }
    os_log_text(OS_LogLevel::Info, t.text);
}

static void trace_chain (TraceOut& t, const void* next, u32 depth);
static void trace_enum_XrResult (TraceOut& t, i32 value);

// The previews puppetxr declares itself (oxr_internal.h), which the registry lacks
static b8 trace_preview (TraceOut& t, const void* base, u32 depth)
{
    const XrBaseInStructure* header = (const XrBaseInStructure*)base;
    switch ((u32)header->type)
    {
    case OXR_TYPE_DYNAMIC_OBJECT_TRACKED_CLASSES_SET_INFO_META:
    {
        const OXR_DynamicObjectTrackedClassesSetInfoMETA* info = (const OXR_DynamicObjectTrackedClassesSetInfoMETA*)base;
        trace_text(t, "{type=XR_TYPE_DYNAMIC_OBJECT_TRACKED_CLASSES_SET_INFO_META, next=");
        trace_chain(t, info->next, depth);
        trace_text(t, ", classes=[");
        for (u32 i = 0; info->classes && i < info->classCount && i < TRACE_MAX_ELEMENTS; i++)
        {
            trace_text(t, i ? ", " : "");
            trace_text(t, info->classes[i] == OXR_DYNAMIC_OBJECT_CLASS_KEYBOARD_META ? "XR_DYNAMIC_OBJECT_CLASS_KEYBOARD_META" : "?");
        }
        trace_text(t, "]}");
        return true;
    }
    case OXR_TYPE_DYNAMIC_OBJECT_DATA_META:
        trace_text(t, "{type=XR_TYPE_DYNAMIC_OBJECT_DATA_META, classType=");
        trace_integer(t, ((const OXR_DynamicObjectDataMETA*)base)->classType, true);
        trace_text(t, "}");
        return true;
    case OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_TRACKER_CREATE_RESULT_META:
    case OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_SET_TRACKED_CLASSES_RESULT_META:
    {
        const OXR_EventDataDynamicObjectResultMETA* event = (const OXR_EventDataDynamicObjectResultMETA*)base;
        trace_text(t, (u32)header->type == OXR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_TRACKER_CREATE_RESULT_META ? "{type=XR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_TRACKER_CREATE_RESULT_META, handle=" : "{type=XR_TYPE_EVENT_DATA_DYNAMIC_OBJECT_SET_TRACKED_CLASSES_RESULT_META, handle=");
        trace_handle(t, event->handle);
        trace_text(t, ", result=");
        trace_enum_XrResult(t, (i32)event->result);
        trace_text(t, "}");
        return true;
    }
    case OXR_TYPE_SELECT_EVENT_CHANNEL_INFO_EXTX1:
    {
        const OXR_SelectEventChannelInfoEXTX1* info = (const OXR_SelectEventChannelInfoEXTX1*)base;
        trace_text(t, "{type=XR_TYPE_SELECT_EVENT_CHANNEL_INFO_EXTX1, timeout=");
        trace_integer(t, info->timeout, false);
        trace_text(t, ", eventChannels=[");
        for (u32 i = 0; info->eventChannels && i < info->eventChannelCount && i < TRACE_MAX_ELEMENTS; i++)
        {
            trace_text(t, i ? ", " : "");
            trace_handle(t, info->eventChannels[i]);
        }
        trace_text(t, "]}");
        return true;
    }
    case OXR_TYPE_PASSTHROUGH_CAMERA_INTRINSICS_METAX1:
    {
        const OXR_PassthroughCameraIntrinsicsMETAX1* intrinsics = (const OXR_PassthroughCameraIntrinsicsMETAX1*)base;
        trace_text(t, "{type=XR_TYPE_PASSTHROUGH_CAMERA_INTRINSICS_METAX1, focalLength={");
        trace_float(t, (f64)intrinsics->focalLength.x);
        trace_text(t, ", ");
        trace_float(t, (f64)intrinsics->focalLength.y);
        trace_text(t, "}, principalPoint={");
        trace_float(t, (f64)intrinsics->principalPoint.x);
        trace_text(t, ", ");
        trace_float(t, (f64)intrinsics->principalPoint.y);
        trace_text(t, "}, sensorResolution={");
        trace_integer(t, intrinsics->sensorResolution.width, false);
        trace_text(t, ", ");
        trace_integer(t, intrinsics->sensorResolution.height, false);
        trace_text(t, "}}");
        return true;
    }
    case OXR_TYPE_PASSTHROUGH_CAMERA_EXTRINSICS_METAX1:
    {
        const XrPosef& pose = ((const OXR_PassthroughCameraExtrinsicsMETAX1*)base)->pose;
        trace_text(t, "{type=XR_TYPE_PASSTHROUGH_CAMERA_EXTRINSICS_METAX1, pose={orientation={");
        const f32 values[7] = { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w, pose.position.x, pose.position.y, pose.position.z };
        for (u32 i = 0; i < 7; i++)
        {
            trace_text(t, i == 4 ? "}, position={" : i ? ", " : "");
            trace_float(t, (f64)values[i]);
        }
        trace_text(t, "}}}");
        return true;
    }
    case OXR_TYPE_PASSTHROUGH_CAMERA_PROVIDER_CREATE_INFO_METAX1:
    {
        const OXR_PassthroughCameraProviderCreateInfoMETAX1* info = (const OXR_PassthroughCameraProviderCreateInfoMETAX1*)base;
        trace_text(t, "{type=XR_TYPE_PASSTHROUGH_CAMERA_PROVIDER_CREATE_INFO_METAX1, next=");
        trace_chain(t, info->next, depth);
        trace_text(t, ", cameraId=");
        trace_integer(t, (i64)info->cameraId, true);
        trace_text(t, ", resolution={");
        trace_integer(t, info->resolution.width, false);
        trace_text(t, ", ");
        trace_integer(t, info->resolution.height, false);
        trace_text(t, "}}");
        return true;
    }
    case OXR_TYPE_PASSTHROUGH_CAMERA_IMAGE_DATA_METAX1:
    {
        const OXR_PassthroughCameraImageDataMETAX1* data = (const OXR_PassthroughCameraImageDataMETAX1*)base;
        trace_text(t, "{type=XR_TYPE_PASSTHROUGH_CAMERA_IMAGE_DATA_METAX1, dataLength=");
        trace_integer(t, data->dataLength, true);
        trace_text(t, ", data=");
        trace_hex(t, (u64)(uintptr_t)data->data);
        trace_text(t, ", timestamp=");
        trace_integer(t, (i64)data->timestamp, true);
        trace_text(t, "}");
        return true;
    }
    default:
        return false;
    }
}

#include "oxr_trace_gen.h"

// Wrappers of the previews: their real functions, set when first handed out
static PFN_xrVoidFunction trace_real_preview[20] = {};

static void trace_object (TraceOut& t, const void* base)
{
    if (!base)
    {
        trace_text(t, "NULL");
        return;
    }
    trace_typed(t, base, 0);
}

// NOLINTBEGIN(misc-const-correctness): signatures of the extensions, the pointers pass through to the runtime
static XRAPI_ATTR XrResult XRAPI_CALL trace_xrCreateDynamicObjectTrackerMETA (XrSession session, const OXR_DynamicObjectTrackerCreateInfoMETA* info, u64* out_tracker)
{
    TraceOut t = {};
    trace_text(t, "trace: xrCreateDynamicObjectTrackerMETA(session=");
    trace_handle(t, (u64)(uintptr_t)session);
    trace_text(t, ", createInfo=");
    trace_object(t, info);
    const XrResult result = ((XrResult (XRAPI_PTR*)(XrSession, const OXR_DynamicObjectTrackerCreateInfoMETA*, u64*))trace_real_preview[0])(session, info, out_tracker);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    if (XR_SUCCEEDED(result) && out_tracker)
    {
        trace_text(t, " {dynamicObjectTracker=");
        trace_handle(t, *out_tracker);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrDestroyDynamicObjectTrackerMETA (u64 tracker)
{
    TraceOut t = {};
    trace_text(t, "trace: xrDestroyDynamicObjectTrackerMETA(dynamicObjectTracker=");
    trace_handle(t, tracker);
    const XrResult result = ((XrResult (XRAPI_PTR*)(u64))trace_real_preview[1])(tracker);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrSetDynamicObjectTrackedClassesMETA (u64 tracker, const OXR_DynamicObjectTrackedClassesSetInfoMETA* info)
{
    TraceOut t = {};
    trace_text(t, "trace: xrSetDynamicObjectTrackedClassesMETA(dynamicObjectTracker=");
    trace_handle(t, tracker);
    trace_text(t, ", setInfo=");
    trace_object(t, info);
    const XrResult result = ((XrResult (XRAPI_PTR*)(u64, const OXR_DynamicObjectTrackedClassesSetInfoMETA*))trace_real_preview[2])(tracker, info);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrGetSpaceDynamicObjectDataMETA (XrSpace space, OXR_DynamicObjectDataMETA* data)
{
    TraceOut t = {};
    trace_text(t, "trace: xrGetSpaceDynamicObjectDataMETA(space=");
    trace_handle(t, (u64)(uintptr_t)space);
    const XrResult result = ((XrResult (XRAPI_PTR*)(XrSpace, OXR_DynamicObjectDataMETA*))trace_real_preview[3])(space, data);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    if (XR_SUCCEEDED(result))
    {
        trace_text(t, " {data=");
        trace_object(t, data);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrCreateEventChannelEXTX1 (XrInstance instance, const OXR_EventChannelCreateInfoEXTX1* info, u64* out_channel)
{
    TraceOut t = {};
    trace_text(t, "trace: xrCreateEventChannelEXTX1(instance=");
    trace_handle(t, (u64)(uintptr_t)instance);
    trace_text(t, ", createInfo=");
    trace_object(t, info);
    const XrResult result = ((XrResult (XRAPI_PTR*)(XrInstance, const OXR_EventChannelCreateInfoEXTX1*, u64*))trace_real_preview[4])(instance, info, out_channel);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    if (XR_SUCCEEDED(result) && out_channel)
    {
        trace_text(t, " {channel=");
        trace_handle(t, *out_channel);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrDestroyEventChannelEXTX1 (u64 channel)
{
    TraceOut t = {};
    trace_text(t, "trace: xrDestroyEventChannelEXTX1(channel=");
    trace_handle(t, channel);
    const XrResult result = ((XrResult (XRAPI_PTR*)(u64))trace_real_preview[5])(channel);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrPollEventChannelEXTX1 (u64 channel, XrEventDataBuffer* event)
{
    TraceOut t = {};
    trace_text(t, "trace: xrPollEventChannelEXTX1(channel=");
    trace_handle(t, channel);
    const XrResult result = ((XrResult (XRAPI_PTR*)(u64, XrEventDataBuffer*))trace_real_preview[6])(channel, event);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    if (result == XR_SUCCESS)
    {
        trace_text(t, " {eventData=");
        trace_object(t, event);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrSelectEventChannelEXTX1 (XrInstance instance, OXR_SelectEventChannelInfoEXTX1* info, uint32_t* channel_with_event)
{
    TraceOut t = {};
    trace_text(t, "trace: xrSelectEventChannelEXTX1(instance=");
    trace_handle(t, (u64)(uintptr_t)instance);
    trace_text(t, ", info=");
    trace_object(t, info);
    const XrResult result = ((XrResult (XRAPI_PTR*)(XrInstance, OXR_SelectEventChannelInfoEXTX1*, uint32_t*))trace_real_preview[7])(instance, info, channel_with_event);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    if (result == XR_SUCCESS && channel_with_event)
    {
        trace_text(t, " {channelWithEvent=");
        trace_integer(t, *channel_with_event, true);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrSetDefaultEventChannelEXTX1 (XrInstance instance, u64 channel)
{
    TraceOut t = {};
    trace_text(t, "trace: xrSetDefaultEventChannelEXTX1(instance=");
    trace_handle(t, (u64)(uintptr_t)instance);
    trace_text(t, ", channel=");
    trace_handle(t, channel);
    const XrResult result = ((XrResult (XRAPI_PTR*)(XrInstance, u64))trace_real_preview[8])(instance, channel);
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
    trace_end(t);
    return result;
}

// XR_METAX1_passthrough_camera_data: a call on a session and a camera id, or on a provider
static void trace_camera_call (TraceOut& t, const char* name, u64 handle, b8 session, b8 camera, u64 camera_id)
{
    trace_text(t, "trace: ");
    trace_text(t, name);
    trace_text(t, session ? "(session=" : "(provider=");
    trace_handle(t, handle);
    if (camera)
    {
        trace_text(t, ", cameraId=");
        trace_integer(t, (i64)camera_id, true);
    }
}

static void trace_result (TraceOut& t, XrResult result)
{
    trace_text(t, ") -> ");
    trace_enum_XrResult(t, (i32)result);
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrEnumeratePassthroughCameraSourcePropertiesMETAX1 (XrSession session, u32 capacity, u32* count, OXR_PassthroughCameraSourcePropertiesMETAX1* properties)
{
    TraceOut t = {};
    trace_camera_call(t, "xrEnumeratePassthroughCameraSourcePropertiesMETAX1", (u64)(uintptr_t)session, true, false, 0);
    trace_text(t, ", capacity=");
    trace_integer(t, capacity, true);
    const XrResult result = ((PFN_xrEnumeratePassthroughCameraSourcePropertiesMETAX1)trace_real_preview[9])(session, capacity, count, properties);
    trace_result(t, result);
    if (XR_SUCCEEDED(result) && count)
    {
        trace_text(t, " {count=");
        trace_integer(t, *count, true);
        for (u32 i = 0; properties && i < *count && i < capacity && i < TRACE_MAX_ELEMENTS; i++)
        {
            trace_text(t, i ? ", {cameraId=" : ", properties=[{cameraId=");
            trace_integer(t, (i64)properties[i].cameraId, true);
            trace_text(t, ", position=");
            trace_integer(t, properties[i].position, true);
            trace_text(t, i + 1 == *count || i + 1 == capacity ? "}]" : "}");
        }
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrEnumeratePassthroughCameraSupportedResolutionsMETAX1 (XrSession session, u64 camera_id, u32 capacity, u32* count, XrExtent2Di* resolutions)
{
    TraceOut t = {};
    trace_camera_call(t, "xrEnumeratePassthroughCameraSupportedResolutionsMETAX1", (u64)(uintptr_t)session, true, true, camera_id);
    trace_text(t, ", capacity=");
    trace_integer(t, capacity, true);
    const XrResult result = ((PFN_xrEnumeratePassthroughCameraSupportedResolutionsMETAX1)trace_real_preview[10])(session, camera_id, capacity, count, resolutions);
    trace_result(t, result);
    if (XR_SUCCEEDED(result) && count)
    {
        trace_text(t, " {count=");
        trace_integer(t, *count, true);
        for (u32 i = 0; resolutions && i < *count && i < capacity && i < TRACE_MAX_ELEMENTS; i++)
        {
            trace_text(t, i ? ", {" : ", resolutions=[{");
            trace_integer(t, resolutions[i].width, false);
            trace_text(t, ", ");
            trace_integer(t, resolutions[i].height, false);
            trace_text(t, i + 1 == *count || i + 1 == capacity ? "}]" : "}");
        }
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrGetPassthroughCameraIntrinsicsMETAX1 (XrSession session, u64 camera_id, OXR_PassthroughCameraIntrinsicsMETAX1* intrinsics)
{
    TraceOut t = {};
    trace_camera_call(t, "xrGetPassthroughCameraIntrinsicsMETAX1", (u64)(uintptr_t)session, true, true, camera_id);
    const XrResult result = ((PFN_xrGetPassthroughCameraIntrinsicsMETAX1)trace_real_preview[11])(session, camera_id, intrinsics);
    trace_result(t, result);
    if (XR_SUCCEEDED(result))
    {
        trace_text(t, " {intrinsics=");
        trace_object(t, intrinsics);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrGetPassthroughCameraExtrinsicsMETAX1 (XrSession session, u64 camera_id, OXR_PassthroughCameraExtrinsicsMETAX1* extrinsics)
{
    TraceOut t = {};
    trace_camera_call(t, "xrGetPassthroughCameraExtrinsicsMETAX1", (u64)(uintptr_t)session, true, true, camera_id);
    const XrResult result = ((PFN_xrGetPassthroughCameraExtrinsicsMETAX1)trace_real_preview[12])(session, camera_id, extrinsics);
    trace_result(t, result);
    if (XR_SUCCEEDED(result))
    {
        trace_text(t, " {extrinsics=");
        trace_object(t, extrinsics);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrCreatePassthroughCameraProviderMETAX1 (XrSession session, const OXR_PassthroughCameraProviderCreateInfoMETAX1* info, u64* out_provider)
{
    TraceOut t = {};
    trace_camera_call(t, "xrCreatePassthroughCameraProviderMETAX1", (u64)(uintptr_t)session, true, false, 0);
    trace_text(t, ", createInfo=");
    trace_object(t, info);
    const XrResult result = ((PFN_xrCreatePassthroughCameraProviderMETAX1)trace_real_preview[13])(session, info, out_provider);
    trace_result(t, result);
    if (XR_SUCCEEDED(result) && out_provider)
    {
        trace_text(t, " {provider=");
        trace_handle(t, *out_provider);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XrResult trace_provider_call (const char* name, u32 slot, u64 provider)
{
    TraceOut t = {};
    trace_camera_call(t, name, provider, false, false, 0);
    const XrResult result = ((XrResult (XRAPI_PTR*)(u64))trace_real_preview[slot])(provider);
    trace_result(t, result);
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrDestroyPassthroughCameraProviderMETAX1 (u64 provider)
{
    return trace_provider_call("xrDestroyPassthroughCameraProviderMETAX1", 14, provider);
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrStartPassthroughCameraProviderMETAX1 (u64 provider)
{
    return trace_provider_call("xrStartPassthroughCameraProviderMETAX1", 15, provider);
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrStopPassthroughCameraProviderMETAX1 (u64 provider)
{
    return trace_provider_call("xrStopPassthroughCameraProviderMETAX1", 16, provider);
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrAcquirePassthroughCameraImageMETAX1 (u64 provider, u64* out_image)
{
    TraceOut t = {};
    trace_camera_call(t, "xrAcquirePassthroughCameraImageMETAX1", provider, false, false, 0);
    const XrResult result = ((PFN_xrAcquirePassthroughCameraImageMETAX1)trace_real_preview[17])(provider, out_image);
    trace_result(t, result);
    if (XR_SUCCEEDED(result) && out_image)
    {
        trace_text(t, " {image=");
        trace_integer(t, (i64)*out_image, true);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrGetPassthroughCameraImageDataMETAX1 (u64 provider, u64 image, OXR_PassthroughCameraImageDataMETAX1* data)
{
    TraceOut t = {};
    trace_camera_call(t, "xrGetPassthroughCameraImageDataMETAX1", provider, false, false, 0);
    trace_text(t, ", image=");
    trace_integer(t, (i64)image, true);
    const XrResult result = ((PFN_xrGetPassthroughCameraImageDataMETAX1)trace_real_preview[18])(provider, image, data);
    trace_result(t, result);
    if (XR_SUCCEEDED(result))
    {
        trace_text(t, " {data=");
        trace_object(t, data);
        trace_text(t, "}");
    }
    trace_end(t);
    return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL trace_xrReleasePassthroughCameraImageMETAX1 (u64 provider, u64 image)
{
    TraceOut t = {};
    trace_camera_call(t, "xrReleasePassthroughCameraImageMETAX1", provider, false, false, 0);
    trace_text(t, ", image=");
    trace_integer(t, (i64)image, true);
    const XrResult result = ((PFN_xrReleasePassthroughCameraImageMETAX1)trace_real_preview[19])(provider, image);
    trace_result(t, result);
    trace_end(t);
    return result;
}

// NOLINTEND(misc-const-correctness)

static const struct
{
    const char*        name;
    PFN_xrVoidFunction wrapper;
} trace_previews[ARRAY_SIZE(trace_real_preview)] = {
    { "xrCreateDynamicObjectTrackerMETA",     (PFN_xrVoidFunction)trace_xrCreateDynamicObjectTrackerMETA },
    { "xrDestroyDynamicObjectTrackerMETA",    (PFN_xrVoidFunction)trace_xrDestroyDynamicObjectTrackerMETA },
    { "xrSetDynamicObjectTrackedClassesMETA", (PFN_xrVoidFunction)trace_xrSetDynamicObjectTrackedClassesMETA },
    { "xrGetSpaceDynamicObjectDataMETA",      (PFN_xrVoidFunction)trace_xrGetSpaceDynamicObjectDataMETA },
    { "xrCreateEventChannelEXTX1",            (PFN_xrVoidFunction)trace_xrCreateEventChannelEXTX1 },
    { "xrDestroyEventChannelEXTX1",           (PFN_xrVoidFunction)trace_xrDestroyEventChannelEXTX1 },
    { "xrPollEventChannelEXTX1",              (PFN_xrVoidFunction)trace_xrPollEventChannelEXTX1 },
    { "xrSelectEventChannelEXTX1",            (PFN_xrVoidFunction)trace_xrSelectEventChannelEXTX1 },
    { "xrSetDefaultEventChannelEXTX1",        (PFN_xrVoidFunction)trace_xrSetDefaultEventChannelEXTX1 },
    { "xrEnumeratePassthroughCameraSourcePropertiesMETAX1",     (PFN_xrVoidFunction)trace_xrEnumeratePassthroughCameraSourcePropertiesMETAX1 },
    { "xrEnumeratePassthroughCameraSupportedResolutionsMETAX1", (PFN_xrVoidFunction)trace_xrEnumeratePassthroughCameraSupportedResolutionsMETAX1 },
    { "xrGetPassthroughCameraIntrinsicsMETAX1",                 (PFN_xrVoidFunction)trace_xrGetPassthroughCameraIntrinsicsMETAX1 },
    { "xrGetPassthroughCameraExtrinsicsMETAX1",                 (PFN_xrVoidFunction)trace_xrGetPassthroughCameraExtrinsicsMETAX1 },
    { "xrCreatePassthroughCameraProviderMETAX1",                (PFN_xrVoidFunction)trace_xrCreatePassthroughCameraProviderMETAX1 },
    { "xrDestroyPassthroughCameraProviderMETAX1",               (PFN_xrVoidFunction)trace_xrDestroyPassthroughCameraProviderMETAX1 },
    { "xrStartPassthroughCameraProviderMETAX1",                 (PFN_xrVoidFunction)trace_xrStartPassthroughCameraProviderMETAX1 },
    { "xrStopPassthroughCameraProviderMETAX1",                  (PFN_xrVoidFunction)trace_xrStopPassthroughCameraProviderMETAX1 },
    { "xrAcquirePassthroughCameraImageMETAX1",                  (PFN_xrVoidFunction)trace_xrAcquirePassthroughCameraImageMETAX1 },
    { "xrGetPassthroughCameraImageDataMETAX1",                  (PFN_xrVoidFunction)trace_xrGetPassthroughCameraImageDataMETAX1 },
    { "xrReleasePassthroughCameraImageMETAX1",                  (PFN_xrVoidFunction)trace_xrReleasePassthroughCameraImageMETAX1 },
};

//
// Public API
//

// `pattern` is one entry of the filter, `end` where it ends
static b8 glob_match (const char* pattern, const char* end, const char* name)
{
    if (pattern == end)
    {
        return *name == 0;
    }
    if (*pattern == '*')
    {
        for (const char* rest = name;; rest++)
        {
            if (glob_match(pattern + 1, end, rest))
            {
                return true;
            }
            if (*rest == 0)
            {
                return false;
            }
        }
    }
    return *name == *pattern && glob_match(pattern + 1, end, name + 1);
}

b8 oxr_trace_matches (const char* filter, const char* name)
{
    for (const char* entry = filter; entry && *entry;)
    {
        while (*entry == ' ' || *entry == ',')
        {
            entry++;
        }
        const char* end = entry;
        while (*end && *end != ',')
        {
            end++;
        }
        const char* last = end;
        while (last > entry && last[-1] == ' ')
        {
            last--;
        }
        if (last > entry && glob_match(entry, last, name))
        {
            return true;
        }
        entry = end;
    }
    return false;
}

PFN_xrVoidFunction oxr_trace_wrap (const char* name, PFN_xrVoidFunction real)
{
    if (!real)
    {
        return real;
    }
    PFN_xrVoidFunction wrapper = trace_wrap_generated(name, real);
    for (u32 i = 0; !wrapper && i < ARRAY_SIZE(trace_previews); i++)
    {
        if (cstr_equal(name, trace_previews[i].name))
        {
            trace_real_preview[i] = real;
            wrapper               = trace_previews[i].wrapper;
        }
    }
    if (!wrapper)
    {
        // A function the registry lacks (a preview puppetxr declares) or one of a platform header
        LOG_INFO("trace: %s is not traced", name);
        return real;
    }
    return wrapper;
}
