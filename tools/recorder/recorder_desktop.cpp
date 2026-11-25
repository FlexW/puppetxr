// The motion recorder on the desktop, through the OpenXR loader (docs/recorder.md)

#include "recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openxr/openxr.h>

static bool parse_options (int argc, const char* const* argv, RC_Options& out)
{
    out = { .get_instance_proc_addr = (void*)&xrGetInstanceProcAddr, .out_dir = ".", .space = RC_Space::LocalFloor, .passthrough = true, .countdown_seconds = 3.0f };
    for (int i = 1; i < argc; i++)
    {
        const bool has_value = i + 1 < argc;
        if (strcmp(argv[i], "--out") == 0 && has_value)
        {
            out.out_dir = argv[++i];
        }
        else if (strcmp(argv[i], "--protocol") == 0 && has_value)
        {
            out.protocol = argv[++i];
        }
        else if (strcmp(argv[i], "--space") == 0 && has_value && (strcmp(argv[i + 1], "stage") == 0 || strcmp(argv[i + 1], "local_floor") == 0))
        {
            out.space = strcmp(argv[++i], "stage") == 0 ? RC_Space::Stage : RC_Space::LocalFloor;
        }
        else if (strcmp(argv[i], "--countdown") == 0 && has_value)
        {
            out.countdown_seconds = strtof(argv[++i], nullptr);
        }
        else if (strcmp(argv[i], "--auto") == 0)
        {
            out.automatic = true;
        }
        else if (strcmp(argv[i], "--no-passthrough") == 0)
        {
            out.passthrough = false;
        }
        else
        {
            (void)fprintf(stderr, "usage: puppetxr-recorder [--out DIR] [--protocol FILE] [--space local_floor|stage] [--countdown S] [--auto] [--no-passthrough]\n");
            return false;
        }
    }
    return true;
}

int main (int argc, char** argv) // NOLINT(misc-const-correctness): the signature of main
{
    RC_Options options = {};
    if (!parse_options(argc, argv, options))
    {
        return 2;
    }
    RC_App* app = rc_create(options);
    if (!app)
    {
        return 1;
    }
    RC_Status status = RC_Status::Running;
    while (status == RC_Status::Running)
    {
        status = rc_frame(app);
    }
    rc_destroy(app);
    return status == RC_Status::Ended ? 0 : 1;
}
