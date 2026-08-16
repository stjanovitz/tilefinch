#include "tilefinch/budget.h"
#include "tilefinch/browser_profile.h"
#include "tilefinch/font.h"
#include "tilefinch/glyph_component.h"
#include "tilefinch/glyph_component_store.h"
#include "tilefinch/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "check failed at %s:%d: %s\n",                     \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

enum {
    HEADER_BYTES = 80,
    DIRECTORY_BYTES = TILEFINCH_GLYPH_COMPONENT_PAGE_COUNT * 2,
    PAGE_BYTES = 36,
    SEQUENCE_BYTES = 40,
    GLYPH_BYTES = 32
};

static void write_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t) (value >> 8);
    bytes[1] = (uint8_t) value;
}

static void write_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}

static bool write_fixture(char path[128])
{
    static const char id[] = "glyph-ja";
    const size_t pages_offset = HEADER_BYTES + DIRECTORY_BYTES;
    const size_t sequences_offset = pages_offset + PAGE_BYTES * 2u;
    const size_t payload_offset = sequences_offset + SEQUENCE_BYTES * 2u;
    const size_t file_size = payload_offset + GLYPH_BYTES * 4u;
    uint8_t *bytes = calloc(file_size, 1);
    CHECK(bytes != NULL);

    memcpy(bytes, "TFGFv1\0\0", 8);
    write_u16(bytes + 8, TILEFINCH_GLYPH_COMPONENT_ABI);
    bytes[10] = TILEFINCH_GLYPH_COMPONENT_MONO;
    bytes[11] = 16;
    bytes[12] = 16;
    bytes[13] = 2;
    bytes[14] = (uint8_t) strlen(id);
    write_u32(bytes + 16, 4);
    write_u16(bytes + 20, 2);
    write_u16(bytes + 22, 2);
    write_u32(bytes + 24, HEADER_BYTES);
    write_u32(bytes + 28, (uint32_t) pages_offset);
    write_u32(bytes + 32, (uint32_t) sequences_offset);
    write_u32(bytes + 36, (uint32_t) payload_offset);
    write_u32(bytes + 40, (uint32_t) file_size);
    memcpy(bytes + 44, id, sizeof(id));

    memset(bytes + HEADER_BYTES, 0xff, DIRECTORY_BYTES);
    write_u16(bytes + HEADER_BYTES + 0x4eu * 2u, 0);
    write_u16(bytes + HEADER_BYTES + 0x1f4u * 2u, 1);

    uint8_t *first_page = bytes + pages_offset;
    write_u32(first_page, 0);
    first_page[4] = 1; /* U+4E00 */
    uint8_t *second_page = first_page + PAGE_BYTES;
    write_u32(second_page, 1);
    second_page[4 + 0x68u / 8u] = (uint8_t) (1u << (0x68u & 7u));

    uint8_t *sequence = bytes + sequences_offset;
    sequence[0] = 2;
    write_u32(sequence + 4, 2);
    write_u32(sequence + 8, 0x00a9u);
    write_u32(sequence + 12, 0xfe0fu);
    sequence += SEQUENCE_BYTES;
    sequence[0] = 3;
    write_u32(sequence + 4, 3);
    write_u32(sequence + 8, 0x1f468u);
    write_u32(sequence + 12, 0x200du);
    write_u32(sequence + 16, 0x1f469u);

    for (size_t glyph = 0; glyph < 4u; glyph++)
        memset(bytes + payload_offset + glyph * GLYPH_BYTES,
               (int) (0x31u + glyph), GLYPH_BYTES);

    snprintf(path, 128, "/tmp/tilefinch-glyph-component-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool written = fwrite(bytes, 1, file_size, file) == file_size
        && fclose(file) == 0;
    free(bytes);
    return written;
}

static bool write_color_fixture(char path[128])
{
    static const char id[] = "emoji-color-v1";
    const size_t pages_offset = HEADER_BYTES + DIRECTORY_BYTES;
    const size_t payload_offset = pages_offset + PAGE_BYTES;
    const size_t payload_bytes = 2u * 2u * 3u;
    const size_t file_size = payload_offset + payload_bytes;
    uint8_t *bytes = calloc(file_size, 1);
    CHECK(bytes != NULL);
    memcpy(bytes, "TFGFv1\0\0", 8);
    write_u16(bytes + 8, TILEFINCH_GLYPH_COMPONENT_ABI);
    bytes[10] = TILEFINCH_GLYPH_COMPONENT_COLOR;
    bytes[11] = 2;
    bytes[12] = 2;
    bytes[13] = 1;
    bytes[14] = (uint8_t) strlen(id);
    write_u32(bytes + 16, 1);
    write_u16(bytes + 20, 1);
    write_u16(bytes + 22, 0);
    write_u32(bytes + 24, HEADER_BYTES);
    write_u32(bytes + 28, (uint32_t) pages_offset);
    write_u32(bytes + 32, (uint32_t) payload_offset);
    write_u32(bytes + 36, (uint32_t) payload_offset);
    write_u32(bytes + 40, (uint32_t) file_size);
    memcpy(bytes + 44, id, sizeof(id));
    memset(bytes + HEADER_BYTES, 0xff, DIRECTORY_BYTES);
    write_u16(bytes + HEADER_BYTES + 0x1f6u * 2u, 0);
    uint8_t *page = bytes + pages_offset;
    write_u32(page, 0);
    page[4] = 1; /* U+1F600 */
    for (size_t pixel = 0; pixel < 4u; pixel++) {
        bytes[payload_offset + pixel * 2u] = 0xf8u;
        bytes[payload_offset + pixel * 2u + 1u] = 0x00u;
        bytes[payload_offset + 8u + pixel] = 0xffu;
    }
    snprintf(path, 128, "/tmp/tilefinch-color-glyph-XXXXXX");
    int descriptor = mkstemp(path);
    CHECK(descriptor >= 0);
    FILE *file = fdopen(descriptor, "wb");
    CHECK(file != NULL);
    bool written = fwrite(bytes, 1, file_size, file) == file_size
        && fclose(file) == 0;
    free(bytes);
    return written;
}

static bool hash_file(const char *path, uint8_t digest[32], uint64_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    TilefinchSha256 sha;
    tilefinch_sha256_init(&sha);
    uint8_t bytes[257];
    uint64_t total = 0;
    bool ok = true;
    while (ok) {
        size_t count = fread(bytes, 1, sizeof(bytes), file);
        if (count != 0) {
            ok = tilefinch_sha256_update(&sha, bytes, count);
            total += count;
        }
        if (count < sizeof(bytes)) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    if (fclose(file) != 0) ok = false;
    if (ok) ok = tilefinch_sha256_final(&sha, digest);
    if (ok) *size = total;
    return ok;
}

static bool read_bounded_file(const char *path, uint8_t **bytes,
                              size_t *length)
{
    *bytes = NULL;
    *length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end <= 0 || end > (long) TILEFINCH_UPDATE_MAX_ENVELOPE_BYTES
        || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    uint8_t *data = malloc((size_t) end);
    bool ok = data != NULL
        && fread(data, 1, (size_t) end, file) == (size_t) end
        && fgetc(file) == EOF;
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        free(data);
        return false;
    }
    *bytes = data;
    *length = (size_t) end;
    return true;
}

static bool copy_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb");
    FILE *output = input != NULL ? fopen(destination, "wb") : NULL;
    if (input == NULL || output == NULL) {
        if (input != NULL) fclose(input);
        if (output != NULL) fclose(output);
        return false;
    }
    uint8_t bytes[16u * 1024u];
    bool ok = true;
    while (ok) {
        size_t count = fread(bytes, 1, sizeof(bytes), input);
        if (count != 0) ok = fwrite(bytes, 1, count, output) == count;
        if (count < sizeof(bytes)) {
            if (ferror(input)) ok = false;
            break;
        }
    }
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    return ok;
}

