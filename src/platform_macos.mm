#include "platform_internal.h"

#include <pthread.h>
#include <time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#if !PUPPETXR_HEADLESS_ONLY
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/CAMetalLayer.h>
#endif

//
// Types
//

// Calls of other threads for the main thread (os_run_on_window_thread, os_post_to_window_thread).
// Created once and kept for the process: a dispatch wakeup may run after os_shutdown
#define OS_MAX_MAIN_CALLS 64

namespace
{

enum class MainCallState : u8
{
    Free,
    Queued,
    Running,
    Done,
};

struct MainCall
{
    OS_WindowThreadFunction* function;
    void*                    user;
    MainCallState            state;
    b8                       posted;
    u64                      order;
};

struct MainQueue
{
    b8              created;
    pthread_mutex_t mutex;
    pthread_cond_t  done;
    OS_Semaphore*   wake;          // the main thread waiting for a mutex sleeps on it
    OS_Atomic32     main_waiting;
    OS_Atomic32     dispatched;    // a wakeup is on the main dispatch queue
    MainCall        calls[OS_MAX_MAIN_CALLS];
    u64             next_order;
    b8              stalled;       // a call was withdrawn; others fail until the main thread serves
    b8              stall_logged;
    b8              full_logged;
    b8              serving;
};

} // namespace

//
// Statics
//

static MainQueue os_main = {};

// Absolute time for pthread_cond_timedwait, whose clock is the realtime one on macOS
static timespec main_deadline (u64 ns)
{
    timespec now = {};
    clock_gettime(CLOCK_REALTIME, &now);
    const u64 total = (u64)now.tv_nsec + ns % 1000000000ull;
    timespec  out   = {};
    out.tv_sec      = now.tv_sec + (time_t)(ns / 1000000000ull) + (time_t)(total / 1000000000ull);
    out.tv_nsec     = (long)(total % 1000000000ull);
    return out;
}

//
// Main thread calls
//

b8 os_mac_on_main_thread ()
{
    return pthread_main_np() != 0;
}

void os_mac_queue_create ()
{
    if (os_main.created)
    {
        return;
    }
    pthread_mutex_init(&os_main.mutex, nullptr);
    pthread_cond_init(&os_main.done, nullptr);
    os_main.wake    = os_semaphore_create(0);
    os_main.created = true;
}

// Runs every queued call, oldest first; a call that waits for a mutex serves no others
static void serve_main_calls ()
{
    if (!os_main.created || os_main.serving || !os_mac_on_main_thread())
    {
        return;
    }
    os_main.serving = true;
    pthread_mutex_lock(&os_main.mutex);
    os_main.stalled      = false;
    os_main.stall_logged = false;
    for (;;)
    {
        MainCall* next = nullptr;
        for (MainCall& call : os_main.calls)
        {
            if (call.state == MainCallState::Queued && (!next || call.order < next->order))
            {
                next = &call;
            }
        }
        if (!next)
        {
            break;
        }
        next->state = MainCallState::Running;
        pthread_mutex_unlock(&os_main.mutex);
        next->function(next->user);
        pthread_mutex_lock(&os_main.mutex);
        next->state = next->posted ? MainCallState::Free : MainCallState::Done;
        pthread_cond_broadcast(&os_main.done);
    }
    pthread_mutex_unlock(&os_main.mutex);
    os_main.serving = false;
}

static void serve_from_dispatch (void* user)
{
    (void)user;
    os_atomic_store(&os_main.dispatched, 0);
    serve_main_calls();
}

// The main thread serves either in its run loop, which the dispatch wakes, or in a mutex wait
static void wake_main_thread ()
{
    os_semaphore_signal(os_main.wake);
    if (os_atomic_compare_swap(&os_main.dispatched, 0, 1))
    {
        dispatch_async_f(dispatch_get_main_queue(), nullptr, serve_from_dispatch);
    }
}

// Under the queue's mutex
static MainCall* queue_main_call (OS_WindowThreadFunction* function, void* user, b8 posted)
{
    for (MainCall& call : os_main.calls)
    {
        if (call.state == MainCallState::Free)
        {
            call = { .function = function, .user = user, .state = MainCallState::Queued, .posted = posted, .order = os_main.next_order++ };
            return &call;
        }
    }
    if (!os_main.full_logged)
    {
        os_main.full_logged = true;
        LOG_WARNING("%u calls wait for the main thread; dropping more", OS_MAX_MAIN_CALLS);
    }
    return nullptr;
}

