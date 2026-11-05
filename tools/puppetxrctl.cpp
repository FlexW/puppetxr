#include "base.h"

#include <stdio.h>
#include <stdlib.h>

//
// Types
//

#define CTL_MAX_SIMULATORS 64
#define CTL_MAX_REQUEST    16384 // a body loaded from a file, two paths
#define CTL_MAX_REPLY      65536
#define CTL_MAX_WORDS      16
#define CTL_EXIT_OK        0
#define CTL_EXIT_FAILED    1 // the simulator refused the command
#define CTL_EXIT_USAGE     2
#define CTL_EXIT_CONNECT   3 // no simulator, ambiguous target, version mismatch or lost connection

namespace
{

struct CTL_Simulator
{
    u32  pid;
    u32  port;
    char address[64];
    char app[128];
    char version[32];
};

struct CTL_Discovery
{
    char          dir[OS_MAX_PATH];
    CTL_Simulator simulators[CTL_MAX_SIMULATORS];
    u32           count;
};

struct CTL_Connection
{
    OS_Socket* socket;
    char       buffer[CTL_MAX_REPLY];
    usize      length;
    i32        next_id;
};

struct CTL_Options
{
    const char* target;
    b8          json;
    u32         timeout_ms;
};

// A float as JSON that reads back bit-exactly
struct CTL_Number
{
    char text[32];
};

} // namespace

//
// Statics
//

static const char* ctl_usage =
    "usage: puppetxrctl [--target <pid|app|host:port>] [--json] [--timeout <ms>] <command> [args]\n"
    "\n"
    "  list                                 running simulators\n"
    "  status\n"
    "  pose <device> [x y z [qx qy qz qw]]  get, or set in stage space; devices: head, left, right\n"
    "  active <device> on|off\n"
    "  haptic left|right [main|trigger|thumb] what the app plays: active, amplitude, frequency, remaining_s\n"
    "  input <device> <input> [x [y]|on|off] get or set, e.g. input right trigger/value 1\n"
    "  controls on|off                      keyboard and mouse control of the simulator\n"
    "  fixed-step on|off\n"
    "  recenter\n"
    "  profile [name]                       get or set the control profile: what keyboard and mouse drive\n"
    "  profiler start <path> | stop         a zone profile capture into a Chrome trace (docs/profiling.md)\n"
    "  mode [body|head|left|right]          the profiles that were the control modes\n"
    "  reset [hands]                        head and hands to their start poses, or hands in front of the head\n"
    "  point left|right <u> <v> [left|right] [grip <pitch>] [ahead <m>]  aim from an eye through (u, v) of its image,\n"
    "                                       0 0 top left; the aim pose, or the grip turned pitch degrees up (OVRPlugin:\n"
    "                                       grip -60); ahead starts the ray that far along it from the eye\n"
    "  key <name> down|up                   press or release a key as a person would, names as in docs/config.md\n"
    "  mouse <dx> <dy>                      relative mouse motion in pixels; turns as the captured mouse does\n"
    "  mirror left|right|both\n"
    "  screenshot <path>                    PNG of the last composited frame, written by the simulator\n"
    "  wait <frames>                        returns once that many more frames ran\n"
    "  pause | resume                       hold the app in xrWaitFrame, or let it run\n"
    "  step <frames>                        run exactly that many frames, then pause\n"
    "  frame fault wait|begin|locate|end <XrResult> [count]   fail the next frame calls, e.g. -2 (runtime failure)\n"
    "  record start <path> | record stop\n"
    "  replay start <path> | replay stop\n"
    "  controller [oculus_touch|khr_simple|meta_touch_pro|meta_touch_plus]\n"
    "  hand left|right [controller|hand]  what tracks the hand; a tracked hand pinches with trigger/value, grabs with squeeze/value\n"
    "  body                                 the body tracking joints in stage space (docs/body-tracking.md)\n"
    "  body get | reset | set <json>        the body as JSON, back to the configured one, or merge JSON onto it\n"
    "  body save <file> | load <file>       the body to a file, or the configured body with the file's on top\n"
    "  body height <m> | pose stand|t_pose|a_pose|arms_forward|sit | bone <joint> <scale>\n"
    "  body joint <joint> rot <x y z w> | tracked on|off | confidence <c>\n"
    "  body arm|leg left|right follow|posed  an arm follows its controller, a leg the floor, or both hold their pose\n"
    "  face                                 the 70 face weights and where the eyes look (docs/face-tracking.md)\n"
    "  face get | reset | set <json> | save <file> | load <file>   as for body\n"
    "  face preset neutral|smile|frown|surprise|eyes_closed|jaw_open | weight <name> <w> | viseme <name> <w>\n"
    "  face blink on|off | gaze <yaw> <pitch> | look <x y z> | look ahead   blinks, the gaze from the head\n"
    "                                       in degrees, or the eyes on a point in stage space\n"
    "  vkb                                  the app's virtual keyboard: shown, pose, scale, text context\n"
    "  vkb key <name>                       where a key's top is; a letter or digit, ',' '.', backspace, enter, shift or space\n"
    "  vkb type <text>                      type into the shown keyboard at the next frame; \\n is enter, \\b backspace\n"
    "  props                                the props: id, kind, motion, pose, where it is now\n"
    "  prop add qr <text> <x y z> [qx qy qz qw]     a QR code facing its +Z; prop add keyboard <x y z> [q]\n"
    "  prop move <id> <x y z> [qx qy qz qw]\n"
    "  prop motion <id> still | linear <x y z> <speed> | circle <radius> <speed>\n"
    "  prop size <id> <meters> | prop text <id> <text> | prop remove <id>\n"
    "  room [name|none]\n"
    "  anchors                              stored anchors of the app and simulated user, local then cloud\n"
    "  anchors wipe [cloud]                 empty the local store, with cloud also the user's cloud anchors\n"
    "  anchors fault save|erase|load|share <XrResult> [count]   fail the next requests, e.g. -1000169002 (network timeout)\n"
    "  anchors timing <latency frames> <localize frames>\n"
    "  origin                               where the stage origin lay in the room at instance creation\n"
    "  drift                                tracking drift: where STAGE lies in the stage at creation, and its rate\n"
    "  drift <x y z> [yaw] | drift reset    move the tracking origin now, metres and degrees about +Y; the room stays\n"
    "  drift rate <x y z> [yaw]             drift every frame by m/s and degrees/s; drift rate 0 0 0 stops\n"
    "  exit                                 ask the app to end its session\n"
    "  send <json>                          one raw request\n"
    "  shell                                commands or raw JSON lines from stdin, JSON replies on stdout\n"
    "\n"
    "Simulators listen only when started with PUPPETXR_CONTROL=1. Client and simulator must be the same version.\n";

