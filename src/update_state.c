#include "tilefinch/update.h"

#include <string.h>

#include "tilefinch/sha256.h"

static void state_put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

static void state_put_u64(uint8_t *bytes, uint64_t value)
{
    for (size_t index = 0; index < 8; index++) {
        bytes[7u - index] = (uint8_t) value;
        value >>= 8;
    }
}

static uint64_t state_u64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (size_t index = 0; index < 8; index++)
        value = value << 8 | bytes[index];
    return value;
}

TilefinchUpdateStatus tilefinch_update_state_encode(
    const TilefinchUpdateState *state,
    uint8_t output[TILEFINCH_UPDATE_STATE_BYTES])
{
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'S', 'v', '1', 0, 0
    };
    if (state == NULL || output == NULL
        || (state->active_slot != TILEFINCH_UPDATE_SLOT_A
            && state->active_slot != TILEFINCH_UPDATE_SLOT_B)
        || state->previous_slot > TILEFINCH_UPDATE_SLOT_B
        || state->pending_slot > TILEFINCH_UPDATE_SLOT_B
        || state->trial > TILEFINCH_UPDATE_TRIAL_STARTED
        || ((state->pending_slot == TILEFINCH_UPDATE_SLOT_NONE)
            != (state->trial == TILEFINCH_UPDATE_TRIAL_NONE))
        || (state->candidate_downgrade
            && (state->pending_slot == TILEFINCH_UPDATE_SLOT_NONE
                || state->candidate_sequence
                       == TILEFINCH_UPDATE_DEVELOPER_SEQUENCE
                || state->candidate_sequence
                       >= state->installed_sequence))) {
        return TILEFINCH_UPDATE_INVALID_ARGUMENT;
    }
    memset(output, 0, TILEFINCH_UPDATE_STATE_BYTES);
    memcpy(output, magic, sizeof(magic));
    state_put_u16(output + 8, 1);
    state_put_u64(output + 10, state->generation);
    output[18] = (uint8_t) state->active_slot;
    output[19] = (uint8_t) state->previous_slot;
    output[20] = (uint8_t) state->pending_slot;
    output[21] = (uint8_t) state->trial
        | (state->candidate_downgrade ? 0x80u : 0u);
    state_put_u64(output + 22, state->installed_sequence);
    memcpy(output + 30, state->installed_sha256, 32);
    state_put_u64(output + 62, state->previous_sequence);
    memcpy(output + 70, state->previous_sha256, 32);
    state_put_u64(output + 102, state->candidate_sequence);
    memcpy(output + 110, state->candidate_sha256, 32);
    if (!tilefinch_sha256_digest(output, 142, output + 142))
        return TILEFINCH_UPDATE_IO;
    return TILEFINCH_UPDATE_OK;
}

