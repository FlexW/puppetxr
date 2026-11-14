#include "platform_internal.h"

#if !PUPPETXR_HEADLESS_ONLY

// The window system's libraries are loaded at runtime, so libpuppetxr needs none of them to load
// (cmake/check_glibc.cmake); only their headers are needed to build
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xcursor/Xcursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

// X11 names its null resource None, which is a key and an event type here
static constexpr XID X_NONE = None;
#undef None

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

//
// Types
//

#define LX_MAX_TEXT 65536 // clipboard text kept for other clients

// The two Vulkan surface infos and the calls the drivers make, declared here instead of including
// vulkan.h (a third-party header goes into at most two files)
namespace
{

typedef void* (*VkGetInstanceProcAddrFn)(void* instance, const char* name);

struct VkXlibSurfaceCreateInfo
{
    i32         sType; // VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR
    const void* pNext;
    u32         flags;
    Display*    dpy;
    Window      window;
};

struct VkWaylandSurfaceCreateInfo
{
    i32         sType; // VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR
    const void* pNext;
    u32         flags;
    void*       display;
    void*       surface;
};

typedef i32 (*VkCreateSurfaceFn)(void* instance, const void* info, const void* allocator, u64* surface);

#define VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR    1000004000
#define VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR 1000006000

} // namespace

//
// Statics
//

// Linux input key codes (linux/input-event-codes.h) to keys; X11 key codes are these plus 8
static const OS_Key lx_evdev_keys[] = {
    OS_Key::None, OS_Key::Escape,
    OS_Key::Digit1, OS_Key::Digit2, OS_Key::Digit3, OS_Key::Digit4, OS_Key::Digit5, OS_Key::Digit6, OS_Key::Digit7, OS_Key::Digit8, OS_Key::Digit9, OS_Key::Digit0,
    OS_Key::Minus, OS_Key::Equals, OS_Key::Backspace, OS_Key::Tab,
    OS_Key::Q, OS_Key::W, OS_Key::E, OS_Key::R, OS_Key::T, OS_Key::Y, OS_Key::U, OS_Key::I, OS_Key::O, OS_Key::P,
    OS_Key::LeftBracket, OS_Key::RightBracket, OS_Key::Enter, OS_Key::LeftCtrl,
    OS_Key::A, OS_Key::S, OS_Key::D, OS_Key::F, OS_Key::G, OS_Key::H, OS_Key::J, OS_Key::K, OS_Key::L,
    OS_Key::Semicolon, OS_Key::Apostrophe, OS_Key::Grave, OS_Key::LeftShift, OS_Key::Backslash,
    OS_Key::Z, OS_Key::X, OS_Key::C, OS_Key::V, OS_Key::B, OS_Key::N, OS_Key::M,
    OS_Key::Comma, OS_Key::Period, OS_Key::Slash, OS_Key::RightShift, OS_Key::KeypadMultiply, OS_Key::LeftAlt, OS_Key::Space, OS_Key::CapsLock,
    OS_Key::F1, OS_Key::F2, OS_Key::F3, OS_Key::F4, OS_Key::F5, OS_Key::F6, OS_Key::F7, OS_Key::F8, OS_Key::F9, OS_Key::F10,
    OS_Key::NumLock, OS_Key::ScrollLock,
    OS_Key::Keypad7, OS_Key::Keypad8, OS_Key::Keypad9, OS_Key::KeypadMinus,
    OS_Key::Keypad4, OS_Key::Keypad5, OS_Key::Keypad6, OS_Key::KeypadPlus,
    OS_Key::Keypad1, OS_Key::Keypad2, OS_Key::Keypad3, OS_Key::Keypad0, OS_Key::KeypadDecimal,
    OS_Key::None, OS_Key::None, OS_Key::NonUsBackslash, OS_Key::F11, OS_Key::F12,                                     // 84-88
    OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None,                 // 89-95
    OS_Key::KeypadEnter, OS_Key::RightCtrl, OS_Key::KeypadDivide, OS_Key::PrintScreen, OS_Key::RightAlt, OS_Key::None, // 96-101
    OS_Key::Home, OS_Key::ArrowUp, OS_Key::PageUp, OS_Key::ArrowLeft, OS_Key::ArrowRight, OS_Key::End, OS_Key::ArrowDown, OS_Key::PageDown, OS_Key::Insert, OS_Key::Delete, // 102-111
    OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::KeypadEquals, OS_Key::None, OS_Key::Pause, // 112-119
    OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::LeftSuper, OS_Key::RightSuper, OS_Key::Menu, // 120-127
};

static OS_Key key_from_evdev (u32 code)
{
    if (code < ARRAY_SIZE(lx_evdev_keys))
    {
        return lx_evdev_keys[code];
    }
    static const OS_Key high_function_keys[] = { OS_Key::F13, OS_Key::F14, OS_Key::F15, OS_Key::F16, OS_Key::F17, OS_Key::F18, OS_Key::F19, OS_Key::F20, OS_Key::F21, OS_Key::F22, OS_Key::F23, OS_Key::F24 };
    if (code >= 183 && code < 183 + ARRAY_SIZE(high_function_keys))
    {
        return high_function_keys[code - 183];
    }
    return OS_Key::None;
}

static u32 evdev_from_key (OS_Key key)
{
    for (u32 code = 1; code < ARRAY_SIZE(lx_evdev_keys); code++)
    {
        if (lx_evdev_keys[code] == key)
        {
            return code;
        }
    }
    if (key >= OS_Key::F13 && key <= OS_Key::F24)
    {
        return 183 + ((u32)key - (u32)OS_Key::F13);
    }
    return 0;
}

static b8 load_symbols (const char* library_name, const char* const* names, void** out, u32 count, b8 log)
{
    OS_Library* library = os_library_open(library_name);
    if (!library)
    {
        if (log)
        {
            LOG_INFO("No %s", library_name);
        }
        return false;
    }
    for (u32 i = 0; i < count; i++)
    {
        out[i] = os_library_symbol(library, names[i]);
        if (!out[i])
        {
            LOG_WARNING("%s has no %s", library_name, names[i]);
            return false;
        }
    }
    // Kept loaded for the process: unloading a window system library that still has state (libX11's
    // per display data, libwayland's thread-local queues) breaks those who loaded it too
    return true;
}

static VkGetInstanceProcAddrFn vulkan_get_proc (const void* vk_get_instance_proc_addr)
{
    if (vk_get_instance_proc_addr)
    {
        return (VkGetInstanceProcAddrFn)(uintptr_t)vk_get_instance_proc_addr;
    }
    OS_Library* loader = os_vulkan_loader();
    return loader ? (VkGetInstanceProcAddrFn)os_library_symbol(loader, "vkGetInstanceProcAddr") : nullptr;
}

// UTF-8 of one code point, 0 bytes for none
static u32 utf8_encode (u32 code, char* out)
{
    if (code < 0x80)
    {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800)
    {
        out[0] = (char)(0xC0 | (code >> 6));
        out[1] = (char)(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000)
    {
        out[0] = (char)(0xE0 | (code >> 12));
        out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F));
        return 3;
    }
    if (code < 0x110000)
    {
        out[0] = (char)(0xF0 | (code >> 18));
        out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[3] = (char)(0x80 | (code & 0x3F));
        return 4;
    }
    return 0;
}

// What a key prints on the layout, from the keysym of its first level: letters upper case, dead
// keys as the accent they put on the next key
static b8 label_from_keysym (u32 keysym, char* out, usize size)
{
    static const struct
    {
        u32 keysym;
        u32 code;
    } dead_keys[] = {
        { 0xfe50, '`' }, { 0xfe51, 0xB4 }, { 0xfe52, '^' }, { 0xfe53, '~' }, { 0xfe54, 0xAF }, { 0xfe55, 0x2D8 },
        { 0xfe56, 0x2D9 }, { 0xfe57, 0xA8 }, { 0xfe58, 0xB0 }, { 0xfe59, 0x2DD }, { 0xfe5a, 0x2C7 }, { 0xfe5b, 0xB8 },
    };
    u32 code = 0;
    if ((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF))
    {
        code = keysym;
    }
    else if ((keysym & 0xFF000000u) == 0x01000000u)
    {
        code = keysym & 0x00FFFFFFu;
    }
    for (const auto& dead : dead_keys)
    {
        code = keysym == dead.keysym ? dead.code : code;
    }
    if (code == 0 || code == ' ')
    {
        return false;
    }
    if ((code >= 'a' && code <= 'z') || (code >= 0xE0 && code <= 0xFE && code != 0xF7))
    {
        code -= 0x20;
    }
    char      text[8] = {};
    const u32 length  = utf8_encode(code, text);
    return length > 0 && cstr_copy(out, size, text);
}

// Keys whose label is the keysym's character; the others keep the US names (Space, Return, ...)
static b8 printable_key (OS_Key key)
{
    return (key >= OS_Key::A && key <= OS_Key::Digit9) || key == OS_Key::Grave || key == OS_Key::LeftBracket || key == OS_Key::RightBracket ||
           (key >= OS_Key::Minus && key <= OS_Key::NonUsBackslash);
}

//
// X11
//

#define X11_FUNCTIONS(F) \
    F(XInitThreads) F(XOpenDisplay) F(XCloseDisplay) F(XConnectionNumber) F(XDefaultScreen) F(XRootWindow) \
    F(XDefaultVisual) F(XDefaultDepth) F(XGetVisualInfo) F(XCreateColormap) F(XFreeColormap) F(XCreateWindow) \
    F(XDestroyWindow) F(XMapRaised) F(XUnmapWindow) F(XStoreName) F(XChangeProperty) F(XInternAtom) \
    F(XSetWMProtocols) F(XSelectInput) F(XPending) F(XNextEvent) F(XFlush) F(XSync) F(XFree) F(XSendEvent) \
    F(XGetWindowProperty) F(XQueryPointer) F(XWarpPointer) F(XGrabPointer) F(XUngrabPointer) F(XDefineCursor) \
    F(XCreateFontCursor) F(XFreeCursor) F(XCreatePixmapCursor) F(XCreateBitmapFromData) F(XFreePixmap) \
    F(XSetSelectionOwner) F(XGetSelectionOwner) F(XConvertSelection) F(XCheckTypedWindowEvent) F(XOpenIM) \
    F(XCloseIM) F(XCreateIC) F(XDestroyIC) F(XSetICFocus) F(XUnsetICFocus) F(Xutf8LookupString) F(XFilterEvent) \
    F(XkbSetDetectableAutoRepeat) F(XkbKeycodeToKeysym) F(XSetLocaleModifiers) F(XAllocSizeHints) \
    F(XSetWMNormalHints) F(XGetEventData) F(XFreeEventData) F(XTranslateCoordinates) F(XSetInputFocus) \
    F(XGetGeometry) F(XSetICValues) F(XVaCreateNestedList) F(XQueryExtension)

#define XI_FUNCTIONS(F) F(XISelectEvents) F(XIQueryVersion)

#define XRANDR_FUNCTIONS(F) \
    F(XRRGetScreenResourcesCurrent) F(XRRFreeScreenResources) F(XRRGetCrtcInfo) F(XRRFreeCrtcInfo)

#define XCURSOR_FUNCTIONS(F) F(XcursorLibraryLoadCursor)

#define LX_DECLARE(name)  decltype(&::name) name; // NOLINT(bugprone-macro-parentheses)
#define LX_NAME(name)     #name,

namespace
{

struct X11Functions
{
    X11_FUNCTIONS(LX_DECLARE)
};

struct XIFunctions
{
    XI_FUNCTIONS(LX_DECLARE)
};

struct XRandRFunctions
{
    XRANDR_FUNCTIONS(LX_DECLARE)
};

struct XcursorFunctions
{
    XCURSOR_FUNCTIONS(LX_DECLARE)
};

struct X11Window
{
    Window   window;
    Colormap colormap;
    XIC      ic;
    b8       mapped;
    b8       maximize_when_shown;
    b8       focused;
    f32      last_x;      // the cursor's last position, for relative motion outside the relative mode
    f32      last_y;
    b8       last_valid;
    b8       grabbed;
    i32      saved_x;     // where the cursor was when the relative mode began, put back after it
    i32      saved_y;
};

struct X11State
{
    b8               loaded;
    b8               xi_loaded;
    b8               xrandr_loaded;
    b8               xcursor_loaded;
    X11Functions     x;
    XIFunctions      xi;
    XRandRFunctions  xrr;
    XcursorFunctions xcursor;
    Display*         display;
    i32              screen;
    Window           root;
    XIM              im;
    i32              xi_opcode;
    b8               xi_raw;        // raw motion is selected on the root window
    u32              relative_count;
    b8               keys_down[256];
    Cursor           cursors[(u32)OS_Cursor::Count];
    Atom             wm_protocols;
    Atom             wm_delete_window;
    Atom             net_wm_ping;
    Atom             net_wm_name;
    Atom             net_wm_state;
    Atom             net_wm_state_max_vert;
    Atom             net_wm_state_max_horz;
    Atom             net_active_window;
    Atom             utf8_string;
    Atom             clipboard;
    Atom             targets;
    Atom             selection_property;
    char*            clipboard_text; // what we offer while we own the clipboard
};

} // namespace

static X11State lx_x11 = {};

static const char* const lx_x11_names[]     = { X11_FUNCTIONS(LX_NAME) };
static const char* const lx_xi_names[]      = { XI_FUNCTIONS(LX_NAME) };
static const char* const lx_xrandr_names[]  = { XRANDR_FUNCTIONS(LX_NAME) };
static const char* const lx_xcursor_names[] = { XCURSOR_FUNCTIONS(LX_NAME) };

static const char* const lx_x11_vulkan_extensions[] = { "VK_KHR_surface", "VK_KHR_xlib_surface" };

static X11Window* x11_data (OS_Window* window) // NOLINT(misc-const-correctness): callers pass the windows they change
{
    return (X11Window*)window->data;
}

static OS_Window* x11_find (Window xwindow)
{
    for (u32 i = 0; i < OS_MAX_WINDOWS; i++)
    {
        OS_Window* window = os_window_at(i);
        if (window && window->driver == &os_x11_driver && window->data && x11_data(window)->window == xwindow)
        {
            return window;
        }
    }
    return nullptr;
}

static u8 x11_modifiers (u32 state)
{
    return (u8)(((state & ShiftMask) ? OS_MODIFIER_SHIFT : 0) | ((state & ControlMask) ? OS_MODIFIER_CTRL : 0) | ((state & Mod1Mask) ? OS_MODIFIER_ALT : 0) | ((state & Mod4Mask) ? OS_MODIFIER_SUPER : 0));
}

static Atom x11_atom (const char* name)
{
    return lx_x11.x.XInternAtom(lx_x11.display, name, False);
}

static b8 x11_load ()
{
    if (!lx_x11.loaded)
    {
        lx_x11.loaded = load_symbols("libX11.so.6", lx_x11_names, (void**)&lx_x11.x, ARRAY_SIZE(lx_x11_names), true);
        if (!lx_x11.loaded)
        {
            return false;
        }
        lx_x11.xi_loaded      = load_symbols("libXi.so.6", lx_xi_names, (void**)&lx_x11.xi, ARRAY_SIZE(lx_xi_names), true);
        lx_x11.xrandr_loaded  = load_symbols("libXrandr.so.2", lx_xrandr_names, (void**)&lx_x11.xrr, ARRAY_SIZE(lx_xrandr_names), false);
        lx_x11.xcursor_loaded = load_symbols("libXcursor.so.1", lx_xcursor_names, (void**)&lx_x11.xcursor, ARRAY_SIZE(lx_xcursor_names), false);
    }
    return true;
}

// Themed cursors where libXcursor finds the theme, the core font's otherwise
static Cursor x11_load_cursor (OS_Cursor shape)
{
    static const char* const theme_names[] = { "default", "text", "move", "ns-resize", "ew-resize", "nesw-resize", "nwse-resize", "pointer", "wait", "progress", "not-allowed" };
    static const u32         font_shapes[] = { XC_left_ptr, XC_xterm, XC_fleur, XC_sb_v_double_arrow, XC_sb_h_double_arrow, XC_bottom_left_corner, XC_bottom_right_corner, XC_hand2, XC_watch, XC_watch, XC_pirate };
    static_assert(ARRAY_SIZE(theme_names) == (u32)OS_Cursor::Hidden && ARRAY_SIZE(font_shapes) == (u32)OS_Cursor::Hidden);
    if (shape == OS_Cursor::Hidden)
    {
        static const char empty[8] = {};
        const Pixmap      pixmap   = lx_x11.x.XCreateBitmapFromData(lx_x11.display, lx_x11.root, empty, 8, 8);
        XColor            black    = {};
        const Cursor      cursor   = lx_x11.x.XCreatePixmapCursor(lx_x11.display, pixmap, pixmap, &black, &black, 0, 0);
        lx_x11.x.XFreePixmap(lx_x11.display, pixmap);
        return cursor;
    }
    const Cursor cursor = lx_x11.xcursor_loaded ? lx_x11.xcursor.XcursorLibraryLoadCursor(lx_x11.display, theme_names[(u32)shape]) : 0;
    return cursor ? cursor : lx_x11.x.XCreateFontCursor(lx_x11.display, font_shapes[(u32)shape]);
}

static Cursor x11_cursor (OS_Cursor shape)
{
    Cursor& cursor = lx_x11.cursors[(u32)shape];
    if (!cursor)
    {
        cursor = x11_load_cursor(shape);
    }
    return cursor;
}

static b8 x11_init ()
{
    if (!x11_load())
    {
        return false;
    }
    // The display is used by GLX and Vulkan on the app's threads while the window thread reads it
    lx_x11.x.XInitThreads();
    lx_x11.display = lx_x11.x.XOpenDisplay(nullptr);
    if (!lx_x11.display)
    {
        return false;
    }
    lx_x11.screen                = lx_x11.x.XDefaultScreen(lx_x11.display);
    lx_x11.root                  = lx_x11.x.XRootWindow(lx_x11.display, lx_x11.screen);
    lx_x11.wm_protocols          = x11_atom("WM_PROTOCOLS");
    lx_x11.wm_delete_window      = x11_atom("WM_DELETE_WINDOW");
    lx_x11.net_wm_ping           = x11_atom("_NET_WM_PING");
    lx_x11.net_wm_name           = x11_atom("_NET_WM_NAME");
    lx_x11.net_wm_state          = x11_atom("_NET_WM_STATE");
    lx_x11.net_wm_state_max_vert = x11_atom("_NET_WM_STATE_MAXIMIZED_VERT");
    lx_x11.net_wm_state_max_horz = x11_atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    lx_x11.net_active_window     = x11_atom("_NET_ACTIVE_WINDOW");
    lx_x11.utf8_string           = x11_atom("UTF8_STRING");
    lx_x11.clipboard             = x11_atom("CLIPBOARD");
    lx_x11.targets               = x11_atom("TARGETS");
    lx_x11.selection_property    = x11_atom("PUPPETXR_SELECTION");
    // A held key repeats as presses alone, which then carry the repeat flag
    Bool detectable = False;
    lx_x11.x.XkbSetDetectableAutoRepeat(lx_x11.display, True, &detectable);
    lx_x11.x.XSetLocaleModifiers("");
    lx_x11.im = lx_x11.x.XOpenIM(lx_x11.display, nullptr, nullptr, nullptr);
    i32 event = 0;
    i32 error = 0;
    lx_x11.xi_opcode = 0;
    if (lx_x11.xi_loaded)
    {
        i32 major = 2;
        i32 minor = 0;
        if (!lx_x11.x.XQueryExtension(lx_x11.display, "XInputExtension", &lx_x11.xi_opcode, &event, &error) || lx_x11.xi.XIQueryVersion(lx_x11.display, &major, &minor) != Success)
        {
            lx_x11.xi_opcode = 0;
        }
    }
    memory_zero(lx_x11.keys_down, sizeof(lx_x11.keys_down));
    return true;
}