static void version_string (char* out, usize size)
{
    cstr_format(out, size, "%u.%u.%u", (u32)PUPPETXR_BUILD_VERSION_MAJOR, (u32)PUPPETXR_BUILD_VERSION_MINOR, (u32)PUPPETXR_BUILD_VERSION_PATCH);
}

static void discovery_entry (const char* name, void* user)
{
    CTL_Discovery& discovery = *(CTL_Discovery*)user;
    const usize    length    = cstr_length(name);
    if (length < 6 || !cstr_equal(name + length - 5, ".json") || discovery.count == CTL_MAX_SIMULATORS)
    {
        return;
    }
    char path[OS_MAX_PATH];
    cstr_format(path, sizeof(path), "%s/%s", discovery.dir, name);
    // A file nobody holds belongs to a simulator that is gone, crashed or not
    if (os_lock_file_is_stale(path))
    {
        os_delete_file(path);
        return;
    }
    JsonParser* parser = json_parse_from_file(path);
    if (!parser)
    {
        return;
    }
    const JsonValue root      = json_get_root(parser);
    CTL_Simulator&  simulator = discovery.simulators[discovery.count];
    simulator                 = {};
    json_find_uint(root, "pid", simulator.pid);
    json_find_uint(root, "port", simulator.port);
    json_find_string(root, "address", simulator.address, nullptr, sizeof(simulator.address), "127.0.0.1");
    json_find_string(root, "app", simulator.app, nullptr, sizeof(simulator.app));
    json_find_string(root, "version", simulator.version, nullptr, sizeof(simulator.version));
    json_release(parser);
    if (simulator.port > 0 && simulator.port <= 65535)
    {
        discovery.count++;
    }
}

static i32 compare_simulators (const CTL_Simulator* a, const CTL_Simulator* b)
{
    return a->pid < b->pid ? -1 : a->pid > b->pid ? 1 : 0;
}

static void discover (CTL_Discovery& discovery)
{
    discovery = {};
    char runtime[OS_MAX_PATH];
    if (!os_runtime_dir(runtime, sizeof(runtime)))
    {
        return;
    }
    cstr_format(discovery.dir, sizeof(discovery.dir), "%s/puppetxr", runtime);
    os_list_directory(discovery.dir, discovery_entry, &discovery);
    quicksort(discovery.simulators, discovery.count, compare_simulators);
}

static b8 parse_u32 (const char* text, u32& out_value)
{
    if (!text || !text[0])
    {
        return false;
    }
    u64 value = 0;
    for (const char* c = text; *c; c++)
    {
        if (*c < '0' || *c > '9' || value > 0xFFFFFFFFull)
        {
            return false;
        }
        value = value * 10 + (u64)(*c - '0');
    }
    out_value = (u32)value;
    return value <= 0xFFFFFFFFull;
}

// "host:port" connects directly, a number picks a pid, anything else an app name
static b8 resolve_target (const char* target, CTL_Simulator& out_simulator, b8 report)
{
    const char* colon = target ? cstr_find(target, ":") : nullptr;
    if (colon)
    {
        out_simulator = {};
        const usize host = (usize)(colon - target);
        if (host == 0 || host >= sizeof(out_simulator.address) || !parse_u32(colon + 1, out_simulator.port) || out_simulator.port > 65535)
        {
            if (report)
            {
                (void)fprintf(stderr, "puppetxrctl: target %s is not host:port\n", target);
            }
            return false;
        }
        memory_copy(out_simulator.address, target, host);
        out_simulator.address[host] = 0;
        return true;
    }

    CTL_Discovery discovery = {};
    discover(discovery);
    u32 pid      = 0;
    u32 matches  = 0;
    const b8 by_pid = target && parse_u32(target, pid);
    for (u32 i = 0; i < discovery.count; i++)
    {
        const CTL_Simulator& simulator = discovery.simulators[i];
        if (!target || (by_pid && simulator.pid == pid) || (!by_pid && cstr_equal(simulator.app, target)))
        {
            out_simulator = simulator;
            matches++;
        }
    }
    if (matches == 1)
    {
        return true;
    }
    if (!report)
    {
        return false;
    }
    if (matches == 0)
    {
        (void)fprintf(stderr, discovery.count == 0 ? "puppetxrctl: no simulator is listening; start the app with PUPPETXR_CONTROL=1\n" : "puppetxrctl: no simulator matches %s; see puppetxrctl list\n", target ? target : "");
        return false;
    }
    (void)fprintf(stderr, "puppetxrctl: %u simulators match, pick one with --target:\n", matches);
    for (u32 i = 0; i < discovery.count; i++)
    {
        (void)fprintf(stderr, "  %u  %s\n", discovery.simulators[i].pid, discovery.simulators[i].app);
    }
    return false;
}

static b8 send_all (const CTL_Connection& connection, const char* data, usize size, u32 timeout_ms)
{
    while (size > 0)
    {
        usize                 sent   = 0;
        const OS_SocketResult result = os_socket_send(connection.socket, data, size, sent);
        if (result == OS_SocketResult::Closed)
        {
            return false;
        }
        if (result == OS_SocketResult::WouldBlock && !os_socket_wait(connection.socket, timeout_ms, true))
        {
            return false;
        }
        data += sent;
        size -= sent;
    }
    return true;
}

// The next reply line, null-terminated in the connection buffer; false on timeout or close
static b8 receive_line (CTL_Connection& connection, u32 timeout_ms, char*& out_line)
{
    const u64 deadline = os_time_ns() + (u64)timeout_ms * 1000000ull;
    while (true)
    {
        for (usize i = 0; i < connection.length; i++)
        {
            if (connection.buffer[i] == '\n')
            {
                connection.buffer[i] = 0;
                out_line             = connection.buffer;
                return true;
            }
        }
        const u64 now = os_time_ns();
        if (now >= deadline || connection.length == sizeof(connection.buffer) - 1 || !os_socket_wait(connection.socket, (u32)((deadline - now) / 1000000ull) + 1))
        {
            return false;
        }
        usize received = 0;
        if (os_socket_receive(connection.socket, connection.buffer + connection.length, sizeof(connection.buffer) - 1 - connection.length, received) == OS_SocketResult::Closed)
        {
            return false;
        }
        connection.length += received;
    }
}

static void consume_line (CTL_Connection& connection, const char* line)
{
    const usize used = cstr_length(line) + 1;
    memory_move(connection.buffer, connection.buffer + used, connection.length - used);
    connection.length -= used;
}

static b8 request (CTL_Connection& connection, const char* line, u32 timeout_ms, char* out_reply, usize reply_size)
{
    if (!send_all(connection, line, cstr_length(line), timeout_ms) || !send_all(connection, "\n", 1, timeout_ms))
    {
        (void)fprintf(stderr, "puppetxrctl: the connection to the simulator was lost\n");
        return false;
    }
    char* reply = nullptr;
    if (!receive_line(connection, timeout_ms, reply))
    {
        (void)fprintf(stderr, "puppetxrctl: no reply within %u ms; the app may not be running frames\n", timeout_ms);
        return false;
    }
    cstr_copy(out_reply, reply_size, reply);
    consume_line(connection, reply);
    return true;
}

