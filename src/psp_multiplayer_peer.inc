/* Worker-owned peer decision transitions, shared with host event-order tests. */
static void multiplayer_consider_candidate(
    PspMultiplayerSession *session, const struct sockaddr_in *source,
    uint32_t sender, const unsigned char *payload, size_t payload_length)
{
    /* One page approval prompt owns this slot until rejection or termination.
       A later HELLO must not change which peer an already queued answer means. */
    if (!session->opened
        && session->candidate_state == PSP_MULTIPLAYER_CANDIDATE_NONE) {
        char name[TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT + 1u];
        size_t copy = payload_length < TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT
            ? payload_length : TILEFINCH_MULTIPLAYER_PEER_NAME_LIMIT;
        memcpy(name, payload, copy);
        name[copy] = '\0';
        if (!multiplayer_emit_peer(
            session, TILEFINCH_MULTIPLAYER_EVENT_PEER_REQUEST,
            source, sender, name, "incoming")) return;
        /* The worker alone applies decisions, after receive returns. Refused
           notification leaves NONE so the peer's next HELLO can retry. */
        session->candidate = *source;
        session->candidate_nonce = sender;
        memcpy(session->candidate_name, name, copy + 1u);
        session->candidate_state = PSP_MULTIPLAYER_CANDIDATE_REPORTED;
    }
}

static void multiplayer_apply_peer_approval(
    PspMultiplayerSession *session, uint32_t peer_identity, bool accepted)
{
    if (!session->opened
        && session->candidate_state != PSP_MULTIPLAYER_CANDIDATE_NONE
        && peer_identity == session->candidate_nonce) {
        if (!accepted) {
            session->candidate_state = PSP_MULTIPLAYER_CANDIDATE_NONE;
            session->remote_valid = false;
            session->remote_nonce = 0;
            session->cookie = 0;
        } else {
            uint32_t cookie = session->cookie_secret
                ^ session->local_nonce ^ session->candidate_nonce
                ^ ntohl(session->candidate.sin_addr.s_addr)
                ^ (uint32_t) ntohs(session->candidate.sin_port);
            session->cookie = cookie == 0 ? 1u : cookie;
            session->remote = session->candidate;
            session->remote_valid = true;
            session->remote_nonce = session->candidate_nonce;
            session->candidate_state = PSP_MULTIPLAYER_CANDIDATE_ACCEPTED;
            session->last_handshake_us = 0;
        }
    }
}

static bool multiplayer_candidate_confirms(
    const PspMultiplayerSession *session, const struct sockaddr_in *source,
    uint32_t sender, uint32_t receiver, uint32_t cookie)
{
    return session->candidate_state == PSP_MULTIPLAYER_CANDIDATE_ACCEPTED
        && session->remote_valid
        && multiplayer_same_endpoint(source, &session->remote)
        && sender == session->remote_nonce
        && receiver == session->local_nonce && cookie == session->cookie;
}