static void x11_quit ()
{
    for (Cursor& cursor : lx_x11.cursors)
    {
        if (cursor)
        {
            lx_x11.x.XFreeCursor(lx_x11.display, cursor);
            cursor = 0;
        }
    }
    if (lx_x11.im)
    {
        lx_x11.x.XCloseIM(lx_x11.im);
        lx_x11.im = nullptr;
    }
    os_memory_release(lx_x11.clipboard_text);
    lx_x11.clipboard_text = nullptr;
    lx_x11.x.XCloseDisplay(lx_x11.display);
    lx_x11.display = nullptr;
    lx_x11.relative_count = 0;
    lx_x11.xi_raw = false;
}

static b8 x11_create_window (OS_Window* window, const OS_WindowDesc& desc)
{
    Visual* visual = lx_x11.x.XDefaultVisual(lx_x11.display, lx_x11.screen);
    i32     depth  = lx_x11.x.XDefaultDepth(lx_x11.display, lx_x11.screen);
    if (desc.x11_visual)
    {
        XVisualInfo wanted = {};
        wanted.visualid    = desc.x11_visual;
        i32          count = 0;
        XVisualInfo* found = lx_x11.x.XGetVisualInfo(lx_x11.display, VisualIDMask, &wanted, &count);
        if (!found || count == 0)
        {
            LOG_ERROR("No X visual 0x%x", desc.x11_visual);
            return false;
        }
        visual = found->visual;
        depth  = found->depth;
        lx_x11.x.XFree(found);
    }
    X11Window* data = (X11Window*)os_memory_alloc(sizeof(X11Window));
    data->colormap  = lx_x11.x.XCreateColormap(lx_x11.display, lx_x11.root, visual, AllocNone);
    XSetWindowAttributes attributes = {};
    attributes.colormap             = data->colormap;
    attributes.border_pixel         = 0;
    attributes.background_pixel     = 0;
    attributes.event_mask           = KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | FocusChangeMask | StructureNotifyMask | PropertyChangeMask | LeaveWindowMask;
    data->window = lx_x11.x.XCreateWindow(lx_x11.display, lx_x11.root, 0, 0, desc.width ? desc.width : 1, desc.height ? desc.height : 1, 0, depth, InputOutput, visual,
                                          CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &attributes);
    if (!data->window)
    {
        lx_x11.x.XFreeColormap(lx_x11.display, data->colormap);
        os_memory_release(data);
        return false;
    }
    Atom protocols[] = { lx_x11.wm_delete_window, lx_x11.net_wm_ping };
    lx_x11.x.XSetWMProtocols(lx_x11.display, data->window, protocols, ARRAY_SIZE(protocols));
    lx_x11.x.XStoreName(lx_x11.display, data->window, desc.title);
    lx_x11.x.XChangeProperty(lx_x11.display, data->window, lx_x11.net_wm_name, lx_x11.utf8_string, 8, PropModeReplace, (const u8*)desc.title, (i32)cstr_length(desc.title));
    if (lx_x11.im)
    {
        data->ic = lx_x11.x.XCreateIC(lx_x11.im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, data->window, XNFocusWindow, data->window, nullptr);
    }
    if (lx_x11.xi_opcode)
    {
        XIEventMask mask       = {};
        u8          bits[4]    = {};
        mask.deviceid          = XIAllMasterDevices;
        mask.mask_len          = sizeof(bits);
        mask.mask              = bits;
        XISetMask(bits, XI_RawMotion);
        lx_x11.xi.XISelectEvents(lx_x11.display, lx_x11.root, &mask, 1);
        lx_x11.xi_raw = true;
    }
    window->data    = data;
    window->width   = desc.width;
    window->height  = desc.height;
    window->density = 1.0f;
    window->hidden  = true;
    if (!desc.hidden)
    {
        window->driver->show(window);
        window->hidden = false;
    }
    lx_x11.x.XFlush(lx_x11.display);
    return true;
}

static void x11_destroy_window (OS_Window* window)
{
    X11Window* data = x11_data(window);
    if (data->grabbed)
    {
        lx_x11.x.XUngrabPointer(lx_x11.display, CurrentTime);
    }
    if (data->ic)
    {
        lx_x11.x.XDestroyIC(data->ic);
    }
    lx_x11.x.XDestroyWindow(lx_x11.display, data->window);
    lx_x11.x.XFreeColormap(lx_x11.display, data->colormap);
    lx_x11.x.XSync(lx_x11.display, False);
    os_memory_release(data);
    window->data = nullptr;
}

static b8 x11_native_handles (OS_Window* window, OS_WindowNative& out_native)
{
    out_native.window  = x11_data(window)->window;
    out_native.display = lx_x11.display;
    return true;
}

static f32 x11_refresh_rate (OS_Window* window)
{
    if (!lx_x11.xrandr_loaded)
    {
        return 0.0f;
    }
    i32    x     = 0;
    i32    y     = 0;
    Window child = 0;
    lx_x11.x.XTranslateCoordinates(lx_x11.display, x11_data(window)->window, lx_x11.root, (i32)window->width / 2, (i32)window->height / 2, &x, &y, &child);
    XRRScreenResources* resources = lx_x11.xrr.XRRGetScreenResourcesCurrent(lx_x11.display, lx_x11.root);
    if (!resources)
    {
        return 0.0f;
    }
    f32 rate = 0.0f;
    for (i32 c = 0; c < resources->ncrtc && rate == 0.0f; c++)
    {
        XRRCrtcInfo* crtc = lx_x11.xrr.XRRGetCrtcInfo(lx_x11.display, resources, resources->crtcs[c]);
        if (!crtc)
        {
            continue;
        }
        const b8 inside = crtc->mode != X_NONE && x >= crtc->x && y >= crtc->y && x < crtc->x + (i32)crtc->width && y < crtc->y + (i32)crtc->height;
        for (i32 m = 0; inside && m < resources->nmode; m++)
        {
            const XRRModeInfo& mode = resources->modes[m];
            if (mode.id == crtc->mode && mode.hTotal && mode.vTotal)
            {
                rate = (f32)((f64)mode.dotClock / ((f64)mode.hTotal * (f64)mode.vTotal));
            }
        }
        lx_x11.xrr.XRRFreeCrtcInfo(crtc);
    }
    lx_x11.xrr.XRRFreeScreenResources(resources);
    return rate;
}

static const char* const* x11_vulkan_extensions (u32& out_count)
{
    out_count = ARRAY_SIZE(lx_x11_vulkan_extensions);
    return lx_x11_vulkan_extensions;
}

static b8 x11_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface)
{
    const VkGetInstanceProcAddrFn get    = vulkan_get_proc(vk_get_instance_proc_addr);
    const VkCreateSurfaceFn       create = get ? (VkCreateSurfaceFn)get(vk_instance, "vkCreateXlibSurfaceKHR") : nullptr;
    if (!create)
    {
        LOG_ERROR("The Vulkan instance has no vkCreateXlibSurfaceKHR");
        return false;
    }
    const VkXlibSurfaceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, .pNext = nullptr, .flags = 0, .dpy = lx_x11.display, .window = x11_data(window)->window };
    const i32                     result = create(vk_instance, &info, nullptr, &out_surface);
    if (result != 0)
    {
        LOG_ERROR("vkCreateXlibSurfaceKHR failed: %d", result);
        return false;
    }
    return true;
}

static void x11_set_relative_mouse (OS_Window* window, b8 relative)
{
    X11Window* data = x11_data(window);
    if (relative)
    {
        Window       root_return  = 0;
        Window       child_return = 0;
        i32          wx           = 0;
        i32          wy           = 0;
        u32          mask         = 0;
        lx_x11.x.XQueryPointer(lx_x11.display, lx_x11.root, &root_return, &child_return, &data->saved_x, &data->saved_y, &wx, &wy, &mask);
        data->grabbed = lx_x11.x.XGrabPointer(lx_x11.display, data->window, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, data->window, x11_cursor(OS_Cursor::Hidden), CurrentTime) == GrabSuccess;
        lx_x11.relative_count++;
    }
    else
    {
        if (data->grabbed)
        {
            lx_x11.x.XUngrabPointer(lx_x11.display, CurrentTime);
            data->grabbed = false;
        }
        // The cursor comes back where looking began, not where the grab left it
        lx_x11.x.XWarpPointer(lx_x11.display, X_NONE, lx_x11.root, 0, 0, 0, 0, data->saved_x, data->saved_y);
        lx_x11.relative_count = lx_x11.relative_count > 0 ? lx_x11.relative_count - 1 : 0;
        data->last_valid      = false;
    }
    lx_x11.x.XFlush(lx_x11.display);
}