static b8 reply_ok (const char* reply)
{
    JsonParser* parser = json_parse_from_string(reply);
    b8          ok     = false;
    if (parser)
    {
        json_find_bool(json_get_root(parser), "ok", ok);
        json_release(parser);
    }
    return ok;
}

static b8 connect (const CTL_Simulator& simulator, u32 timeout_ms, CTL_Connection& out_connection)
{
    out_connection        = {};
    out_connection.socket = os_socket_connect(simulator.address, (u16)simulator.port, timeout_ms);
    if (!out_connection.socket)
    {
        (void)fprintf(stderr, "puppetxrctl: cannot connect to %s:%u\n", simulator.address, simulator.port);
        return false;
    }
    char version[32];
    version_string(version, sizeof(version));
    char hello[128];
    cstr_format(hello, sizeof(hello), "{\"id\":0,\"cmd\":\"hello\",\"version\":\"%s\"}", version);
    char reply[CTL_MAX_REPLY];
    if (!request(out_connection, hello, timeout_ms, reply, sizeof(reply)))
    {
        return false;
    }
    if (!reply_ok(reply))
    {
        JsonParser* parser = json_parse_from_string(reply);
        char        message[256] = {};
        if (parser)
        {
            json_find_string(json_get_root(parser), "message", message, nullptr, sizeof(message));
            json_release(parser);
        }
        (void)fprintf(stderr, "puppetxrctl: %s\n", message[0] ? message : reply);
        return false;
    }
    out_connection.next_id = 1;
    return true;
}

static void append (char* out, usize size, const char* fmt, ...) PRINTF_LIKE(3, 4);
static void append (char* out, usize size, const char* fmt, ...)
{
    const usize length = cstr_length(out);
    va_list     args   = {};
    va_start(args, fmt);
    cstr_format_v(out + length, size - length, fmt, args);
    va_end(args);
}

static void append_string (char* out, usize size, const char* key, const char* value)
{
    append(out, size, ",\"%s\":\"", key);
    for (const char* c = value; *c; c++)
    {
        if (*c == '"' || *c == '\\')
        {
            append(out, size, "\\%c", *c);
        }
        else if ((u8)*c >= 0x20)
        {
            append(out, size, "%c", *c);
        }
    }
    append(out, size, "\"");
}

// Text for the virtual keyboard: the escapes \n (enter), \b (backspace) and \\ pass on as such
static void append_typed (char* out, usize size, u32 count, const char* const* words)
{
    append(out, size, ",\"text\":\"");
    for (u32 w = 0; w < count; w++)
    {
        if (w > 0)
        {
            append(out, size, " ");
        }
        for (const char* c = words[w]; *c; c++)
        {
            if (*c == '\\' && (c[1] == 'n' || c[1] == 'b' || c[1] == '\\'))
            {
                append(out, size, "\\%c", c[1]);
                c++;
            }
            else if (*c == '"' || *c == '\\')
            {
                append(out, size, "\\%c", *c);
            }
            else if (*c == '\n' || *c == '\b')
            {
                append(out, size, "\\%c", *c == '\n' ? 'n' : 'b');
            }
            else if ((u8)*c >= 0x20)
            {
                append(out, size, "%c", *c);
            }
        }
    }
    append(out, size, "\"");
}

// Paths are read and written by the simulator, which may run in another directory
static void append_path (char* out, usize size, const char* key, const char* path)
{
    char absolute[OS_MAX_PATH] = {};
    if (os_path_is_absolute(path) || !os_current_directory(absolute, sizeof(absolute)))
    {
        cstr_copy(absolute, sizeof(absolute), path);
    }
    else
    {
        const usize length    = cstr_length(absolute);
        const b8    separated = length > 0 && (absolute[length - 1] == '/' || absolute[length - 1] == '\\');
        cstr_format(absolute + length, sizeof(absolute) - length, "%s%s", separated ? "" : "/", path);
    }
    append_string(out, size, key, absolute);
}

static b8 parse_float (const char* text, f32& out_value)
{
    char* end = nullptr;
    out_value = text ? strtof(text, &end) : 0.0f;
    return text && end && end != text && *end == 0;
}

static b8 parse_switch (const char* text, b8& out_on)
{
    out_on = cstr_equal(text, "on") || cstr_equal(text, "1") || cstr_equal(text, "true");
    return out_on || cstr_equal(text, "off") || cstr_equal(text, "0") || cstr_equal(text, "false");
}

// Position and an optional orientation: x y z [qx qy qz qw]
static b8 append_prop_pose (u32 count, const char* const* words, char* out, usize size)
{
    f32 values[7] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    if (count != 3 && count != 7)
    {
        return false;
    }
    for (u32 i = 0; i < count; i++)
    {
        if (!parse_float(words[i], values[i]))
        {
            return false;
        }
    }
    append(out, size, ",\"position\":[%.9g,%.9g,%.9g]", (f64)values[0], (f64)values[1], (f64)values[2]);
    if (count == 7)
    {
        append(out, size, ",\"orientation\":[%.9g,%.9g,%.9g,%.9g]", (f64)values[3], (f64)values[4], (f64)values[5], (f64)values[6]);
    }
    return true;
}

// prop add|move|motion|size|text|remove ...; the words after "prop"
static b8 build_prop_request (u32 count, const char* const* words, char* out, usize size)
{
    const char* verb = words[0];
    f32         a    = 0.0f;
    f32         b    = 0.0f;
    if (cstr_equal(verb, "add") && count >= 2 && cstr_equal(words[1], "qr"))
    {
        append(out, size, ",\"cmd\":\"add_prop\",\"kind\":\"qr\"");
        if (count < 3)
        {
            return false;
        }
        append_string(out, size, "text", words[2]);
        return append_prop_pose(count - 3, words + 3, out, size);
    }
    if (cstr_equal(verb, "add") && count >= 2 && cstr_equal(words[1], "keyboard"))
    {
        append(out, size, ",\"cmd\":\"add_prop\",\"kind\":\"keyboard\"");
        return append_prop_pose(count - 2, words + 2, out, size);
    }
    if (count < 2)
    {
        return false;
    }
    u32 id = 0;
    if (!parse_u32(words[1], id))
    {
        return false;
    }
    append(out, size, ",\"cmd\":\"%s\",\"prop\":%u", cstr_equal(verb, "remove") ? "remove_prop" : "update_prop", id);
    if (cstr_equal(verb, "remove"))
    {
        return count == 2;
    }
    if (cstr_equal(verb, "move"))
    {
        return append_prop_pose(count - 2, words + 2, out, size);
    }
    if (cstr_equal(verb, "size") && count == 3 && parse_float(words[2], a))
    {
        append(out, size, ",\"size\":%.9g", (f64)a);
        return true;
    }
    if (cstr_equal(verb, "text") && count == 3)
    {
        append_string(out, size, "text", words[2]);
        return true;
    }
    if (cstr_equal(verb, "motion") && count == 3 && cstr_equal(words[2], "still"))
    {
        append(out, size, ",\"motion\":\"still\"");
        return true;
    }
    if (cstr_equal(verb, "motion") && count == 5 && cstr_equal(words[2], "circle") && parse_float(words[3], a) && parse_float(words[4], b))
    {
        append(out, size, ",\"motion\":\"circle\",\"radius\":%.9g,\"speed\":%.9g", (f64)a, (f64)b);
        return true;
    }
    f32 target[3] = {};
    if (cstr_equal(verb, "motion") && count == 7 && cstr_equal(words[2], "linear") && parse_float(words[3], target[0]) && parse_float(words[4], target[1]) && parse_float(words[5], target[2]) && parse_float(words[6], b))
    {
        append(out, size, ",\"motion\":\"linear\",\"target\":[%.9g,%.9g,%.9g],\"speed\":%.9g", (f64)target[0], (f64)target[1], (f64)target[2], (f64)b);
        return true;
    }
    return false;
}

