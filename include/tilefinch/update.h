#ifndef TILEFINCH_UPDATE_H
#define TILEFINCH_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tilefinch/budget.h"
#include "tilefinch/fetch.h"

#define TILEFINCH_UPDATE_PLATFORM_PSP UINT16_C(1)
#define TILEFINCH_UPDATE_PACKAGE_TFUP UINT16_C(1)
#define TILEFINCH_UPDATE_PACKAGE_VOICE UINT16_C(2)
#define TILEFINCH_UPDATE_PACKAGE_GLYPH UINT16_C(3)
#define TILEFINCH_UPDATE_LAUNCHER_PROTOCOL UINT16_C(1)
#define TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES (16u * 1024u)
#define TILEFINCH_UPDATE_MAX_MANIFEST_BYTES 1024u
#define TILEFINCH_UPDATE_MAX_ROOT_ROTATIONS 8u
#define TILEFINCH_UPDATE_MAX_KEYS_PER_ROLE 6u
#define TILEFINCH_UPDATE_MAX_SIGNATURES 12u
#define TILEFINCH_UPDATE_MAX_PACKAGE_BYTES (32u * 1024u * 1024u)
#define TILEFINCH_UPDATE_MAX_PACKAGE_TABLE_BYTES \
    (16u + 16u * 1024u)
#define TILEFINCH_UPDATE_MAX_FILES 64u
#define TILEFINCH_UPDATE_MAX_PATH_BYTES 128u
#define TILEFINCH_UPDATE_MAX_NOTES_BYTES 512u
#define TILEFINCH_UPDATE_DEVELOPER_SEQUENCE UINT64_MAX

typedef enum {
    TILEFINCH_UPDATE_TRUST_SIGNED = 0,
    TILEFINCH_UPDATE_TRUST_DEVELOPER_UNSIGNED = 1
} TilefinchUpdateTrust;

typedef enum {
    TILEFINCH_UPDATE_OK = 0,
    TILEFINCH_UPDATE_INVALID_ARGUMENT,
    TILEFINCH_UPDATE_TRUNCATED,
    TILEFINCH_UPDATE_BAD_MAGIC,
    TILEFINCH_UPDATE_UNSUPPORTED_SCHEMA,
    TILEFINCH_UPDATE_LIMIT,
    TILEFINCH_UPDATE_TRAILING_BYTES,
    TILEFINCH_UPDATE_BAD_STRING,
    TILEFINCH_UPDATE_BAD_KEY,
    TILEFINCH_UPDATE_DUPLICATE_KEY,
    TILEFINCH_UPDATE_BAD_SIGNATURE,
    TILEFINCH_UPDATE_THRESHOLD,
    TILEFINCH_UPDATE_ROOT_CHAIN,
    TILEFINCH_UPDATE_EXPIRED,
    TILEFINCH_UPDATE_CLOCK_UNAVAILABLE,
    TILEFINCH_UPDATE_WRONG_PLATFORM,
    TILEFINCH_UPDATE_LAUNCHER_TOO_OLD,
    TILEFINCH_UPDATE_DOWNGRADE,
    TILEFINCH_UPDATE_EQUIVOCATION,
    TILEFINCH_UPDATE_PACKAGE_MISMATCH,
    TILEFINCH_UPDATE_BAD_PATH,
    TILEFINCH_UPDATE_DUPLICATE_PATH,
    TILEFINCH_UPDATE_IO,
    TILEFINCH_UPDATE_NO_SPACE,
    TILEFINCH_UPDATE_CANCELLED
} TilefinchUpdateStatus;

typedef struct {
    uint8_t id[32];
    uint8_t point[65];
} TilefinchUpdatePublicKey;

typedef struct {
    uint32_t version;
    uint64_t expires_unix;
    uint8_t root_threshold;
    uint8_t release_threshold;
    uint8_t root_key_count;
    uint8_t release_key_count;
    TilefinchUpdatePublicKey root_keys[TILEFINCH_UPDATE_MAX_KEYS_PER_ROLE];
    TilefinchUpdatePublicKey release_keys[TILEFINCH_UPDATE_MAX_KEYS_PER_ROLE];
} TilefinchUpdateRoot;

typedef struct {
    uint32_t root_version;
    uint64_t release_sequence;
    uint64_t expires_unix;
    uint16_t minimum_launcher_protocol;
    uint16_t package_format;
    uint64_t package_size;
    uint8_t package_sha256[32];
    char version[32];
    char tag[64];
    char asset[96];
    char notes[TILEFINCH_UPDATE_MAX_NOTES_BYTES + 1u];
    size_t notes_length;
    /* Optional, signed compatibility epoch carried in the existing notes
       field. Zero/false is valid for older manifests. */
    uint16_t optional_decoder_abi;
    bool optional_decoder_abi_valid;
} TilefinchUpdateManifest;

