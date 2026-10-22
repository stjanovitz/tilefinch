#!/usr/bin/env python3
"""Generate bounded, data-only locale downloads and their EBOOT trust pins."""
import argparse
import hashlib
from pathlib import Path
from devanagari_sequences import encoded as hindi_sequences
from indic_sequences import encoded as indic_sequences

ROOT = Path(__file__).resolve().parents[1]
# The runtime bounds of include/tilefinch/ui_language.h.
ROW_LIMIT = 2048
BYTE_LIMIT = 64 * 1024 - 1
# From v12 a catalog stores no English keys ("TFUI2", src/ui_language.c).
# A row is found by the 32-bit FNV-1a hash of its ASCII-lowercased key (what
# strcasecmp matched) and confirmed by the key's byte length.
HASHED_VERSION = 12
KEY_LENGTH_LIMIT = 255
CATALOGS = [(1, [("es", "Spanish", "EMBEDDED"), ("fr", "French", "EMBEDDED"),
                 ("de", "German", "EMBEDDED"), ("ja", "Japanese", "JAPANESE")]),
            (2, [("ru", "Russian", "CYRILLIC"), ("uk", "Ukrainian", "CYRILLIC"),
                 ("zh-hans", "Chinese (Simplified)", "CHINESE_SIMPLIFIED"),
                 ("ko", "Korean", "KOREAN")]),
            (3, [("hi", "Hindi", "DEVANAGARI"),
                 ("ar", "Arabic", "ARABIC")]),
            (4, [("es", "Spanish", "EMBEDDED"), ("fr", "French", "EMBEDDED"),
                 ("de", "German", "EMBEDDED"), ("ja", "Japanese", "JAPANESE"),
                 ("ru", "Russian", "CYRILLIC"), ("uk", "Ukrainian", "CYRILLIC"),
                 ("zh-hans", "Chinese (Simplified)", "CHINESE_SIMPLIFIED"),
                 ("ko", "Korean", "KOREAN"), ("hi", "Hindi", "DEVANAGARI"),
                 ("ar", "Arabic", "ARABIC")])]
CATALOGS.append((5, CATALOGS[-1][1]))
CATALOGS.append((6, CATALOGS[-1][1]))
CATALOGS.append((7, CATALOGS[-1][1]))
CATALOGS.append((8, [("pt", "Portuguese", "EMBEDDED")]))
CATALOGS.append((9, [("bn", "Bengali", "BENGALI_ASSAMESE"),
                     ("as", "Assamese", "BENGALI_ASSAMESE")]))
latest_languages = {}
for _, languages in CATALOGS:
    for language in languages:
        latest_languages[language[0]] = language
CATALOGS.append((10, list(latest_languages.values())))
CATALOGS.append((11, list(latest_languages.values())))
CATALOGS.append((12, list(latest_languages.values())))
ACTIVE_VERSION = CATALOGS[-1][0]
# Keys a version drops because the browser no longer looks them up. Every
# other key of the previous version must remain, so a newly added locale
# still translates everything in use.
RETIRED_KEYS = {12: {
    # "Built in" and "Loading": the painter shows "Built-in" for both
    # rows, and the bare "Loading" was never drawn.
    "Built in", "Loading",
    "Browser text size", "Check for updates", "Check weekly", "Checking",
    "Chrome theme", "Clear cache", "Clear history",
    "L/R CATEGORY   X SELECT   O BACK", "Network profile", "Page colors",
    "Page text size", "Save playback positions", "Search engine",
    "Select Like or Unlike.",
    "Select the thumbnail to retry playback. The player provides play, "
    "pause, seek, time, and exit controls.",
    "Settings > Video > Audio & subtitles", "Signed in on this device.",
    "Text entry", "Update channel"}}
# The signed glyph-pack revision whose pre-shaped clusters cover a catalog
# version's labels: (glyphs, first catalog version) -> sequence. A catalog
# that adds shaped labels needs a rebuilt pack and a new entry here.
MINIMUM_GLYPH_SEQUENCES = {
    ("DEVANAGARI", 1): 2, ("DEVANAGARI", 5): 3, ("DEVANAGARI", 7): 4,
    ("DEVANAGARI", 10): 5, ("DEVANAGARI", 11): 6, ("DEVANAGARI", 12): 7,
    ("BENGALI_ASSAMESE", 11): 2, ("BENGALI_ASSAMESE", 12): 3}


def minimum_glyph_sequence(glyphs, version):
    return max([sequence for (script, first), sequence
                in MINIMUM_GLYPH_SEQUENCES.items()
                if script == glyphs and first <= version] or [1])

def key_hash(key):
    """FNV-1a (32-bit) of the ASCII-lowercased key: style_fnv1a with fold."""
    value = 2166136261
    for byte in key.lower().encode("ascii"):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def hashed_catalog(code, rows, column):
    """A TFUI2 catalog: its header line, the big-endian row count, the
    rows' key hashes in increasing order, each key's byte length, then each
    value ending in a line feed."""
    ordered = sorted(rows, key=lambda row: key_hash(row[0]))
    hashes = [key_hash(row[0]) for row in ordered]
    if len(set(hashes)) != len(hashes):
        collided = sorted(row[0] for row in ordered
                          if hashes.count(key_hash(row[0])) > 1)
        raise ValueError(f"English key hash collision: {collided}")
    if any(not 0 < len(row[0]) <= KEY_LENGTH_LIMIT for row in ordered):
        raise ValueError("English key too long for its length byte")
    return (f"TFUI2\t{code}\n".encode("ascii")
            + len(ordered).to_bytes(2, "big")
            + b"".join(value.to_bytes(4, "big") for value in hashes)
            + bytes(len(row[0]) for row in ordered)
            + "".join(f"{row[column]}\n" for row in ordered).encode("utf-8"))