TilefinchUpdateStatus tilefinch_update_state_decode(
    const uint8_t *bytes, size_t length, TilefinchUpdateState *state)
{
    static const uint8_t magic[8] = {
        'T', 'F', 'U', 'S', 'v', '1', 0, 0
    };
    if (bytes == NULL || state == NULL)
        return TILEFINCH_UPDATE_INVALID_ARGUMENT;
    if (length != TILEFINCH_UPDATE_STATE_BYTES)
        return TILEFINCH_UPDATE_TRUNCATED;
    if (memcmp(bytes, magic, sizeof(magic)) != 0)
        return TILEFINCH_UPDATE_BAD_MAGIC;
    if (((uint16_t) bytes[8] << 8 | bytes[9]) != 1)
        return TILEFINCH_UPDATE_UNSUPPORTED_SCHEMA;
    if ((bytes[21] & 0x7cu) != 0)
        return TILEFINCH_UPDATE_BAD_STRING;
    uint8_t digest[32];
    if (!tilefinch_sha256_digest(bytes, 142, digest)
        || memcmp(digest, bytes + 142, 32) != 0)
        return TILEFINCH_UPDATE_PACKAGE_MISMATCH;
    TilefinchUpdateState parsed = {
        .generation = state_u64(bytes + 10),
        .active_slot = (TilefinchUpdateSlot) bytes[18],
        .previous_slot = (TilefinchUpdateSlot) bytes[19],
        .pending_slot = (TilefinchUpdateSlot) bytes[20],
        .trial = (TilefinchUpdateTrialState) (bytes[21] & 0x7fu),
        .installed_sequence = state_u64(bytes + 22),
        .previous_sequence = state_u64(bytes + 62),
        .candidate_sequence = state_u64(bytes + 102),
        .candidate_downgrade = (bytes[21] & 0x80u) != 0
    };
    memcpy(parsed.installed_sha256, bytes + 30, 32);
    memcpy(parsed.previous_sha256, bytes + 70, 32);
    memcpy(parsed.candidate_sha256, bytes + 110, 32);
    if ((parsed.active_slot != TILEFINCH_UPDATE_SLOT_A
         && parsed.active_slot != TILEFINCH_UPDATE_SLOT_B)
        || parsed.previous_slot > TILEFINCH_UPDATE_SLOT_B
        || parsed.pending_slot > TILEFINCH_UPDATE_SLOT_B
        || parsed.trial > TILEFINCH_UPDATE_TRIAL_STARTED
        || ((parsed.pending_slot == TILEFINCH_UPDATE_SLOT_NONE)
            != (parsed.trial == TILEFINCH_UPDATE_TRIAL_NONE))
        || (parsed.candidate_downgrade
            && (parsed.pending_slot == TILEFINCH_UPDATE_SLOT_NONE
                || parsed.candidate_sequence
                       == TILEFINCH_UPDATE_DEVELOPER_SEQUENCE
                || parsed.candidate_sequence
                       >= parsed.installed_sequence))) {
        return TILEFINCH_UPDATE_BAD_STRING;
    }
    *state = parsed;
    return TILEFINCH_UPDATE_OK;
}

bool tilefinch_update_state_select(
    const uint8_t *first, size_t first_length,
    const uint8_t *second, size_t second_length,
    TilefinchUpdateState *state, unsigned *selected_copy)
{
    TilefinchUpdateState decoded[2];
    bool valid[2] = {
        tilefinch_update_state_decode(
            first, first_length, &decoded[0]) == TILEFINCH_UPDATE_OK,
        tilefinch_update_state_decode(
            second, second_length, &decoded[1]) == TILEFINCH_UPDATE_OK
    };
    if (!valid[0] && !valid[1]) return false;
    unsigned selected = !valid[0] ? 1u
        : (!valid[1] || decoded[0].generation >= decoded[1].generation
               ? 0u : 1u);
    if (state != NULL) *state = decoded[selected];
    if (selected_copy != NULL) *selected_copy = selected;
    return true;
}

TilefinchUpdateBootAction tilefinch_update_boot_decide(
    const TilefinchUpdateState *state, bool recovery_button,
    bool pending_slot_verified, TilefinchUpdateSlot *slot)
{
    if (state == NULL) return TILEFINCH_UPDATE_BOOT_RECOVERY;
    if (recovery_button
        && state->previous_slot != TILEFINCH_UPDATE_SLOT_NONE) {
        if (slot != NULL) *slot = state->previous_slot;
        return TILEFINCH_UPDATE_BOOT_PREVIOUS;
    }
    if (state->trial == TILEFINCH_UPDATE_TRIAL_STARTED) {
        if (slot != NULL) *slot = state->active_slot;
        return TILEFINCH_UPDATE_BOOT_RECOVERY;
    }
    if (state->trial == TILEFINCH_UPDATE_TRIAL_PENDING) {
        if (!pending_slot_verified) {
            if (slot != NULL) *slot = state->active_slot;
            return TILEFINCH_UPDATE_BOOT_RECOVERY;
        }
        if (slot != NULL) *slot = state->pending_slot;
        return TILEFINCH_UPDATE_BOOT_START_TRIAL;
    }
    if (slot != NULL) *slot = state->active_slot;
    return TILEFINCH_UPDATE_BOOT_ACTIVE;
}

bool tilefinch_update_state_start_trial(TilefinchUpdateState *state)
{
    if (state == NULL || state->trial != TILEFINCH_UPDATE_TRIAL_PENDING
        || state->pending_slot == TILEFINCH_UPDATE_SLOT_NONE
        || state->generation == UINT64_MAX) return false;
    state->generation++;
    state->trial = TILEFINCH_UPDATE_TRIAL_STARTED;
    /* The current launcher has already verified and consumed the explicit
       downgrade authorization. Clear its encoding before the historical
       browser starts: older schema-1 readers treat bit 7 of the trial byte
       as part of the enum and would otherwise reject both journal copies,
       preventing the trial from ever confirming healthy. */
    state->candidate_downgrade = false;
    return true;
}