b8 os_mac_run_on_main (OS_WindowThreadFunction* function, void* user, u32 timeout_ms)
{
    if (!os_main.created)
    {
        return false;
    }
    pthread_mutex_lock(&os_main.mutex);
    MainCall* call = os_main.stalled ? nullptr : queue_main_call(function, user, false);
    if (!call)
    {
        pthread_mutex_unlock(&os_main.mutex);
        return false;
    }
    wake_main_thread();
    PROF_ZONE("window thread call wait");
    const u64 deadline = os_monotonic_ns() + (u64)timeout_ms * 1000000ull;
    while (call->state != MainCallState::Done)
    {
        const u64 now = os_monotonic_ns();
        if (call->state == MainCallState::Queued && now >= deadline)
        {
            call->state     = MainCallState::Free;
            os_main.stalled = true;
            if (!os_main.stall_logged)
            {
                os_main.stall_logged = true;
                LOG_WARNING(OS_WITHDRAWN_CALL_LOG " within %u ms; window calls fail until it comes by (the app blocks it on the thread that calls OpenXR)", timeout_ms);
            }
            pthread_mutex_unlock(&os_main.mutex);
            return false;
        }
        // A running call is waited out: it cannot be withdrawn
        const u64      remaining = call->state == MainCallState::Queued ? deadline - now : 100000000ull;
        const timespec until     = main_deadline(remaining + 1000000ull);
        pthread_cond_timedwait(&os_main.done, &os_main.mutex, &until);
    }
    call->state = MainCallState::Free;
    pthread_mutex_unlock(&os_main.mutex);
    return true;
}

void os_mac_post_to_main (OS_WindowThreadFunction* function, void* user)
{
    if (!os_main.created)
    {
        return;
    }
    pthread_mutex_lock(&os_main.mutex);
    const MainCall* call = queue_main_call(function, user, true);
    pthread_mutex_unlock(&os_main.mutex);
    if (call)
    {
        wake_main_thread();
    }
}

void os_mac_serve (u64 ns)
{
    serve_main_calls();
    if (ns > 0)
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, (f64)ns * 1e-9, false);
        serve_main_calls();
    }
}

void os_mac_lock_on_main (OS_Mutex* mutex)
{
    os_atomic_add(&os_main.main_waiting, 1);
    for (;;)
    {
        serve_main_calls();
        if (os_sys_mutex_try_lock(mutex))
        {
            break;
        }
        os_sys_semaphore_wait_ns(os_main.wake, 5000000ull);
    }
    os_atomic_add(&os_main.main_waiting, -1);
}

void os_mac_unlocked ()
{
    if (os_main.created && os_atomic_load(&os_main.main_waiting) > 0)
    {
        os_semaphore_signal(os_main.wake);
    }
}

#if !PUPPETXR_HEADLESS_ONLY

//
// Cocoa: windows on the main thread
//
// The app may own NSApp and dispatch the events itself (Unity's editor, Godot); a local monitor,
// which fires inside -[NSApplication sendEvent:] whoever calls it, takes the keys and moves of
// our windows, which NSApp routes to the key window alone. Buttons, the wheel and text reach the
// view as usual.

@interface PUPPETXRView : NSView <NSTextInputClient>
{
@public
    OS_Window* os_window;
    NSRect     text_rect; // where an input method shows its candidates, in view coordinates
}
@end

@interface PUPPETXRWindow : NSWindow
@end

@interface PUPPETXRWindowDelegate : NSObject <NSWindowDelegate>
{
@public
    OS_Window* os_window;
}
@end

namespace
{

struct CocoaWindow
{
    PUPPETXRWindow*         window;
    PUPPETXRView*           view;
    PUPPETXRWindowDelegate* delegate;
    CAMetalLayer*           layer;
    b8                      maximize_when_shown;
    b8                      last_valid;
    f32                     last_x;  // points
    f32                     last_y;
    NSPoint                 saved_cursor; // screen point where the relative mode began
};

struct CocoaState
{
    id        monitor;
    b8        iso_keyboard;
    NSCursor* hidden_cursor;
};

struct VkMetalSurfaceCreateInfo
{
    i32         sType; // VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT
    const void* pNext;
    u32         flags;
    const void* pLayer;
};

typedef void* (*VkGetInstanceProcAddrFn)(void* instance, const char* name);
typedef i32   (*VkCreateSurfaceFn)(void* instance, const void* info, const void* allocator, u64* surface);

} // namespace

static CocoaState os_cocoa = {};

static const char* const cocoa_vulkan_extensions_list[] = { "VK_KHR_surface", "VK_EXT_metal_surface" };

static CocoaWindow* cocoa_data (OS_Window* window)
{
    return (CocoaWindow*)window->data;
}