static void x11_send_wm_state (Window xwindow, b8 add, Atom first, Atom second)
{
    XEvent event               = {};
    event.xclient.type         = ClientMessage;
    event.xclient.window       = xwindow;
    event.xclient.message_type = lx_x11.net_wm_state;
    event.xclient.format       = 32;
    event.xclient.data.l[0]    = add ? 1 : 0;
    event.xclient.data.l[1]    = (long)first;
    event.xclient.data.l[2]    = (long)second;
    event.xclient.data.l[3]    = 1;
    lx_x11.x.XSendEvent(lx_x11.display, lx_x11.root, False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
}

static b8 x11_has_wm_state (Window xwindow, Atom state)
{
    Atom           type   = 0;
    i32            format = 0;
    unsigned long  count  = 0;
    unsigned long  after  = 0;
    u8*            data   = nullptr;
    b8             found  = false;
    if (lx_x11.x.XGetWindowProperty(lx_x11.display, xwindow, lx_x11.net_wm_state, 0, 64, False, XA_ATOM, &type, &format, &count, &after, &data) == Success && data)
    {
        const Atom* atoms = (const Atom*)data;
        for (unsigned long i = 0; i < count; i++)
        {
            found = found || atoms[i] == state;
        }
        lx_x11.x.XFree(data);
    }
    return found;
}

static void x11_handle_event (XEvent& event);

// Handles events until `done` says so or the timeout passes, as the window manager answers a request
static void x11_wait_for (b8 (*done)(OS_Window* window), OS_Window* window, u64 timeout_ns)
{
    const u64 deadline = os_monotonic_ns() + timeout_ns;
    lx_x11.x.XFlush(lx_x11.display);
    while (!done(window))
    {
        const u64 now = os_monotonic_ns();
        if (now >= deadline)
        {
            return;
        }
        if (lx_x11.x.XPending(lx_x11.display) > 0)
        {
            XEvent event = {};
            lx_x11.x.XNextEvent(lx_x11.display, &event);
            x11_handle_event(event);
            continue;
        }
        pollfd entry = { .fd = lx_x11.x.XConnectionNumber(lx_x11.display), .events = POLLIN, .revents = 0 };
        poll(&entry, 1, (int)((deadline - now) / 1000000ull) + 1);
    }
}

static b8 x11_is_maximized (OS_Window* window)
{
    const Window xwindow = x11_data(window)->window;
    return x11_has_wm_state(xwindow, lx_x11.net_wm_state_max_vert) && x11_has_wm_state(xwindow, lx_x11.net_wm_state_max_horz);
}

static b8 x11_is_mapped (OS_Window* window)
{
    return x11_data(window)->mapped;
}

// A hidden window is maximized as it maps; a shown one through the window manager, which may
// answer a few events later
static b8 x11_maximize (OS_Window* window)
{
    X11Window* data = x11_data(window);
    if (!data->mapped)
    {
        const Atom states[] = { lx_x11.net_wm_state_max_vert, lx_x11.net_wm_state_max_horz };
        lx_x11.x.XChangeProperty(lx_x11.display, data->window, lx_x11.net_wm_state, XA_ATOM, 32, PropModeReplace, (const u8*)states, 2);
        data->maximize_when_shown = true;
        return true;
    }
    x11_send_wm_state(data->window, true, lx_x11.net_wm_state_max_vert, lx_x11.net_wm_state_max_horz);
    x11_wait_for(x11_is_maximized, window, 100000000ull);
    return true;
}

static b8 x11_maximized (OS_Window* window)
{
    return x11_data(window)->maximize_when_shown || x11_is_maximized(window);
}

static void x11_show (OS_Window* window)
{
    X11Window* data = x11_data(window);
    lx_x11.x.XMapRaised(lx_x11.display, data->window);
    x11_wait_for(x11_is_mapped, window, 200000000ull);
    // Raised in front of the process's other windows, as the window manager allows
    XEvent event               = {};
    event.xclient.type         = ClientMessage;
    event.xclient.window       = data->window;
    event.xclient.message_type = lx_x11.net_active_window;
    event.xclient.format       = 32;
    event.xclient.data.l[0]    = 1;
    event.xclient.data.l[1]    = CurrentTime;
    lx_x11.x.XSendEvent(lx_x11.display, lx_x11.root, False, SubstructureNotifyMask | SubstructureRedirectMask, &event);
    if (data->maximize_when_shown)
    {
        x11_wait_for(x11_is_maximized, window, 100000000ull);
        data->maximize_when_shown = false;
    }
    lx_x11.x.XFlush(lx_x11.display);
}

static void x11_hide (OS_Window* window)
{
    X11Window* data = x11_data(window);
    lx_x11.x.XUnmapWindow(lx_x11.display, data->window);
    data->mapped = false;
    lx_x11.x.XFlush(lx_x11.display);
}

static void x11_set_cursor (OS_Window* window, OS_Cursor cursor)
{
    lx_x11.x.XDefineCursor(lx_x11.display, x11_data(window)->window, x11_cursor(cursor));
    lx_x11.x.XFlush(lx_x11.display);
}

static void x11_text_input (OS_Window* window, b8 active, f32 x, f32 y, f32 height)
{
    const X11Window* data = x11_data(window);
    if (!data->ic)
    {
        return;
    }
    if (active)
    {
        XPoint        spot       = { (short)x, (short)(y + height) };
        XVaNestedList attributes = lx_x11.x.XVaCreateNestedList(0, XNSpotLocation, &spot, nullptr);
        lx_x11.x.XSetICValues(data->ic, XNPreeditAttributes, attributes, nullptr);
        lx_x11.x.XFree(attributes);
        lx_x11.x.XSetICFocus(data->ic);
    }
    else
    {
        lx_x11.x.XUnsetICFocus(data->ic);
    }
}

static b8 x11_selection_arrived (OS_Window* window)
{
    XEvent event = {};
    return lx_x11.x.XCheckTypedWindowEvent(lx_x11.display, x11_data(window)->window, SelectionNotify, &event) != 0;
}

static b8 x11_clipboard_text (OS_Window* window, char* out_text, usize size)
{
    const Window owner = lx_x11.x.XGetSelectionOwner(lx_x11.display, lx_x11.clipboard);
    if (owner == X_NONE)
    {
        return false;
    }
    if (owner == x11_data(window)->window)
    {
        return lx_x11.clipboard_text && cstr_copy(out_text, size, lx_x11.clipboard_text);
    }
    lx_x11.x.XConvertSelection(lx_x11.display, lx_x11.clipboard, lx_x11.utf8_string, lx_x11.selection_property, x11_data(window)->window, CurrentTime);
    lx_x11.x.XFlush(lx_x11.display);
    // The owner answers through the server; other events wait in Xlib's queue meanwhile
    const u64 deadline = os_monotonic_ns() + 200000000ull;
    b8        arrived  = false;
    while (!arrived && os_monotonic_ns() < deadline)
    {
        arrived = x11_selection_arrived(window);
        if (!arrived)
        {
            pollfd entry = { .fd = lx_x11.x.XConnectionNumber(lx_x11.display), .events = POLLIN, .revents = 0 };
            poll(&entry, 1, 5);
            lx_x11.x.XPending(lx_x11.display);
        }
    }
    if (!arrived)
    {
        return false;
    }
    Atom          type   = 0;
    i32           format = 0;
    unsigned long count  = 0;
    unsigned long after  = 0;
    u8*           data   = nullptr;
    b8            fits   = false;
    if (lx_x11.x.XGetWindowProperty(lx_x11.display, x11_data(window)->window, lx_x11.selection_property, 0, (long)(size / 4), True, AnyPropertyType, &type, &format, &count, &after, &data) == Success && data)
    {
        fits = after == 0 && format == 8 && count < size;
        if (fits)
        {
            memory_copy(out_text, data, count);
            out_text[count] = 0;
        }
        lx_x11.x.XFree(data);
    }
    return fits;
}

static void x11_set_clipboard_text (OS_Window* window, const char* text)
{
    const usize length = cstr_length(text);
    if (length >= LX_MAX_TEXT)
    {
        return;
    }
    os_memory_release(lx_x11.clipboard_text);
    lx_x11.clipboard_text = (char*)os_memory_alloc(length + 1);
    memory_copy(lx_x11.clipboard_text, text, length + 1);
    lx_x11.x.XSetSelectionOwner(lx_x11.display, lx_x11.clipboard, x11_data(window)->window, CurrentTime);
    lx_x11.x.XFlush(lx_x11.display);
}

static void x11_answer_selection (const XSelectionRequestEvent& request)
{
    XEvent reply              = {};
    reply.xselection.type      = SelectionNotify;
    reply.xselection.requestor = request.requestor;
    reply.xselection.selection = request.selection;
    reply.xselection.target    = request.target;
    reply.xselection.time      = request.time;
    reply.xselection.property  = X_NONE;
    const Atom property        = request.property != X_NONE ? request.property : request.target;
    if (lx_x11.clipboard_text && request.target == lx_x11.targets)
    {
        const Atom offered[] = { lx_x11.targets, lx_x11.utf8_string, XA_STRING };
        lx_x11.x.XChangeProperty(lx_x11.display, request.requestor, property, XA_ATOM, 32, PropModeReplace, (const u8*)offered, ARRAY_SIZE(offered));
        reply.xselection.property = property;
    }
    else if (lx_x11.clipboard_text && (request.target == lx_x11.utf8_string || request.target == XA_STRING))
    {
        lx_x11.x.XChangeProperty(lx_x11.display, request.requestor, property, request.target, 8, PropModeReplace, (const u8*)lx_x11.clipboard_text, (i32)cstr_length(lx_x11.clipboard_text));
        reply.xselection.property = property;
    }
    lx_x11.x.XSendEvent(lx_x11.display, request.requestor, False, 0, &reply);
}

static b8 x11_key_label (OS_Key key, char* out, usize size)
{
    const u32 code = evdev_from_key(key);
    if (!code || !printable_key(key))
    {
        return false;
    }
    return label_from_keysym((u32)lx_x11.x.XkbKeycodeToKeysym(lx_x11.display, (KeyCode)(code + 8), 0, 0), out, size);
}

static OS_Key x11_button_key (u32 button)
{
    switch (button)
    {
    case Button1:
        return OS_Key::MouseLeft;
    case Button2:
        return OS_Key::MouseMiddle;
    case Button3:
        return OS_Key::MouseRight;
    case 8:
        return OS_Key::MouseX1;
    case 9:
        return OS_Key::MouseX2;
    default:
        return OS_Key::None;
    }
}

static void x11_handle_key (OS_Window* window, XKeyEvent& key)
{
    const b8     down    = key.type == KeyPress;
    const u32    code    = key.keycode;
    const b8     repeat  = down && code < 256 && lx_x11.keys_down[code];
    if (code < 256)
    {
        lx_x11.keys_down[code] = down;
    }
    const OS_Key os_key = code >= 8 ? key_from_evdev(code - 8) : OS_Key::None;
    if (os_key != OS_Key::None)
    {
        os_queue_event(window, { .type = down ? OS_RawEventType::KeyDown : OS_RawEventType::KeyUp, .key = os_key, .modifiers = x11_modifiers(key.state), .repeat = repeat });
    }
    const X11Window* data = x11_data(window);
    if (!down || !data->ic)
    {
        return;
    }
    char   text[OS_EVENT_TEXT_LENGTH] = {};
    KeySym keysym                     = 0;
    Status status                     = 0;
    const i32 length = lx_x11.x.Xutf8LookupString(data->ic, &key, text, sizeof(text) - 1, &keysym, &status);
    if ((status == XLookupChars || status == XLookupBoth) && length > 0 && (u8)text[0] >= 0x20 && text[0] != 0x7F)
    {
        OS_RawEvent event = { .type = OS_RawEventType::Text };
        memory_copy(event.text, text, (usize)length);
        os_queue_event(window, event);
    }
}

// While a window holds the pointer, its moves come from XInput2's raw motion, which the grab and
// the screen's edges do not stop
static void x11_handle_raw_motion (const XGenericEventCookie& cookie)
{
    if (lx_x11.relative_count == 0)
    {
        return;
    }
    const XIRawEvent* raw       = (const XIRawEvent*)cookie.data;
    f64               values[2] = {};
    const f64*        input     = raw->raw_values;
    for (i32 i = 0; i < raw->valuators.mask_len * 8 && i < 2; i++)
    {
        if (XIMaskIsSet(raw->valuators.mask, i))
        {
            values[i] = *input++;
        }
    }
    for (u32 i = 0; i < OS_MAX_WINDOWS; i++)
    {
        OS_Window* window = os_window_at(i);
        if (window && window->driver == &os_x11_driver && window->relative)
        {
            const X11Window* data = x11_data(window);
            os_queue_event(window, { .type = OS_RawEventType::MouseMotion, .x = data->last_x, .y = data->last_y, .dx = (f32)values[0], .dy = (f32)values[1] });
        }
    }
}

static void x11_handle_event (XEvent& event)
{
    if (event.type == GenericEvent)
    {
        XGenericEventCookie& cookie = event.xcookie;
        if (cookie.extension == lx_x11.xi_opcode && lx_x11.x.XGetEventData(lx_x11.display, &cookie))
        {
            if (cookie.evtype == XI_RawMotion)
            {
                x11_handle_raw_motion(cookie);
            }
            lx_x11.x.XFreeEventData(lx_x11.display, &cookie);
        }
        return;
    }
    // The input method takes the keys it composes from
    if (lx_x11.x.XFilterEvent(&event, X_NONE))
    {
        return;
    }
    if (event.type == SelectionRequest)
    {
        x11_answer_selection(event.xselectionrequest);
        return;
    }
    if (event.type == SelectionClear)
    {
        os_memory_release(lx_x11.clipboard_text);
        lx_x11.clipboard_text = nullptr;
        return;
    }
    OS_Window* window = x11_find(event.xany.window);
    if (!window)
    {
        return;
    }
    X11Window* data = x11_data(window);
    switch (event.type)
    {
    case KeyPress:
    case KeyRelease:
        x11_handle_key(window, event.xkey);
        break;
    case ButtonPress:
    case ButtonRelease:
    {
        const b8  down = event.type == ButtonPress;
        const u32 b    = event.xbutton.button;
        const u8  mods = x11_modifiers(event.xbutton.state);
        if (b >= 4 && b <= 7)
        {
            if (down)
            {
                os_queue_event(window, { .type = OS_RawEventType::MouseWheel, .modifiers = mods, .dx = b == 6 ? -1.0f : (b == 7 ? 1.0f : 0.0f), .dy = b == 4 ? 1.0f : (b == 5 ? -1.0f : 0.0f) });
            }
            break;
        }
        const OS_Key key = x11_button_key(b);
        if (key != OS_Key::None)
        {
            os_queue_event(window, { .type = down ? OS_RawEventType::MouseButtonDown : OS_RawEventType::MouseButtonUp, .key = key, .modifiers = mods, .x = (f32)event.xbutton.x, .y = (f32)event.xbutton.y });
        }
        break;
    }
    case MotionNotify:
    {
        const f32 x = (f32)event.xmotion.x;
        const f32 y = (f32)event.xmotion.y;
        if (!window->relative)
        {
            const f32 dx = data->last_valid ? x - data->last_x : 0.0f;
            const f32 dy = data->last_valid ? y - data->last_y : 0.0f;
            os_queue_event(window, { .type = OS_RawEventType::MouseMotion, .modifiers = x11_modifiers(event.xmotion.state), .x = x, .y = y, .dx = dx, .dy = dy });
            data->last_x     = x;
            data->last_y     = y;
            data->last_valid = true;
        }
        break;
    }
    case LeaveNotify:
        data->last_valid = false;
        break;
    case FocusIn:
    case FocusOut:
        // Grabs move the focus around without the user switching windows
        if (event.xfocus.mode == NotifyNormal || event.xfocus.mode == NotifyWhileGrabbed)
        {
            data->focused = event.type == FocusIn;
            os_queue_event(window, { .type = data->focused ? OS_RawEventType::FocusGained : OS_RawEventType::FocusLost });
            if (!data->focused)
            {
                memory_zero(lx_x11.keys_down, sizeof(lx_x11.keys_down));
            }
        }
        break;
    case ConfigureNotify:
        os_queue_resize(window, (u32)event.xconfigure.width, (u32)event.xconfigure.height, 1.0f);
        break;
    case MapNotify:
        data->mapped = true;
        break;
    case UnmapNotify:
        data->mapped = false;
        break;
    case ClientMessage:
        if (event.xclient.message_type == lx_x11.wm_protocols && (Atom)event.xclient.data.l[0] == lx_x11.wm_delete_window)
        {
            os_queue_event(window, { .type = OS_RawEventType::WindowClose });
        }
        else if (event.xclient.message_type == lx_x11.wm_protocols && (Atom)event.xclient.data.l[0] == lx_x11.net_wm_ping)
        {
            XEvent reply         = event;
            reply.xclient.window = lx_x11.root;
            lx_x11.x.XSendEvent(lx_x11.display, lx_x11.root, False, SubstructureNotifyMask | SubstructureRedirectMask, &reply);
        }
        break;
    default:
        break;
    }
}

static void x11_pump ()
{
    while (lx_x11.x.XPending(lx_x11.display) > 0)
    {
        XEvent event = {};
        lx_x11.x.XNextEvent(lx_x11.display, &event);
        x11_handle_event(event);
    }
    lx_x11.x.XFlush(lx_x11.display);
}

static void x11_wait (u64 timeout_ns)
{
    if (lx_x11.x.XPending(lx_x11.display) == 0)
    {
        pollfd entries[2] = {
            { .fd = lx_x11.x.XConnectionNumber(lx_x11.display), .events = POLLIN, .revents = 0 },
            { .fd = (int)os_sys_wake_handle(), .events = POLLIN, .revents = 0 },
        };
        poll(entries, 2, (int)(timeout_ns / 1000000ull));
    }
    os_sys_wake_clear();
}

const OS_VideoDriver os_x11_driver = {
    .name                  = "x11",
    .init                  = x11_init,
    .quit                  = x11_quit,
    .create_window         = x11_create_window,
    .destroy_window        = x11_destroy_window,
    .native_handles        = x11_native_handles,
    .refresh_rate          = x11_refresh_rate,
    .vulkan_extensions     = x11_vulkan_extensions,
    .create_vulkan_surface = x11_create_vulkan_surface,
    .set_relative_mouse    = x11_set_relative_mouse,
    .maximize              = x11_maximize,
    .maximized             = x11_maximized,
    .show                  = x11_show,
    .hide                  = x11_hide,
    .set_cursor            = x11_set_cursor,
    .text_input            = x11_text_input,
    .clipboard_text        = x11_clipboard_text,
    .set_clipboard_text    = x11_set_clipboard_text,
    .key_label             = x11_key_label,
    .pump                  = x11_pump,
    .wait                  = x11_wait,
};

//
// Wayland
//
// libwayland-client's ABI is stable, so its few types are declared here instead of including its
// headers; the protocols beyond the core one are tables written from their XML. Decorations come
// from the compositor (xdg-decoration) where it draws them; elsewhere (GNOME) puppetxr draws a
// title bar of its own on a subsurface, and the edges around the window take resize drags.

#define WL_MAX_OUTPUTS       8
#define WL_MARSHAL_DESTROY   1u
#define WL_TEXT_MIME         "text/plain;charset=utf-8"
#define WL_BUTTON_LEFT       0x110
#define WL_BUTTON_RIGHT      0x111
#define WL_BUTTON_MIDDLE     0x112
#define WL_BUTTON_SIDE       0x113
#define WL_BUTTON_EXTRA      0x114
#define WL_SEAT_POINTER      1u
#define WL_SEAT_KEYBOARD     2u
#define WL_OUTPUT_MODE_CURRENT 1u
#define WL_XDG_STATE_MAXIMIZED 1u
#define WL_DECORATION_SERVER_SIDE 2u
#define WL_LOCK_PERSISTENT   2u
#define WL_KEYMAP_XKB_V1     1u
#define WL_XDG_STATE_ACTIVATED 4u
#define WL_TITLE_HEIGHT      32   // points
#define WL_BORDER            8    // points around the window that resize it
#define WL_BUTTON_WIDTH      44
#define WL_DOUBLE_CLICK_NS   400000000ull
#define WL_SHM_ARGB8888      0u

namespace
{

struct WlProxy;
struct WlDisplay;
struct WlInterface;

struct WlMessage
{
    const char*               name;
    const char*               signature;
    const WlInterface* const* types;
};

struct WlInterface
{
    const char*      name;
    i32              version;
    i32              method_count;
    const WlMessage* methods;
    i32              event_count;
    const WlMessage* events;
};

struct WlArray
{
    usize size;
    usize alloc;
    void* data;
};

typedef void (*WlListener)();

} // namespace

#define WL_FUNCTIONS(F) \
    F(WlDisplay*, wl_display_connect, (const char* name)) \
    F(void, wl_display_disconnect, (WlDisplay* display)) \
    F(i32, wl_display_get_fd, (WlDisplay* display)) \
    F(i32, wl_display_dispatch_pending, (WlDisplay* display)) \
    F(i32, wl_display_flush, (WlDisplay* display)) \
    F(i32, wl_display_roundtrip, (WlDisplay* display)) \
    F(i32, wl_display_prepare_read, (WlDisplay* display)) \
    F(i32, wl_display_read_events, (WlDisplay* display)) \
    F(void, wl_display_cancel_read, (WlDisplay* display)) \
    F(i32, wl_display_get_error, (WlDisplay* display)) \
    F(WlProxy*, wl_proxy_marshal_flags, (WlProxy* proxy, u32 opcode, const WlInterface* interface, u32 version, u32 flags, ...)) \
    F(i32, wl_proxy_add_listener, (WlProxy* proxy, WlListener* listener, void* user)) \
    F(void, wl_proxy_destroy, (WlProxy* proxy)) \
    F(u32, wl_proxy_get_version, (WlProxy* proxy))

#define WL_INTERFACES(F) \
    F(wl_registry_interface) F(wl_compositor_interface) F(wl_surface_interface) F(wl_seat_interface) \
    F(wl_pointer_interface) F(wl_keyboard_interface) F(wl_output_interface) F(wl_shm_interface) \
    F(wl_callback_interface) F(wl_data_device_manager_interface) F(wl_data_device_interface) \
    F(wl_data_source_interface) F(wl_data_offer_interface) F(wl_subcompositor_interface) \
    F(wl_subsurface_interface) F(wl_shm_pool_interface) F(wl_buffer_interface)

#define WL_CURSOR_FUNCTIONS(F) \
    F(void*, wl_cursor_theme_load, (const char* name, i32 size, WlProxy* shm)) \
    F(void, wl_cursor_theme_destroy, (void* theme)) \
    F(void*, wl_cursor_theme_get_cursor, (void* theme, const char* name)) \
    F(WlProxy*, wl_cursor_image_get_buffer, (void* image))

#define XKB_FUNCTIONS(F) \
    F(xkb_context_new) F(xkb_context_unref) F(xkb_keymap_new_from_string) F(xkb_keymap_unref) \
    F(xkb_keymap_key_repeats) F(xkb_keymap_key_get_syms_by_level) F(xkb_state_new) F(xkb_state_unref) \
    F(xkb_state_update_mask) F(xkb_state_key_get_one_sym) F(xkb_state_key_get_utf8) \
    F(xkb_state_mod_name_is_active) F(xkb_compose_table_new_from_locale) F(xkb_compose_table_unref) \
    F(xkb_compose_state_new) F(xkb_compose_state_unref) F(xkb_compose_state_feed) \
    F(xkb_compose_state_get_status) F(xkb_compose_state_get_utf8) F(xkb_compose_state_reset)

#define WL_DECLARE_FN(ret, name, args) ret (*name) args; // NOLINT(bugprone-macro-parentheses): a declarator
#define WL_NAME_FN(ret, name, args)    #name,
#define WL_DECLARE_DATA(name)          const WlInterface* name;

namespace
{

struct WaylandFunctions
{
    WL_FUNCTIONS(WL_DECLARE_FN)
    WL_INTERFACES(WL_DECLARE_DATA)
};

struct WaylandCursorFunctions
{
    WL_CURSOR_FUNCTIONS(WL_DECLARE_FN)
};

struct XkbFunctions
{
    XKB_FUNCTIONS(LX_DECLARE)
};

struct WaylandOutput
{
    WlProxy* output;
    u32      name;     // the registry's
    f32      refresh;  // Hz of the current mode
    i32      scale;
};

struct WaylandWindow
{
    WlProxy* surface;
    WlProxy* xdg_surface;
    WlProxy* toplevel;
    WlProxy* decoration;
    b8       decorated;     // puppetxr draws the title bar
    b8       activated;
    WlProxy* title_surface;
    WlProxy* title_subsurface;
    WlProxy* title_viewport;
    WlProxy* title_buffer;
    u32      title_drawn_width;  // what the title bar was drawn for, to skip redrawing it
    f32      title_drawn_scale;
    i32      title_drawn_hover;
    b8       title_drawn_activated;
    b8       title_drawn_maximized;
    WlProxy* edge_surface;
    WlProxy* edge_subsurface;
    WlProxy* edge_viewport;
    WlProxy* edge_buffer;
    i32      hover;          // the title bar button under the pointer, -1 for none
    i32      pressed;        // the button the press began on, -1 for none
    WlProxy* viewport;
    WlProxy* fractional;
    WlProxy* locked;
    WlProxy* relative;
    u32      logical_width;  // points
    u32      logical_height;
    u32      pending_width;  // from the last toplevel configure, 0 for ours
    u32      pending_height;
    b8       pending_maximized;
    b8       configured;
    b8       maximized;
    b8       maximize_when_shown;
    f32      fractional_scale; // 0 until the compositor sends one
    i32      buffer_scale;     // integer scale without fractional scaling
    i32      output;           // index into the outputs, -1 for none
    char     title[128];
};

struct WaylandState
{
    b8                     loaded;
    b8                     cursor_loaded;
    b8                     xkb_loaded;
    WaylandFunctions       wl;
    WaylandCursorFunctions wlc;
    XkbFunctions           xkb;
    WlDisplay*             display;
    WlProxy*               registry;
    WlProxy*               compositor;
    WlProxy*               wm_base;
    WlProxy*               seat;
    WlProxy*               pointer;
    WlProxy*               keyboard;
    WlProxy*               shm;
    WlProxy*               decoration_manager;
    WlProxy*               relative_manager;
    WlProxy*               constraints;
    WlProxy*               fractional_manager;
    WlProxy*               viewporter;
    WlProxy*               cursor_shape_manager;
    WlProxy*               cursor_shape_device;
    WlProxy*               text_input_manager;
    WlProxy*               text_input;        // zwp_text_input_v3 of the seat
    OS_Window*             text_input_focus;  // the window the text input entered
    OS_Window*             text_input_window; // the window that wants text, os_window_text_input
    b8                     text_input_enabled;
    f32                    text_input_x;      // where the input method shows its candidates, points
    f32                    text_input_y;
    f32                    text_input_height;
    char*                  text_commit;       // the input method's text until its done event
    WlProxy*               data_device_manager;
    WlProxy*               data_device;
    WlProxy*               selection_offer;   // what another client offers
    WlProxy*               data_source;       // what we offer
    char*                  data_source_text;
    WlProxy*               cursor_surface;
    void*                  cursor_theme;
    WlProxy*               subcompositor;
    OS_Window*             decoration_focus;  // the window whose title bar or edges the pointer is on
    b8                     decoration_title;  // on the title bar, else on the edges
    f32                    decoration_x;      // the pointer on the title bar or edge surface, points
    f32                    decoration_y;
    u64                    title_click_ns;    // the last press on the title bar, for double clicks
    u32                    compositor_version;
    WaylandOutput          outputs[WL_MAX_OUTPUTS];
    u32                    outputs_count;

    OS_Window*             pointer_focus;
    OS_Window*             keyboard_focus;
    u32                    pointer_serial;    // of the pointer's enter, for its cursor
    u32                    input_serial;      // of the last input event, for the clipboard
    f32                    pointer_x;
    f32                    pointer_y;
    f32                    wheel_x;           // gathered until the pointer's frame event
    f32                    wheel_y;
    b8                     wheel_discrete;
    u8                     modifiers;

    struct xkb_context*       xkb_context;
    struct xkb_keymap*        keymap;
    struct xkb_state*         xkb_state;
    struct xkb_compose_table* compose_table;
    struct xkb_compose_state* compose_state;
    i32                    repeat_rate;       // per second, 0 for none
    i32                    repeat_delay_ms;
    u32                    repeat_key;        // evdev code of the repeating key, 0 for none
    u64                    repeat_next_ns;
};

} // namespace

static WaylandState lx_wl = {};

static void wl_pointer_set_cursor (OS_Cursor shape);
static void wl_pointer_set_shape (u32 shape, const char* theme_name);
static void wl_text_input_update ();
static b8   wl_decoration_enter (const WlProxy* surface, f32 x, f32 y);
static void wl_decoration_leave ();
static void wl_decoration_motion (f32 x, f32 y);
static void wl_decoration_button (u32 serial, u32 button, b8 down);
static void wl_update_decorations (OS_Window* window);

static const char* const lx_wl_names[] = { WL_FUNCTIONS(WL_NAME_FN) WL_INTERFACES(LX_NAME) };
static const char* const lx_wlc_names[] = { WL_CURSOR_FUNCTIONS(WL_NAME_FN) };
static const char* const lx_xkb_names[] = { XKB_FUNCTIONS(LX_NAME) };

static const char* const lx_wayland_vulkan_extensions[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };

// The protocols beyond the core one, from their XML. Only the requests and events puppetxr uses
// need their argument types; the order of every message is the protocol's.
namespace
{

extern const WlInterface wl_xdg_wm_base;
extern const WlInterface wl_xdg_surface;
extern const WlInterface wl_xdg_toplevel;
extern const WlInterface wl_xdg_decoration_manager;
extern const WlInterface wl_xdg_toplevel_decoration;
extern const WlInterface wl_relative_pointer_manager;
extern const WlInterface wl_relative_pointer;
extern const WlInterface wl_pointer_constraints;
extern const WlInterface wl_locked_pointer;
extern const WlInterface wl_fractional_scale_manager;
extern const WlInterface wl_fractional_scale;
extern const WlInterface wl_viewporter;
extern const WlInterface wl_viewport;
extern const WlInterface wl_cursor_shape_manager;
extern const WlInterface wl_cursor_shape_device;
extern const WlInterface wl_text_input_manager;
extern const WlInterface wl_text_input;

} // namespace

static const WlInterface* wl_types_none[]          = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
static const WlInterface* wl_types_xdg_surface[]   = { &wl_xdg_surface, nullptr };
static const WlInterface* wl_types_xdg_toplevel[]  = { &wl_xdg_toplevel };
static const WlInterface* wl_types_decoration[]    = { &wl_xdg_toplevel_decoration, &wl_xdg_toplevel };
static const WlInterface* wl_types_relative[]      = { &wl_relative_pointer, nullptr };
static const WlInterface* wl_types_locked[]        = { &wl_locked_pointer, nullptr, nullptr, nullptr, nullptr };
static const WlInterface* wl_types_fractional[]    = { &wl_fractional_scale, nullptr };
static const WlInterface* wl_types_viewport[]      = { &wl_viewport, nullptr };
static const WlInterface* wl_types_shape_device[]  = { &wl_cursor_shape_device, nullptr };
static const WlInterface* wl_types_text_input[]    = { &wl_text_input, nullptr };

static const WlMessage wl_xdg_wm_base_requests[] = {
    { "destroy", "", wl_types_none }, { "create_positioner", "n", wl_types_none }, { "get_xdg_surface", "no", wl_types_xdg_surface }, { "pong", "u", wl_types_none },
};
static const WlMessage wl_xdg_wm_base_events[] = { { "ping", "u", wl_types_none } };
namespace
{
const WlInterface wl_xdg_wm_base = { "xdg_wm_base", 1, 4, wl_xdg_wm_base_requests, 1, wl_xdg_wm_base_events };
} // namespace

static const WlMessage wl_xdg_surface_requests[] = {
    { "destroy", "", wl_types_none }, { "get_toplevel", "n", wl_types_xdg_toplevel }, { "get_popup", "n?oo", wl_types_none },
    { "set_window_geometry", "iiii", wl_types_none }, { "ack_configure", "u", wl_types_none },
};
static const WlMessage wl_xdg_surface_events[] = { { "configure", "u", wl_types_none } };
namespace
{
const WlInterface wl_xdg_surface = { "xdg_surface", 1, 5, wl_xdg_surface_requests, 1, wl_xdg_surface_events };
} // namespace

static const WlMessage wl_xdg_toplevel_requests[] = {
    { "destroy", "", wl_types_none }, { "set_parent", "?o", wl_types_none }, { "set_title", "s", wl_types_none }, { "set_app_id", "s", wl_types_none },
    { "show_window_menu", "ouii", wl_types_none }, { "move", "ou", wl_types_none }, { "resize", "ouu", wl_types_none }, { "set_max_size", "ii", wl_types_none },
    { "set_min_size", "ii", wl_types_none }, { "set_maximized", "", wl_types_none }, { "unset_maximized", "", wl_types_none }, { "set_fullscreen", "?o", wl_types_none },
    { "unset_fullscreen", "", wl_types_none }, { "set_minimized", "", wl_types_none },
};
static const WlMessage wl_xdg_toplevel_events[] = { { "configure", "iia", wl_types_none }, { "close", "", wl_types_none } };
namespace
{
const WlInterface wl_xdg_toplevel = { "xdg_toplevel", 1, 14, wl_xdg_toplevel_requests, 2, wl_xdg_toplevel_events };
} // namespace

static const WlMessage wl_xdg_decoration_manager_requests[] = { { "destroy", "", wl_types_none }, { "get_toplevel_decoration", "no", wl_types_decoration } };
namespace
{
const WlInterface wl_xdg_decoration_manager = { "zxdg_decoration_manager_v1", 1, 2, wl_xdg_decoration_manager_requests, 0, nullptr };
} // namespace
static const WlMessage wl_xdg_toplevel_decoration_requests[] = { { "destroy", "", wl_types_none }, { "set_mode", "u", wl_types_none }, { "unset_mode", "", wl_types_none } };
static const WlMessage wl_xdg_toplevel_decoration_events[] = { { "configure", "u", wl_types_none } };
namespace
{
const WlInterface wl_xdg_toplevel_decoration = { "zxdg_toplevel_decoration_v1", 1, 3, wl_xdg_toplevel_decoration_requests, 1, wl_xdg_toplevel_decoration_events };
} // namespace

static const WlMessage wl_relative_pointer_manager_requests[] = { { "destroy", "", wl_types_none }, { "get_relative_pointer", "no", wl_types_relative } };
namespace
{
const WlInterface wl_relative_pointer_manager = { "zwp_relative_pointer_manager_v1", 1, 2, wl_relative_pointer_manager_requests, 0, nullptr };
} // namespace
static const WlMessage wl_relative_pointer_requests[] = { { "destroy", "", wl_types_none } };
static const WlMessage wl_relative_pointer_events[] = { { "relative_motion", "uuffff", wl_types_none } };
namespace
{
const WlInterface wl_relative_pointer = { "zwp_relative_pointer_v1", 1, 1, wl_relative_pointer_requests, 1, wl_relative_pointer_events };
} // namespace

static const WlMessage wl_pointer_constraints_requests[] = { { "destroy", "", wl_types_none }, { "lock_pointer", "noo?ou", wl_types_locked }, { "confine_pointer", "noo?ou", wl_types_none } };
namespace
{
const WlInterface wl_pointer_constraints = { "zwp_pointer_constraints_v1", 1, 3, wl_pointer_constraints_requests, 0, nullptr };
} // namespace
static const WlMessage wl_locked_pointer_requests[] = { { "destroy", "", wl_types_none }, { "set_cursor_position_hint", "ff", wl_types_none }, { "set_region", "?o", wl_types_none } };
static const WlMessage wl_locked_pointer_events[] = { { "locked", "", wl_types_none }, { "unlocked", "", wl_types_none } };
namespace
{
const WlInterface wl_locked_pointer = { "zwp_locked_pointer_v1", 1, 3, wl_locked_pointer_requests, 2, wl_locked_pointer_events };
} // namespace

static const WlMessage wl_fractional_scale_manager_requests[] = { { "destroy", "", wl_types_none }, { "get_fractional_scale", "no", wl_types_fractional } };
namespace
{
const WlInterface wl_fractional_scale_manager = { "wp_fractional_scale_manager_v1", 1, 2, wl_fractional_scale_manager_requests, 0, nullptr };
} // namespace
static const WlMessage wl_fractional_scale_requests[] = { { "destroy", "", wl_types_none } };
static const WlMessage wl_fractional_scale_events[] = { { "preferred_scale", "u", wl_types_none } };
namespace
{
const WlInterface wl_fractional_scale = { "wp_fractional_scale_v1", 1, 1, wl_fractional_scale_requests, 1, wl_fractional_scale_events };
} // namespace

static const WlMessage wl_viewporter_requests[] = { { "destroy", "", wl_types_none }, { "get_viewport", "no", wl_types_viewport } };
namespace
{
const WlInterface wl_viewporter = { "wp_viewporter", 1, 2, wl_viewporter_requests, 0, nullptr };
} // namespace
static const WlMessage wl_viewport_requests[] = { { "destroy", "", wl_types_none }, { "set_source", "ffff", wl_types_none }, { "set_destination", "ii", wl_types_none } };
namespace
{
const WlInterface wl_viewport = { "wp_viewport", 1, 3, wl_viewport_requests, 0, nullptr };
} // namespace

static const WlMessage wl_cursor_shape_manager_requests[] = { { "destroy", "", wl_types_none }, { "get_pointer", "no", wl_types_shape_device }, { "get_tablet_tool_v2", "no", wl_types_none } };
namespace
{
const WlInterface wl_cursor_shape_manager = { "wp_cursor_shape_manager_v1", 1, 3, wl_cursor_shape_manager_requests, 0, nullptr };
} // namespace
static const WlMessage wl_cursor_shape_device_requests[] = { { "destroy", "", wl_types_none }, { "set_shape", "uu", wl_types_none } };
namespace
{
const WlInterface wl_cursor_shape_device = { "wp_cursor_shape_device_v1", 1, 2, wl_cursor_shape_device_requests, 0, nullptr };
} // namespace

static const WlMessage wl_text_input_manager_requests[] = { { "destroy", "", wl_types_none }, { "get_text_input", "no", wl_types_text_input } };
static const WlMessage wl_text_input_requests[] = {
    { "destroy", "", wl_types_none }, { "enable", "", wl_types_none }, { "disable", "", wl_types_none }, { "set_surrounding_text", "sii", wl_types_none },
    { "set_text_change_cause", "u", wl_types_none }, { "set_content_type", "uu", wl_types_none }, { "set_cursor_rectangle", "iiii", wl_types_none }, { "commit", "", wl_types_none },
};
static const WlMessage wl_text_input_events[] = {
    { "enter", "o", wl_types_none }, { "leave", "o", wl_types_none }, { "preedit_string", "?sii", wl_types_none },
    { "commit_string", "?s", wl_types_none }, { "delete_surrounding_text", "uu", wl_types_none }, { "done", "u", wl_types_none },
};
namespace
{
const WlInterface wl_text_input_manager = { "zwp_text_input_manager_v3", 1, 2, wl_text_input_manager_requests, 0, nullptr };
const WlInterface wl_text_input         = { "zwp_text_input_v3", 1, 8, wl_text_input_requests, 6, wl_text_input_events };
} // namespace

static WaylandWindow* wl_data (OS_Window* window) // NOLINT(misc-const-correctness): callers pass the windows they change
{
    return (WaylandWindow*)window->data;
}

static f32 wl_fixed (i32 value)
{
    return (f32)value / 256.0f;
}

static i32 wl_to_fixed (f32 value)
{
    return (i32)(value * 256.0f);
}

static void wl_destroy (WlProxy*& proxy, u32 destroy_opcode)
{
    if (proxy)
    {
        lx_wl.wl.wl_proxy_marshal_flags(proxy, destroy_opcode, nullptr, lx_wl.wl.wl_proxy_get_version(proxy), WL_MARSHAL_DESTROY);
        proxy = nullptr;
    }
}

static void wl_listen (WlProxy* proxy, const void* const* listener, void* user)
{
    lx_wl.wl.wl_proxy_add_listener(proxy, (WlListener*)(uintptr_t)listener, user);
}

static OS_Window* wl_window_of_surface (const WlProxy* surface)
{
    for (u32 i = 0; i < OS_MAX_WINDOWS; i++)
    {
        OS_Window* window = os_window_at(i);
        if (window && window->driver == &os_wayland_driver && window->data && wl_data(window)->surface == surface)
        {
            return window;
        }
    }
    return nullptr;
}

static f32 wl_scale (const WaylandWindow* data)
{
    return data->fractional_scale > 0.0f ? data->fractional_scale : (f32)(data->buffer_scale > 0 ? data->buffer_scale : 1);
}

// The surface's size in pixels follows its size in points and its scale
static void wl_apply_size (OS_Window* window)
{
    const WaylandWindow* data = wl_data(window);
    const f32      scale = wl_scale(data);
    const u32      width  = (u32)((f32)data->logical_width * scale + 0.5f);
    const u32      height = (u32)((f32)data->logical_height * scale + 0.5f);
    if (data->viewport)
    {
        lx_wl.wl.wl_proxy_marshal_flags(data->viewport, 2, nullptr, 1, 0, (i32)data->logical_width, (i32)data->logical_height); // set_destination
    }
    else if (data->fractional_scale <= 0.0f && lx_wl.compositor_version >= 3)
    {
        lx_wl.wl.wl_proxy_marshal_flags(data->surface, 8, nullptr, lx_wl.wl.wl_proxy_get_version(data->surface), 0, data->buffer_scale > 0 ? data->buffer_scale : 1); // set_buffer_scale
    }
    os_queue_resize(window, width, height, scale);
    wl_update_decorations(window);
}

//
// Wayland: registry, outputs, seat
//

static void wl_output_geometry (void*, WlProxy*, i32, i32, i32, i32, i32, const char*, const char*, i32)
{
}

static void wl_output_mode (void* user, WlProxy*, u32 flags, i32, i32, i32 refresh_mhz)
{
    WaylandOutput* output = (WaylandOutput*)user;
    if (flags & WL_OUTPUT_MODE_CURRENT)
    {
        output->refresh = (f32)refresh_mhz / 1000.0f;
    }
}

static void wl_output_done (void*, WlProxy*)
{
}

static void wl_output_scale (void* user, WlProxy*, i32 scale)
{
    ((WaylandOutput*)user)->scale = scale;
}

static const void* const wl_output_listener[] = { (void*)wl_output_geometry, (void*)wl_output_mode, (void*)wl_output_done, (void*)wl_output_scale };

namespace
{
extern const void* const wl_text_input_listener[6];
} // namespace

static void wl_pointer_enter (void*, WlProxy*, u32 serial, const WlProxy* surface, i32 x, i32 y)
{
    lx_wl.pointer_serial = serial;
    if (wl_decoration_enter(surface, wl_fixed(x), wl_fixed(y)))
    {
        lx_wl.pointer_focus = nullptr;
        return;
    }
    lx_wl.pointer_focus  = wl_window_of_surface(surface);
    lx_wl.pointer_x      = wl_fixed(x);
    lx_wl.pointer_y      = wl_fixed(y);
    if (lx_wl.pointer_focus)
    {
        wl_pointer_set_cursor(lx_wl.pointer_focus->relative ? OS_Cursor::Hidden : lx_wl.pointer_focus->cursor);
        os_queue_event(lx_wl.pointer_focus, { .type = OS_RawEventType::MouseMotion, .modifiers = lx_wl.modifiers, .x = lx_wl.pointer_x, .y = lx_wl.pointer_y });
    }
}

static void wl_pointer_leave (void*, WlProxy*, u32, WlProxy*)
{
    lx_wl.pointer_focus = nullptr;
    wl_decoration_leave();
}

static void wl_pointer_motion (void*, WlProxy*, u32, i32 x, i32 y)
{
    const f32 nx = wl_fixed(x);
    const f32 ny = wl_fixed(y);
    if (lx_wl.decoration_focus)
    {
        wl_decoration_motion(nx, ny);
        return;
    }
    if (lx_wl.pointer_focus && !lx_wl.pointer_focus->relative)
    {
        os_queue_event(lx_wl.pointer_focus, { .type = OS_RawEventType::MouseMotion, .modifiers = lx_wl.modifiers, .x = nx, .y = ny, .dx = nx - lx_wl.pointer_x, .dy = ny - lx_wl.pointer_y });
    }
    lx_wl.pointer_x = nx;
    lx_wl.pointer_y = ny;
}

static void wl_pointer_button (void*, WlProxy*, u32 serial, u32, u32 button, u32 state)
{
    lx_wl.input_serial = serial;
    if (lx_wl.decoration_focus)
    {
        wl_decoration_button(serial, button, state != 0);
        return;
    }
    OS_Key key         = OS_Key::None;
    switch (button)
    {
    case WL_BUTTON_LEFT:
        key = OS_Key::MouseLeft;
        break;
    case WL_BUTTON_RIGHT:
        key = OS_Key::MouseRight;
        break;
    case WL_BUTTON_MIDDLE:
        key = OS_Key::MouseMiddle;
        break;
    case WL_BUTTON_SIDE:
        key = OS_Key::MouseX1;
        break;
    case WL_BUTTON_EXTRA:
        key = OS_Key::MouseX2;
        break;
    default:
        return;
    }
    if (lx_wl.pointer_focus)
    {
        os_queue_event(lx_wl.pointer_focus, { .type = state ? OS_RawEventType::MouseButtonDown : OS_RawEventType::MouseButtonUp, .key = key, .modifiers = lx_wl.modifiers, .x = lx_wl.pointer_x, .y = lx_wl.pointer_y });
    }
}

// Ten units of a continuous axis are one notch, as libinput reports a wheel's
static void wl_pointer_axis (void*, WlProxy*, u32, u32 axis, i32 value)
{
    if (lx_wl.wheel_discrete)
    {
        return;
    }
    (axis == 0 ? lx_wl.wheel_y : lx_wl.wheel_x) += wl_fixed(value) / 10.0f;
}

static void wl_pointer_frame (void*, WlProxy*)
{
    if (lx_wl.pointer_focus && (lx_wl.wheel_x != 0.0f || lx_wl.wheel_y != 0.0f))
    {
        os_queue_event(lx_wl.pointer_focus, { .type = OS_RawEventType::MouseWheel, .modifiers = lx_wl.modifiers, .dx = lx_wl.wheel_x, .dy = -lx_wl.wheel_y });
    }
    lx_wl.wheel_x        = 0.0f;
    lx_wl.wheel_y        = 0.0f;
    lx_wl.wheel_discrete = false;
}

static void wl_pointer_axis_source (void*, WlProxy*, u32)
{
}

static void wl_pointer_axis_stop (void*, WlProxy*, u32, u32)
{
}

static void wl_pointer_axis_discrete (void*, WlProxy*, u32 axis, i32 discrete)
{
    if (!lx_wl.wheel_discrete)
    {
        lx_wl.wheel_x        = 0.0f;
        lx_wl.wheel_y        = 0.0f;
        lx_wl.wheel_discrete = true;
    }
    (axis == 0 ? lx_wl.wheel_y : lx_wl.wheel_x) += (f32)discrete;
}

static const void* const wl_pointer_listener[] = {
    (void*)wl_pointer_enter, (void*)wl_pointer_leave, (void*)wl_pointer_motion, (void*)wl_pointer_button, (void*)wl_pointer_axis,
    (void*)wl_pointer_frame, (void*)wl_pointer_axis_source, (void*)wl_pointer_axis_stop, (void*)wl_pointer_axis_discrete,
};

static void wl_keyboard_keymap (void*, WlProxy*, u32 format, i32 fd, u32 size)
{
    if (format != WL_KEYMAP_XKB_V1 || !lx_wl.xkb_loaded)
    {
        close(fd);
        return;
    }
    void* const text = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (text == MAP_FAILED)
    {
        return;
    }
    struct xkb_keymap* keymap = lx_wl.xkb.xkb_keymap_new_from_string(lx_wl.xkb_context, (const char*)text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(text, size);
    if (!keymap)
    {
        return;
    }
    if (lx_wl.xkb_state)
    {
        lx_wl.xkb.xkb_state_unref(lx_wl.xkb_state);
    }
    if (lx_wl.keymap)
    {
        lx_wl.xkb.xkb_keymap_unref(lx_wl.keymap);
    }
    lx_wl.keymap    = keymap;
    lx_wl.xkb_state = lx_wl.xkb.xkb_state_new(keymap);
}

static void wl_keyboard_enter (void*, WlProxy*, u32 serial, const WlProxy* surface, WlArray*)
{
    lx_wl.input_serial   = serial;
    lx_wl.keyboard_focus = wl_window_of_surface(surface);
    if (lx_wl.keyboard_focus)
    {
        os_queue_event(lx_wl.keyboard_focus, { .type = OS_RawEventType::FocusGained });
    }
}

static void wl_keyboard_leave (void*, WlProxy*, u32, WlProxy*)
{
    if (lx_wl.keyboard_focus)
    {
        os_queue_event(lx_wl.keyboard_focus, { .type = OS_RawEventType::FocusLost });
    }
    lx_wl.keyboard_focus = nullptr;
    lx_wl.repeat_key     = 0;
}

// The text a key types, through the compose table for dead keys and compose sequences
static void wl_key_text (u32 code)
{
    // With text input on, the input method commits the text, plain characters included
    if (!lx_wl.xkb_state || !lx_wl.keyboard_focus || (lx_wl.text_input_enabled && lx_wl.text_input_focus == lx_wl.keyboard_focus))
    {
        return;
    }
    const xkb_keycode_t keycode = code + 8;
    const xkb_keysym_t  keysym  = lx_wl.xkb.xkb_state_key_get_one_sym(lx_wl.xkb_state, keycode);
    OS_RawEvent         event   = { .type = OS_RawEventType::Text };
    if (lx_wl.compose_state && lx_wl.xkb.xkb_compose_state_feed(lx_wl.compose_state, keysym) == XKB_COMPOSE_FEED_ACCEPTED)
    {
        switch (lx_wl.xkb.xkb_compose_state_get_status(lx_wl.compose_state))
        {
        case XKB_COMPOSE_COMPOSING:
            return;
        case XKB_COMPOSE_COMPOSED:
            lx_wl.xkb.xkb_compose_state_get_utf8(lx_wl.compose_state, event.text, sizeof(event.text));
            lx_wl.xkb.xkb_compose_state_reset(lx_wl.compose_state);
            break;
        case XKB_COMPOSE_CANCELLED:
            lx_wl.xkb.xkb_compose_state_reset(lx_wl.compose_state);
            return;
        case XKB_COMPOSE_NOTHING:
            lx_wl.xkb.xkb_state_key_get_utf8(lx_wl.xkb_state, keycode, event.text, sizeof(event.text));
            break;
        }
    }
    else
    {
        lx_wl.xkb.xkb_state_key_get_utf8(lx_wl.xkb_state, keycode, event.text, sizeof(event.text));
    }
    if ((u8)event.text[0] >= 0x20 && event.text[0] != 0x7F)
    {
        os_queue_event(lx_wl.keyboard_focus, event);
    }
}

static void wl_keyboard_key (void*, WlProxy*, u32 serial, u32, u32 code, u32 state)
{
    lx_wl.input_serial = serial;
    const b8     down  = state != 0;
    const OS_Key key   = key_from_evdev(code);
    if (lx_wl.keyboard_focus && key != OS_Key::None)
    {
        os_queue_event(lx_wl.keyboard_focus, { .type = down ? OS_RawEventType::KeyDown : OS_RawEventType::KeyUp, .key = key, .modifiers = lx_wl.modifiers });
    }
    if (down)
    {
        wl_key_text(code);
        const b8 repeats = lx_wl.keymap && lx_wl.repeat_rate > 0 && lx_wl.xkb.xkb_keymap_key_repeats(lx_wl.keymap, code + 8);
        lx_wl.repeat_key     = repeats ? code : 0;
        lx_wl.repeat_next_ns = os_monotonic_ns() + (u64)(lx_wl.repeat_delay_ms > 0 ? lx_wl.repeat_delay_ms : 0) * (u64)1000000;
    }
    else if (code == lx_wl.repeat_key)
    {
        lx_wl.repeat_key = 0;
    }
}

static b8 wl_modifier_active (const char* name)
{
    return lx_wl.xkb.xkb_state_mod_name_is_active(lx_wl.xkb_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void wl_keyboard_modifiers (void*, WlProxy*, u32, u32 depressed, u32 latched, u32 locked, u32 group)
{
    if (!lx_wl.xkb_state)
    {
        return;
    }
    lx_wl.xkb.xkb_state_update_mask(lx_wl.xkb_state, depressed, latched, locked, 0, 0, group);
    lx_wl.modifiers = (u8)((wl_modifier_active(XKB_MOD_NAME_SHIFT) ? OS_MODIFIER_SHIFT : 0) | (wl_modifier_active(XKB_MOD_NAME_CTRL) ? OS_MODIFIER_CTRL : 0) | (wl_modifier_active(XKB_MOD_NAME_ALT) ? OS_MODIFIER_ALT : 0) | (wl_modifier_active(XKB_MOD_NAME_LOGO) ? OS_MODIFIER_SUPER : 0));
}

static void wl_keyboard_repeat_info (void*, WlProxy*, i32 rate, i32 delay)
{
    lx_wl.repeat_rate     = rate;
    lx_wl.repeat_delay_ms = delay;
}

static const void* const wl_keyboard_listener[] = {
    (void*)wl_keyboard_keymap, (void*)wl_keyboard_enter, (void*)wl_keyboard_leave, (void*)wl_keyboard_key, (void*)wl_keyboard_modifiers, (void*)wl_keyboard_repeat_info,
};

// The repeats of a held key, which Wayland leaves to the client
static void wl_repeat_keys ()
{
    if (!lx_wl.repeat_key || !lx_wl.keyboard_focus)
    {
        return;
    }
    const u64 now = os_monotonic_ns();
    while (lx_wl.repeat_key && now >= lx_wl.repeat_next_ns)
    {
        const OS_Key key = key_from_evdev(lx_wl.repeat_key);
        if (key != OS_Key::None)
        {
            os_queue_event(lx_wl.keyboard_focus, { .type = OS_RawEventType::KeyDown, .key = key, .modifiers = lx_wl.modifiers, .repeat = true });
        }
        wl_key_text(lx_wl.repeat_key);
        lx_wl.repeat_next_ns += (u64)1000000000 / (u64)lx_wl.repeat_rate;
    }
}

static void wl_relative_motion (void* user, WlProxy*, u32, u32, i32, i32, i32 dx_unaccel, i32 dy_unaccel)
{
    OS_Window* window = (OS_Window*)user;
    if (window->relative)
    {
        os_queue_event(window, { .type = OS_RawEventType::MouseMotion, .modifiers = lx_wl.modifiers, .x = lx_wl.pointer_x, .y = lx_wl.pointer_y, .dx = wl_fixed(dx_unaccel), .dy = wl_fixed(dy_unaccel) });
    }
}

static const void* const wl_relative_pointer_listener[] = { (void*)wl_relative_motion };

static void wl_data_offer_offer (void*, WlProxy*, const char*)
{
}

static void wl_data_offer_actions (void*, WlProxy*, u32)
{
}

static const void* const wl_data_offer_listener[] = { (void*)wl_data_offer_offer, (void*)wl_data_offer_actions, (void*)wl_data_offer_actions };

static void wl_device_data_offer (void*, WlProxy*, WlProxy* offer)
{
    wl_listen(offer, wl_data_offer_listener, nullptr);
}

static void wl_device_enter (void*, WlProxy*, u32, WlProxy*, i32, i32, WlProxy*)
{
}

static void wl_device_leave (void*, WlProxy*)
{
}

static void wl_device_motion (void*, WlProxy*, u32, i32, i32)
{
}

static void wl_device_drop (void*, WlProxy*)
{
}

static void wl_device_selection (void*, WlProxy*, WlProxy* offer)
{
    if (lx_wl.selection_offer && lx_wl.selection_offer != offer)
    {
        wl_destroy(lx_wl.selection_offer, 2);
    }
    lx_wl.selection_offer = offer;
}

static const void* const wl_data_device_listener[] = {
    (void*)wl_device_data_offer, (void*)wl_device_enter, (void*)wl_device_leave, (void*)wl_device_motion, (void*)wl_device_drop, (void*)wl_device_selection,
};

static void wl_seat_capabilities (void*, WlProxy* seat, u32 capabilities)
{
    if ((capabilities & WL_SEAT_POINTER) && !lx_wl.pointer)
    {
        lx_wl.pointer = lx_wl.wl.wl_proxy_marshal_flags(seat, 0, lx_wl.wl.wl_pointer_interface, lx_wl.wl.wl_proxy_get_version(seat), 0, nullptr);
        wl_listen(lx_wl.pointer, wl_pointer_listener, nullptr);
        if (lx_wl.cursor_shape_manager)
        {
            lx_wl.cursor_shape_device = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.cursor_shape_manager, 1, &wl_cursor_shape_device, 1, 0, nullptr, lx_wl.pointer);
        }
    }
    if ((capabilities & WL_SEAT_KEYBOARD) && !lx_wl.keyboard)
    {
        lx_wl.keyboard = lx_wl.wl.wl_proxy_marshal_flags(seat, 1, lx_wl.wl.wl_keyboard_interface, lx_wl.wl.wl_proxy_get_version(seat), 0, nullptr);
        wl_listen(lx_wl.keyboard, wl_keyboard_listener, nullptr);
    }
}

static void wl_seat_name (void*, WlProxy*, const char*)
{
}

static const void* const wl_seat_listener[] = { (void*)wl_seat_capabilities, (void*)wl_seat_name };

static void wl_wm_base_ping (void*, WlProxy* wm_base, u32 serial)
{
    lx_wl.wl.wl_proxy_marshal_flags(wm_base, 3, nullptr, 1, 0, serial);
}

static const void* const wl_wm_base_listener[] = { (void*)wl_wm_base_ping };

static WlProxy* wl_bind (WlProxy* registry, u32 name, const WlInterface* interface, u32 version)
{
    return lx_wl.wl.wl_proxy_marshal_flags(registry, 0, interface, version, 0, name, interface->name, version, nullptr);
}

static u32 wl_min (u32 a, u32 b)
{
    return a < b ? a : b;
}

static void wl_registry_global (void*, WlProxy* registry, u32 name, const char* interface, u32 version)
{
    if (cstr_equal(interface, "wl_compositor"))
    {
        lx_wl.compositor_version = wl_min(version, 4);
        lx_wl.compositor         = wl_bind(registry, name, lx_wl.wl.wl_compositor_interface, lx_wl.compositor_version);
    }
    else if (cstr_equal(interface, "xdg_wm_base"))
    {
        lx_wl.wm_base = wl_bind(registry, name, &wl_xdg_wm_base, 1);
        wl_listen(lx_wl.wm_base, wl_wm_base_listener, nullptr);
    }
    else if (cstr_equal(interface, "wl_seat") && !lx_wl.seat)
    {
        lx_wl.seat = wl_bind(registry, name, lx_wl.wl.wl_seat_interface, wl_min(version, 5));
        wl_listen(lx_wl.seat, wl_seat_listener, nullptr);
    }
    else if (cstr_equal(interface, "wl_shm"))
    {
        lx_wl.shm = wl_bind(registry, name, lx_wl.wl.wl_shm_interface, 1);
    }
    else if (cstr_equal(interface, "wl_output") && lx_wl.outputs_count < WL_MAX_OUTPUTS)
    {
        WaylandOutput& output = lx_wl.outputs[lx_wl.outputs_count++];
        output                = { .output = wl_bind(registry, name, lx_wl.wl.wl_output_interface, wl_min(version, 2)), .name = name, .refresh = 0.0f, .scale = 1 };
        wl_listen(output.output, wl_output_listener, &output);
    }
    else if (cstr_equal(interface, "zxdg_decoration_manager_v1"))
    {
        lx_wl.decoration_manager = wl_bind(registry, name, &wl_xdg_decoration_manager, 1);
    }
    else if (cstr_equal(interface, "zwp_relative_pointer_manager_v1"))
    {
        lx_wl.relative_manager = wl_bind(registry, name, &wl_relative_pointer_manager, 1);
    }
    else if (cstr_equal(interface, "zwp_pointer_constraints_v1"))
    {
        lx_wl.constraints = wl_bind(registry, name, &wl_pointer_constraints, 1);
    }
    else if (cstr_equal(interface, "wp_fractional_scale_manager_v1"))
    {
        lx_wl.fractional_manager = wl_bind(registry, name, &wl_fractional_scale_manager, 1);
    }
    else if (cstr_equal(interface, "wp_viewporter"))
    {
        lx_wl.viewporter = wl_bind(registry, name, &wl_viewporter, 1);
    }
    else if (cstr_equal(interface, "wp_cursor_shape_manager_v1"))
    {
        lx_wl.cursor_shape_manager = wl_bind(registry, name, &wl_cursor_shape_manager, 1);
    }
    else if (cstr_equal(interface, "wl_subcompositor"))
    {
        lx_wl.subcompositor = wl_bind(registry, name, lx_wl.wl.wl_subcompositor_interface, 1);
    }
    else if (cstr_equal(interface, "zwp_text_input_manager_v3"))
    {
        lx_wl.text_input_manager = wl_bind(registry, name, &wl_text_input_manager, 1);
    }
    else if (cstr_equal(interface, "wl_data_device_manager"))
    {
        lx_wl.data_device_manager = wl_bind(registry, name, lx_wl.wl.wl_data_device_manager_interface, wl_min(version, 3));
    }
}

static void wl_registry_global_remove (void*, WlProxy*, u32)
{
}

static const void* const wl_registry_listener[] = { (void*)wl_registry_global, (void*)wl_registry_global_remove };

static b8 wl_load ()
{
    if (!lx_wl.loaded)
    {
        lx_wl.loaded = load_symbols("libwayland-client.so.0", lx_wl_names, (void**)&lx_wl.wl, ARRAY_SIZE(lx_wl_names), true);
        if (!lx_wl.loaded)
        {
            return false;
        }
        lx_wl.cursor_loaded   = load_symbols("libwayland-cursor.so.0", lx_wlc_names, (void**)&lx_wl.wlc, ARRAY_SIZE(lx_wlc_names), false);
        lx_wl.xkb_loaded      = load_symbols("libxkbcommon.so.0", lx_xkb_names, (void**)&lx_wl.xkb, ARRAY_SIZE(lx_xkb_names), true);
    }
    return true;
}

static void wl_quit ();

static b8 wl_init ()
{
    if (!os_env("WAYLAND_DISPLAY") || !wl_load())
    {
        return false;
    }
    lx_wl.display = lx_wl.wl.wl_display_connect(nullptr);
    if (!lx_wl.display)
    {
        return false;
    }
    lx_wl.registry = lx_wl.wl.wl_proxy_marshal_flags((WlProxy*)lx_wl.display, 1, lx_wl.wl.wl_registry_interface, 1, 0, nullptr);
    wl_listen(lx_wl.registry, wl_registry_listener, nullptr);
    // The globals, then what binding them sent: the seat's capabilities, the outputs' modes
    lx_wl.wl.wl_display_roundtrip(lx_wl.display);
    lx_wl.wl.wl_display_roundtrip(lx_wl.display);
    if (!lx_wl.compositor || !lx_wl.wm_base)
    {
        LOG_WARNING("The Wayland compositor has no xdg-shell");
        wl_quit();
        return false;
    }
    if (lx_wl.xkb_loaded)
    {
        lx_wl.xkb_context = lx_wl.xkb.xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        const char* locale = os_env("LC_ALL");
        locale             = locale && locale[0] ? locale : os_env("LC_CTYPE");
        locale             = locale && locale[0] ? locale : os_env("LANG");
        lx_wl.compose_table = lx_wl.xkb_context ? lx_wl.xkb.xkb_compose_table_new_from_locale(lx_wl.xkb_context, locale && locale[0] ? locale : "C", XKB_COMPOSE_COMPILE_NO_FLAGS) : nullptr;
        lx_wl.compose_state = lx_wl.compose_table ? lx_wl.xkb.xkb_compose_state_new(lx_wl.compose_table, XKB_COMPOSE_STATE_NO_FLAGS) : nullptr;
    }
    if (lx_wl.data_device_manager && lx_wl.seat)
    {
        lx_wl.data_device = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.data_device_manager, 1, lx_wl.wl.wl_data_device_interface, lx_wl.wl.wl_proxy_get_version(lx_wl.data_device_manager), 0, nullptr, lx_wl.seat);
        wl_listen(lx_wl.data_device, wl_data_device_listener, nullptr);
    }
    if (lx_wl.text_input_manager && lx_wl.seat)
    {
        lx_wl.text_input = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.text_input_manager, 1, &wl_text_input, 1, 0, nullptr, lx_wl.seat);
        wl_listen(lx_wl.text_input, wl_text_input_listener, nullptr);
    }
    if (lx_wl.compositor)
    {
        lx_wl.cursor_surface = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.compositor, 0, lx_wl.wl.wl_surface_interface, lx_wl.compositor_version, 0, nullptr);
    }
    lx_wl.wl.wl_display_roundtrip(lx_wl.display);
    return true;
}