// Turns command words into one request; false for a usage error
// A joint, limb or pose name as the body JSON takes it; the simulator rejects unknown ones
static b8 body_name (const char* text)
{
    if (!text || !text[0])
    {
        return false;
    }
    for (const char* c = text; *c; c++)
    {
        if (!((*c >= 'a' && *c <= 'z') || *c == '_'))
        {
            return false;
        }
    }
    return true;
}

// The file's JSON on one line: the protocol is a line per request, and JSON strings hold no newlines
static b8 append_body_file (char* out, usize size, const char* path)
{
    u8*   data   = nullptr;
    usize length = 0;
    if (!os_read_file(path, &data, &length))
    {
        (void)fprintf(stderr, "puppetxrctl: cannot read %s\n", path);
        return false;
    }
    defer { os_memory_release(data); };
    const usize used = cstr_length(out);
    if (used + length + 2 >= size)
    {
        (void)fprintf(stderr, "puppetxrctl: %s is too large\n", path);
        return false;
    }
    for (usize i = 0; i < length; i++)
    {
        const char c       = (char)data[i];
        out[used + i]      = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
    }
    out[used + length] = 0;
    return true;
}

// body [get | reset | set <json> | save <file> | load <file> | height <m> | pose <name> |
// bone <joint> <scale> | joint <joint> rot <x y z w> | joint <joint> tracked on|off |
// joint <joint> confidence <c> | arm|leg left|right follow|posed]
static CTL_Number number (f32 value)
{
    CTL_Number out = {};
    json_format_f32(out.text, sizeof(out.text), value);
    return out;
}

// drift | drift reset | drift <x y z> [yaw] | drift rate <x y z> [yaw per second]; all the words
static b8 build_drift_request (u32 count, const char* const* words, char* out, usize size)
{
    if (count == 1)
    {
        append(out, size, ",\"cmd\":\"get_tracking_drift\"");
        return true;
    }
    if (count == 2 && cstr_equal(words[1], "reset"))
    {
        append(out, size, ",\"cmd\":\"reset_tracking_drift\"");
        return true;
    }
    const b8  rate      = cstr_equal(words[1], "rate");
    const u32 first     = rate ? 2 : 1;
    const u32 given     = count - first;
    f32       values[4] = {};
    if (given != 3 && given != 4)
    {
        return false;
    }
    for (u32 i = 0; i < given; i++)
    {
        if (!parse_float(words[first + i], values[i]))
        {
            return false;
        }
    }
    append(out, size, rate ? ",\"cmd\":\"set_tracking_drift_rate\",\"velocity\":[%.9g,%.9g,%.9g],\"yaw_rate\":%.9g" : ",\"cmd\":\"set_tracking_drift\",\"position\":[%.9g,%.9g,%.9g],\"yaw_degrees\":%.9g", (f64)values[0], (f64)values[1], (f64)values[2], (f64)values[3]);
    return true;
}

static b8 build_body_request (u32 count, const char* const* words, char* out, usize size)
{
    const char* verb = count >= 2 ? words[1] : "";
    f32         values[4] = {};
    b8          on        = false;
    if (count == 1)
    {
        append(out, size, ",\"cmd\":\"get_body_joints\"");
    }
    else if ((cstr_equal(verb, "get") || cstr_equal(verb, "save")) && count == (cstr_equal(verb, "get") ? 2u : 3u))
    {
        append(out, size, ",\"cmd\":\"get_body\"");
    }
    else if (cstr_equal(verb, "reset") && count == 2)
    {
        append(out, size, ",\"cmd\":\"reset_body\"");
    }
    else if (cstr_equal(verb, "load") && count == 3)
    {
        append(out, size, ",\"cmd\":\"set_body\",\"replace\":true,\"body\":");
        if (!append_body_file(out, size, words[2]))
        {
            return false;
        }
    }
    else if (cstr_equal(verb, "set") && count == 3)
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":%s", words[2]);
    }
    else if (cstr_equal(verb, "height") && count == 3 && parse_float(words[2], values[0]))
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":{\"height\":%s}", number(values[0]).text);
    }
    else if (cstr_equal(verb, "pose") && count == 3 && body_name(words[2]))
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":{\"pose\":\"%s\"}", words[2]);
    }
    else if (cstr_equal(verb, "bone") && count == 4 && body_name(words[2]) && parse_float(words[3], values[0]))
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":{\"bones\":{\"%s\":%s}}", words[2], number(values[0]).text);
    }
    else if (cstr_equal(verb, "joint") && count == 8 && body_name(words[2]) && cstr_equal(words[3], "rot") && parse_float(words[4], values[0]) && parse_float(words[5], values[1]) && parse_float(words[6], values[2]) && parse_float(words[7], values[3]))
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":{\"joints\":{\"%s\":{\"rotation\":[%s,%s,%s,%s]}}}", words[2], number(values[0]).text, number(values[1]).text, number(values[2]).text, number(values[3]).text);
    }
    else if (cstr_equal(verb, "joint") && count == 5 && body_name(words[2]) && cstr_equal(words[3], "tracked") && parse_switch(words[4], on))
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":{\"joints\":{\"%s\":{\"tracked\":%s}}}", words[2], on ? "true" : "false");
    }
    else if (cstr_equal(verb, "joint") && count == 5 && body_name(words[2]) && cstr_equal(words[3], "confidence") && parse_float(words[4], values[0]))
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":{\"joints\":{\"%s\":{\"confidence\":%s}}}", words[2], number(values[0]).text);
    }
    else if ((cstr_equal(verb, "arm") || cstr_equal(verb, "leg")) && count == 4 && (cstr_equal(words[2], "left") || cstr_equal(words[2], "right")) && (cstr_equal(words[3], "follow") || cstr_equal(words[3], "posed")))
    {
        append(out, size, ",\"cmd\":\"set_body\",\"body\":{\"limbs\":{\"%s_%s\":\"%s\"}}", words[2], verb, words[3]);
    }
    else
    {
        return false;
    }
    return true;
}