// Virtual key codes (Carbon's kVK_*) to keys
static OS_Key cocoa_key (u16 code)
{
    static const OS_Key keys[0x80] = {
        OS_Key::A, OS_Key::S, OS_Key::D, OS_Key::F, OS_Key::H, OS_Key::G, OS_Key::Z, OS_Key::X,                                 // 00
        OS_Key::C, OS_Key::V, OS_Key::NonUsBackslash, OS_Key::B, OS_Key::Q, OS_Key::W, OS_Key::E, OS_Key::R,                     // 08
        OS_Key::Y, OS_Key::T, OS_Key::Digit1, OS_Key::Digit2, OS_Key::Digit3, OS_Key::Digit4, OS_Key::Digit6, OS_Key::Digit5,    // 10
        OS_Key::Equals, OS_Key::Digit9, OS_Key::Digit7, OS_Key::Minus, OS_Key::Digit8, OS_Key::Digit0, OS_Key::RightBracket, OS_Key::O, // 18
        OS_Key::U, OS_Key::LeftBracket, OS_Key::I, OS_Key::P, OS_Key::Enter, OS_Key::L, OS_Key::J, OS_Key::Apostrophe,          // 20
        OS_Key::K, OS_Key::Semicolon, OS_Key::Backslash, OS_Key::Comma, OS_Key::Slash, OS_Key::N, OS_Key::M, OS_Key::Period,     // 28
        OS_Key::Tab, OS_Key::Space, OS_Key::Grave, OS_Key::Backspace, OS_Key::None, OS_Key::Escape, OS_Key::RightSuper, OS_Key::LeftSuper, // 30
        OS_Key::LeftShift, OS_Key::CapsLock, OS_Key::LeftAlt, OS_Key::LeftCtrl, OS_Key::RightShift, OS_Key::RightAlt, OS_Key::RightCtrl, OS_Key::None, // 38
        OS_Key::F17, OS_Key::KeypadDecimal, OS_Key::None, OS_Key::KeypadMultiply, OS_Key::None, OS_Key::KeypadPlus, OS_Key::None, OS_Key::NumLock, // 40
        OS_Key::None, OS_Key::None, OS_Key::None, OS_Key::KeypadDivide, OS_Key::KeypadEnter, OS_Key::None, OS_Key::KeypadMinus, OS_Key::F18, // 48
        OS_Key::F19, OS_Key::KeypadEquals, OS_Key::Keypad0, OS_Key::Keypad1, OS_Key::Keypad2, OS_Key::Keypad3, OS_Key::Keypad4, OS_Key::Keypad5, // 50
        OS_Key::Keypad6, OS_Key::Keypad7, OS_Key::F20, OS_Key::Keypad8, OS_Key::Keypad9, OS_Key::None, OS_Key::None, OS_Key::None, // 58
        OS_Key::F5, OS_Key::F6, OS_Key::F7, OS_Key::F3, OS_Key::F8, OS_Key::F9, OS_Key::None, OS_Key::F11,                       // 60
        OS_Key::None, OS_Key::F13, OS_Key::F16, OS_Key::F14, OS_Key::None, OS_Key::F10, OS_Key::Menu, OS_Key::F12,               // 68
        OS_Key::None, OS_Key::F15, OS_Key::Insert, OS_Key::Home, OS_Key::PageUp, OS_Key::Delete, OS_Key::F4, OS_Key::End,        // 70
        OS_Key::F2, OS_Key::PageDown, OS_Key::F1, OS_Key::ArrowLeft, OS_Key::ArrowRight, OS_Key::ArrowDown, OS_Key::ArrowUp, OS_Key::None, // 78
    };
    // ISO keyboards report the key left of 1 and the one right of left shift swapped
    if (os_cocoa.iso_keyboard && (code == kVK_ISO_Section || code == kVK_ANSI_Grave))
    {
        code = code == kVK_ISO_Section ? (u16)kVK_ANSI_Grave : (u16)kVK_ISO_Section;
    }
    return code < 0x80 ? keys[code] : OS_Key::None;
}

static u16 cocoa_code_of_key (OS_Key key)
{
    for (u16 code = 0; code < 0x80; code++)
    {
        if (cocoa_key(code) == key)
        {
            return code;
        }
    }
    return 0xFFFF;
}

static u8 cocoa_modifiers (NSEventModifierFlags flags)
{
    return (u8)(((flags & NSEventModifierFlagShift) ? OS_MODIFIER_SHIFT : 0) | ((flags & NSEventModifierFlagControl) ? OS_MODIFIER_CTRL : 0) | ((flags & NSEventModifierFlagOption) ? OS_MODIFIER_ALT : 0) |
                ((flags & NSEventModifierFlagCommand) ? OS_MODIFIER_SUPER : 0));
}