/*
 * Optional release-ceremony proof. Ordinary builds do not embed the offline
 * update root, and ordinary test runs do not have release artifacts, so this
 * stays dormant unless TILEFINCH_GLYPH_PROOF_DIR is supplied. A release build
 * configured with the real public root then verifies the exact signed files
 * through the same consumer and pack parser used on the PSP.
 */
static bool test_release_artifacts(void)
{
    const char *directory = getenv("TILEFINCH_GLYPH_PROOF_DIR");
    const char *stage_root = getenv("TILEFINCH_GLYPH_STAGE_ROOT");
    if (directory == NULL || directory[0] == '\0') return true;
    if (stage_root != NULL && stage_root[0] != '\0')
        CHECK(mkdir(stage_root, 0777) == 0);
    CHECK(tilefinch_update_root_is_configured());
    TilefinchUpdateRoot root = {0};
    CHECK(tilefinch_update_embedded_root(&root));
    static const unsigned probes[TILEFINCH_GLYPH_PACK_COUNT] = {
        0x65e5u, 0x6c49u, 0x6f22u, 0xac00u, 0x1f600u
    };
    for (TilefinchGlyphPack pack = 0;
         pack < TILEFINCH_GLYPH_PACK_COUNT; pack++) {
        const TilefinchGlyphPackSpec *spec = tilefinch_glyph_pack_spec(pack);
        CHECK(spec != NULL);
        char envelope_path[TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT];
        char package_path[TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT];
        int envelope_written = snprintf(
            envelope_path, sizeof(envelope_path), "%s/%s",
            directory, spec->metadata_asset);
        int package_written = snprintf(
            package_path, sizeof(package_path), "%s/%s",
            directory, spec->pack_asset);
        CHECK(envelope_written > 0
              && (size_t) envelope_written < sizeof(envelope_path));
        CHECK(package_written > 0
              && (size_t) package_written < sizeof(package_path));
        uint8_t *envelope = NULL;
        size_t envelope_length = 0;
        CHECK(read_bounded_file(
            envelope_path, &envelope, &envelope_length));
        TilefinchUpdateVerifiedEnvelope verified = {0};
        TilefinchUpdateStatus status = tilefinch_update_verify_glyph_envelope(
            envelope, envelope_length,
            &(TilefinchUpdateVerifyOptions) {
                .embedded_root = &root,
                .crypto = tilefinch_update_default_crypto(),
                .now_unix = UINT64_C(1786600000),
                .clock_valid = true,
                .launcher_protocol = TILEFINCH_UPDATE_LAUNCHER_PROTOCOL
            }, &verified);
        CHECK(status == TILEFINCH_UPDATE_OK);
        CHECK(verified.manifest.package_format
              == TILEFINCH_UPDATE_PACKAGE_GLYPH);
        CHECK(verified.manifest.release_sequence == 1u);
        CHECK(strcmp(verified.manifest.tag, "components-v1") == 0);
        CHECK(strcmp(verified.manifest.asset, spec->pack_asset) == 0);
        uint8_t package_digest[32];
        uint64_t package_size = 0;
        CHECK(hash_file(package_path, package_digest, &package_size));
        CHECK(package_size == verified.manifest.package_size);
        CHECK(memcmp(package_digest,
                     verified.manifest.package_sha256, 32) == 0);

        Budget budget;
        budget_init(&budget, 4u * 1024u * 1024u);
        TilefinchGlyphProvider *provider =
            tilefinch_glyph_provider_create(&budget);
        CHECK(provider != NULL);
        CHECK(tilefinch_glyph_provider_attach(
            provider, package_path, spec->id));
        uint32_t key = 0;
        unsigned width = 0;
        CHECK(tilefinch_glyph_provider_has_codepoint(
            provider, probes[pack], &key, &width));
        CHECK(width > 0u);
        tilefinch_glyph_provider_destroy(provider);
        CHECK(budget.current == 0);

        if (stage_root != NULL && stage_root[0] != '\0'
            && (pack == TILEFINCH_GLYPH_PACK_JAPANESE
                || pack == TILEFINCH_GLYPH_PACK_COLOR_EMOJI)) {
            char staged_package[TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT];
            int staged_written = snprintf(
                staged_package, sizeof(staged_package), "%s/%s.download",
                stage_root, spec->id);
            CHECK(staged_written > 0
                  && (size_t) staged_written < sizeof(staged_package));
            CHECK(copy_file(package_path, staged_package));
            TilefinchGlyphComponentInstall *install =
                tilefinch_glyph_component_install_create(
                    &budget, &(TilefinchGlyphComponentInstallOptions) {
                        .package_path = staged_package,
                        .envelope = envelope,
                        .envelope_length = envelope_length,
                        .manifest = &verified.manifest,
                        .manifest_digest = verified.manifest_digest,
                        .install_root = stage_root,
                        .pack = pack
                    });
            CHECK(install != NULL);
            TilefinchUpdateInstallSnapshot snapshot = {0};
            for (size_t pump = 0; pump < 4096u; pump++) {
                CHECK(tilefinch_glyph_component_install_pump(
                    install, 16u * 1024u));
                CHECK(tilefinch_glyph_component_install_snapshot(
                    install, &snapshot));
                if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE)
                    break;
            }
            CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE
                  && snapshot.status == TILEFINCH_UPDATE_OK);
            tilefinch_glyph_component_install_destroy(install);
            TilefinchInstallPaths paths = {.slotted = true};
            snprintf(paths.install_root, sizeof(paths.install_root), "%s",
                     stage_root);
            uint64_t installed_sequence = 0;
            uint8_t installed_sha256[32];
            CHECK(tilefinch_glyph_component_installed_identity(
                &budget, &paths, pack, &root, &installed_sequence,
                installed_sha256));
            CHECK(installed_sequence == 1u
                  && memcmp(installed_sha256,
                            package_digest, sizeof(package_digest)) == 0);
            CHECK(budget.current == 0);
        }
        free(envelope);
    }
    if (stage_root != NULL && stage_root[0] != '\0') {
        Budget profile_budget;
        budget_init(&profile_budget, 2u * 1024u * 1024u);
        BrowserProfile *profile = browser_profile_create(&profile_budget);
        CHECK(profile != NULL);
        browser_profile_set_glyph_language(
            profile, BROWSER_GLYPH_LANGUAGE_JAPANESE);
        browser_profile_set_color_emoji(profile, true);
        CHECK(browser_profile_add_bookmark(
            profile, "https://example.com/",
            "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e  "
            "\xe6\xbc\xa2\xe5\xad\x97  "
            "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4  "
            "\xf0\x9f\x98\x80  "
            "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
            "\xf0\x9f\x91\xa7\xe2\x80\x8d\xf0\x9f\x91\xa6"));
        char profile_path[TILEFINCH_GLYPH_COMPONENT_PATH_LIMIT];
        int profile_written = snprintf(
            profile_path, sizeof(profile_path), "%s/profile.cfg", stage_root);
        CHECK(profile_written > 0
              && (size_t) profile_written < sizeof(profile_path));
        CHECK(browser_profile_save(profile, profile_path));
        browser_profile_destroy(profile);
        CHECK(profile_budget.current == 0);
        puts("glyph-component-tests ppsspp-stage=PASS");
    }
    puts("glyph-component-tests release-proof=PASS");
    return true;
}