// face [get | reset | set <json> | save <file> | load <file> | preset <name> | weight <name> <w> |
// viseme <name> <w> | blink on|off | gaze <yaw> <pitch> | look <x y z> | look ahead]
static b8 build_face_request (u32 count, const char* const* words, char* out, usize size)
{
    const char* verb      = count >= 2 ? words[1] : "";
    f32         values[3] = {};
    b8          on        = false;
    if (count == 1)
    {
        append(out, size, ",\"cmd\":\"get_face_frame\"");
    }
    else if ((cstr_equal(verb, "get") && count == 2) || (cstr_equal(verb, "save") && count == 3))
    {
        append(out, size, ",\"cmd\":\"get_face\"");
    }
    else if (cstr_equal(verb, "reset") && count == 2)
    {
        append(out, size, ",\"cmd\":\"reset_face\"");
    }
    else if (cstr_equal(verb, "load") && count == 3)
    {
        append(out, size, ",\"cmd\":\"set_face\",\"replace\":true,\"face\":");
        if (!append_body_file(out, size, words[2]))
        {
            return false;
        }
    }
    else if (cstr_equal(verb, "set") && count == 3)
    {
        append(out, size, ",\"cmd\":\"set_face\",\"face\":%s", words[2]);
    }
    else if (cstr_equal(verb, "preset") && count == 3 && body_name(words[2]))
    {
        append(out, size, ",\"cmd\":\"set_face\",\"face\":{\"preset\":\"%s\"}", words[2]);
    }
    else if ((cstr_equal(verb, "weight") || cstr_equal(verb, "viseme")) && count == 4 && body_name(words[2]) && parse_float(words[3], values[0]))
    {
        append(out, size, ",\"cmd\":\"set_face\",\"face\":{\"%ss\":{\"%s\":%s}}", verb, words[2], number(values[0]).text);
    }
    else if (cstr_equal(verb, "blink") && count == 3 && parse_switch(words[2], on))
    {
        append(out, size, ",\"cmd\":\"set_face\",\"face\":{\"blink\":%s}", on ? "true" : "false");
    }
    else if (cstr_equal(verb, "gaze") && count == 4 && parse_float(words[2], values[0]) && parse_float(words[3], values[1]))
    {
        append(out, size, ",\"cmd\":\"set_face\",\"face\":{\"gaze\":[%s,%s],\"look_at\":null}", number(values[0]).text, number(values[1]).text);
    }
    else if (cstr_equal(verb, "look") && count == 3 && cstr_equal(words[2], "ahead"))
    {
        append(out, size, ",\"cmd\":\"set_face\",\"face\":{\"look_at\":null}");
    }
    else if (cstr_equal(verb, "look") && count == 5 && parse_float(words[2], values[0]) && parse_float(words[3], values[1]) && parse_float(words[4], values[2]))
    {
        append(out, size, ",\"cmd\":\"set_face\",\"face\":{\"look_at\":[%s,%s,%s]}", number(values[0]).text, number(values[1]).text, number(values[2]).text);
    }
    else
    {
        return false;
    }
    return true;
}

// body save, face save: the reply's object of that key into the file
static b8 save_body (const char* reply, const char* key, const char* path)
{
    JsonParser* parser = json_parse_from_string(reply);
    if (!parser)
    {
        return false;
    }
    defer { json_release(parser); };
    const char* text   = nullptr;
    usize       length = 0;
    if (!json_raw(json_find(json_get_root(parser), key), text, length))
    {
        return false;
    }
    char line[CTL_MAX_REPLY];
    cstr_format(line, sizeof(line), "%.*s\n", (int)length, text);
    if (!os_write_file(path, line, cstr_length(line)))
    {
        (void)fprintf(stderr, "puppetxrctl: cannot write %s\n", path);
        return false;
    }
    return true;
}