typedef bool (*TilefinchUpdateP256Verify)(
    void *opaque, const uint8_t public_point[65],
    const uint8_t digest[32], const uint8_t signature[64]);

typedef struct {
    TilefinchUpdateP256Verify verify;
    void *opaque;
} TilefinchUpdateCrypto;

typedef struct {
    const TilefinchUpdateRoot *embedded_root;
    TilefinchUpdateCrypto crypto;
    uint64_t now_unix;
    bool clock_valid;
    uint16_t launcher_protocol;
    uint64_t installed_sequence;
    uint8_t installed_package_sha256[32];
    bool installed_sequence_valid;
    bool installed_pair_valid;
    /* Optional distribution tag selected locally by the user. A valid
       signature is still rejected when its manifest names another tag. */
    const char *expected_tag;
    /* Explicit local selection of a historical signed release. This relaxes
       only the monotonic sequence comparison; all other verification stays
       authoritative. */
    bool allow_downgrade;
} TilefinchUpdateVerifyOptions;

typedef struct {
    TilefinchUpdateRoot terminal_root;
    TilefinchUpdateManifest manifest;
    uint8_t manifest_digest[32];
} TilefinchUpdateVerifiedEnvelope;

const char *tilefinch_update_status_name(TilefinchUpdateStatus status);
bool tilefinch_update_query_free_space(
    const char *directory, uint64_t *available);
bool tilefinch_update_key_id(
    const uint8_t public_point[65], uint8_t output[32]);
bool tilefinch_update_signature_is_low_s(const uint8_t signature[64]);
TilefinchUpdateStatus tilefinch_update_parse_root(
    const uint8_t *bytes, size_t length, TilefinchUpdateRoot *root);
TilefinchUpdateStatus tilefinch_update_verify_envelope(
    const uint8_t *bytes, size_t length,
    const TilefinchUpdateVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified);
TilefinchUpdateStatus tilefinch_update_verify_voice_envelope(
    const uint8_t *bytes, size_t length,
    const TilefinchUpdateVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified);
TilefinchUpdateStatus tilefinch_update_verify_glyph_envelope(
    const uint8_t *bytes, size_t length,
    const TilefinchUpdateVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified);
/*
 * Parse the deliberately unsigned, locally opted-in Developer envelope.
 * It must contain no root rotations and no signatures. Package bounds,
 * platform, launcher compatibility, size and digests remain authoritative.
 */
TilefinchUpdateStatus tilefinch_update_parse_developer_envelope(
    const uint8_t *bytes, size_t length, uint16_t launcher_protocol,
    TilefinchUpdateVerifiedEnvelope *verified);
TilefinchUpdateCrypto tilefinch_update_default_crypto(void);
bool tilefinch_update_embedded_root(TilefinchUpdateRoot *root);
bool tilefinch_update_root_is_configured(void);

typedef struct {
    char path[TILEFINCH_UPDATE_MAX_PATH_BYTES + 1u];
    uint64_t size;
    uint64_t payload_offset;
    uint8_t sha256[32];
} TilefinchUpdatePackageEntry;

typedef struct {
    uint16_t file_count;
    uint32_t table_length;
    uint64_t payload_start;
    TilefinchUpdatePackageEntry entries[TILEFINCH_UPDATE_MAX_FILES];
} TilefinchUpdatePackage;

TilefinchUpdateStatus tilefinch_update_parse_package_table(
    const uint8_t *bytes, size_t length, uint64_t package_size,
    TilefinchUpdatePackage *package);
bool tilefinch_update_package_path_allowed(const char *path);
TilefinchUpdateStatus tilefinch_update_parse_voice_package_table(
    const uint8_t *bytes, size_t length, uint64_t package_size,
    TilefinchUpdatePackage *package);
bool tilefinch_update_voice_package_path_allowed(const char *path);

typedef enum {
    TILEFINCH_UPDATE_SLOT_NONE = 0,
    TILEFINCH_UPDATE_SLOT_A = 1,
    TILEFINCH_UPDATE_SLOT_B = 2
} TilefinchUpdateSlot;

typedef enum {
    TILEFINCH_UPDATE_TRIAL_NONE = 0,
    TILEFINCH_UPDATE_TRIAL_PENDING = 1,
    TILEFINCH_UPDATE_TRIAL_STARTED = 2
} TilefinchUpdateTrialState;

