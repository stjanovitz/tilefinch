"""Bounded offline cluster inventories for future Indic glyph revisions.

This is not a general runtime OpenType shaper. Each selected complete cluster
must be shaped offline by the pack producer. Existing Hindi catalogs continue
using devanagari_sequences.py so their published bytes remain immutable.
"""
import unicodedata

MAX_SEQUENCES = 4096
MAX_CODEPOINTS = 8
# Two catalog columns (Bengali and Assamese) of at most 64 KiB each.
MAX_LABEL_BYTES = 2 * 65536
SCRIPTS = {
    "hi": (0x0900, 0x097f, "\u094d"),
    "bn": (0x0980, 0x09ff, "\u09cd"),
    "as": (0x0980, 0x09ff, "\u09cd"),
}


def clusters(text, language):
    first, last, virama = SCRIPTS[language]
    if len(text.encode("utf-8")) > MAX_LABEL_BYTES:
        raise ValueError("text exceeds offline inventory input bound")
    current = ""
    for character in text:
        cp = ord(character)
        script = first <= cp <= last
        # A join control belongs to a virama sequence, never to a preceding
        # ordinary syllable or across a word boundary.
        join = character in ("\u200c", "\u200d") and current.endswith(virama)
        continued = script and (unicodedata.category(character).startswith("M")
            or current.endswith(virama)
            or current.endswith((virama + "\u200c", virama + "\u200d")))
        if current and (join or continued):
            current += character
        else:
            if current:
                yield current
            current = character if script else ""
        if len(current) > MAX_CODEPOINTS:
            raise ValueError("complete cluster exceeds glyph-pack bound")
    if current:
        yield current


def inventory(labels, language):
    if language not in SCRIPTS:
        raise ValueError("unsupported Indic language")
    result = set()
    def add(text):
        for spelling in (text, unicodedata.normalize("NFC", text),
                         unicodedata.normalize("NFD", text)):
            item = tuple(map(ord, spelling))
            if 2 <= len(item) <= MAX_CODEPOINTS:
                result.add(item)
            elif len(item) > MAX_CODEPOINTS:
                raise ValueError("normalized cluster exceeds glyph-pack bound")
        if len(result) > MAX_SEQUENCES:
            raise ValueError("inventory exceeds glyph-pack sequence bound")
    label_bytes = 0
    for label in labels:
        length = len(label.encode("utf-8"))
        if length > MAX_LABEL_BYTES - label_bytes:
            raise ValueError("labels exceed offline inventory input bound")
        label_bytes += length
        for cluster in clusters(label, language):
            add(cluster)
    if language == "hi":
        # Preserve every existing common Hindi cluster. Expanding canonical
        # aliases for the whole Cartesian inventory would exceed 4096; add
        # those aliases only for actual labels above, never drop old coverage.
        from devanagari_sequences import inventory as legacy_hindi_inventory
        result.update(legacy_hindi_inventory([]))
        vowels = ("", "ा", "ि", "ी", "ु", "ू", "ृ", "े", "ै", "ो", "ौ")
        bases = [base.replace("्", "्" + joiner) for base in ("क्ष", "ज्ञ", "श्र")
                 for joiner in ("\u200c", "\u200d")]
        finals = ("", "ं", "ँ")
    else:
        consonants = [chr(cp) for cp in range(0x0995, 0x09ba)
                      if unicodedata.category(chr(cp)) == "Lo"]
        consonants += ["ড়", "ঢ়", "য়", "ৰ", "ৱ"]
        vowels = ("", "া", "ি", "ী", "ু", "ূ", "ৃ", "ে", "ৈ", "ো", "ৌ")
        bases = consonants + ["ক্ষ", "জ্ঞ", "ত্র", "শ্র", "প্র", "ক্র", "স্ত", "ন্দ", "দ্ধ", "স্ক"]
        bases += ["র্" + consonant for consonant in consonants]
        bases += ["ৰ্" + consonant for consonant in ("ক", "গ", "ত", "দ", "ম", "য", "ৱ")]
        finals = ("", "ং", "ঁ")
    for base in bases:
        for vowel in vowels:
            for final in finals:
                add(base + vowel + final)
    if len(result) > MAX_SEQUENCES:
        raise ValueError("inventory exceeds glyph-pack sequence bound")
    return sorted(result)


def encoded(labels, language):
    return ("# Generated bounded Indic clusters; shape complete sequences offline.\n" +
            "".join(" ".join(f"{cp:04X}" for cp in item) + "\n"
                    for item in inventory(labels, language))).encode("ascii")


if __name__ == "__main__":
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--language", choices=tuple(SCRIPTS), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("labels", nargs="*", help="additional complete text, not codepoint fragments")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(encoded(args.labels, args.language))