static OS_Window* cocoa_window_of (NSWindow* ns_window)
{
    for (u32 i = 0; i < OS_MAX_WINDOWS; i++)
    {
        OS_Window* window = os_window_at(i);
        if (window && window->driver == &os_cocoa_driver && window->data && cocoa_data(window)->window == ns_window)
        {
            return window;
        }
    }
    return nullptr;
}

static void cocoa_queue_size (OS_Window* window)
{
    CocoaWindow*  data    = cocoa_data(window);
    const NSSize  points  = data->view.bounds.size;
    const CGFloat density = data->window.backingScaleFactor;
    if (data->layer)
    {
        data->layer.contentsScale = density;
        data->layer.drawableSize  = NSMakeSize(points.width * density, points.height * density);
    }
    os_queue_resize(window, (u32)(points.width * density + 0.5), (u32)(points.height * density + 0.5), (f32)density);
}

static NSPoint cocoa_view_point (CocoaWindow* data, NSEvent* event)
{
    return [data->view convertPoint:event.locationInWindow fromView:nil];
}

// Modifier keys come as flag changes; the device-dependent bits (IOKit's NX_DEVICE*KEYMASK) tell
// the left key from the right
static void cocoa_flags_changed (OS_Window* window, NSEvent* event)
{
    static const struct
    {
        u16 code;
        u64 mask;
    } modifiers[] = {
        { kVK_Shift, 0x02 }, { kVK_RightShift, 0x04 }, { kVK_Control, 0x01 }, { kVK_RightControl, 0x2000 }, { kVK_Option, 0x20 },
        { kVK_RightOption, 0x40 }, { kVK_Command, 0x08 }, { kVK_RightCommand, 0x10 }, { kVK_CapsLock, NSEventModifierFlagCapsLock },
    };
    const u16 code = event.keyCode;
    for (const auto& modifier : modifiers)
    {
        if (modifier.code == code)
        {
            const b8 down = ((u64)event.modifierFlags & modifier.mask) != 0;
            os_queue_event(window, { .type = down ? OS_RawEventType::KeyDown : OS_RawEventType::KeyUp, .key = cocoa_key(code), .modifiers = cocoa_modifiers(event.modifierFlags) });
        }
    }
}

static NSEvent* cocoa_monitor_event (NSEvent* event)
{
    OS_Window* window = event.window ? cocoa_window_of(event.window) : nullptr;
    if (!window)
    {
        return event;
    }
    CocoaWindow* data = cocoa_data(window);
    switch (event.type)
    {
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp:
    {
        const OS_Key key = cocoa_key(event.keyCode);
        if (key != OS_Key::None)
        {
            os_queue_event(window, { .type = event.type == NSEventTypeKeyDown ? OS_RawEventType::KeyDown : OS_RawEventType::KeyUp, .key = key, .modifiers = cocoa_modifiers(event.modifierFlags), .repeat = event.type == NSEventTypeKeyDown && event.ARepeat });
        }
        break;
    }
    case NSEventTypeFlagsChanged:
        cocoa_flags_changed(window, event);
        break;
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
    {
        const NSPoint point = cocoa_view_point(data, event);
        const f32     x     = (f32)point.x;
        const f32     y     = (f32)point.y;
        const f32     dx    = window->relative ? (f32)event.deltaX : (data->last_valid ? x - data->last_x : 0.0f);
        const f32     dy    = window->relative ? (f32)event.deltaY : (data->last_valid ? y - data->last_y : 0.0f);
        if (!window->relative)
        {
            data->last_x     = x;
            data->last_y     = y;
            data->last_valid = true;
        }
        os_queue_event(window, { .type = OS_RawEventType::MouseMotion, .modifiers = cocoa_modifiers(event.modifierFlags), .x = data->last_x, .y = data->last_y, .dx = dx, .dy = dy });
        break;
    }
    default:
        break;
    }
    return event;
}

static void cocoa_button (PUPPETXRView* view, NSEvent* event, OS_Key key, b8 down)
{
    OS_Window* window = view->os_window;
    if (!window || key == OS_Key::None)
    {
        return;
    }
    const NSPoint point = [view convertPoint:event.locationInWindow fromView:nil];
    os_queue_event(window, { .type = down ? OS_RawEventType::MouseButtonDown : OS_RawEventType::MouseButtonUp, .key = key, .modifiers = cocoa_modifiers(event.modifierFlags), .x = (f32)point.x, .y = (f32)point.y });
}

static OS_Key cocoa_other_button (NSInteger number)
{
    switch (number)
    {
    case 2:
        return OS_Key::MouseMiddle;
    case 3:
        return OS_Key::MouseX1;
    case 4:
        return OS_Key::MouseX2;
    default:
        return OS_Key::None;
    }
}

