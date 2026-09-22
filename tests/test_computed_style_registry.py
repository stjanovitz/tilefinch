#!/usr/bin/env python3
"""Keep the computed-style registry and the getter's dispatch in agreement.

`COMPUTED_STYLE_PROPERTIES` in src/js_dom_bindings.c is the single answer to
which properties a computed style has: `name in style`, `length`, `item()`,
enumeration, and the identifiers js_computed_style_get() dispatches on are all
generated from it. A serializer for an unregistered property therefore does
not compile. What the compiler cannot see is checked here:

  * the rows are in strcmp order, which the binary search depends on, and each
    identifier is the one its name implies;
  * nothing in the getter goes back to matching the property by string, which
    is how a value could again resolve for a name `in` says does not exist;
  * every registered property has something that resolves it: an identifier
    arm, or a sparse-family flag. Otherwise it would be enumerated with an
    empty value, which CSSOM never produces.

The last point is also checked at run time, with real values, by
`tilefinch-browser-engine-tests --computed-style-values-only` and by the
checked-in dump of every property (`--computed-style-golden`).
"""

from pathlib import Path
import re
import sys

# Presentation attributes the getter resolves only for SVG elements on which
# the attribute is relevant. They are not properties of an ordinary element's
# declaration, so they stay out of the registry by design. Anything else the
# getter matches must be registered.
SVG_ONLY = "SVG_PROP"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    source = (root / "src" / "js_dom_bindings.c").read_text(encoding="utf-8")
    failures: list[str] = []

    table = source[source.index("#define COMPUTED_STYLE_PROPERTIES(X)"):]
    table = table[:table.index("typedef enum {")]
    rows = re.findall(
        r'X\((\w+), "([^"]+)", (true|false), ([^)]*)\)', table)
    names = [name for _, name, _, _ in rows]
    registry = set(names)
    if len(rows) < 150:
        failures.append(f"registry parsed only {len(rows)} rows")
    if len(registry) != len(names):
        failures.append("registry lists a property twice")
    # computed_style_property_find() is a binary search over strcmp order.
    if names != sorted(names):
        misplaced = next(
            later for earlier, later in zip(names, names[1:])
            if later < earlier)
        failures.append(
            f"registry is not in strcmp order at {misplaced!r}; "
            "the binary search would miss it")
    for identifier, name, _, _ in rows:
        if name != name.lower() or not re.fullmatch(r"-?[a-z][a-z0-9-]*", name):
            failures.append(f"registry name {name!r} is not a dashed "
                            "lower-case property name")
        if identifier != name.lstrip("-").upper().replace("-", "_"):
            failures.append(
                f"{name}: identifier {identifier} does not follow its name")

    handled: set[str] = set()
    for signature in (
            "JSValue js_computed_style_get(",
            "bool computed_style_serialize_retained(",
            "const char *computed_style_sparse_modern_initial("):
        body = function_body(source, signature)
        handled |= set(re.findall(r"\bCSP_([A-Z0-9_]+)\b", body))
        if re.search(r"property_equal\(\s*name\b", body):
            failures.append(
                f"{signature.split('(')[0].split()[-1]} matches a property "
                "by string again; dispatch on its CSP_ identifier so an "
                "unregistered property cannot resolve")
    svg_only = set(re.findall(SVG_ONLY + r'\("([^"]+)"\)', source))
    if len(handled) < 100:
        failures.append(
            f"found only {len(handled)} dispatched identifiers; the getter "
            "no longer looks the way this test reads it")

    for identifier, name, _, flags in rows:
        if identifier in handled or "CSP_SPARSE_" in flags \
                or name in svg_only:
            continue
        failures.append(
            f"{name}: registered but nothing in the getter resolves it, so "
            "it would be enumerated with an empty value")

    for failure in failures:
        print(failure, file=sys.stderr)
    if not failures:
        print(f"computed-style registry: {len(registry)} properties agree "
              "with the getter")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