static b8 build_request (u32 count, const char* const* words, i32 id, char* out, usize size, u32& inout_timeout_ms)
{
    if (count == 0)
    {
        return false;
    }
    const char* command = words[0];
    out[0]              = 0;
    append(out, size, "{\"id\":%d", id);
    b8 on = false;

    if (cstr_equal(command, "status") && count == 1)
    {
        append(out, size, ",\"cmd\":\"status\"");
    }
    else if (cstr_equal(command, "pose") && (count == 2 || count == 5 || count == 9))
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 2 ? "get_pose" : "set_pose");
        append_string(out, size, "device", words[1]);
        f32 values[7] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        for (u32 i = 2; i < count; i++)
        {
            if (!parse_float(words[i], values[i - 2]))
            {
                return false;
            }
        }
        if (count > 2)
        {
            append(out, size, ",\"position\":[%.9g,%.9g,%.9g],\"orientation\":[%.9g,%.9g,%.9g,%.9g]", (f64)values[0], (f64)values[1], (f64)values[2], (f64)values[3], (f64)values[4], (f64)values[5], (f64)values[6]);
        }
    }
    else if (cstr_equal(command, "hand") && (count == 2 || count == 3))
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 2 ? "get_hand_source" : "set_hand_source");
        append_string(out, size, "device", words[1]);
        if (count == 3)
        {
            append_string(out, size, "source", words[2]);
        }
    }
    else if (cstr_equal(command, "vkb") && count == 1)
    {
        append(out, size, ",\"cmd\":\"get_virtual_keyboard\"");
    }
    else if (cstr_equal(command, "vkb") && count == 3 && cstr_equal(words[1], "key"))
    {
        append(out, size, ",\"cmd\":\"get_virtual_keyboard_key\"");
        append_string(out, size, "key", words[2]);
    }
    else if (cstr_equal(command, "vkb") && count >= 3 && cstr_equal(words[1], "type"))
    {
        append(out, size, ",\"cmd\":\"type_text\"");
        append_typed(out, size, count - 2, words + 2);
    }
    else if (cstr_equal(command, "props") && count == 1)
    {
        append(out, size, ",\"cmd\":\"get_props\"");
    }
    else if (cstr_equal(command, "prop") && count >= 2)
    {
        if (!build_prop_request(count - 1, words + 1, out, size))
        {
            return false;
        }
    }
    else if (cstr_equal(command, "haptic") && (count == 2 || count == 3))
    {
        append(out, size, ",\"cmd\":\"get_haptic\"");
        append_string(out, size, "device", words[1]);
        if (count == 3)
        {
            append_string(out, size, "output", words[2]);
        }
    }
    else if (cstr_equal(command, "active") && count == 3 && parse_switch(words[2], on))
    {
        append(out, size, ",\"cmd\":\"set_active\"");
        append_string(out, size, "device", words[1]);
        append(out, size, ",\"active\":%s", on ? "true" : "false");
    }
    else if (cstr_equal(command, "input") && count >= 3 && count <= 5)
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 3 ? "get_input" : "set_input");
        append_string(out, size, "device", words[1]);
        append_string(out, size, "input", words[2]);
        f32 x = 0.0f;
        f32 y = 0.0f;
        if (count == 4 && parse_switch(words[3], on))
        {
            append(out, size, ",\"value\":%s", on ? "true" : "false");
        }
        else if (count >= 4)
        {
            if (!parse_float(words[3], x) || (count == 5 && !parse_float(words[4], y)))
            {
                return false;
            }
            append(out, size, ",\"value\":[%.9g,%.9g]", (f64)x, (f64)y);
        }
    }
    else if ((cstr_equal(command, "controls") || cstr_equal(command, "fixed-step")) && count == 2 && parse_switch(words[1], on))
    {
        append(out, size, ",\"cmd\":\"%s\",\"enabled\":%s", cstr_equal(command, "controls") ? "set_controls" : "set_fixed_step", on ? "true" : "false");
    }
    else if ((cstr_equal(command, "pause") || cstr_equal(command, "resume")) && count == 1)
    {
        append(out, size, ",\"cmd\":\"%s\"", command);
    }
    else if (cstr_equal(command, "step") && count == 2)
    {
        u32 frames = 0;
        if (!parse_u32(words[1], frames) || frames == 0)
        {
            return false;
        }
        append(out, size, ",\"cmd\":\"step\",\"frames\":%u", frames);
        inout_timeout_ms = inout_timeout_ms > 60000u ? inout_timeout_ms : 60000u;
    }
    else if (cstr_equal(command, "recenter") && count == 1)
    {
        append(out, size, ",\"cmd\":\"recenter\"");
    }
    else if (cstr_equal(command, "profiler") && count == 3 && cstr_equal(words[1], "start"))
    {
        append(out, size, ",\"cmd\":\"profiler_start\"");
        append_string(out, size, "path", words[2]);
    }
    else if (cstr_equal(command, "profiler") && count == 2 && cstr_equal(words[1], "stop"))
    {
        append(out, size, ",\"cmd\":\"profiler_stop\"");
    }
    else if (cstr_equal(command, "body"))
    {
        if (!build_body_request(count, words, out, size))
        {
            return false;
        }
    }
    else if (cstr_equal(command, "face"))
    {
        if (!build_face_request(count, words, out, size))
        {
            return false;
        }
    }
    else if (cstr_equal(command, "profile") && count <= 2)
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 1 ? "get_profile" : "set_profile");
        if (count == 2)
        {
            append_string(out, size, "profile", words[1]);
        }
    }
    else if (cstr_equal(command, "mode") && count <= 2)
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 1 ? "get_control_mode" : "set_control_mode");
        if (count == 2)
        {
            append_string(out, size, "mode", words[1]);
        }
    }
    else if (cstr_equal(command, "reset") && (count == 1 || (count == 2 && cstr_equal(words[1], "hands"))))
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 1 ? "reset_poses" : "reset_hands");
    }
    else if (cstr_equal(command, "point") && count >= 4 && count <= 9)
    {
        f32 u = 0.0f;
        f32 v = 0.0f;
        if (!parse_float(words[2], u) || !parse_float(words[3], v))
        {
            return false;
        }
        u32 next = 4;
        append(out, size, ",\"cmd\":\"point_hand\"");
        append_string(out, size, "device", words[1]);
        if (next < count && !cstr_equal(words[next], "grip") && !cstr_equal(words[next], "ahead"))
        {
            append_string(out, size, "eye", words[next++]);
        }
        if (next + 1 < count && cstr_equal(words[next], "grip"))
        {
            f32 pitch = 0.0f;
            if (!parse_float(words[next + 1], pitch))
            {
                return false;
            }
            append(out, size, ",\"grip_pitch\":%.9g", (f64)pitch);
            next += 2;
        }
        if (next + 1 < count && cstr_equal(words[next], "ahead"))
        {
            f32 ahead = 0.0f;
            if (!parse_float(words[next + 1], ahead))
            {
                return false;
            }
            append(out, size, ",\"ahead\":%.9g", (f64)ahead);
            next += 2;
        }
        if (next != count)
        {
            return false;
        }
        append(out, size, ",\"u\":%.9g,\"v\":%.9g", (f64)u, (f64)v);
    }
    else if (cstr_equal(command, "key") && count == 3 && (cstr_equal(words[2], "down") || cstr_equal(words[2], "up")))
    {
        append(out, size, ",\"cmd\":\"key\"");
        append_string(out, size, "key", words[1]);
        append(out, size, ",\"down\":%s", cstr_equal(words[2], "down") ? "true" : "false");
    }
    else if (cstr_equal(command, "mouse") && count == 3)
    {
        f32 dx = 0.0f;
        f32 dy = 0.0f;
        if (!parse_float(words[1], dx) || !parse_float(words[2], dy))
        {
            return false;
        }
        append(out, size, ",\"cmd\":\"mouse\",\"delta\":[%.9g,%.9g]", (f64)dx, (f64)dy);
    }
    else if (cstr_equal(command, "lens") && count == 3 && cstr_equal(words[1], "mask") && (cstr_equal(words[2], "on") || cstr_equal(words[2], "off")))
    {
        append(out, size, ",\"cmd\":\"set_lens_mask\",\"on\":%s", cstr_equal(words[2], "on") ? "true" : "false");
    }
    else if (cstr_equal(command, "ui") && count == 2 && (cstr_equal(words[1], "show") || cstr_equal(words[1], "hide")))
    {
        append(out, size, ",\"cmd\":\"show_ui\",\"shown\":%s", cstr_equal(words[1], "show") ? "true" : "false");
    }
    else if (cstr_equal(command, "ui") && count >= 3 && (cstr_equal(words[1], "click") || cstr_equal(words[1], "find")))
    {
        // Labels have spaces: the words after "click" or "find" are one label
        char label[48] = {};
        usize used     = 0;
        for (u32 i = 2; i < count; i++)
        {
            used += cstr_format(label + used, sizeof(label) - used, "%s%s", i > 2 ? " " : "", words[i]);
            if (used >= sizeof(label))
            {
                return false;
            }
        }
        append(out, size, cstr_equal(words[1], "click") ? ",\"cmd\":\"ui_click\"" : ",\"cmd\":\"ui_find\"");
        append_string(out, size, "label", label);
    }
    else if (cstr_equal(command, "mirror") && count == 2)
    {
        append(out, size, ",\"cmd\":\"set_mirror_view\"");
        append_string(out, size, "view", words[1]);
    }
    else if (cstr_equal(command, "screenshot") && count == 2)
    {
        append(out, size, ",\"cmd\":\"screenshot\"");
        append_path(out, size, "path", words[1]);
    }
    else if (cstr_equal(command, "wait") && count == 2)
    {
        u32 frames = 0;
        if (!parse_u32(words[1], frames))
        {
            return false;
        }
        append(out, size, ",\"cmd\":\"wait_frames\",\"frames\":%u", frames);
        inout_timeout_ms = inout_timeout_ms > 60000u ? inout_timeout_ms : 60000u;
    }
    else if ((cstr_equal(command, "record") || cstr_equal(command, "replay")) && count >= 2)
    {
        if (cstr_equal(words[1], "start") && count == 3)
        {
            append(out, size, ",\"cmd\":\"%s_start\"", command);
            append_path(out, size, "path", words[2]);
        }
        else if (cstr_equal(words[1], "stop") && count == 2)
        {
            append(out, size, ",\"cmd\":\"%s_stop\"", command);
        }
        else
        {
            return false;
        }
    }
    else if (cstr_equal(command, "controller") && count <= 2)
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 1 ? "get_controller" : "set_controller");
        if (count == 2)
        {
            append_string(out, size, "controller", words[1]);
        }
    }
    else if (cstr_equal(command, "room") && count <= 2)
    {
        append(out, size, ",\"cmd\":\"%s\"", count == 1 ? "get_room" : "set_room");
        if (count == 2)
        {
            append_string(out, size, "room", cstr_equal(words[1], "none") ? "" : words[1]);
        }
    }
    else if (cstr_equal(command, "anchors") && count == 1)
    {
        append(out, size, ",\"cmd\":\"get_anchors\"");
    }
    else if (cstr_equal(command, "anchors") && cstr_equal(words[1], "wipe") && (count == 2 || (count == 3 && cstr_equal(words[2], "cloud"))))
    {
        append(out, size, ",\"cmd\":\"wipe_anchors\",\"cloud\":%s", count == 3 ? "true" : "false");
    }
    else if (cstr_equal(command, "anchors") && cstr_equal(words[1], "fault") && (count == 4 || count == 5))
    {
        char* end      = nullptr;
        const long code = strtol(words[3], &end, 10);
        u32  times     = 1;
        if (!end || *end || (count == 5 && !parse_u32(words[4], times)))
        {
            return false;
        }
        append(out, size, ",\"cmd\":\"set_anchor_fault\"");
        append_string(out, size, "op", words[2]);
        append(out, size, ",\"result\":%ld,\"count\":%u", code, times);
    }
    else if (cstr_equal(command, "frame") && (count == 4 || count == 5) && cstr_equal(words[1], "fault"))
    {
        char*      end   = nullptr;
        const long code  = strtol(words[3], &end, 10);
        u32        times = 1;
        if (!end || *end || (count == 5 && !parse_u32(words[4], times)))
        {
            return false;
        }
        append(out, size, ",\"cmd\":\"set_frame_fault\"");
        append_string(out, size, "op", words[2]);
        append(out, size, ",\"result\":%ld,\"count\":%u", code, times);
    }
    else if (cstr_equal(command, "anchors") && cstr_equal(words[1], "timing") && count == 4)
    {
        u32 latency  = 0;
        u32 localize = 0;
        if (!parse_u32(words[2], latency) || !parse_u32(words[3], localize))
        {
            return false;
        }
        append(out, size, ",\"cmd\":\"set_anchor_timing\",\"latency_frames\":%u,\"localize_frames\":%u", latency, localize);
    }
    else if (cstr_equal(command, "origin") && count == 1)
    {
        append(out, size, ",\"cmd\":\"get_origin_offset\"");
    }
    else if (cstr_equal(command, "drift"))
    {
        if (!build_drift_request(count, words, out, size))
        {
            return false;
        }
    }
    else if (cstr_equal(command, "exit") && count == 1)
    {
        append(out, size, ",\"cmd\":\"request_exit\"");
    }
    else
    {
        return false;
    }
    append(out, size, "}");
    return true;
}