static NSCursor* cocoa_cursor (OS_Cursor shape)
{
    switch (shape)
    {
    case OS_Cursor::TextInput:
        return [NSCursor IBeamCursor];
    case OS_Cursor::ResizeAll:
        return [NSCursor closedHandCursor];
    case OS_Cursor::ResizeNS:
        return [NSCursor resizeUpDownCursor];
    case OS_Cursor::ResizeEW:
        return [NSCursor resizeLeftRightCursor];
    case OS_Cursor::ResizeNESW:
        return [NSCursor respondsToSelector:@selector(_windowResizeNorthEastSouthWestCursor)] ? [NSCursor performSelector:@selector(_windowResizeNorthEastSouthWestCursor)] : [NSCursor closedHandCursor];
    case OS_Cursor::ResizeNWSE:
        return [NSCursor respondsToSelector:@selector(_windowResizeNorthWestSouthEastCursor)] ? [NSCursor performSelector:@selector(_windowResizeNorthWestSouthEastCursor)] : [NSCursor closedHandCursor];
    case OS_Cursor::Hand:
        return [NSCursor pointingHandCursor];
    case OS_Cursor::NotAllowed:
        return [NSCursor operationNotAllowedCursor];
    case OS_Cursor::Hidden:
        if (!os_cocoa.hidden_cursor)
        {
            NSImage* image         = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
            os_cocoa.hidden_cursor = [[NSCursor alloc] initWithImage:image hotSpot:NSZeroPoint];
            [image release];
        }
        return os_cocoa.hidden_cursor;
    default:
        return [NSCursor arrowCursor];
    }
}

@implementation PUPPETXRView

- (BOOL)acceptsFirstResponder
{
    return YES;
}

// The click that focuses the mirror window also clicks the panels, instead of only focusing
- (BOOL)acceptsFirstMouse:(NSEvent*)event
{
    (void)event;
    return YES;
}

- (BOOL)isFlipped
{
    return YES;
}

- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    if (os_window)
    {
        cocoa_queue_size(os_window);
    }
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    if (os_window)
    {
        cocoa_queue_size(os_window);
    }
}

- (void)resetCursorRects
{
    [super resetCursorRects];
    if (os_window)
    {
        [self addCursorRect:self.bounds cursor:cocoa_cursor(os_window->relative ? OS_Cursor::Hidden : os_window->cursor)];
    }
}

- (void)mouseExited:(NSEvent*)event
{
    (void)event;
    if (os_window && !os_window->relative)
    {
        cocoa_data(os_window)->last_valid = false;
        os_queue_event(os_window, { .type = OS_RawEventType::MouseLeave });
    }
}

- (void)mouseDown:(NSEvent*)event
{
    cocoa_button(self, event, OS_Key::MouseLeft, true);
}

- (void)mouseUp:(NSEvent*)event
{
    cocoa_button(self, event, OS_Key::MouseLeft, false);
}

- (void)rightMouseDown:(NSEvent*)event
{
    cocoa_button(self, event, OS_Key::MouseRight, true);
}

- (void)rightMouseUp:(NSEvent*)event
{
    cocoa_button(self, event, OS_Key::MouseRight, false);
}

- (void)otherMouseDown:(NSEvent*)event
{
    cocoa_button(self, event, cocoa_other_button(event.buttonNumber), true);
}

- (void)otherMouseUp:(NSEvent*)event
{
    cocoa_button(self, event, cocoa_other_button(event.buttonNumber), false);
}

// A trackpad's precise deltas are points, a wheel's lines; positive away from the user whatever
// the scrolling direction the user picked
- (void)scrollWheel:(NSEvent*)event
{
    if (!os_window)
    {
        return;
    }
    const f64 scale = event.hasPreciseScrollingDeltas ? 0.1 : 1.0;
    f32       dx    = (f32)(-event.scrollingDeltaX * scale);
    f32       dy    = (f32)(event.scrollingDeltaY * scale);
    if (event.isDirectionInvertedFromDevice)
    {
        dx = -dx;
        dy = -dy;
    }
    if (dx != 0.0f || dy != 0.0f)
    {
        os_queue_event(os_window, { .type = OS_RawEventType::MouseWheel, .modifiers = cocoa_modifiers(event.modifierFlags), .dx = dx, .dy = dy });
    }
}

// Keys come through the monitor; the view takes the text they type
- (void)keyDown:(NSEvent*)event
{
    [self interpretKeyEvents:@[ event ]];
}

- (void)keyUp:(NSEvent*)event
{
    (void)event;
}

- (void)flagsChanged:(NSEvent*)event
{
    (void)event;
}

