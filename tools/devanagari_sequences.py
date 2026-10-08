"""Bounded pre-shaped Hindi clusters shared by the UI and glyph-pack build.

This is an inventory, not a runtime shaper. Rare clusters outside this list
still use the ordinary scalar fallback. All interface clusters are included.
"""
import unicodedata

VIRAMA = "\u094d"

def clusters(text):
    current = ""
    for character in text:
        cp = ord(character)
        indic = 0x0900 <= cp <= 0x097f or 0xa8e0 <= cp <= 0xa8ff
        mark = unicodedata.category(character).startswith("M")
        if current and indic and (mark or current.endswith(VIRAMA)):
            current += character
        else:
            if current:
                yield current
            current = character if indic else ""
    if current:
        yield current

def inventory(labels):
    result = {tuple(map(ord, cluster)) for label in labels
              for cluster in clusters(label) if len(cluster) > 1}
    vowels = ("", "ा", "ि", "ी", "ु", "ू", "ृ", "े", "ै", "ो", "ौ")
    consonants = [chr(cp) for cp in range(0x0915, 0x093a)]
    consonants += [chr(cp) for cp in range(0x0958, 0x0960)]
    # Frequent conjuncts, including reph; avoid a combinatorial all-pairs pack.
    bases = consonants + ["क्ष", "त्र", "ज्ञ", "श्र", "प्र", "क्र", "ग्र", "द्र",
                          "ब्र", "भ्र", "स्त", "स्थ", "स्क", "स्व", "न्त", "न्द",
                          "म्प", "म्ब", "ल्ल", "द्ध", "त्त", "द्व", "त्व", "ष्ट"]
    bases += ["र्" + consonant for consonant in consonants]
    for base in bases:
        for vowel in vowels:
            for final in ("", "ं", "ँ"):
                item = tuple(map(ord, base + vowel + final))
                if 2 <= len(item) <= 8:
                    result.add(item)
    if len(result) > 4096 or any(len(item) > 8 for item in result):
        raise ValueError("Hindi clusters exceed the optional pack bounds")
    return sorted(result)

def encoded(labels):
    return ("# Generated Hindi interface and common syllable clusters.\n" +
            "".join(" ".join(f"{cp:04X}" for cp in item) + "\n"
                    for item in inventory(labels))).encode("ascii")