static void print_reply (const char* reply, b8 json)
{
    if (json)
    {
        printf("%s\n", reply);
        return;
    }
    JsonParser* parser = json_parse_from_string(reply);
    if (!parser)
    {
        printf("%s\n", reply);
        return;
    }
    const JsonValue root    = json_get_root(parser);
    b8              ok      = false;
    u32             printed = 0;
    json_find_bool(root, "ok", ok);
    if (!ok)
    {
        char error[64] = {};
        char message[256] = {};
        json_find_string(root, "error", error, nullptr, sizeof(error));
        json_find_string(root, "message", message, nullptr, sizeof(message));
        (void)fprintf(stderr, "puppetxrctl: %s (%s)\n", error, message);
    }
    for (const JsonMember member : json_members(root))
    {
        char key[64] = {};
        json_as_string(member.key, key, nullptr, sizeof(key));
        const char* text   = nullptr;
        usize       length = 0;
        if (!ok || cstr_equal(key, "id") || cstr_equal(key, "ok") || !json_raw(member.value, text, length))
        {
            continue;
        }
        printf("%s: %.*s\n", key, (int)length, text);
        printed++;
    }
    if (ok && printed == 0)
    {
        printf("ok\n");
    }
    json_release(parser);
}

static i32 run_list (b8 json)
{
    CTL_Discovery discovery = {};
    discover(discovery);
    if (json)
    {
        printf("[");
        for (u32 i = 0; i < discovery.count; i++)
        {
            const CTL_Simulator& s = discovery.simulators[i];
            printf("%s{\"pid\":%u,\"address\":\"%s\",\"port\":%u,\"version\":\"%s\",\"app\":\"%s\"}", i ? "," : "", s.pid, s.address, s.port, s.version, s.app);
        }
        printf("]\n");
        return CTL_EXIT_OK;
    }
    if (discovery.count == 0)
    {
        printf("no simulator is listening; start the app with PUPPETXR_CONTROL=1\n");
        return CTL_EXIT_OK;
    }
    printf("%-8s %-21s %-10s %s\n", "PID", "ADDRESS", "VERSION", "APP");
    for (u32 i = 0; i < discovery.count; i++)
    {
        const CTL_Simulator& s = discovery.simulators[i];
        char address[96];
        cstr_format(address, sizeof(address), "%s:%u", s.address, s.port);
        printf("%-8u %-21s %-10s %s\n", s.pid, address, s.version, s.app);
    }
    return CTL_EXIT_OK;
}

