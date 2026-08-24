#include "tilefinch/xmb_redirect_policy.h"

#include <stddef.h>

static bool bounded_string_equal(const char *left, const char *right)
{
    if (left == NULL || right == NULL) return false;
    for (unsigned int index = 0; index < 64u; index++) {
        if (left[index] != right[index]) return false;
        if (left[index] == '\0') return true;
    }
    return false;
}

bool tilefinch_xmb_redirect_should_attempt(
    const char *module_name,
    bool controller_sample_valid,
    uint32_t buttons,
    uint32_t bypass_button,
    bool launch_already_armed)
{
    return controller_sample_valid
        && (buttons & bypass_button) == 0u
        && tilefinch_xmb_redirect_module_matches(
            module_name, launch_already_armed);
}

bool tilefinch_xmb_redirect_module_matches(
    const char *module_name, bool launch_already_armed)
{
    return !launch_already_armed
        && bounded_string_equal(
            module_name, TILEFINCH_XMB_BROWSER_MODULE);
}