typedef struct {
    uint64_t generation;
    TilefinchUpdateSlot active_slot;
    TilefinchUpdateSlot previous_slot;
    TilefinchUpdateSlot pending_slot;
    TilefinchUpdateTrialState trial;
    uint64_t installed_sequence;
    uint8_t installed_sha256[32];
    uint64_t previous_sequence;
    uint8_t previous_sha256[32];
    uint64_t candidate_sequence;
    uint8_t candidate_sha256[32];
    bool candidate_downgrade;
} TilefinchUpdateState;

#define TILEFINCH_UPDATE_STATE_BYTES 174u

TilefinchUpdateStatus tilefinch_update_state_encode(
    const TilefinchUpdateState *state,
    uint8_t output[TILEFINCH_UPDATE_STATE_BYTES]);
TilefinchUpdateStatus tilefinch_update_state_decode(
    const uint8_t *bytes, size_t length, TilefinchUpdateState *state);
bool tilefinch_update_state_select(
    const uint8_t *first, size_t first_length,
    const uint8_t *second, size_t second_length,
    TilefinchUpdateState *state, unsigned *selected_copy);

typedef bool (*TilefinchUpdateFaultHook)(
    void *opaque, const char *operation);

bool tilefinch_update_journal_load(
    const char *data_dir, TilefinchUpdateState *state,
    unsigned *selected_copy);
bool tilefinch_update_journal_store(
    const char *data_dir, const TilefinchUpdateState *state,
    TilefinchUpdateFaultHook fault, void *fault_opaque);

typedef enum {
    TILEFINCH_UPDATE_BOOT_ACTIVE = 0,
    TILEFINCH_UPDATE_BOOT_PREVIOUS,
    TILEFINCH_UPDATE_BOOT_START_TRIAL,
    TILEFINCH_UPDATE_BOOT_RECOVERY
} TilefinchUpdateBootAction;

TilefinchUpdateBootAction tilefinch_update_boot_decide(
    const TilefinchUpdateState *state, bool recovery_button,
    bool pending_slot_verified, TilefinchUpdateSlot *slot);
bool tilefinch_update_state_start_trial(TilefinchUpdateState *state);
bool tilefinch_update_state_retry_trial(TilefinchUpdateState *state);
bool tilefinch_update_state_discard_trial(TilefinchUpdateState *state);
bool tilefinch_update_state_confirm_healthy(TilefinchUpdateState *state);
bool tilefinch_update_state_raise_installed_floor(
    TilefinchUpdateState *state, TilefinchUpdateSlot running_slot,
    uint64_t release_sequence);

typedef struct {
    const TilefinchUpdateRoot *embedded_root;
    uint64_t now_unix;
    bool clock_valid;
    uint16_t launcher_protocol;
    uint64_t installed_sequence;
    const uint8_t *installed_package_sha256;
    bool installed_sequence_valid;
    bool installed_pair_valid;
    bool allow_downgrade;
    TilefinchUpdateTrust trust;
} TilefinchUpdateSlotVerifyOptions;

TilefinchUpdateStatus tilefinch_update_verify_slot(
    const char *slot_dir, const TilefinchUpdateSlotVerifyOptions *options,
    TilefinchUpdateVerifiedEnvelope *verified);
bool tilefinch_update_slot_is_developer(const char *slot_dir);

typedef enum {
    TILEFINCH_UPDATE_CLIENT_IDLE = 0,
    TILEFINCH_UPDATE_CLIENT_CHECKING,
    TILEFINCH_UPDATE_CLIENT_AVAILABLE,
    TILEFINCH_UPDATE_CLIENT_DOWNLOADING,
    TILEFINCH_UPDATE_CLIENT_CANCELLING,
    TILEFINCH_UPDATE_CLIENT_DOWNLOADED,
    TILEFINCH_UPDATE_CLIENT_UP_TO_DATE,
    TILEFINCH_UPDATE_CLIENT_ERROR
} TilefinchUpdateClientPhase;

typedef struct TilefinchUpdateClient TilefinchUpdateClient;

typedef enum {
    TILEFINCH_UPDATE_ARTIFACT_BROWSER = 0,
    TILEFINCH_UPDATE_ARTIFACT_VOICE_COMPONENT = 1,
    TILEFINCH_UPDATE_ARTIFACT_GLYPH_COMPONENT = 2
} TilefinchUpdateArtifact;