bool tilefinch_update_state_retry_trial(TilefinchUpdateState *state)
{
    if (state == NULL || state->trial != TILEFINCH_UPDATE_TRIAL_STARTED
        || state->pending_slot == TILEFINCH_UPDATE_SLOT_NONE
        || state->generation == UINT64_MAX) return false;
    state->generation++;
    state->trial = TILEFINCH_UPDATE_TRIAL_PENDING;
    /* START_TRIAL clears the compatibility-breaking journal marker before
       entering historical code. A user-requested retry returns to the
       current launcher, which can reconstruct the authorization from the
       pinned candidate/floor pair before verifying that slot again. */
    state->candidate_downgrade =
        state->candidate_sequence != TILEFINCH_UPDATE_DEVELOPER_SEQUENCE
        && state->candidate_sequence < state->installed_sequence;
    return true;
}

bool tilefinch_update_state_discard_trial(TilefinchUpdateState *state)
{
    if (state == NULL || state->pending_slot == TILEFINCH_UPDATE_SLOT_NONE
        || state->generation == UINT64_MAX) return false;
    state->generation++;
    state->pending_slot = TILEFINCH_UPDATE_SLOT_NONE;
    state->trial = TILEFINCH_UPDATE_TRIAL_NONE;
    state->candidate_sequence = 0;
    memset(state->candidate_sha256, 0, 32);
    state->candidate_downgrade = false;
    return true;
}

bool tilefinch_update_state_confirm_healthy(TilefinchUpdateState *state)
{
    if (state == NULL || state->trial != TILEFINCH_UPDATE_TRIAL_STARTED
        || state->pending_slot == TILEFINCH_UPDATE_SLOT_NONE
        || state->generation == UINT64_MAX) return false;
    TilefinchUpdateSlot old_slot = state->active_slot;
    uint64_t old_sequence = state->installed_sequence;
    uint8_t old_sha256[32];
    memcpy(old_sha256, state->installed_sha256, 32);
    state->generation++;
    state->active_slot = state->pending_slot;
    /* An unsigned Developer trial changes the active slot but must not move
       or replace the anti-rollback floor established by signed releases. */
    if (state->candidate_sequence != TILEFINCH_UPDATE_DEVELOPER_SEQUENCE) {
        state->installed_sequence = state->candidate_sequence;
        memcpy(state->installed_sha256, state->candidate_sha256, 32);
    }
    state->previous_slot = old_slot;
    state->previous_sequence = old_sequence;
    memcpy(state->previous_sha256, old_sha256, 32);
    state->pending_slot = TILEFINCH_UPDATE_SLOT_NONE;
    state->trial = TILEFINCH_UPDATE_TRIAL_NONE;
    state->candidate_sequence = 0;
    memset(state->candidate_sha256, 0, 32);
    state->candidate_downgrade = false;
    return true;
}

bool tilefinch_update_state_raise_installed_floor(
    TilefinchUpdateState *state, TilefinchUpdateSlot running_slot,
    uint64_t release_sequence)
{
    if (state == NULL
        || (running_slot != TILEFINCH_UPDATE_SLOT_A
            && running_slot != TILEFINCH_UPDATE_SLOT_B)
        || state->active_slot != running_slot
        || state->trial != TILEFINCH_UPDATE_TRIAL_NONE
        || state->pending_slot != TILEFINCH_UPDATE_SLOT_NONE
        || release_sequence <= state->installed_sequence
        || state->generation == UINT64_MAX) {
        return false;
    }
    state->generation++;
    state->installed_sequence = release_sequence;
    /*
     * The running browser knows its compiled sequence but cannot derive the
     * exact signed TFUP hash from an already-extracted initial/manual slot.
     * Keep equivocation disabled until the next signed trial supplies that
     * pair; the monotonic downgrade floor remains enforceable immediately.
     */
    memset(state->installed_sha256, 0, sizeof(state->installed_sha256));
    return true;
}
