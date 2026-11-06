/* Callback-safe stop requests and observations. Retirement and Budget changes
   remain on the browser thread; neither worker may be omitted from teardown. */
static void psp_network_workers_request_quiesce(void)
{
    fetch_background_transport_request_quiesce();
    psp_multiplayer_network_request_quiesce();
}

static bool psp_network_workers_is_quiesced(void)
{
    return fetch_background_transport_is_quiesced()
        && psp_multiplayer_network_quiesced();
}
