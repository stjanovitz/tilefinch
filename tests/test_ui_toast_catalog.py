#!/usr/bin/env python3
"""Every literal status toast the shipping PSP browser can show has a key in
the active interface catalog, and the catalog keeps no key nothing uses.

draw_toast translates ui->status at paint time, one line at a time, keeping
a trailing progress count (" 45%") verbatim (psp_ui.c, ui_toast_line_text).
A literal with no key stays English in every language, so this test lists
each one. Run it with --list to print the gaps instead of failing.

Statuses composed from runtime text (an engine or fetch error, a file name,
"THEME %s", "ALLOWLIST LOADED - %u ADDED") have no whole-line key and are
not checked.

The same lookup translates the other English the painter shows from state:
a setting's reload notices, a theme file's error, the video player's
headline, and the Update screen's status, notes and button. Their literal
sources are listed here too, including a literal routed through a local
variable ("status = ...; psp_ui_show_status(ui, status, ...)") and the
static tables and section headings the Site data & storage screens
translate when they draw a row, and the Site information screen's actions.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 \
    and not sys.argv[1].startswith("-") else Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

from test_psp_sdk_contracts import (  # noqa: E402
    call_arguments, without_comments)
import generate_ui_translations  # noqa: E402

# Macros the shipping psp-browser-script build defines; every other macro
# (validation logging, power tests, UI timing, ...) is undefined there.
SHIPPING_DEFINED = {"__PSP__", "__GNUC__", "TILEFINCH_PSP_LIVE_NETWORK",
                    "TILEFINCH_HAVE_PSP_VOICE"}

# (function, index of the status argument)
STATUS_CALLS = (
    ("psp_ui_show_status", 1), ("psp_ui_keep_status", 1),
    ("psp_ui_show_status_ms", 1), ("psp_ui_keep_progress_status", 1),
    ("psp_ui_show_wifi_sign_in_status", 1), ("psp_ui_show_tls_status", 1),
    ("psp_ui_show_heavy_scripts_status", 1),
    ("psp_supervisor_show_status", 1),
    # Shown by the supervisor when the user cancels the cooperative work.
    ("psp_work_cooperate_begin", 5),
    # Offline-library messages the caller shows as a toast.
    ("offline_status", 1),
    # A file-download offer's failure toast.
    ("psp_app_offer_file", 3),
    # Interface-language download teardown shows its status argument.
    ("language_abort", 1),
    # Labels, hints and second lines the painter translates directly.
    ("tilefinch_ui_text", 0), ("tilefinch_ui_logical_text", 0),
    # A setting's reloading, saved and reload-failed notices.
    ("psp_app_reload_after_setting", 2), ("psp_app_reload_after_setting", 3),
    ("psp_app_reload_after_setting", 4),
    ("psp_app_reload_for_site_security_setting", 2),
    ("psp_app_reload_for_site_security_setting", 3),
    # A theme file's error, shown as a toast.
    ("theme_error", 2),
    # The video player's headline, translated when it is painted.
    ("psp_media_raise_error", 1), ("psp_media_job_failed", 2),
    ("psp_ui_media_set_error", 1), ("psp_ui_media_set_error_reason", 1),
    ("psp_ui_media_set_resolving_progress", 1),
    # The Update screen's status, notes and button label.
    ("psp_ui_set_update", 2), ("psp_ui_set_update", 3),
    ("psp_ui_set_update", 5),
    ("update_client_error", 2), ("install_error", 2),
    # The font-pack offer's confirmation box.
    ("psp_ui_set_glyph_offer_error", 2),
)

SOURCES = ("src/psp_app/*.c", "src/psp_script_main.c", "src/psp_ui.c",
           "src/psp_ui_menu.c", "src/psp_ui_media_8888.c",
           "src/psp_offline_store.c", "src/psp_glyph_component_session.c",
           "src/psp_ui_theme.c", "src/psp_media_session.c",
           "src/psp_media_open.c", "src/psp_media_seek.c",
           "src/psp_update_session.c", "src/update_client.c",
           "src/update_installer.c", "src/psp_ui_language_repair.c",
           "src/psp_ui_settings.c")

# Functions whose returned literals the caller shows as a toast.
STATUS_RETURNS = ("psp_offline_store_route_cancel_status",
                  "psp_glyph_component_installed_status",
                  # The font-pack offer shows a failed check's reason.
                  "glyph_begin_check",
                  # The acknowledgement shown before an action is dispatched
                  # (psp_present_action_ack).
                  "psp_ui_action_acknowledgement")

# An installer job's message: a font-pack install started from the page's
# language-pack offer shows it as a toast when the install fails or is
# cancelled (psp_app_glyph_component.c, glyph_offer_track). Progress text
# ending in "..." is never shown; the toast shows "Installing 45%" instead.
JOB_MESSAGE_SOURCES = ("src/glyph_component_store.c",)
JOB_MESSAGE_CALLS = (("install_fail", 2),
                     ("install_fail_before_activation", 2))
JOB_MESSAGE_BUFFER = re.compile(r"\s*job->message\b")

# snprintf into a buffer named like this ("status", "ready_status")
# composes a toast. A format with no conversion is the whole toast; one
# made only of whole-line "%s" (each optionally followed by its " %u%%"
# count, as "%s\n%s" joins a pack label and its state) makes each literal
# argument a line. Other formats ("THEME %s") build text no catalog key
# names whole and are not checked.
STATUS_BUFFER = re.compile(r"\s*\w*status\b")
# The Update screen shows an update download's or install's message whole,
# including its progress text ("DOWNLOADING..."), and a theme error buffer
# is shown as a toast.
MESSAGE_BUFFER = {"update_client.c": re.compile(r"\s*client->message\b"),
                  "update_installer.c": re.compile(r"\s*job->message\b"),
                  "psp_app_settings.c": re.compile(r"\s*\w*error\b")}
IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
# State the painter translates when it draws it, copied in with
# copy_string(destination, capacity, text).
PAINTED_STATE = re.compile(
    r"\s*(?:media->status|ui->update_status|ui->update_primary_label)")
WHOLE_LINE_FORMAT = re.compile(r"%s(?: %u%%)?(?:\n%s(?: %u%%)?)*\n?")
RETURN_EXPRESSION = re.compile(r'\breturn\b((?:[^;"]|"(?:[^"\\]|\\.)*")*);')
# Static tables whose every entry the painter translates as it draws it:
# a site's storage choice and its explanation, the Site data & storage
# rows, and the Site information actions. Their section headings and
# empty-list line are UiSiteDataLine literals with a negative row.
PAINTED_TABLES = {"psp_ui.c": ("ui_site_storage_states", "explanations",
                               "fixed", "site_info_actions")}
# A plain on/off setting's two confirmations, initialized by name in its
# descriptor (psp_ui_settings.c) and shown by the app's generic handler.
STATUS_FIELD = re.compile(
    r'\.(?:on|off)_status\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')
SITE_DATA_HEADING = re.compile(
    r'\(UiSiteDataLine\)\s*\{\s*("(?:[^"\\]|\\.)*")\s*,\s*-')

# A count a status ends with is shown as is after the translated text.
PROGRESS_SUFFIX = re.compile(r" [0-9]+%$")
LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')
LITERAL_RUN = re.compile(r'(?:"(?:[^"\\]|\\.)*"\s*)+')
DEFINE = re.compile(r'^\s*#\s*define\s+(\w+)\s+((?:"(?:[^"\\]|\\.)*"\s*)+)$',
                    re.MULTILINE)


def split_arguments(arguments):
    """Top-level call arguments. Unlike the contract test's helper, a
    comma or bracket inside a string literal ("FINISH, THEN RETRY") does
    not split or nest."""
    parts, start, depth, quote, escaped = [], 0, 0, None, False
    for index, char in enumerate(arguments):
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(arguments[start:index].strip())
            start = index + 1
    parts.append(arguments[start:].strip())
    return parts


def shipping_text(source):
    """Blank the lines a shipping build does not compile."""
    def evaluate(expression):
        expression = re.sub(
            r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
            lambda m: "1" if (m.group(1) or m.group(2)) in SHIPPING_DEFINED
            else "0", expression)
        expression = (expression.replace("&&", " and ")
                      .replace("||", " or ").replace("!", " not "))
        expression = re.sub(
            r"\b[A-Za-z_]\w*\b",
            lambda m: m.group(0) if m.group(0) in ("and", "or", "not")
            else "0", expression)
        try:
            return bool(eval(expression, {"__builtins__": {}}))
        except Exception:
            return True  # Unknown: keep both branches.

    output, stack, active = [], [], True
    lines = source.split("\n")
    index = 0
    while index < len(lines):
        line = lines[index]
        while line.rstrip().endswith("\\") and index + 1 < len(lines):
            index += 1
            line = line.rstrip()[:-1] + " " + lines[index]
        directive = re.match(
            r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
        if directive is None:
            output.append(line if active else "")
        else:
            kind, expression = directive.group(1), directive.group(2).strip()
            if kind in ("ifdef", "ifndef", "if"):
                if kind == "if":
                    taken = evaluate(expression)
                else:
                    taken = (expression.split()[0] in SHIPPING_DEFINED) \
                        == (kind == "ifdef")
                stack.append((active, taken))
                active = active and taken
            elif kind == "elif":
                parent, done = stack[-1]
                taken = not done and evaluate(expression)
                stack[-1] = (parent, done or taken)
                active = parent and taken
            elif kind == "else":
                parent, done = stack[-1]
                stack[-1] = (parent, True)
                active = parent and not done
            else:
                active = stack.pop()[0]
            output.append("")
        index += 1
    return "\n".join(output)


def literal_values(expression, macros):
    for name, value in macros.items():
        expression = re.sub(r"\b%s\b" % name, value, expression)
    for run in LITERAL_RUN.finditer(expression):
        text = "".join(LITERAL.findall(run.group(0)))
        yield text.encode("utf-8").decode("unicode_escape")


def function_body(source, name):
    """The braces-delimited body of NAME's definition, or ""."""
    for match in re.finditer(r"\b%s\s*\([^;{]*\)\s*\{" % re.escape(name),
                             source):
        depth, start = 0, match.end() - 1
        for index in range(start, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
    return ""


def calls_with_offsets(source, function):
    """(offset, arguments) of each call of FUNCTION."""
    for match in re.finditer(r"\b%s\s*\(" % re.escape(function), source):
        depth, quote, escaped = 1, None, False
        cursor = match.end()
        while cursor < len(source) and depth:
            char = source[cursor]
            if quote is not None:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
            elif char in "\"'":
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            cursor += 1
        yield match.start(), source[match.end():cursor - 1]


def enclosing_body(source, offset):
    """The top-level braces-delimited block containing OFFSET, or ""."""
    depth, start = 0, None
    quote, escaped = None, False
    for index, char in enumerate(source):
        if quote is not None:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in "\"'":
            quote = char
        elif char == "{":
            if depth == 0:
                start = index
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0 and start is not None:
                if start <= offset <= index:
                    return source[start:index + 1]
                start = None
    return ""


def assigned_expressions(body, name):
    """Right-hand sides assigned to local NAME ("name = ...;", including a
    declaration's initializer)."""
    return [m.group(1) for m in re.finditer(
        r"(?<![\w.>])%s\s*=(?!=)((?:[^;\"]|\"(?:[^\"\\]|\\.)*\")*);"
        % re.escape(name), body)]


def toast_keys(text):
    """The catalog keys draw_toast looks up for one status."""
    for line in text.split("\n"):
        line = PROGRESS_SUFFIX.sub("", line)
        if line:
            yield line


def job_message_expressions(source):
    """The literal messages an installer job can hand to the toast."""
    expressions = []
    for function, index in JOB_MESSAGE_CALLS:
        for arguments in call_arguments(source, function):
            parts = split_arguments(arguments)
            if len(parts) > index:
                expressions.append(parts[index])
    for arguments in call_arguments(source, "snprintf"):
        parts = split_arguments(arguments)
        if len(parts) == 3 and JOB_MESSAGE_BUFFER.fullmatch(parts[0]):
            expressions.append(parts[2])
    return [expression for expression in expressions
            if not expression.rstrip().endswith('..."')]


def validation_only_sources():
    """Files CMake compiles only into the validation psp-browser-script.

    Read from the target_sources block that adds the scripted-input harness,
    so a unit moved there stops counting as shipped without a second list."""
    cmake = (ROOT / "cmake/TilefinchTargets.cmake").read_text(encoding="utf-8")
    for block in re.findall(r"target_sources\(psp-browser-script PRIVATE(.*?)\)",
                            cmake, re.S):
        if "src/psp_app/psp_app_input_script.c" in block:
            return {ROOT / name for name in re.findall(r"src/[\w/]+\.c", block)}
    raise AssertionError("validation-only psp-browser-script sources not found")


def shipped_toasts():
    macros = {}
    sources = []
    validation_only = validation_only_sources()
    for pattern in SOURCES + JOB_MESSAGE_SOURCES:
        sources += [path for path in sorted(ROOT.glob(pattern))
                    if path not in validation_only]
    sources += sorted(ROOT.glob("src/psp_app/*.h"))
    for path in sources:
        source = without_comments(path.read_text(encoding="utf-8"))
        for match in DEFINE.finditer(source):
            macros[match.group(1)] = match.group(2)
    found = {}
    for path in sources:
        if path.suffix != ".c":
            continue
        source = shipping_text(
            without_comments(path.read_text(encoding="utf-8")))
        expressions = []
        for function, index in STATUS_CALLS:
            for offset, arguments in calls_with_offsets(source, function):
                parts = split_arguments(arguments)
                if len(parts) <= index or "snprintf" in parts[index]:
                    continue
                expressions.append(parts[index])
                # A literal routed through a local variable.
                if IDENTIFIER.fullmatch(parts[index]):
                    expressions += assigned_expressions(
                        enclosing_body(source, offset), parts[index])
        for arguments in call_arguments(source, "snprintf"):
            parts = split_arguments(arguments)
            message_buffer = MESSAGE_BUFFER.get(path.name)
            if len(parts) < 3 or not (
                    STATUS_BUFFER.match(parts[0])
                    or (message_buffer is not None
                        and message_buffer.fullmatch(parts[0]))):
                continue
            formats = list(literal_values(parts[2], macros))
            if formats and all("%" not in text for text in formats):
                expressions.append(parts[2])
            elif len(formats) == 1 and WHOLE_LINE_FORMAT.fullmatch(formats[0]):
                expressions += parts[3:]
        for arguments in call_arguments(source, "copy_string"):
            parts = split_arguments(arguments)
            if len(parts) == 3 and PAINTED_STATE.fullmatch(parts[0]):
                expressions.append(parts[2])
        for name in PAINTED_TABLES.get(path.name, ()):
            for match in re.finditer(
                    r"\bconst\s+%s\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}"
                    % re.escape(name), source):
                expressions.append(match.group(1))
        expressions += SITE_DATA_HEADING.findall(source)
        expressions += STATUS_FIELD.findall(source)
        for function in STATUS_RETURNS:
            body = function_body(source, function)
            expressions += [m.group(1)
                            for m in RETURN_EXPRESSION.finditer(body)]
        if any(path.match(pattern) for pattern in JOB_MESSAGE_SOURCES):
            expressions += job_message_expressions(source)
        for expression in expressions:
            for value in literal_values(expression, macros):
                for key in toast_keys(value):
                    found.setdefault(key, set()).add(path.name)
    return found


def active_catalog():
    version, languages = generate_ui_translations.CATALOGS[-1]
    return {row[0].lower(): row
            for row in generate_ui_translations.read_rows(version, languages)}


def join_adjacent_literals(text):
    """Join adjacent C string literals ("a " "b", on one line or across
    lines) the way the compiler does, so split keys match."""
    return re.sub(r'"\s*"', "", text)


def referenced_keys(catalog):
    """Catalog keys a C string or a {{key}} page template mentions."""
    corpus = []
    for pattern in ("src/**/*.c", "src/**/*.h", "src/**/*.inc",
                    "src/bootstrap/*.js", "include/**/*.h"):
        for path in ROOT.glob(pattern):
            if "generated" not in path.parts \
                    or path.name == "ui_languages.inc":
                corpus.append(path.read_text(encoding="utf-8",
                                             errors="replace").lower())
    corpus = join_adjacent_literals("\n".join(corpus))
    toasts = {key.lower() for key in shipped_toasts()}
    return {key for key in catalog
            if '"' + key + '"' in corpus or "{{" + key + "}}" in corpus
            or key in toasts}


class UiToastCatalogTests(unittest.TestCase):
    def test_every_shipped_toast_has_a_catalog_key(self):
        catalog = active_catalog()
        missing = sorted(key for key in shipped_toasts()
                         if key.lower() not in catalog)
        self.assertEqual([], missing)

    def test_catalog_keeps_no_unreferenced_key(self):
        catalog = active_catalog()
        unused = sorted(catalog[key][0]
                        for key in set(catalog) - referenced_keys(catalog))
        self.assertEqual([], unused)

    def test_helper_routed_toasts_are_seen(self):
        # Statuses that reach ui->status through a helper, a returned
        # literal, or a composed snprintf line.
        toasts = shipped_toasts()
        for key in ("LANGUAGE DOWNLOAD CANCELLED", "STOPPING...",
                    "STOPPING ZIP CHECK...", "Downloading...", "Checking...",
                    "Downloading", "Installing", "Installed",
                    "Language pack installed",
                    "Installed. Reload the page to redraw it",
                    "FONT PACK READY AFTER RESTART",
                    # An action's acknowledgement.
                    "OPENING HOME...",
                    # snprintf into ready_status.
                    "PAGE READY", "PAGE READY - TAB STATE LIMITED",
                    # A comma inside the literal.
                    "WAIT FOR THE PAGE TO FINISH, THEN RETRY",
                    # A font-pack install's failure and cancel messages.
                    "FONT PACK COULD NOT BE ACTIVATED",
                    "FONT PACK RECOVERY FAILED", "CANCELLED"):
            self.assertIn(key, toasts)
        # Painted from state: a literal routed through a local variable,
        # a setting's reload notices, a theme error buffer, the Update
        # screen's messages and button, and the video player's headline.
        for key in ("SITE LIST FULL", "SCRIPT CACHE SET BY BOOT.CFG",
                    "JAVASCRIPT ON - RELOADING", "SETTING SAVED - RELOAD FAILED",
                    "THEME NAME INVALID", "INVALID THEME COLOR",
                    "ACTIVATING UPDATE...", "UPDATE COULD NOT BE ACTIVATED",
                    "DOWNLOAD VERIFIED", "UNSIGNED DOWNLOAD HASHED",
                    "CHECK NOW", "Decoder rebuild needed",
                    "STABLE CHANNEL - SIGNED", "STARTING VIDEO",
                    "VIDEO DECODER NEEDS APP RESTART", "Restarting decoder",
                    "NETWORK NOT READY FOR FONT PACK"):
            self.assertIn(key, toasts)
        # A plain on/off setting's confirmations, from its descriptor.
        for key in ("URL HISTORY ON", "URL HISTORY OFF", "ROTATE HINT AUTO",
                    "YOUTUBE RESULTS COMPACT - RELOAD TO APPLY"):
            self.assertIn(key, toasts)
        # Site data & storage: a storage choice, its explanation, a row,
        # a section heading and the empty-list line.
        for key in ("This session", "On the Memory Stick until you exit.",
                    "Clear session storage", "CLEAR ALL SITES",
                    "No site is storing data"):
            self.assertIn(key, toasts)
        # Site information: the title, the connection states, the data
        # labels, the actions (a painted table) and the issuer placeholders
        # psp_app_input.c translates when it fills the screen.
        for key in ("Site information", "Secure", "TLS status unavailable",
                    "Certificate warning", "Not secure", "Cookies:",
                    "Storage:", "Permissions & controls",
                    "Clear data for this site", "Reset permissions",
                    "Certificate issuer unavailable", "No TLS certificate"):
            self.assertIn(key, toasts)
        # Install progress is shown as "Installing 45%", never as text.
        self.assertNotIn("FINALIZING FONT PACK...", toasts)
        # The test menu exists only in the validation build.
        self.assertNotIn("POWER TEST CHANGING...", toasts)
        # So do the validation-only units CMake adds to psp-browser-script.
        self.assertIn(ROOT / "src/psp_app/psp_app_validation_loop.c",
                      validation_only_sources())
        self.assertNotIn("POWER TEST STOPPED", toasts)
        self.assertNotIn("UNPLUG AC POWER, THEN START TEST", toasts)

    def test_arguments_split_outside_literals(self):
        self.assertEqual(['ui', '"A, B (C"', 'f(1, 2)'],
                         split_arguments('ui, "A, B (C", f(1, 2)'))

    def test_local_variable_routes_are_followed(self):
        source = ('void f(int a) {\n    const char *status = "A";\n'
                  '    if (a) status = a > 1 ? "B" : "C";\n'
                  '    if (status == "X") return;\n'
                  '    show(status);\n}\n'
                  'void g(void) { status = "D"; }\n')
        offset = source.index("show(")
        body = enclosing_body(source, offset)
        self.assertNotIn('"D"', body)
        self.assertEqual(['"A"', 'a > 1 ? "B" : "C"'],
                         [e.strip() for e in assigned_expressions(body, "status")])

    def test_adjacent_literals_join_on_one_line(self):
        self.assertEqual('f("A B");', join_adjacent_literals('f("A " "B");'))
        self.assertEqual('f("A B");',
                         join_adjacent_literals('f("A "\n    "B");'))

    def test_suffix_rule_matches_the_painter(self):
        self.assertEqual(["SAVING SCREENSHOT"],
                         list(toast_keys("SAVING SCREENSHOT 45%")))
        self.assertEqual(["A", "B"], list(toast_keys("A\nB")))


if __name__ == "__main__":
    if "--list" in sys.argv:
        catalog = active_catalog()
        toasts = shipped_toasts()
        for key in sorted(toasts, key=str.lower):
            if key.lower() not in catalog:
                print("missing\t%s\t%s" % (key, ",".join(sorted(toasts[key]))))
        for key in sorted(set(catalog) - referenced_keys(catalog)):
            print("unused\t%s" % catalog[key][0])
        raise SystemExit(0)
    unittest.main(argv=[sys.argv[0]] + [a for a in sys.argv[2:]
                                         if a.startswith("-")])