static void wl_quit ()
{
    if (lx_wl.cursor_theme)
    {
        lx_wl.wlc.wl_cursor_theme_destroy(lx_wl.cursor_theme);
        lx_wl.cursor_theme = nullptr;
    }
    if (lx_wl.compose_state)
    {
        lx_wl.xkb.xkb_compose_state_unref(lx_wl.compose_state);
    }
    if (lx_wl.compose_table)
    {
        lx_wl.xkb.xkb_compose_table_unref(lx_wl.compose_table);
    }
    if (lx_wl.xkb_state)
    {
        lx_wl.xkb.xkb_state_unref(lx_wl.xkb_state);
    }
    if (lx_wl.keymap)
    {
        lx_wl.xkb.xkb_keymap_unref(lx_wl.keymap);
    }
    if (lx_wl.xkb_context)
    {
        lx_wl.xkb.xkb_context_unref(lx_wl.xkb_context);
    }
    os_memory_release(lx_wl.data_source_text);
    os_memory_release(lx_wl.text_commit);
    // Disconnecting ends the objects on the compositor's side; the client's proxies need freeing
    WlProxy* const proxies[] = {
        lx_wl.cursor_shape_device, lx_wl.pointer, lx_wl.keyboard, lx_wl.data_device, lx_wl.selection_offer, lx_wl.data_source, lx_wl.cursor_surface,
        lx_wl.seat, lx_wl.shm, lx_wl.decoration_manager, lx_wl.relative_manager, lx_wl.constraints, lx_wl.fractional_manager, lx_wl.viewporter,
        lx_wl.cursor_shape_manager, lx_wl.data_device_manager, lx_wl.text_input, lx_wl.text_input_manager, lx_wl.subcompositor, lx_wl.wm_base, lx_wl.compositor, lx_wl.registry,
    };
    for (WlProxy* proxy : proxies)
    {
        if (proxy)
        {
            lx_wl.wl.wl_proxy_destroy(proxy);
        }
    }
    for (u32 i = 0; i < lx_wl.outputs_count; i++)
    {
        lx_wl.wl.wl_proxy_destroy(lx_wl.outputs[i].output);
    }
    if (lx_wl.display)
    {
        lx_wl.wl.wl_display_disconnect(lx_wl.display);
    }
    const WaylandState kept = lx_wl;
    lx_wl                   = {};
    lx_wl.loaded            = kept.loaded;
    lx_wl.cursor_loaded     = kept.cursor_loaded;
    lx_wl.xkb_loaded        = kept.xkb_loaded;
    lx_wl.wl                = kept.wl;
    lx_wl.wlc               = kept.wlc;
    lx_wl.xkb               = kept.xkb;
}