typedef struct {
    const TilefinchUpdateRoot *embedded_root;
    uint64_t installed_sequence;
    const uint8_t *installed_package_sha256;
    bool installed_sequence_valid;
    bool installed_pair_valid;
    uint16_t launcher_protocol;
    const char *repository_owner;
    const char *repository_name;
    /* Optional fixed GitHub tag, such as v0.1.4. Signed browser updates only. */
    const char *release_tag;
    /* NULL keeps the fixed Stable GitHub endpoint. Developer requires a
       non-NULL metadata override and the explicit DEVELOPER_UNSIGNED trust
       value; no failure path may infer that mode. A separate package URL is
       optional for unrelated public share links such as OneDrive. */
    const char *metadata_url_override;
    const char *package_url_override;
    bool package_relative_to_metadata;
    bool allow_downgrade;
    TilefinchUpdateTrust trust;
    const char *package_part_path;
    /* Required for glyph components; for example
       tilefinch-glyph-ja-v1.tfgm. The client validates this as a bounded
       GitHub asset name rather than accepting a URL. */
    const char *metadata_asset;
    TilefinchUpdateArtifact artifact;
} TilefinchUpdateClientOptions;

typedef struct {
    TilefinchUpdateClientPhase phase;
    TilefinchUpdateStatus status;
    TilefinchUpdateManifest manifest;
    uint64_t bytes_received;
    uint64_t bytes_total;
    uint8_t manifest_digest[32];
    char message[96];
} TilefinchUpdateClientSnapshot;

TilefinchUpdateClient *tilefinch_update_client_create(
    Budget *budget, const TilefinchUpdateClientOptions *options);
void tilefinch_update_client_destroy(TilefinchUpdateClient *client);
bool tilefinch_update_client_begin_check(
    TilefinchUpdateClient *client, uint64_t now_unix, bool clock_valid);
bool tilefinch_update_client_begin_download(TilefinchUpdateClient *client);
bool tilefinch_update_client_cancel(TilefinchUpdateClient *client);
bool tilefinch_update_client_pump(
    TilefinchUpdateClient *client, uint64_t maximum_time_us);
bool tilefinch_update_client_snapshot(
    const TilefinchUpdateClient *client,
    TilefinchUpdateClientSnapshot *snapshot);
const uint8_t *tilefinch_update_client_envelope(
    const TilefinchUpdateClient *client, size_t *length);

/* Bounded updater URL policy shared by boot configuration and the client. */
bool tilefinch_update_url_is_valid(const char *url, size_t capacity);
/* Converts recognized public OneDrive/SharePoint share links to download
   mode. Other valid URLs are copied unchanged. */
bool tilefinch_update_prepare_download_url(
    const char *url, char *output, size_t capacity);

typedef enum {
    TILEFINCH_UPDATE_INSTALL_VERIFYING = 0,
    TILEFINCH_UPDATE_INSTALL_READING_TABLE,
    TILEFINCH_UPDATE_INSTALL_PREPARING,
    TILEFINCH_UPDATE_INSTALL_EXTRACTING,
    TILEFINCH_UPDATE_INSTALL_FINALIZING,
    TILEFINCH_UPDATE_INSTALL_PROMOTING,
    TILEFINCH_UPDATE_INSTALL_COMPLETE,
    TILEFINCH_UPDATE_INSTALL_ERROR,
    TILEFINCH_UPDATE_INSTALL_CANCELLED
} TilefinchUpdateInstallPhase;

typedef struct TilefinchUpdateInstallJob TilefinchUpdateInstallJob;

typedef struct {
    const char *package_path;
    const uint8_t *envelope;
    size_t envelope_length;
    const TilefinchUpdateManifest *manifest;
    const uint8_t *manifest_digest;
    const char *install_root;
    const char *data_dir;
    TilefinchUpdateSlot inactive_slot;
    TilefinchUpdateState current_state;
    TilefinchUpdateTrust trust;
    bool allow_downgrade;
    TilefinchUpdateFaultHook fault;
    void *fault_opaque;
} TilefinchUpdateInstallOptions;

typedef struct {
    TilefinchUpdateInstallPhase phase;
    TilefinchUpdateStatus status;
    uint64_t bytes_processed;
    uint64_t bytes_total;
    size_t files_completed;
    size_t files_total;
    char message[96];
} TilefinchUpdateInstallSnapshot;

TilefinchUpdateInstallJob *tilefinch_update_install_create(
    Budget *budget, const TilefinchUpdateInstallOptions *options);
void tilefinch_update_install_destroy(TilefinchUpdateInstallJob *job);
bool tilefinch_update_install_cancel(TilefinchUpdateInstallJob *job);
bool tilefinch_update_install_pump(
    TilefinchUpdateInstallJob *job, size_t maximum_bytes);
bool tilefinch_update_install_snapshot(
    const TilefinchUpdateInstallJob *job,
    TilefinchUpdateInstallSnapshot *snapshot);

#endif