- (void)insertText:(id)string replacementRange:(NSRange)range
{
    (void)range;
    NSString* text = [string isKindOfClass:[NSAttributedString class]] ? [string string] : string;
    if (!os_window || text.length == 0)
    {
        return;
    }
    const char* utf8 = text.UTF8String;
    if ((u8)utf8[0] < 0x20 || utf8[0] == 0x7F)
    {
        return;
    }
    OS_RawEvent event = { .type = OS_RawEventType::Text };
    cstr_copy(event.text, sizeof(event.text), utf8);
    os_queue_event(os_window, event);
}

- (void)doCommandBySelector:(SEL)selector
{
    (void)selector;
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selected replacementRange:(NSRange)replacement
{
    (void)string;
    (void)selected;
    (void)replacement;
}

- (void)unmarkText
{
}

- (NSRange)selectedRange
{
    return NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange
{
    return NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText
{
    return NO;
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    (void)range;
    (void)actual;
    return nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText
{
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actual
{
    (void)range;
    (void)actual;
    return [self.window convertRectToScreen:[self convertRect:text_rect toView:nil]];
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
    (void)point;
    return 0;
}

@end

@implementation PUPPETXRWindow

- (BOOL)canBecomeKeyWindow
{
    return YES;
}

- (BOOL)canBecomeMainWindow
{
    return YES;
}

@end

@implementation PUPPETXRWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender
{
    (void)sender;
    if (os_window)
    {
        os_queue_event(os_window, { .type = OS_RawEventType::WindowClose });
    }
    return NO;
}

- (void)windowDidBecomeKey:(NSNotification*)notification
{
    (void)notification;
    if (os_window)
    {
        os_queue_event(os_window, { .type = OS_RawEventType::FocusGained });
    }
}

- (void)windowDidResignKey:(NSNotification*)notification
{
    (void)notification;
    if (os_window)
    {
        os_queue_event(os_window, { .type = OS_RawEventType::FocusLost });
    }
}

- (void)windowDidResize:(NSNotification*)notification
{
    (void)notification;
    if (os_window)
    {
        cocoa_queue_size(os_window);
    }
}

@end

// Inside NSApp's running event loop (Unity's editor and players, NSApplicationMain apps) the app
// dispatches the events itself; a pump there runs the app's frames nested in ours
static b8 app_runs_event_loop ()
{
    const CFRunLoopMode mode = CFRunLoopCopyCurrentMode(CFRunLoopGetMain());
    if (!mode)
    {
        return false;
    }
    CFRelease(mode);
    return NSApp && NSApp.isRunning;
}

static b8 cocoa_init ()
{
    @autoreleasepool
    {
        const b8 had_app = NSApp != nil;
        [NSApplication sharedApplication];
        if (!NSApp)
        {
            return false;
        }
        // A plain executable may show no windows until it says it is an app; an app's own policy stays
        if (NSApp.activationPolicy == NSApplicationActivationPolicyProhibited)
        {
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }
        if (!had_app && !NSApp.isRunning)
        {
            [NSApp finishLaunching];
        }
        // Once per process; it does nothing for windows that are not ours
        if (!os_cocoa.monitor)
        {
            const NSEventMask mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged | NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;
            os_cocoa.monitor       = [[NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* event) { return cocoa_monitor_event(event); }] retain];
        }
        os_cocoa.iso_keyboard = KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
    }
    return true;
}

static void cocoa_quit ()
{
}

static b8 cocoa_create_window (OS_Window* window, const OS_WindowDesc& desc)
{
    @autoreleasepool
    {
        CocoaWindow* data = (CocoaWindow*)os_memory_alloc(sizeof(CocoaWindow));
        window->data      = data;
        const NSRect content = NSMakeRect(0.0, 0.0, (CGFloat)(desc.width ? desc.width : 1), (CGFloat)(desc.height ? desc.height : 1));
        data->window         = [[PUPPETXRWindow alloc] initWithContentRect:content styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
        data->window.releasedWhenClosed      = NO;
        data->window.acceptsMouseMovedEvents = YES;
        data->window.title                   = [NSString stringWithUTF8String:desc.title];
        data->view                           = [[PUPPETXRView alloc] initWithFrame:content];
        data->view->os_window                = window;
        NSTrackingArea* tracking             = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect owner:data->view userInfo:nil];
        [data->view addTrackingArea:tracking];
        [tracking release];
        data->window.contentView             = data->view;
        [data->window makeFirstResponder:data->view];
        data->delegate            = [[PUPPETXRWindowDelegate alloc] init];
        data->delegate->os_window = window;
        data->window.delegate     = data->delegate;
        [data->window center];
        window->hidden = true;
        cocoa_queue_size(window);
        if (!desc.hidden)
        {
            window->driver->show(window);
            window->hidden = false;
        }
    }
    return true;
}

static void cocoa_destroy_window (OS_Window* window)
{
    @autoreleasepool
    {
        CocoaWindow* data = cocoa_data(window);
        if (window->relative)
        {
            CGAssociateMouseAndMouseCursorPosition(true);
        }
        data->view->os_window     = nullptr;
        data->delegate->os_window = nullptr;
        data->window.delegate     = nil;
        [data->window orderOut:nil];
        [data->window close];
        [data->layer release];
        [data->delegate release];
        [data->view release];
        [data->window release];
        os_memory_release(data);
        window->data = nullptr;
    }
}

// One layer per window, made on first use and kept with the window
static void* cocoa_metal_layer (OS_Window* window)
{
    CocoaWindow* data = cocoa_data(window);
    if (!data->layer)
    {
        data->layer          = [[CAMetalLayer layer] retain];
        data->view.wantsLayer = YES;
        data->view.layer      = data->layer;
        cocoa_queue_size(window);
    }
    return data->layer;
}

static f32 cocoa_refresh_rate (OS_Window* window)
{
    NSScreen* screen = cocoa_data(window)->window.screen ? cocoa_data(window)->window.screen : [NSScreen mainScreen];
    if (@available(macOS 12.0, *))
    {
        return screen ? (f32)screen.maximumFramesPerSecond : 0.0f;
    }
    return 0.0f;
}

static const char* const* cocoa_vulkan_extensions (u32& out_count)
{
    out_count = ARRAY_SIZE(cocoa_vulkan_extensions_list);
    return cocoa_vulkan_extensions_list;
}

static b8 cocoa_create_vulkan_surface (OS_Window* window, void* vk_instance, const void* vk_get_instance_proc_addr, u64& out_surface)
{
    OS_Library*                   loader = vk_get_instance_proc_addr ? nullptr : os_vulkan_loader();
    const VkGetInstanceProcAddrFn get    = vk_get_instance_proc_addr ? (VkGetInstanceProcAddrFn)(uintptr_t)vk_get_instance_proc_addr : (VkGetInstanceProcAddrFn)os_library_symbol(loader, "vkGetInstanceProcAddr");
    const VkCreateSurfaceFn       create = get ? (VkCreateSurfaceFn)get(vk_instance, "vkCreateMetalSurfaceEXT") : nullptr;
    const VkMetalSurfaceCreateInfo info  = { .sType = 1000217000, .pNext = nullptr, .flags = 0, .pLayer = cocoa_metal_layer(window) };
    const i32                     result = create ? create(vk_instance, &info, nullptr, &out_surface) : -1;
    os_library_close(loader);
    if (result != 0)
    {
        LOG_ERROR("vkCreateMetalSurfaceEXT failed or is missing: %d", result);
        return false;
    }
    return true;
}

// The cursor hides and stays put while the moves turn; it comes back where looking began
static void cocoa_set_relative_mouse (OS_Window* window, b8 relative)
{
    CocoaWindow* data = cocoa_data(window);
    if (relative)
    {
        data->saved_cursor = [NSEvent mouseLocation];
        CGAssociateMouseAndMouseCursorPosition(false);
    }
    else
    {
        CGAssociateMouseAndMouseCursorPosition(true);
        // Cocoa's screen points start bottom left, Quartz's top left of the main screen
        const CGFloat height = NSMaxY([NSScreen screens].firstObject.frame);
        CGWarpMouseCursorPosition(CGPointMake(data->saved_cursor.x, height - data->saved_cursor.y));
        data->last_valid = false;
    }
    window->relative = relative;
    [data->window invalidateCursorRectsForView:data->view];
}

// Zoomed to the screen's visible frame without -[NSWindow zoom:]'s animation of some 240 ms on
// the thread showing it; a hidden window gets it when shown
static void cocoa_zoom (CocoaWindow* data)
{
    NSScreen* screen = data->window.screen ? data->window.screen : [NSScreen mainScreen];
    if (screen)
    {
        [data->window setFrame:screen.visibleFrame display:YES animate:NO];
    }
}

static b8 cocoa_maximize (OS_Window* window)
{
    CocoaWindow* data = cocoa_data(window);
    if (!data->window.visible)
    {
        data->maximize_when_shown = true;
    }
    else
    {
        cocoa_zoom(data);
    }
    return true;
}

static b8 cocoa_maximized (OS_Window* window)
{
    const CocoaWindow* data = cocoa_data(window);
    return data->maximize_when_shown || data->window.zoomed;
}

static void cocoa_show (OS_Window* window)
{
    CocoaWindow* data = cocoa_data(window);
    [data->window makeKeyAndOrderFront:nil];
    if (data->maximize_when_shown)
    {
        data->maximize_when_shown = false;
        cocoa_zoom(data);
    }
    cocoa_queue_size(window);
}

static void cocoa_hide (OS_Window* window)
{
    [cocoa_data(window)->window orderOut:nil];
}

static void cocoa_set_cursor (OS_Window* window, OS_Cursor cursor)
{
    CocoaWindow* data = cocoa_data(window);
    window->cursor    = cursor;
    [data->window invalidateCursorRectsForView:data->view];
}

static void cocoa_text_input (OS_Window* window, b8 active, f32 x, f32 y, f32 height)
{
    CocoaWindow* data = cocoa_data(window);
    data->view->text_rect = NSMakeRect((CGFloat)x, (CGFloat)y, 1.0, (CGFloat)height);
    if (!active)
    {
        [data->view.inputContext discardMarkedText];
    }
}

static b8 cocoa_clipboard_text (OS_Window* window, char* out_text, usize size)
{
    (void)window;
    @autoreleasepool
    {
        NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return text && [text lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < size && cstr_copy(out_text, size, text.UTF8String);
    }
}

static void cocoa_set_clipboard_text (OS_Window* window, const char* text)
{
    (void)window;
    @autoreleasepool
    {
        NSPasteboard* board = [NSPasteboard generalPasteboard];
        [board clearContents];
        [board setString:[NSString stringWithUTF8String:text] forType:NSPasteboardTypeString];
    }
}

// What the key prints on the layout in use, without modifiers; dead keys give their accent
static b8 cocoa_key_label (OS_Key key, char* out, usize size)
{
    const u16 code = cocoa_code_of_key(key);
    if (code == 0xFFFF || !((key >= OS_Key::A && key <= OS_Key::Digit9) || key == OS_Key::Grave || key == OS_Key::LeftBracket || key == OS_Key::RightBracket || (key >= OS_Key::Minus && key <= OS_Key::NonUsBackslash)))
    {
        return false;
    }
    b8 found = false;
    @autoreleasepool
    {
        TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
        CFDataRef         layout = source ? (CFDataRef)TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) : nullptr;
        UniChar           chars[4] = {};
        UniCharCount      length   = 0;
        UInt32            dead     = 0;
        // The ISO swap undone: the layout wants the key's own code
        u16 raw = code;
        if (os_cocoa.iso_keyboard && (raw == kVK_ISO_Section || raw == kVK_ANSI_Grave))
        {
            raw = raw == kVK_ISO_Section ? (u16)kVK_ANSI_Grave : (u16)kVK_ISO_Section;
        }
        if (layout && UCKeyTranslate((const UCKeyboardLayout*)CFDataGetBytePtr(layout), raw, kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &dead, 4, &length, chars) == noErr && length > 0 && chars[0] > 0x20)
        {
            NSString* text = [[NSString stringWithCharacters:chars length:length] uppercaseString];
            found          = cstr_copy(out, size, text.UTF8String);
        }
        if (source)
        {
            CFRelease(source);
        }
    }
    return found;
}

static void cocoa_pump ()
{
    if (app_runs_event_loop())
    {
        return;
    }
    @autoreleasepool
    {
        for (;;)
        {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!event)
            {
                break;
            }
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
    }
}

static void cocoa_wait (u64 timeout_ns)
{
    os_sleep_ns(timeout_ns);
}

const OS_VideoDriver os_cocoa_driver = {
    .name                  = "cocoa",
    .init                  = cocoa_init,
    .quit                  = cocoa_quit,
    .create_window         = cocoa_create_window,
    .destroy_window        = cocoa_destroy_window,
    .refresh_rate          = cocoa_refresh_rate,
    .vulkan_extensions     = cocoa_vulkan_extensions,
    .create_vulkan_surface = cocoa_create_vulkan_surface,
    .metal_layer           = cocoa_metal_layer,
    .set_relative_mouse    = cocoa_set_relative_mouse,
    .maximize              = cocoa_maximize,
    .maximized             = cocoa_maximized,
    .show                  = cocoa_show,
    .hide                  = cocoa_hide,
    .set_cursor            = cocoa_set_cursor,
    .text_input            = cocoa_text_input,
    .clipboard_text        = cocoa_clipboard_text,
    .set_clipboard_text    = cocoa_set_clipboard_text,
    .key_label             = cocoa_key_label,
    .pump                  = cocoa_pump,
    .wait                  = cocoa_wait,
};

#else

static b8 no_display ()
{
    return false;
}

const OS_VideoDriver os_cocoa_driver = { .name = "cocoa", .init = no_display };

#endif