//
// Wayland: windows
//

static void wl_surface_enter (void* user, WlProxy*, const WlProxy* output)
{
    OS_Window*     window = (OS_Window*)user;
    WaylandWindow* data   = wl_data(window);
    for (u32 i = 0; i < lx_wl.outputs_count; i++)
    {
        if (lx_wl.outputs[i].output == output)
        {
            data->output = (i32)i;
            // Without fractional scaling, the integer scale of the output the window is on
            if (data->fractional_scale <= 0.0f && data->buffer_scale != lx_wl.outputs[i].scale)
            {
                data->buffer_scale = lx_wl.outputs[i].scale;
                wl_apply_size(window);
            }
        }
    }
}

static void wl_surface_leave (void*, WlProxy*, WlProxy*)
{
}

static const void* const wl_surface_listener[] = { (void*)wl_surface_enter, (void*)wl_surface_leave };

static void wl_fractional_preferred (void* user, WlProxy*, u32 scale_120)
{
    OS_Window* window             = (OS_Window*)user;
    wl_data(window)->fractional_scale = (f32)scale_120 / 120.0f;
    wl_apply_size(window);
}

static const void* const wl_fractional_listener[] = { (void*)wl_fractional_preferred };

//
// Wayland: a title bar of our own where the compositor draws none (GNOME). It is a subsurface
// above the window, drawn on the CPU into shared memory: the title, minimize, maximize and close.
// A transparent subsurface below the window, a 1x1 buffer stretched by its viewport, takes the
// drags of the edges around it. Moving and resizing are the compositor's (xdg_toplevel move and
// resize), so they feel like every other window's.
//