def read_rows(version, languages):
    source = ROOT / f"translations/ui/v{version}/source.tsv"
    lines = source.read_text(encoding="utf-8").splitlines()
    if lines[0] != "\t".join(["English"] + [label for _, label, _ in languages]):
        raise ValueError("unexpected language columns")
    rows = [line.split("\t") for line in lines[1:]]
    if not 0 < len(rows) <= ROW_LIMIT or any(len(r) != len(languages) + 1 for r in rows):
        raise ValueError("invalid row count or columns")
    rows.sort(key=lambda row: row[0].lower().encode("ascii"))
    if len({r[0].lower() for r in rows}) != len(rows):
        raise ValueError("duplicate English key")
    for row in rows:
        for value in row:
            if not value or len(value.encode("utf-8")) > 256 or "%" in value \
                    or any(ord(c) < 32 for c in value):
                raise ValueError("invalid display string")
    return rows

def outputs(resource_dir=None):
    result = {}
    manifest = ['/* Generated by tools/generate_ui_translations.py. */',
                'static const TilefinchUiLanguageSpec ui_languages[] = {',
                '    {"en", "English", BROWSER_GLYPH_LANGUAGE_EMBEDDED, NULL, 1, 0, NULL},']
    base_keys = None
    # Keep the newest resource for each language in its original registration
    # order. Adding a locale must not renumber saved settings or republish the
    # immutable catalogs used by existing languages.
    active = {}
    for version, languages in CATALOGS:
        rows = read_rows(version, languages)
        keys = {r[0].lower() for r in rows}
        retired = {key.lower() for key in RETIRED_KEYS.get(version, ())}
        if retired & keys:
            raise ValueError("retired key still present")
        if base_keys is not None and not base_keys - retired <= keys:
            raise ValueError("new locales must translate every existing key")
        base_keys = keys
        if any(glyphs == "BENGALI_ASSAMESE" for _, _, glyphs in languages):
            labels = [row[column] for row in rows for column, (_, _, glyphs)
                      in enumerate(languages, 1) if glyphs == "BENGALI_ASSAMESE"]
            result[ROOT / f"translations/ui/v{version}/bengali-assamese.sequences"] = \
                indic_sequences(labels, "bn")
        for column, (code, label, glyphs) in enumerate(languages, 1):
            data = hashed_catalog(code, rows, column) \
                if version >= HASHED_VERSION else (f"TFUI1\t{code}\n" + "".join(
                    f"{r[0]}\t{r[column]}\n" for r in rows)).encode("utf-8")
            if len(data) > (BYTE_LIMIT if version >= 12 else 32768):
                raise ValueError("locale exceeds runtime bound")
            resource_key = f"translations/ui/v{version}/{code}.tful"
            if resource_dir is not None:
                result[resource_dir / resource_key] = data
            active[code] = (f'    {{"{code}", "{label}", BROWSER_GLYPH_LANGUAGE_{glyphs}, '
                            f'"{hashlib.sha256(data).hexdigest()}", {version}, '
                            f'{minimum_glyph_sequence(glyphs, version)}, '
                            f'"{resource_key}"}},')
            if code == "hi":
                result[ROOT / f"translations/ui/v{version}/hi.sequences"] = \
                    hindi_sequences(r[column] for r in rows)
    manifest += list(active.values())
    manifest += ['};', '_Static_assert(sizeof(ui_languages) / sizeof(ui_languages[0])',
                 '    == TILEFINCH_UI_LANGUAGE_COUNT, "locale catalog mismatch");', '']
    result[ROOT / "src/generated/ui_languages.inc"] = "\n".join(manifest).encode()
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--resource-dir", type=Path,
                        help="export download/test data outside the source tree")
    parser.add_argument("--resources-only", action="store_true",
                        help="export resources but only verify committed pins/sequences")
    parser.add_argument("--list-resources", action="store_true",
                        help="list generated resource keys for build dependencies")
    args = parser.parse_args()
    if args.list_resources:
        for version, languages in CATALOGS:
            for code, _, _ in languages:
                print(f"translations/ui/v{version}/{code}.tful")
        raise SystemExit(0)
    if args.resources_only and args.resource_dir is None:
        parser.error("--resources-only requires --resource-dir")
    if args.resource_dir is not None:
        args.resource_dir = args.resource_dir.resolve()
        if args.resource_dir in (ROOT, ROOT / "translations") or ROOT / "translations" in args.resource_dir.parents:
            parser.error("resource exports must not be stored in the authored translations tree")
    for path, data in outputs(args.resource_dir).items():
        resource = args.resource_dir is not None and args.resource_dir in path.parents
        if args.check or (args.resources_only and not resource):
            if not path.exists() or path.read_bytes() != data:
                raise SystemExit(f"stale UI translations: {path}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists() or path.read_bytes() != data:
                path.write_bytes(data)