static bool test_bounded_pack_provider(void)
{
    char path[128];
    char color_path[128];
    CHECK(write_fixture(path));
    CHECK(write_color_fixture(color_path));
    Budget budget;
    budget_init(&budget, 4u * 1024u * 1024u);
    TilefinchGlyphProvider *provider =
        tilefinch_glyph_provider_create(&budget);
    CHECK(provider != NULL);
    CHECK(!tilefinch_glyph_provider_attach(provider, path, "wrong-id"));
    CHECK(tilefinch_glyph_provider_attach(
        provider, path, "glyph-ja"));
    CHECK(tilefinch_glyph_provider_attach(
        provider, color_path, "emoji-color-v1"));
    CHECK(tilefinch_glyph_provider_pack_count(provider) == 2);
    CHECK(font_optional_glyph_provider_install(provider));
    FontSet fonts;
    CHECK(font_set_load(
        &fonts, &budget, TILEFINCH_TEST_SANS_FONT, NULL, NULL, NULL, NULL,
        NULL, NULL, 2u * 1024u * 1024u));
    const FontFace *face = font_set_face(&fonts, FONT_SANS);
    CHECK(face != NULL);

    uint32_t key = 0;
    unsigned width = 0;
    CHECK(tilefinch_glyph_provider_has_codepoint(
        provider, 0x4e00u, &key, &width));
    CHECK(width == 16);
    TilefinchGlyphSource source;
    CHECK(!tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(tilefinch_glyph_provider_key_pending(provider, key));
    FontGlyph glyph;
    CHECK(font_glyph_load(face, 0x4e00u, 16, false, &glyph));
    CHECK(glyph.provider_pending && glyph.colors == NULL);
    font_glyph_destroy(face, &glyph);
    bool changed = true;
    size_t bytes_read = 99;
    CHECK(tilefinch_glyph_provider_pump(
        provider, GLYPH_BYTES - 1u, &changed, &bytes_read));
    CHECK(!changed && bytes_read == 0);
    CHECK(tilefinch_glyph_provider_key_pending(provider, key));
    CHECK(tilefinch_glyph_provider_pump(
        provider, GLYPH_BYTES * 2u, &changed, &bytes_read));
    CHECK(changed && bytes_read == GLYPH_BYTES * 2u);
    CHECK(tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(source.kind == TILEFINCH_GLYPH_COMPONENT_MONO);
    CHECK(source.width == 16 && source.height == 16);
    CHECK(source.pixels[0] == 0x31u);
    CHECK(font_glyph_load(face, 0x4e00u, 16, false, &glyph));
    CHECK(!glyph.provider_pending && glyph.colors == NULL
          && glyph.width == 16 && glyph.height == 16);
    font_glyph_destroy(face, &glyph);

    static const char family[] =
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9!";
    size_t used = 0;
    CHECK(tilefinch_glyph_provider_match(
        provider, family, sizeof(family) - 1u, &used, &key, &width));
    CHECK(used == 11u && width == 16);
    CHECK(!tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(tilefinch_glyph_provider_pump(
        provider, GLYPH_BYTES * 2u, &changed, &bytes_read));
    CHECK(tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(source.pixels[0] == 0x34u);
    unsigned sequence_key = 0;
    size_t font_used = 0;
    CHECK(font_optional_glyph_match_sequence(
        family, sizeof(family) - 1u, &font_used, &sequence_key));
    CHECK(font_used == 11u && sequence_key == key);
    size_t sequence_start = 99u;
    CHECK(font_optional_glyph_match_sequence_ending_at(
        family, font_used, &sequence_start, &sequence_key));
    CHECK(sequence_start == 0 && sequence_key == key);
    CHECK(font_text_width_at_size_fixed(
        face, family, font_used, 16 * 64, false) == 16 * 64);
    CHECK(font_glyph_load_at_size(
        face, sequence_key, 16 * 64, false, &glyph));
    CHECK(!glyph.provider_pending && glyph.colors == NULL
          && glyph.width == 16 && glyph.height == 16);
    font_glyph_destroy(face, &glyph);

    static const char copyright_emoji[] = "\xc2\xa9\xef\xb8\x8f";
    CHECK(font_optional_glyph_match_sequence(
        copyright_emoji, sizeof(copyright_emoji) - 1u,
        &font_used, &sequence_key));
    CHECK(font_used == sizeof(copyright_emoji) - 1u);
    CHECK(font_optional_glyph_match_sequence_ending_at(
        copyright_emoji, font_used, &sequence_start, &key));
    CHECK(sequence_start == 0 && key == sequence_key);

    CHECK(tilefinch_glyph_provider_has_codepoint(
        provider, 0x1f600u, &key, &width));
    CHECK(!tilefinch_glyph_provider_source(provider, key, &source));
    CHECK(tilefinch_glyph_provider_pump(
        provider, 12u, &changed, &bytes_read));
    CHECK(changed && bytes_read == 12u);
    CHECK(font_glyph_load(face, 0x1f600u, 16, false, &glyph));
    CHECK(!glyph.provider_pending && glyph.colors != NULL
          && glyph.pixels != NULL && glyph.width == 16 && glyph.height == 16
          && glyph.colors[0] == UINT16_C(0xf800)
          && glyph.pixels[0] == 255u);
    font_glyph_destroy(face, &glyph);

    font_set_destroy(&fonts);
    CHECK(font_optional_glyph_provider_uninstall(provider));
    tilefinch_glyph_provider_destroy(provider);
    CHECK(budget.current == 0);

    uint8_t package_digest[32];
    uint64_t package_size = 0;
    CHECK(hash_file(path, package_digest, &package_size));
    char root[] = "/tmp/tilefinch-glyph-store-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    TilefinchInstallPaths paths = {.slotted = true};
    snprintf(paths.install_root, sizeof(paths.install_root), "%s", root);
    TilefinchUpdateManifest manifest = {
        .package_format = TILEFINCH_UPDATE_PACKAGE_GLYPH,
        .package_size = package_size
    };
    memcpy(manifest.package_sha256, package_digest, 32);
    snprintf(manifest.asset, sizeof(manifest.asset), "%s",
             tilefinch_glyph_pack_spec(TILEFINCH_GLYPH_PACK_JAPANESE)
                 ->pack_asset);
    uint8_t envelope[16] = {0};
    uint8_t manifest_digest[32];
    memset(manifest_digest, 0x5a, sizeof(manifest_digest));
    TilefinchGlyphComponentInstall *install =
        tilefinch_glyph_component_install_create(
            &budget, &(TilefinchGlyphComponentInstallOptions) {
                .package_path = path,
                .envelope = envelope,
                .envelope_length = sizeof(envelope),
                .manifest = &manifest,
                .manifest_digest = manifest_digest,
                .install_root = root,
                .pack = TILEFINCH_GLYPH_PACK_JAPANESE
            });
    CHECK(install != NULL);
    TilefinchUpdateInstallSnapshot snapshot = {0};
    for (size_t pump = 0; pump < 4096u; pump++) {
        CHECK(tilefinch_glyph_component_install_pump(install, 7u));
        CHECK(tilefinch_glyph_component_install_snapshot(install, &snapshot));
        if (snapshot.phase >= TILEFINCH_UPDATE_INSTALL_COMPLETE) break;
    }
    CHECK(snapshot.phase == TILEFINCH_UPDATE_INSTALL_COMPLETE);
    tilefinch_glyph_component_install_destroy(install);
    char installed[TILEFINCH_INSTALL_PATH_LIMIT];
    CHECK(tilefinch_glyph_component_resolve(
              &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
              installed, sizeof(installed))
          && strstr(installed, "/glyph-ja/active/pack.tfgf") != NULL);
    CHECK(tilefinch_glyph_component_remove(
              &paths, TILEFINCH_GLYPH_PACK_JAPANESE)
          && !tilefinch_glyph_component_resolve(
                 &paths, TILEFINCH_GLYPH_PACK_JAPANESE,
                 installed, sizeof(installed)));
    char cleanup[TILEFINCH_INSTALL_PATH_LIMIT];
    snprintf(cleanup, sizeof(cleanup),
             "%s/components/glyph-ja/UNINSTALLED", root);
    CHECK(unlink(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components/glyph-ja", root);
    CHECK(rmdir(cleanup) == 0);
    snprintf(cleanup, sizeof(cleanup), "%s/components", root);
    CHECK(rmdir(cleanup) == 0 && rmdir(root) == 0);
    CHECK(remove(color_path) == 0);
    return true;
}

int main(void)
{
    if (!test_bounded_pack_provider()) return 1;
    if (!test_release_artifacts()) return 1;
    puts("glyph-component-tests status=PASS");
    return 0;
}