namespace
{

struct ShmBuffer
{
    WlProxy* buffer;
    u32*     pixels;
    usize    size;
};

} // namespace

// The classic 5x7 font, one byte per column, bit 0 the top row, for ASCII 32 to 126
static const u8 wl_font[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x5F, 0x00, 0x00 }, { 0x00, 0x07, 0x00, 0x07, 0x00 }, { 0x14, 0x7F, 0x14, 0x7F, 0x14 },
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, { 0x23, 0x13, 0x08, 0x64, 0x62 }, { 0x36, 0x49, 0x55, 0x22, 0x50 }, { 0x00, 0x05, 0x03, 0x00, 0x00 },
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, { 0x00, 0x41, 0x22, 0x1C, 0x00 }, { 0x08, 0x2A, 0x1C, 0x2A, 0x08 }, { 0x08, 0x08, 0x3E, 0x08, 0x08 },
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, { 0x08, 0x08, 0x08, 0x08, 0x08 }, { 0x00, 0x60, 0x60, 0x00, 0x00 }, { 0x20, 0x10, 0x08, 0x04, 0x02 },
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, { 0x00, 0x42, 0x7F, 0x40, 0x00 }, { 0x42, 0x61, 0x51, 0x49, 0x46 }, { 0x21, 0x41, 0x45, 0x4B, 0x31 },
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, { 0x27, 0x45, 0x45, 0x45, 0x39 }, { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, { 0x01, 0x71, 0x09, 0x05, 0x03 },
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, { 0x06, 0x49, 0x49, 0x29, 0x1E }, { 0x00, 0x36, 0x36, 0x00, 0x00 }, { 0x00, 0x56, 0x36, 0x00, 0x00 },
    { 0x08, 0x14, 0x22, 0x41, 0x00 }, { 0x14, 0x14, 0x14, 0x14, 0x14 }, { 0x00, 0x41, 0x22, 0x14, 0x08 }, { 0x02, 0x01, 0x51, 0x09, 0x06 },
    { 0x32, 0x49, 0x79, 0x41, 0x3E }, { 0x7E, 0x11, 0x11, 0x11, 0x7E }, { 0x7F, 0x49, 0x49, 0x49, 0x36 }, { 0x3E, 0x41, 0x41, 0x41, 0x22 },
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, { 0x7F, 0x49, 0x49, 0x49, 0x41 }, { 0x7F, 0x09, 0x09, 0x09, 0x01 }, { 0x3E, 0x41, 0x49, 0x49, 0x7A },
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, { 0x00, 0x41, 0x7F, 0x41, 0x00 }, { 0x20, 0x40, 0x41, 0x3F, 0x01 }, { 0x7F, 0x08, 0x14, 0x22, 0x41 },
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, { 0x7F, 0x04, 0x08, 0x10, 0x7F }, { 0x3E, 0x41, 0x41, 0x41, 0x3E },
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, { 0x3E, 0x41, 0x51, 0x21, 0x5E }, { 0x7F, 0x09, 0x19, 0x29, 0x46 }, { 0x46, 0x49, 0x49, 0x49, 0x31 },
    { 0x01, 0x01, 0x7F, 0x01, 0x01 }, { 0x3F, 0x40, 0x40, 0x40, 0x3F }, { 0x1F, 0x20, 0x40, 0x20, 0x1F }, { 0x3F, 0x40, 0x38, 0x40, 0x3F },
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, { 0x07, 0x08, 0x70, 0x08, 0x07 }, { 0x61, 0x51, 0x49, 0x45, 0x43 }, { 0x00, 0x7F, 0x41, 0x41, 0x00 },
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, { 0x00, 0x41, 0x41, 0x7F, 0x00 }, { 0x04, 0x02, 0x01, 0x02, 0x04 }, { 0x40, 0x40, 0x40, 0x40, 0x40 },
    { 0x00, 0x01, 0x02, 0x04, 0x00 }, { 0x20, 0x54, 0x54, 0x54, 0x78 }, { 0x7F, 0x48, 0x44, 0x44, 0x38 }, { 0x38, 0x44, 0x44, 0x44, 0x20 },
    { 0x38, 0x44, 0x44, 0x48, 0x7F }, { 0x38, 0x54, 0x54, 0x54, 0x18 }, { 0x08, 0x7E, 0x09, 0x01, 0x02 }, { 0x0C, 0x52, 0x52, 0x52, 0x3E },
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, { 0x00, 0x44, 0x7D, 0x40, 0x00 }, { 0x20, 0x40, 0x44, 0x3D, 0x00 }, { 0x7F, 0x10, 0x28, 0x44, 0x00 },
    { 0x00, 0x41, 0x7F, 0x40, 0x00 }, { 0x7C, 0x04, 0x18, 0x04, 0x78 }, { 0x7C, 0x08, 0x04, 0x04, 0x78 }, { 0x38, 0x44, 0x44, 0x44, 0x38 },
    { 0x7C, 0x14, 0x14, 0x14, 0x08 }, { 0x08, 0x14, 0x14, 0x18, 0x7C }, { 0x7C, 0x08, 0x04, 0x04, 0x08 }, { 0x48, 0x54, 0x54, 0x54, 0x20 },
    { 0x04, 0x3F, 0x44, 0x40, 0x20 }, { 0x3C, 0x40, 0x40, 0x20, 0x7C }, { 0x1C, 0x20, 0x40, 0x20, 0x1C }, { 0x3C, 0x40, 0x30, 0x40, 0x3C },
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, { 0x0C, 0x50, 0x50, 0x50, 0x3C }, { 0x44, 0x64, 0x54, 0x4C, 0x44 }, { 0x00, 0x08, 0x36, 0x41, 0x00 },
    { 0x00, 0x00, 0x7F, 0x00, 0x00 }, { 0x00, 0x41, 0x36, 0x08, 0x00 }, { 0x08, 0x04, 0x08, 0x10, 0x08 },
};

#define WL_TITLE_BACKGROUND          0xFF2B2D31u
#define WL_TITLE_BACKGROUND_INACTIVE 0xFF34363Bu
#define WL_TITLE_TEXT                0xFFE6E6E6u
#define WL_TITLE_TEXT_INACTIVE       0xFF8C8F94u
#define WL_TITLE_HOVER               0xFF45484Eu
#define WL_TITLE_CLOSE_HOVER         0xFFC42B1Cu

// xdg_toplevel's resize edges
#define WL_EDGE_TOP    1u
#define WL_EDGE_BOTTOM 2u
#define WL_EDGE_LEFT   4u
#define WL_EDGE_RIGHT  8u

static ShmBuffer wl_shm_buffer (u32 width, u32 height)
{
    ShmBuffer out = {};
    if (!lx_wl.shm || width == 0 || height == 0)
    {
        return out;
    }
    const usize size = (usize)width * height * 4;
    const i32   fd   = memfd_create("puppetxr-decoration", MFD_CLOEXEC);
    if (fd < 0)
    {
        return out;
    }
    void* const pixels = ftruncate(fd, (off_t)size) == 0 ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (pixels == MAP_FAILED)
    {
        close(fd);
        return out;
    }
    WlProxy* const pool = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.shm, 0, lx_wl.wl.wl_shm_pool_interface, lx_wl.wl.wl_proxy_get_version(lx_wl.shm), 0, nullptr, fd, (i32)size);
    out.buffer          = lx_wl.wl.wl_proxy_marshal_flags(pool, 0, lx_wl.wl.wl_buffer_interface, lx_wl.wl.wl_proxy_get_version(pool), 0, nullptr, 0, (i32)width, (i32)height, (i32)(width * 4), WL_SHM_ARGB8888);
    lx_wl.wl.wl_proxy_marshal_flags(pool, 1, nullptr, lx_wl.wl.wl_proxy_get_version(pool), WL_MARSHAL_DESTROY);
    close(fd);
    out.pixels = (u32*)pixels;
    out.size   = size;
    return out;
}

static void wl_fill (const ShmBuffer& target, i32 width, i32 height, i32 x, i32 y, i32 w, i32 h, u32 color)
{
    const i32 x0 = x > 0 ? x : 0;
    const i32 y0 = y > 0 ? y : 0;
    const i32 x1 = x + w < width ? x + w : width;
    const i32 y1 = y + h < height ? y + h : height;
    for (i32 row = y0; row < y1; row++)
    {
        for (i32 column = x0; column < x1; column++)
        {
            target.pixels[(usize)row * (usize)width + (usize)column] = color;
        }
    }
}

// The title bar button at a point of the title bar, in points: 0 minimize, 1 maximize, 2 close
static i32 wl_title_button_at (const WaylandWindow* data, f32 x, f32 y)
{
    const f32 buttons = (f32)data->logical_width - 3.0f * (f32)WL_BUTTON_WIDTH;
    if (y < 0.0f || y >= (f32)WL_TITLE_HEIGHT || x < buttons || x >= (f32)data->logical_width)
    {
        return -1;
    }
    return (i32)((x - buttons) / (f32)WL_BUTTON_WIDTH);
}

static void wl_draw_title (OS_Window* window)
{
    WaylandWindow* data   = wl_data(window);
    const f32      scale  = wl_scale(data);
    const i32      width  = (i32)((f32)data->logical_width * scale + 0.5f);
    const i32      height = (i32)((f32)WL_TITLE_HEIGHT * scale + 0.5f);
    if (data->title_buffer && data->title_drawn_width == data->logical_width && data->title_drawn_scale == scale && data->title_drawn_hover == data->hover &&
        data->title_drawn_activated == data->activated && data->title_drawn_maximized == data->maximized)
    {
        return;
    }
    const ShmBuffer target = wl_shm_buffer((u32)width, (u32)height);
    if (!target.buffer)
    {
        return;
    }
    const u32 text_color = data->activated ? WL_TITLE_TEXT : WL_TITLE_TEXT_INACTIVE;
    wl_fill(target, width, height, 0, 0, width, height, data->activated ? WL_TITLE_BACKGROUND : WL_TITLE_BACKGROUND_INACTIVE);

    // Buttons from the right: close, maximize, minimize; an icon is 10 points with lines of one
    const i32 button = (i32)((f32)WL_BUTTON_WIDTH * scale + 0.5f);
    const i32 icon   = (i32)(10.0f * scale + 0.5f);
    const i32 line   = scale >= 1.5f ? (i32)(scale + 0.5f) : 1;
    for (i32 b = 0; b < 3; b++)
    {
        const i32 x0 = width - (3 - b) * button;
        if (data->hover == b)
        {
            wl_fill(target, width, height, x0, 0, button, height, b == 2 ? WL_TITLE_CLOSE_HOVER : WL_TITLE_HOVER);
        }
        const i32 left = x0 + (button - icon) / 2;
        const i32 top  = (height - icon) / 2;
        switch (b)
        {
        case 0:
            wl_fill(target, width, height, left, top + icon / 2, icon, line, text_color);
            break;
        case 1:
        {
            // Restore draws the window behind as well
            const i32 inset = data->maximized ? icon / 4 : 0;
            if (inset > 0)
            {
                wl_fill(target, width, height, left + inset, top, icon - inset, line, text_color);
                wl_fill(target, width, height, left + icon - line, top, line, icon - inset, text_color);
            }
            const i32 x = left;
            const i32 y = top + inset;
            const i32 s = icon - inset;
            wl_fill(target, width, height, x, y, s, line, text_color);
            wl_fill(target, width, height, x, y + s - line, s, line, text_color);
            wl_fill(target, width, height, x, y, line, s, text_color);
            wl_fill(target, width, height, x + s - line, y, line, s, text_color);
            break;
        }
        default:
            for (i32 i = 0; i < icon; i++)
            {
                wl_fill(target, width, height, left + i, top + i, line, line, text_color);
                wl_fill(target, width, height, left + icon - 1 - i, top + i, line, line, text_color);
            }
            break;
        }
    }

    // The title in the 5x7 font, two pixels per font pixel at one pixel per point
    const i32 dot     = (i32)(scale * 2.0f + 0.5f) > 1 ? (i32)(scale * 2.0f + 0.5f) : 1;
    const i32 advance = 6 * dot;
    const i32 room    = width - 3 * button - 2 * advance;
    const i32 count   = (i32)cstr_length(data->title) < room / advance ? (i32)cstr_length(data->title) : (room > 0 ? room / advance : 0);
    i32       x       = (width - 3 * button - count * advance) / 2;
    const i32 y       = (height - 7 * dot) / 2;
    for (i32 i = 0; i < count; i++, x += advance)
    {
        const u8 c     = (u8)data->title[i];
        const u8* glyph = wl_font[(c >= 32 && c < 127 ? c : '?') - 32];
        for (i32 column = 0; column < 5; column++)
        {
            for (i32 row = 0; row < 7; row++)
            {
                if (glyph[column] & (1u << row))
                {
                    wl_fill(target, width, height, x + column * dot, y + row * dot, dot, dot, text_color);
                }
            }
        }
    }
    munmap(target.pixels, target.size);

    const u32 version = lx_wl.wl.wl_proxy_get_version(data->title_surface);
    lx_wl.wl.wl_proxy_marshal_flags(data->title_surface, 1, nullptr, version, 0, target.buffer, 0, 0);
    lx_wl.wl.wl_proxy_marshal_flags(data->title_surface, 2, nullptr, version, 0, 0, 0, INT32_MAX, INT32_MAX);
    if (data->title_viewport)
    {
        lx_wl.wl.wl_proxy_marshal_flags(data->title_viewport, 2, nullptr, 1, 0, (i32)data->logical_width, (i32)WL_TITLE_HEIGHT);
    }
    else if (lx_wl.compositor_version >= 3)
    {
        lx_wl.wl.wl_proxy_marshal_flags(data->title_surface, 8, nullptr, version, 0, (i32)scale);
    }
    lx_wl.wl.wl_proxy_marshal_flags(data->title_surface, 6, nullptr, version, 0);
    wl_destroy(data->title_buffer, 0);
    data->title_buffer          = target.buffer;
    data->title_drawn_width     = data->logical_width;
    data->title_drawn_scale     = scale;
    data->title_drawn_hover     = data->hover;
    data->title_drawn_activated = data->activated;
    data->title_drawn_maximized = data->maximized;
}

static WlProxy* wl_subsurface_of (const WlProxy* surface, const WlProxy* parent)
{
    WlProxy* const subsurface = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.subcompositor, 1, lx_wl.wl.wl_subsurface_interface, 1, 0, nullptr, surface, parent);
    lx_wl.wl.wl_proxy_marshal_flags(subsurface, 5, nullptr, 1, 0); // set_desync: its commits apply at once
    return subsurface;
}

