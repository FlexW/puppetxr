/* Compiled as strict C89 so the public header stays valid C89 */
#include <puppetxr.h>

static void log_message (PUPPETXR_LogLevel level, const char* message, void* user)
{
    (void)level;
    (void)message;
    (void)user;
}

int puppetxr_header_is_c (void)
{
    PUPPETXR_LogCallback callback = log_message;
    PUPPETXR_Api api    = { 0 };
    PUPPETXR_Status status = { sizeof(PUPPETXR_Status) };
    PUPPETXR_Pose pose  = { { 0.0f, 1.6f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f } };
    PUPPETXR_ControlMode mode = PUPPETXR_CONTROL_MODE_BODY;
    (void)mode;
    (void)callback;
    (void)status;
    (void)pose;
    return puppetxr_load_api(&api) == PUPPETXR_ERROR_NOT_LOADED && PUPPETXR_VERSION_GET_MAJOR(PUPPETXR_MAKE_VERSION(1, 2, 3)) == 1;
}
