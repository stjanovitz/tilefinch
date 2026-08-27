#include "tilefinch/xmb_redirect_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const uint32_t left_trigger = 0x0100u;

    assert(tilefinch_xmb_redirect_should_attempt(
        TILEFINCH_XMB_BROWSER_MODULE,
        true, 0x4000u, 0x4000u | 0x2000u, left_trigger, false));
    assert(tilefinch_xmb_redirect_module_matches(
        TILEFINCH_XMB_BROWSER_MODULE, false));
    assert(!tilefinch_xmb_redirect_module_matches(
        TILEFINCH_XMB_BROWSER_MODULE, true));
    assert(!tilefinch_xmb_redirect_module_matches(
        "htmlviewer_plugin_module_extra", false));
    assert(!tilefinch_xmb_redirect_should_attempt(
        TILEFINCH_XMB_BROWSER_MODULE,
        true, 0x4000u | left_trigger,
        0x4000u | 0x2000u, left_trigger, false));
    assert(!tilefinch_xmb_redirect_should_attempt(
        TILEFINCH_XMB_BROWSER_MODULE,
        false, 0x4000u, 0x4000u | 0x2000u, left_trigger, false));
    assert(!tilefinch_xmb_redirect_should_attempt(
        TILEFINCH_XMB_BROWSER_MODULE,
        true, 0u, 0x4000u | 0x2000u, left_trigger, false));
    assert(!tilefinch_xmb_redirect_should_attempt(
        TILEFINCH_XMB_BROWSER_MODULE,
        true, 0x2000u, 0x4000u | 0x2000u, left_trigger, true));
    assert(!tilefinch_xmb_redirect_should_attempt(
        "htmlviewer_utility_module",
        true, 0x4000u, 0x4000u | 0x2000u, left_trigger, false));
    assert(!tilefinch_xmb_redirect_should_attempt(
        "htmlviewer_plugin_module_extra",
        true, 0x4000u, 0x4000u | 0x2000u, left_trigger, false));
    assert(!tilefinch_xmb_redirect_should_attempt(
        NULL, true, 0x4000u,
        0x4000u | 0x2000u, left_trigger, false));

    puts("xmb redirect policy tests passed");
    return 0;
}