static void wl_create_decorations (OS_Window* window)
{
    WaylandWindow* data = wl_data(window);
    if (!lx_wl.subcompositor || !lx_wl.shm)
    {
        LOG_INFO("The Wayland compositor draws no decorations and has no subsurfaces; the mirror window has no title bar");
        return;
    }
    data->title_surface    = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.compositor, 0, lx_wl.wl.wl_surface_interface, lx_wl.compositor_version, 0, nullptr);
    data->title_subsurface = wl_subsurface_of(data->title_surface, data->surface);
    if (lx_wl.viewporter)
    {
        data->title_viewport  = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.viewporter, 1, &wl_viewport, 1, 0, nullptr, data->title_surface);
        data->edge_surface    = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.compositor, 0, lx_wl.wl.wl_surface_interface, lx_wl.compositor_version, 0, nullptr);
        data->edge_subsurface = wl_subsurface_of(data->edge_surface, data->surface);
        lx_wl.wl.wl_proxy_marshal_flags(data->edge_subsurface, 3, nullptr, 1, 0, data->surface); // place_below
        data->edge_viewport = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.viewporter, 1, &wl_viewport, 1, 0, nullptr, data->edge_surface);
        const ShmBuffer clear = wl_shm_buffer(1, 1);
        if (clear.buffer)
        {
            clear.pixels[0] = 0;
            munmap(clear.pixels, clear.size);
            data->edge_buffer = clear.buffer;
        }
    }
    data->decorated         = true;
    data->hover             = -1;
    data->pressed           = -1;
    data->title_drawn_width = 0;
}

static void wl_destroy_decorations (OS_Window* window)
{
    WaylandWindow* data = wl_data(window);
    if (lx_wl.decoration_focus == window)
    {
        lx_wl.decoration_focus = nullptr;
    }
    wl_destroy(data->title_buffer, 0);
    wl_destroy(data->title_viewport, 0);
    wl_destroy(data->title_subsurface, 0);
    wl_destroy(data->title_surface, 0);
    wl_destroy(data->edge_buffer, 0);
    wl_destroy(data->edge_viewport, 0);
    wl_destroy(data->edge_subsurface, 0);
    wl_destroy(data->edge_surface, 0);
    data->decorated = false;
}

// Positions and the window geometry apply with the window's next commit; the title bar and the
// edges commit their own content
static void wl_update_decorations (OS_Window* window)
{
    const WaylandWindow* data = wl_data(window);
    if (!data->decorated)
    {
        return;
    }
    lx_wl.wl.wl_proxy_marshal_flags(data->title_subsurface, 1, nullptr, 1, 0, 0, -WL_TITLE_HEIGHT);
    wl_draw_title(window);
    if (data->edge_surface)
    {
        const u32 version = lx_wl.wl.wl_proxy_get_version(data->edge_surface);
        if (data->maximized || !data->edge_buffer)
        {
            lx_wl.wl.wl_proxy_marshal_flags(data->edge_surface, 1, nullptr, version, 0, nullptr, 0, 0);
        }
        else
        {
            lx_wl.wl.wl_proxy_marshal_flags(data->edge_subsurface, 1, nullptr, 1, 0, -WL_BORDER, -WL_TITLE_HEIGHT - WL_BORDER);
            lx_wl.wl.wl_proxy_marshal_flags(data->edge_viewport, 2, nullptr, 1, 0, (i32)data->logical_width + 2 * WL_BORDER, (i32)data->logical_height + WL_TITLE_HEIGHT + 2 * WL_BORDER);
            lx_wl.wl.wl_proxy_marshal_flags(data->edge_surface, 1, nullptr, version, 0, data->edge_buffer, 0, 0);
            lx_wl.wl.wl_proxy_marshal_flags(data->edge_surface, 2, nullptr, version, 0, 0, 0, INT32_MAX, INT32_MAX);
        }
        lx_wl.wl.wl_proxy_marshal_flags(data->edge_surface, 6, nullptr, version, 0);
    }
    if (data->xdg_surface)
    {
        lx_wl.wl.wl_proxy_marshal_flags(data->xdg_surface, 3, nullptr, 1, 0, 0, -WL_TITLE_HEIGHT, (i32)data->logical_width, (i32)data->logical_height + WL_TITLE_HEIGHT); // set_window_geometry
    }
    lx_wl.wl.wl_display_flush(lx_wl.display);
}

// The edges a point of the edge surface resizes; corners reach twice the border along each side
static u32 wl_edges_at (const WaylandWindow* data, f32 x, f32 y)
{
    const f32 width  = (f32)(data->logical_width + 2 * WL_BORDER);
    const f32 height = (f32)(data->logical_height + WL_TITLE_HEIGHT + 2 * WL_BORDER);
    const f32 border = (f32)WL_BORDER;
    const f32 corner = 2.0f * border;
    b8        left   = x < border;
    b8        right  = x >= width - border;
    b8        top    = y < border;
    b8        bottom = y >= height - border;
    if (left || right)
    {
        top    = top || y < corner;
        bottom = bottom || y >= height - corner;
    }
    if (top || bottom)
    {
        left  = left || x < corner;
        right = right || x >= width - corner;
    }
    return (top ? WL_EDGE_TOP : 0) | (bottom ? WL_EDGE_BOTTOM : 0) | (left ? WL_EDGE_LEFT : 0) | (right ? WL_EDGE_RIGHT : 0);
}

static void wl_decoration_cursor ()
{
    if (lx_wl.decoration_title)
    {
        wl_pointer_set_shape(1, "default");
        return;
    }
    // wp_cursor_shape_device_v1: e 18, n 19, ne 20, nw 21, s 22, se 23, sw 24, w 25
    switch (wl_edges_at(wl_data(lx_wl.decoration_focus), lx_wl.decoration_x, lx_wl.decoration_y))
    {
    case WL_EDGE_TOP:
        wl_pointer_set_shape(19, "n-resize");
        break;
    case WL_EDGE_BOTTOM:
        wl_pointer_set_shape(22, "s-resize");
        break;
    case WL_EDGE_LEFT:
        wl_pointer_set_shape(25, "w-resize");
        break;
    case WL_EDGE_RIGHT:
        wl_pointer_set_shape(18, "e-resize");
        break;
    case WL_EDGE_TOP | WL_EDGE_LEFT:
        wl_pointer_set_shape(21, "nw-resize");
        break;
    case WL_EDGE_TOP | WL_EDGE_RIGHT:
        wl_pointer_set_shape(20, "ne-resize");
        break;
    case WL_EDGE_BOTTOM | WL_EDGE_LEFT:
        wl_pointer_set_shape(24, "sw-resize");
        break;
    case WL_EDGE_BOTTOM | WL_EDGE_RIGHT:
        wl_pointer_set_shape(23, "se-resize");
        break;
    default:
        wl_pointer_set_shape(1, "default");
        break;
    }
}

static b8 wl_decoration_enter (const WlProxy* surface, f32 x, f32 y)
{
    for (u32 i = 0; i < OS_MAX_WINDOWS; i++)
    {
        OS_Window* window = os_window_at(i);
        if (!window || window->driver != &os_wayland_driver || !window->data || !wl_data(window)->decorated)
        {
            continue;
        }
        const WaylandWindow* data = wl_data(window);
        if (surface == data->title_surface || surface == data->edge_surface)
        {
            lx_wl.decoration_focus = window;
            lx_wl.decoration_title = surface == data->title_surface;
            wl_decoration_motion(x, y);
            return true;
        }
    }
    return false;
}

static void wl_decoration_leave ()
{
    if (lx_wl.decoration_focus)
    {
        WaylandWindow* data = wl_data(lx_wl.decoration_focus);
        data->hover         = -1;
        data->pressed       = -1;
        wl_draw_title(lx_wl.decoration_focus);
    }
    lx_wl.decoration_focus = nullptr;
}

static void wl_decoration_motion (f32 x, f32 y)
{
    lx_wl.decoration_x = x;
    lx_wl.decoration_y = y;
    WaylandWindow* data = wl_data(lx_wl.decoration_focus);
    if (lx_wl.decoration_title)
    {
        data->hover = wl_title_button_at(data, x, y);
        wl_draw_title(lx_wl.decoration_focus);
    }
    wl_decoration_cursor();
}

static void wl_decoration_button (u32 serial, u32 button, b8 down)
{
    OS_Window* const window = lx_wl.decoration_focus;
    WaylandWindow*   data   = wl_data(window);
    if (!data->toplevel)
    {
        return;
    }
    const u32 version = lx_wl.wl.wl_proxy_get_version(data->toplevel);
    if (!lx_wl.decoration_title)
    {
        const u32 edges = wl_edges_at(data, lx_wl.decoration_x, lx_wl.decoration_y);
        if (down && button == WL_BUTTON_LEFT && edges && !data->maximized)
        {
            lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 6, nullptr, version, 0, lx_wl.seat, serial, edges); // resize
        }
        return;
    }
    const i32 at = wl_title_button_at(data, lx_wl.decoration_x, lx_wl.decoration_y);
    if (button == WL_BUTTON_LEFT && down && at >= 0)
    {
        data->pressed = at;
    }
    else if (button == WL_BUTTON_LEFT && down)
    {
        const u64 now = os_monotonic_ns();
        if (lx_wl.title_click_ns && now - lx_wl.title_click_ns < WL_DOUBLE_CLICK_NS)
        {
            lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, data->maximized ? 10 : 9, nullptr, version, 0); // unset or set maximized
            lx_wl.title_click_ns = 0;
        }
        else
        {
            lx_wl.title_click_ns = now;
            lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 5, nullptr, version, 0, lx_wl.seat, serial); // move
        }
    }
    else if (button == WL_BUTTON_LEFT && !down)
    {
        if (data->pressed >= 0 && data->pressed == at)
        {
            switch (at)
            {
            case 0:
                lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 13, nullptr, version, 0); // set_minimized
                break;
            case 1:
                lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, data->maximized ? 10 : 9, nullptr, version, 0);
                break;
            default:
                os_queue_event(window, { .type = OS_RawEventType::WindowClose });
                break;
            }
        }
        data->pressed = -1;
    }
    else if (button == WL_BUTTON_RIGHT && down && at < 0)
    {
        lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 4, nullptr, version, 0, lx_wl.seat, serial, (i32)lx_wl.decoration_x, (i32)lx_wl.decoration_y); // show_window_menu
    }
    lx_wl.wl.wl_display_flush(lx_wl.display);
}

// A configure's size and state, applied with its acknowledgement
static void wl_apply_configure (OS_Window* window)
{
    WaylandWindow* data = wl_data(window);
    if (data->pending_width > 0 && data->pending_height > 0)
    {
        data->logical_width  = data->pending_width;
        data->logical_height = data->pending_height;
    }
    data->maximized  = data->pending_maximized;
    data->configured = true;
    wl_apply_size(window);
}

static void wl_xdg_surface_configure (void* user, WlProxy* xdg_surface, u32 serial)
{
    OS_Window* window = (OS_Window*)user;
    lx_wl.wl.wl_proxy_marshal_flags(xdg_surface, 4, nullptr, 1, 0, serial); // ack_configure
    wl_apply_configure(window);
    lx_wl.wl.wl_proxy_marshal_flags(wl_data(window)->surface, 6, nullptr, lx_wl.wl.wl_proxy_get_version(wl_data(window)->surface), 0); // commit
}

static const void* const wl_xdg_surface_listener[] = { (void*)wl_xdg_surface_configure };

// The size is the window geometry's, which takes in a title bar of ours
static void wl_toplevel_configure (void* user, WlProxy*, i32 width, i32 height, const WlArray* states)
{
    WaylandWindow* data     = wl_data((OS_Window*)user);
    const i32      title    = data->decorated ? WL_TITLE_HEIGHT : 0;
    data->pending_width     = width > 0 ? (u32)width : 0;
    data->pending_height    = height > title ? (u32)(height - title) : 0;
    data->pending_maximized = false;
    data->activated         = false;
    const u32* state        = (const u32*)states->data;
    for (usize i = 0; i < states->size / sizeof(u32); i++)
    {
        data->pending_maximized = data->pending_maximized || state[i] == WL_XDG_STATE_MAXIMIZED;
        data->activated         = data->activated || state[i] == WL_XDG_STATE_ACTIVATED;
    }
}

static void wl_toplevel_close (void* user, WlProxy*)
{
    os_queue_event((OS_Window*)user, { .type = OS_RawEventType::WindowClose });
}

static const void* const wl_toplevel_listener[] = { (void*)wl_toplevel_configure, (void*)wl_toplevel_close };

static b8 wl_is_configured (OS_Window* window)
{
    return wl_data(window)->configured;
}

static void wl_dispatch_for (b8 (*done)(OS_Window* window), OS_Window* window, u64 timeout_ns);

// The window's role: a surface without one is never shown. Any buffer the app's presents left on
// the surface goes first, as an xdg_surface may not start with one
static void wl_create_role (OS_Window* window)
{
    WaylandWindow* data = wl_data(window);
    const u32      version = lx_wl.wl.wl_proxy_get_version(data->surface);
    lx_wl.wl.wl_proxy_marshal_flags(data->surface, 1, nullptr, version, 0, nullptr, 0, 0); // attach null
    lx_wl.wl.wl_proxy_marshal_flags(data->surface, 6, nullptr, version, 0);                 // commit
    data->configured = false;
    {
        data->xdg_surface = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.wm_base, 2, &wl_xdg_surface, 1, 0, nullptr, data->surface);
        wl_listen(data->xdg_surface, wl_xdg_surface_listener, window);
        data->toplevel = lx_wl.wl.wl_proxy_marshal_flags(data->xdg_surface, 1, &wl_xdg_toplevel, 1, 0, nullptr);
        wl_listen(data->toplevel, wl_toplevel_listener, window);
        lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 2, nullptr, 1, 0, data->title);
        lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 3, nullptr, 1, 0, "puppetxr");
        if (lx_wl.decoration_manager)
        {
            data->decoration = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.decoration_manager, 1, &wl_xdg_toplevel_decoration, 1, 0, nullptr, data->toplevel);
            lx_wl.wl.wl_proxy_marshal_flags(data->decoration, 1, nullptr, 1, 0, WL_DECORATION_SERVER_SIDE);
        }
        else
        {
            wl_create_decorations(window);
        }
        if (data->maximize_when_shown)
        {
            lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 9, nullptr, 1, 0);
        }
        lx_wl.wl.wl_proxy_marshal_flags(data->surface, 6, nullptr, version, 0);
    }
    wl_dispatch_for(wl_is_configured, window, 1000000000ull);
    data->maximize_when_shown = false;
}

static void wl_destroy_role (OS_Window* window)
{
    WaylandWindow* data = wl_data(window);
    wl_destroy_decorations(window);
    wl_destroy(data->decoration, 0);
    wl_destroy(data->toplevel, 0);
    wl_destroy(data->xdg_surface, 0);
    const u32 version = lx_wl.wl.wl_proxy_get_version(data->surface);
    lx_wl.wl.wl_proxy_marshal_flags(data->surface, 1, nullptr, version, 0, nullptr, 0, 0);
    lx_wl.wl.wl_proxy_marshal_flags(data->surface, 6, nullptr, version, 0);
    data->configured = false;
    lx_wl.wl.wl_display_flush(lx_wl.display);
}

static b8 wl_create_window (OS_Window* window, const OS_WindowDesc& desc)
{
    WaylandWindow* data  = (WaylandWindow*)os_memory_alloc(sizeof(WaylandWindow));
    window->data         = data;
    data->logical_width  = desc.width ? desc.width : 1;
    data->logical_height = desc.height ? desc.height : 1;
    data->buffer_scale   = 1;
    data->output         = -1;
    cstr_copy(data->title, sizeof(data->title), desc.title);
    data->surface = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.compositor, 0, lx_wl.wl.wl_surface_interface, lx_wl.compositor_version, 0, nullptr);
    wl_listen(data->surface, wl_surface_listener, window);
    if (lx_wl.fractional_manager && lx_wl.viewporter)
    {
        data->fractional = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.fractional_manager, 1, &wl_fractional_scale, 1, 0, nullptr, data->surface);
        wl_listen(data->fractional, wl_fractional_listener, window);
        data->viewport = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.viewporter, 1, &wl_viewport, 1, 0, nullptr, data->surface);
    }
    window->width   = data->logical_width;
    window->height  = data->logical_height;
    window->density = 1.0f;
    window->hidden  = true;
    if (!desc.hidden)
    {
        wl_create_role(window);
        window->hidden = false;
    }
    wl_apply_size(window);
    lx_wl.wl.wl_display_flush(lx_wl.display);
    return true;
}

static void wl_destroy_window (OS_Window* window)
{
    WaylandWindow* data = wl_data(window);
    if (lx_wl.pointer_focus == window)
    {
        lx_wl.pointer_focus = nullptr;
    }
    if (lx_wl.keyboard_focus == window)
    {
        lx_wl.keyboard_focus = nullptr;
        lx_wl.repeat_key     = 0;
    }
    if (lx_wl.text_input_focus == window || lx_wl.text_input_window == window)
    {
        lx_wl.text_input_focus  = lx_wl.text_input_focus == window ? nullptr : lx_wl.text_input_focus;
        lx_wl.text_input_window = lx_wl.text_input_window == window ? nullptr : lx_wl.text_input_window;
        wl_text_input_update();
    }
    wl_destroy(data->locked, 0);
    wl_destroy(data->relative, 0);
    wl_destroy_decorations(window);
    wl_destroy(data->decoration, 0);
    wl_destroy(data->toplevel, 0);
    wl_destroy(data->xdg_surface, 0);
    wl_destroy(data->viewport, 0);
    wl_destroy(data->fractional, 0);
    wl_destroy(data->surface, 0);
    lx_wl.wl.wl_display_roundtrip(lx_wl.display);
    os_memory_release(data);
    window->data = nullptr;
}

static b8 wl_native_handles (OS_Window* window, OS_WindowNative& out_native)
{
    out_native.window  = (u64)(uintptr_t)wl_data(window)->surface;
    out_native.display = lx_wl.display;
    out_native.wayland = true;
    return true;
}

static f32 wl_refresh_rate (OS_Window* window)
{
    const WaylandWindow* data = wl_data(window);
    if (data->output >= 0)
    {
        return lx_wl.outputs[data->output].refresh;
    }
    return lx_wl.outputs_count > 0 ? lx_wl.outputs[0].refresh : 0.0f;
}

static const char* const* wl_vulkan_extensions (u32& out_count)
{
    out_count = ARRAY_SIZE(lx_wayland_vulkan_extensions);
    return lx_wayland_vulkan_extensions;
}

static b8 wl_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface)
{
    const VkGetInstanceProcAddrFn get    = vulkan_get_proc(vk_get_instance_proc_addr);
    const VkCreateSurfaceFn       create = get ? (VkCreateSurfaceFn)get(vk_instance, "vkCreateWaylandSurfaceKHR") : nullptr;
    if (!create)
    {
        LOG_ERROR("The Vulkan instance has no vkCreateWaylandSurfaceKHR");
        return false;
    }
    const VkWaylandSurfaceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .pNext = nullptr, .flags = 0, .display = lx_wl.display, .surface = wl_data(window)->surface };
    const i32                        result = create(vk_instance, &info, nullptr, &out_surface);
    if (result != 0)
    {
        LOG_ERROR("vkCreateWaylandSurfaceKHR failed: %d", result);
        return false;
    }
    return true;
}