// Words split at spaces; a line starting with { goes out as it is
// Connects once the target listens on another port than `skip_port` (0: any), for at most the
// timeout. Unreal has no simulator between its first instance and its second
static b8 connect_when_listening (const CTL_Options& options, u32 skip_port, CTL_Simulator& out_simulator, CTL_Connection& connection)
{
    const u64 deadline = os_monotonic_ns() + (u64)options.timeout_ms * 1000000ull;
    u32       pid      = 0;
    const b8  by_pid   = options.target && parse_u32(options.target, pid);
    while (os_monotonic_ns() < deadline)
    {
        // A crashed app's discovery file outlives it
        if (by_pid && !os_process_alive(pid))
        {
            (void)fprintf(stderr, "puppetxrctl: process %u is not running\n", pid);
            return false;
        }
        CTL_Simulator next = {};
        // A server may go away between its discovery file and the hello, so a failed connect
        // waits for the next like a missing one
        if (resolve_target(options.target, next, false) && next.port != skip_port)
        {
            os_socket_close(connection.socket);
            const i32 next_id = connection.next_id;
            if (connect(next, options.timeout_ms, connection))
            {
                connection.next_id = next_id > 0 ? next_id : connection.next_id;
                out_simulator      = next;
                return true;
            }
            skip_port = next.port;
        }
        os_sleep_ns(50000000);
    }
    CTL_Simulator reported = {};
    (void)resolve_target(options.target, reported, true);
    return false;
}

// An app may destroy its instance and create another, as Unreal does at start; the new instance
// serves on another port, which the target resolves to once its discovery file is written
static b8 follow_target (const CTL_Options& options, CTL_Simulator& inout_simulator, CTL_Connection& connection)
{
    return connect_when_listening(options, inout_simulator.port, inout_simulator, connection);
}

static i32 run_shell (CTL_Connection& connection, CTL_Simulator& simulator, const CTL_Options& options)
{
    char line[CTL_MAX_REQUEST];
    i32  status = CTL_EXIT_OK;
    while (fgets(line, sizeof(line), stdin))
    {
        usize length = cstr_length(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
        {
            line[--length] = 0;
        }
        if (length == 0 || line[0] == '#')
        {
            continue;
        }
        char  request_line[CTL_MAX_REQUEST];
        u32   timeout = options.timeout_ms;
        if (line[0] == '{')
        {
            cstr_copy(request_line, sizeof(request_line), line);
        }
        else
        {
            const char* words[CTL_MAX_WORDS];
            u32         count = 0;
            for (char* c = line; *c && count < CTL_MAX_WORDS;) // NOLINT(misc-const-correctness): false positive, writes through c
            {
                while (*c == ' ' || *c == '\t')
                {
                    *c++ = 0;
                }
                if (*c)
                {
                    words[count++] = c;
                }
                while (*c && *c != ' ' && *c != '\t')
                {
                    c++;
                }
            }
            if (!build_request(count, words, connection.next_id++, request_line, sizeof(request_line), timeout))
            {
                printf("{\"id\":null,\"ok\":false,\"error\":\"usage\",\"message\":\"see puppetxrctl --help\"}\n");
                (void)fflush(stdout);
                status = CTL_EXIT_FAILED;
                continue;
            }
        }
        char reply[CTL_MAX_REPLY];
        if (!request(connection, request_line, timeout, reply, sizeof(reply)) &&
            (!follow_target(options, simulator, connection) || !request(connection, request_line, timeout, reply, sizeof(reply))))
        {
            return CTL_EXIT_CONNECT;
        }
        printf("%s\n", reply);
        (void)fflush(stdout);
        status = reply_ok(reply) ? status : CTL_EXIT_FAILED;
    }
    return status;
}

//
// Entry point
//

int main (int argc, char** argv) // NOLINT(misc-const-correctness): the signature of main
{
    CTL_Options options = { .target = os_env("PUPPETXR_TARGET"), .json = false, .timeout_ms = 10000 };
    i32         first   = 1;
    for (; first < argc && argv[first][0] == '-'; first++)
    {
        const char* flag = argv[first];
        if ((cstr_equal(flag, "--target") || cstr_equal(flag, "-t")) && first + 1 < argc)
        {
            options.target = argv[++first];
        }
        else if (cstr_equal(flag, "--json"))
        {
            options.json = true;
        }
        else if (cstr_equal(flag, "--timeout") && first + 1 < argc && parse_u32(argv[first + 1], options.timeout_ms))
        {
            first++;
        }
        else if (cstr_equal(flag, "--help") || cstr_equal(flag, "-h"))
        {
            printf("%s", ctl_usage);
            return CTL_EXIT_OK;
        }
        else if (cstr_equal(flag, "--version"))
        {
            char version[32];
            version_string(version, sizeof(version));
            printf("puppetxrctl %s\n", version);
            return CTL_EXIT_OK;
        }
        else
        {
            (void)fprintf(stderr, "%s", ctl_usage);
            return CTL_EXIT_USAGE;
        }
    }
    if (first >= argc)
    {
        (void)fprintf(stderr, "%s", ctl_usage);
        return CTL_EXIT_USAGE;
    }
    const char*        command = argv[first];
    const u32          count   = (u32)(argc - first);
    const char* const* words   = (const char* const*)(argv + first);
    if (cstr_equal(command, "list"))
    {
        return run_list(options.json);
    }

    char request_line[CTL_MAX_REQUEST];
    u32  timeout = options.timeout_ms;
    if (cstr_equal(command, "send") && count == 2)
    {
        cstr_copy(request_line, sizeof(request_line), words[1]);
    }
    else if (!cstr_equal(command, "shell") && !build_request(count, words, 1, request_line, sizeof(request_line), timeout))
    {
        (void)fprintf(stderr, "%s", ctl_usage);
        return CTL_EXIT_USAGE;
    }

    CTL_Simulator   simulator = {};
    CTL_Connection* connection = (CTL_Connection*)os_memory_alloc(sizeof(CTL_Connection));
    defer {
        os_socket_close(connection->socket);
        os_memory_release(connection);
    };
    if (cstr_equal(command, "shell"))
    {
        return connect_when_listening(options, 0, simulator, *connection) ? run_shell(*connection, simulator, options) : CTL_EXIT_CONNECT;
    }
    if (!resolve_target(options.target, simulator, true) || !connect(simulator, options.timeout_ms, *connection))
    {
        return CTL_EXIT_CONNECT;
    }
    char reply[CTL_MAX_REPLY];
    if (!request(*connection, request_line, timeout, reply, sizeof(reply)))
    {
        return CTL_EXIT_CONNECT;
    }
    if ((cstr_equal(command, "body") || cstr_equal(command, "face")) && count == 3 && cstr_equal(words[1], "save") && reply_ok(reply))
    {
        return save_body(reply, command, words[2]) ? CTL_EXIT_OK : CTL_EXIT_FAILED;
    }
    print_reply(reply, options.json);
    return reply_ok(reply) ? CTL_EXIT_OK : CTL_EXIT_FAILED;
}