// Locked in place, the pointer gives relative motion; Wayland cannot move the cursor, so it stays
// where looking began
static void wl_set_relative_mouse (OS_Window* window, b8 relative)
{
    WaylandWindow* data = wl_data(window);
    if (relative && lx_wl.pointer && lx_wl.relative_manager && lx_wl.constraints)
    {
        data->relative = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.relative_manager, 1, &wl_relative_pointer, 1, 0, nullptr, lx_wl.pointer);
        wl_listen(data->relative, wl_relative_pointer_listener, window);
        data->locked = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.constraints, 1, &wl_locked_pointer, 1, 0, nullptr, data->surface, lx_wl.pointer, nullptr, WL_LOCK_PERSISTENT);
        lx_wl.wl.wl_proxy_marshal_flags(data->locked, 1, nullptr, 1, 0, wl_to_fixed(lx_wl.pointer_x), wl_to_fixed(lx_wl.pointer_y));
    }
    else if (!relative)
    {
        wl_destroy(data->locked, 0);
        wl_destroy(data->relative, 0);
    }
    if (lx_wl.pointer_focus == window)
    {
        wl_pointer_set_cursor(relative ? OS_Cursor::Hidden : window->cursor);
    }
    lx_wl.wl.wl_display_flush(lx_wl.display);
}

static b8 wl_is_maximized (OS_Window* window)
{
    return wl_data(window)->maximized;
}

static b8 wl_maximize (OS_Window* window)
{
    WaylandWindow* data = wl_data(window);
    if (!data->toplevel)
    {
        data->maximize_when_shown = true;
        return true;
    }
    lx_wl.wl.wl_proxy_marshal_flags(data->toplevel, 9, nullptr, 1, 0);
    wl_dispatch_for(wl_is_maximized, window, 200000000ull);
    return true;
}

static b8 wl_maximized (OS_Window* window)
{
    const WaylandWindow* data = wl_data(window);
    return data->maximize_when_shown || data->maximized;
}

static void wl_show (OS_Window* window)
{
    const WaylandWindow* data = wl_data(window);
    if (!data->toplevel)
    {
        wl_create_role(window);
    }
}

static void wl_hide (OS_Window* window)
{
    wl_destroy_role(window);
}

// wp_cursor_shape_device_v1's shapes, in OS_Cursor's order
static const u32 wl_cursor_shapes[] = { 1, 9, 13, 27, 26, 28, 29, 4, 6, 5, 15 };
static const char* const wl_cursor_names[] = { "default", "text", "move", "ns-resize", "ew-resize", "nesw-resize", "nwse-resize", "pointer", "wait", "progress", "not-allowed" };

// A wp_cursor_shape_device_v1 shape, else the cursor theme's image of that name; 0 hides it
static void wl_pointer_set_shape (u32 shape, const char* theme_name)
{
    if (!lx_wl.pointer)
    {
        return;
    }
    if (shape == 0)
    {
        lx_wl.wl.wl_proxy_marshal_flags(lx_wl.pointer, 0, nullptr, lx_wl.wl.wl_proxy_get_version(lx_wl.pointer), 0, lx_wl.pointer_serial, nullptr, 0, 0);
    }
    else if (lx_wl.cursor_shape_device)
    {
        lx_wl.wl.wl_proxy_marshal_flags(lx_wl.cursor_shape_device, 1, nullptr, 1, 0, lx_wl.pointer_serial, shape);
    }
    else if (lx_wl.cursor_loaded && lx_wl.shm && lx_wl.cursor_surface)
    {
        if (!lx_wl.cursor_theme)
        {
            const char* size   = os_env("XCURSOR_SIZE");
            const long  pixels = size ? strtol(size, nullptr, 10) : 0;
            lx_wl.cursor_theme = lx_wl.wlc.wl_cursor_theme_load(os_env("XCURSOR_THEME"), pixels > 0 && pixels < 512 ? (i32)pixels : 24, lx_wl.shm);
        }
        // struct wl_cursor { unsigned int image_count; struct wl_cursor_image** images; char* name; }
        // struct wl_cursor_image { uint32_t width, height, hotspot_x, hotspot_y, delay; }
        struct CursorImage
        {
            u32 width;
            u32 height;
            u32 hotspot_x;
            u32 hotspot_y;
            u32 delay;
        };
        struct ThemeCursor
        {
            u32           image_count;
            CursorImage** images;
            char*         name;
        };
        const ThemeCursor* cursor = lx_wl.cursor_theme ? (const ThemeCursor*)lx_wl.wlc.wl_cursor_theme_get_cursor(lx_wl.cursor_theme, theme_name) : nullptr;
        if (!cursor || cursor->image_count == 0)
        {
            return;
        }
        CursorImage* const image   = cursor->images[0];
        const WlProxy* const buffer = lx_wl.wlc.wl_cursor_image_get_buffer(image);
        const u32          version = lx_wl.wl.wl_proxy_get_version(lx_wl.cursor_surface);
        lx_wl.wl.wl_proxy_marshal_flags(lx_wl.pointer, 0, nullptr, lx_wl.wl.wl_proxy_get_version(lx_wl.pointer), 0, lx_wl.pointer_serial, lx_wl.cursor_surface, (i32)image->hotspot_x, (i32)image->hotspot_y);
        lx_wl.wl.wl_proxy_marshal_flags(lx_wl.cursor_surface, 1, nullptr, version, 0, buffer, 0, 0);
        lx_wl.wl.wl_proxy_marshal_flags(lx_wl.cursor_surface, 2, nullptr, version, 0, 0, 0, (i32)image->width, (i32)image->height);
        lx_wl.wl.wl_proxy_marshal_flags(lx_wl.cursor_surface, 6, nullptr, version, 0);
    }
    lx_wl.wl.wl_display_flush(lx_wl.display);
}

static void wl_pointer_set_cursor (OS_Cursor shape)
{
    if (shape == OS_Cursor::Hidden)
    {
        wl_pointer_set_shape(0, nullptr);
        return;
    }
    wl_pointer_set_shape(wl_cursor_shapes[(u32)shape], wl_cursor_names[(u32)shape]);
}

//
// Wayland: input methods (zwp_text_input_v3). The compositor routes the keys of a text field
// through the input method, which shows its candidates itself and commits the composed text; the
// preedit string is not drawn, as ImGui has no place for it
//

// Enabled while the window the text input entered wants text; the state applies on commit
static void wl_text_input_update ()
{
    WlProxy* const input = lx_wl.text_input;
    if (!input)
    {
        return;
    }
    const b8 want = lx_wl.text_input_window && lx_wl.text_input_window == lx_wl.text_input_focus;
    if (want)
    {
        if (!lx_wl.text_input_enabled)
        {
            lx_wl.wl.wl_proxy_marshal_flags(input, 1, nullptr, 1, 0);       // enable
            lx_wl.wl.wl_proxy_marshal_flags(input, 5, nullptr, 1, 0, 0, 0); // content type: no hints, normal purpose
        }
        lx_wl.wl.wl_proxy_marshal_flags(input, 6, nullptr, 1, 0, (i32)lx_wl.text_input_x, (i32)lx_wl.text_input_y, 1, (i32)(lx_wl.text_input_height > 1.0f ? lx_wl.text_input_height : 1.0f));
        lx_wl.wl.wl_proxy_marshal_flags(input, 7, nullptr, 1, 0); // commit
    }
    else if (lx_wl.text_input_enabled)
    {
        lx_wl.wl.wl_proxy_marshal_flags(input, 2, nullptr, 1, 0); // disable
        lx_wl.wl.wl_proxy_marshal_flags(input, 7, nullptr, 1, 0);
    }
    lx_wl.text_input_enabled = want;
    lx_wl.wl.wl_display_flush(lx_wl.display);
}

static void wl_text_input_enter (void*, WlProxy*, const WlProxy* surface)
{
    lx_wl.text_input_focus   = wl_window_of_surface(surface);
    lx_wl.text_input_enabled = false;
    wl_text_input_update();
}

static void wl_text_input_leave (void*, WlProxy*, const WlProxy*)
{
    lx_wl.text_input_focus = nullptr;
    wl_text_input_update();
}

static void wl_text_input_preedit (void*, WlProxy*, const char*, i32, i32)
{
}

static void wl_text_input_commit (void*, WlProxy*, const char* text)
{
    os_memory_release(lx_wl.text_commit);
    lx_wl.text_commit = nullptr;
    const usize length = cstr_length(text);
    if (length > 0 && length < LX_MAX_TEXT)
    {
        lx_wl.text_commit = (char*)os_memory_alloc(length + 1);
        memory_copy(lx_wl.text_commit, text, length + 1);
    }
}

static void wl_text_input_delete (void*, WlProxy*, u32, u32)
{
}

// The committed text goes out in text events, split between whole UTF-8 characters
static void wl_text_input_done (void*, WlProxy*, u32)
{
    const char* text = lx_wl.text_commit;
    while (text && *text && lx_wl.text_input_focus)
    {
        usize length = cstr_length(text);
        length       = length < OS_EVENT_TEXT_LENGTH - 1 ? length : OS_EVENT_TEXT_LENGTH - 1;
        while (length > 0 && ((u8)text[length] & 0xC0) == 0x80)
        {
            length--;
        }
        if (length == 0)
        {
            break;
        }
        OS_RawEvent event = { .type = OS_RawEventType::Text };
        memory_copy(event.text, text, length);
        if ((u8)event.text[0] >= 0x20 && event.text[0] != 0x7F)
        {
            os_queue_event(lx_wl.text_input_focus, event);
        }
        text += length;
    }
    os_memory_release(lx_wl.text_commit);
    lx_wl.text_commit = nullptr;
}

namespace
{
const void* const wl_text_input_listener[6] = {
    (void*)wl_text_input_enter, (void*)wl_text_input_leave, (void*)wl_text_input_preedit, (void*)wl_text_input_commit, (void*)wl_text_input_delete, (void*)wl_text_input_done,
};
} // namespace

static void wl_text_input_state (OS_Window* window, b8 active, f32 x, f32 y, f32 height)
{
    if (active)
    {
        lx_wl.text_input_window = window;
        lx_wl.text_input_x      = x;
        lx_wl.text_input_y      = y;
        lx_wl.text_input_height = height;
    }
    else if (lx_wl.text_input_window == window)
    {
        lx_wl.text_input_window = nullptr;
    }
    wl_text_input_update();
}

static void wl_set_cursor (OS_Window* window, OS_Cursor cursor) // NOLINT(misc-const-correctness): the signature of every driver
{
    if (lx_wl.pointer_focus == window && !window->relative)
    {
        wl_pointer_set_cursor(cursor);
    }
}

static void wl_source_target (void*, WlProxy*, const char*)
{
}

static void wl_source_send (void*, WlProxy*, const char*, i32 fd)
{
    const char* text   = lx_wl.data_source_text ? lx_wl.data_source_text : "";
    usize       length = cstr_length(text);
    while (length > 0)
    {
        const ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            break;
        }
        text += written;
        length -= (usize)written;
    }
    close(fd);
}

static void wl_source_cancelled (void*, WlProxy* source)
{
    if (source == lx_wl.data_source)
    {
        lx_wl.data_source = nullptr;
        os_memory_release(lx_wl.data_source_text);
        lx_wl.data_source_text = nullptr;
    }
    lx_wl.wl.wl_proxy_marshal_flags(source, 1, nullptr, lx_wl.wl.wl_proxy_get_version(source), WL_MARSHAL_DESTROY);
}

static void wl_source_nothing (void*, WlProxy*)
{
}

static void wl_source_action (void*, WlProxy*, u32)
{
}

static const void* const wl_data_source_listener[] = {
    (void*)wl_source_target, (void*)wl_source_send, (void*)wl_source_cancelled, (void*)wl_source_nothing, (void*)wl_source_nothing, (void*)wl_source_action,
};

static void wl_set_clipboard_text (OS_Window* window, const char* text) // NOLINT(misc-const-correctness): the signature of every driver
{
    (void)window;
    const usize length = cstr_length(text);
    if (!lx_wl.data_device || length >= LX_MAX_TEXT)
    {
        return;
    }
    os_memory_release(lx_wl.data_source_text);
    lx_wl.data_source_text = (char*)os_memory_alloc(length + 1);
    memory_copy(lx_wl.data_source_text, text, length + 1);
    WlProxy* const source = lx_wl.wl.wl_proxy_marshal_flags(lx_wl.data_device_manager, 0, lx_wl.wl.wl_data_source_interface, lx_wl.wl.wl_proxy_get_version(lx_wl.data_device_manager), 0, nullptr);
    wl_listen(source, wl_data_source_listener, nullptr);
    static const char* const mimes[] = { WL_TEXT_MIME, "text/plain", "UTF8_STRING", "TEXT", "STRING" };
    for (const char* mime : mimes)
    {
        lx_wl.wl.wl_proxy_marshal_flags(source, 0, nullptr, lx_wl.wl.wl_proxy_get_version(source), 0, mime);
    }
    lx_wl.wl.wl_proxy_marshal_flags(lx_wl.data_device, 1, nullptr, lx_wl.wl.wl_proxy_get_version(lx_wl.data_device), 0, source, lx_wl.input_serial);
    lx_wl.data_source = source;
    lx_wl.wl.wl_display_flush(lx_wl.display);
}

static b8 wl_clipboard_text (OS_Window* window, char* out_text, usize size) // NOLINT(misc-const-correctness): the signature of every driver
{
    (void)window;
    if (lx_wl.data_source)
    {
        return lx_wl.data_source_text && cstr_copy(out_text, size, lx_wl.data_source_text);
    }
    if (!lx_wl.selection_offer)
    {
        return false;
    }
    i32 fds[2] = { -1, -1 };
    if (pipe(fds) != 0)
    {
        return false;
    }
    lx_wl.wl.wl_proxy_marshal_flags(lx_wl.selection_offer, 1, nullptr, lx_wl.wl.wl_proxy_get_version(lx_wl.selection_offer), 0, WL_TEXT_MIME, fds[1]);
    close(fds[1]);
    lx_wl.wl.wl_display_flush(lx_wl.display);
    usize     used     = 0;
    const u64 deadline = os_monotonic_ns() + 200000000ull;
    b8        ended    = false;
    while (!ended && os_monotonic_ns() < deadline)
    {
        pollfd entry = { .fd = fds[0], .events = POLLIN, .revents = 0 };
        if (poll(&entry, 1, 10) <= 0)
        {
            continue;
        }
        char          buffer[4096];
        const ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count <= 0)
        {
            ended = count == 0 || errno != EINTR;
            continue;
        }
        const usize take = used + (usize)count < size ? (usize)count : (size > used + 1 ? size - used - 1 : 0);
        memory_copy(out_text + used, buffer, take);
        used += take;
    }
    close(fds[0]);
    out_text[used < size ? used : size - 1] = 0;
    return ended && used + 1 < size;
}

static b8 wl_key_label (OS_Key key, char* out, usize size)
{
    const u32 code = evdev_from_key(key);
    if (!code || !printable_key(key) || !lx_wl.keymap)
    {
        return false;
    }
    const xkb_keysym_t* syms  = nullptr;
    const i32           count = lx_wl.xkb.xkb_keymap_key_get_syms_by_level(lx_wl.keymap, code + 8, 0, 0, &syms);
    return count > 0 && label_from_keysym(syms[0], out, size);
}

// Reads what the socket has, then dispatches it, without blocking
static void wl_read_events (u64 timeout_ns)
{
    WlDisplay* const display = lx_wl.display;
    while (lx_wl.wl.wl_display_prepare_read(display) != 0)
    {
        lx_wl.wl.wl_display_dispatch_pending(display);
    }
    lx_wl.wl.wl_display_flush(display);
    pollfd entries[2] = {
        { .fd = lx_wl.wl.wl_display_get_fd(display), .events = POLLIN, .revents = 0 },
        { .fd = timeout_ns > 0 ? (int)os_sys_wake_handle() : -1, .events = POLLIN, .revents = 0 },
    };
    if (poll(entries, 2, (int)(timeout_ns / 1000000ull)) > 0 && (entries[0].revents & POLLIN))
    {
        lx_wl.wl.wl_display_read_events(display);
    }
    else
    {
        lx_wl.wl.wl_display_cancel_read(display);
    }
    lx_wl.wl.wl_display_dispatch_pending(display);
}

static void wl_dispatch_for (b8 (*done)(OS_Window* window), OS_Window* window, u64 timeout_ns)
{
    const u64 deadline = os_monotonic_ns() + timeout_ns;
    while (!done(window) && os_monotonic_ns() < deadline && lx_wl.wl.wl_display_get_error(lx_wl.display) == 0)
    {
        wl_read_events(0);
        if (!done(window))
        {
            pollfd entry = { .fd = lx_wl.wl.wl_display_get_fd(lx_wl.display), .events = POLLIN, .revents = 0 };
            poll(&entry, 1, 5);
        }
    }
}

static void wl_pump ()
{
    wl_read_events(0);
    wl_repeat_keys();
}

static void wl_wait (u64 timeout_ns)
{
    if (lx_wl.repeat_key && lx_wl.keyboard_focus)
    {
        const u64 now  = os_monotonic_ns();
        const u64 wait = lx_wl.repeat_next_ns > now ? lx_wl.repeat_next_ns - now : 0;
        timeout_ns     = wait < timeout_ns ? wait : timeout_ns;
    }
    wl_read_events(timeout_ns > 1000000ull ? timeout_ns : 1000000ull);
    os_sys_wake_clear();
}

const OS_VideoDriver os_wayland_driver = {
    .name                  = "wayland",
    .init                  = wl_init,
    .quit                  = wl_quit,
    .create_window         = wl_create_window,
    .destroy_window        = wl_destroy_window,
    .native_handles        = wl_native_handles,
    .refresh_rate          = wl_refresh_rate,
    .vulkan_extensions     = wl_vulkan_extensions,
    .create_vulkan_surface = wl_create_vulkan_surface,
    .set_relative_mouse    = wl_set_relative_mouse,
    .maximize              = wl_maximize,
    .maximized             = wl_maximized,
    .show                  = wl_show,
    .hide                  = wl_hide,
    .set_cursor            = wl_set_cursor,
    .text_input            = wl_text_input_state,
    .clipboard_text        = wl_clipboard_text,
    .set_clipboard_text    = wl_set_clipboard_text,
    .key_label             = wl_key_label,
    .pump                  = wl_pump,
    .wait                  = wl_wait,
};

#else

static b8 no_display ()
{
    return false;
}

const OS_VideoDriver os_wayland_driver = { .name = "wayland", .init = no_display };
const OS_VideoDriver os_x11_driver     = { .name = "x11", .init = no_display };

#endif
