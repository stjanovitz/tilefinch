/* Input events the page sees for the presses a PSP user makes.

   Cross (X) on a highlighted element is the Enter key of a TV or console
   browser: keydown, keypress, the keyboard-style click (detail 0, no
   pointer), keyup. X with the analog cursor showing is a mouse button:
   pointer and mouse events at the cursor, then a click with detail 1. Text
   from the on-screen keyboards arrives as a phone keyboard's does (key
   "Unidentified", keyCode 229); the keyboard session's Done commits one
   composition. Backspace and Enter are their own keys. Each case drives the
   controller the way the PSP frontend does and compares the trusted event
   log the page recorded. */

#include "tilefinch/budget.h"
#include "tilefinch/controller.h"
#include "tilefinch/document.h"
#include "tilefinch/js_runtime.h"
#include "tilefinch/layout.h"
#include "tilefinch/navigation.h"

#include <stdio.h>
#include <string.h>

#define MIB (1024u * 1024u)
#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "input events test failed at %s:%d: %s\n",         \
                __FILE__, __LINE__, #condition);                             \
        return false;                                                        \
    }                                                                        \
} while (0)

static lxb_dom_node_t *find_id(lxb_dom_node_t *node, const char *wanted)
{
    if (node == NULL) return NULL;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t length = 0;
        const char *id = document_attribute(node, "id", &length);
        if (id != NULL && strlen(wanted) == length
            && memcmp(id, wanted, length) == 0) return node;
    }
    for (lxb_dom_node_t *child = node->first_child; child != NULL;
         child = child->next) {
        lxb_dom_node_t *found = find_id(child, wanted);
        if (found != NULL) return found;
    }
    return NULL;
}

/* Every listener runs in the capture phase on window, so the log shows each
   event once with its real target. Unrecorded types do not appear. */
static const char page_prefix[] =
    "<!doctype html><title>Input</title>"
    "<meta name=viewport content='width=device-width'><body>"
    "<form id=form action='/submit'><input id=field name=q value=ab>"
    "<button id=send>Send</button></form>"
    "<input id=loose value=x><input id=picker readonly value=date>"
    "<a id=link href='/next'>Next</a>"
    "<div id=box onclick=\"this.title='clicked'\">Box</div>"
    "<div id=editor contenteditable=true>hi</div>"
    "<script>var L=[];"
    "function who(t){return t===document?'doc':t===document.body?'body'"
    ":t===window?'win':(t&&t.id)||'?'}"
    "function val(t){return t.tagName==='INPUT'||t.tagName==='TEXTAREA'"
    "?t.value:t.textContent}"
    "function rec(e){var s=e.type+'@'+who(e.target)+(e.isTrusted?'':'!untrusted');"
    "if(e instanceof KeyboardEvent)s+=':'+e.key+','+e.code+','+e.keyCode+','"
    "+e.which+','+e.charCode+','+e.isComposing;"
    "else if(e instanceof CompositionEvent)s+=':'+JSON.stringify(e.data)"
    "+','+JSON.stringify(val(e.target));"
    "else if(e instanceof InputEvent)s+=':'+e.inputType+','"
    "+JSON.stringify(e.data)+','+e.isComposing+','+e.cancelable+','"
    "+JSON.stringify(val(e.target));"
    "else if(e.type==='textInput')s+=':'+JSON.stringify(e.data);"
    "else if(e.type==='click')s+=':'+e.constructor.name+','+e.detail+','"
    "+JSON.stringify(e.pointerType)+','+(e.clientX>0);"
    "else if(e.type.indexOf('pointer')===0)s+=':'+e.pointerType;"
    "L.push(s)}"
    "['keydown','keypress','keyup','compositionstart','compositionupdate',"
    "'compositionend','beforeinput','textInput','input','change',"
    "'selectionchange','pointerdown','mousedown','pointerup','mouseup',"
    "'click','submit'].forEach(function(t){window.addEventListener(t,rec,true)});";
static const char page_suffix[] = "</script></body>";

typedef struct {
    Budget budget;
    NavigationSession navigation;
    BrowserController controller;
} InputPage;

static InputPage page;

static bool open_page(const char *script)
{
    char html[8192];
    int length = snprintf(html, sizeof(html), "%s%s%s", page_prefix,
                          script == NULL ? "" : script, page_suffix);
    CHECK(length > 0 && (size_t) length < sizeof(html));
    memset(&page, 0, sizeof(page));
    budget_init(&page.budget, 32 * MIB);
    CHECK(budget_install_lexbor(&page.budget));
    CHECK(navigation_init(&page.navigation, &page.budget, 2));
    navigation_enable_scripts(&page.navigation, 4 * MIB, 1000);
    page.navigation.defer_script_navigation = true;
    CHECK(navigation_commit_html(
        &page.navigation, navigation_begin(&page.navigation),
        "https://input.test/page", html, (size_t) length, 480, NULL, NULL,
        true));
    CHECK(controller_init(&page.controller, &page.navigation));
    CHECK(controller_configure_viewport(&page.controller, 272, 0));
    return true;
}

static bool close_page(void)
{
    navigation_destroy(&page.navigation);
    bool released = page.budget.current == 0;
    budget_uninstall_lexbor(&page.budget);
    CHECK(released);
    return true;
}

static lxb_dom_node_t *node(const char *id)
{
    return find_id(lxb_dom_interface_node(page.navigation.page.document.html),
                   id);
}

static bool evaluate(const char *script)
{
    ScriptResult *result = &page.navigation.page.script_result;
    return script_runtime_evaluate_diagnostic(
        page.navigation.page.runtime, script, "<input-events-test>", result);
}

static bool focus(const char *id)
{
    CHECK(controller_focus_node(&page.controller, node(id)));
    /* Focus moves log their own selection changes; cases start clean. */
    CHECK(evaluate("L.length=0"));
    return true;
}

static bool log_is(const char *expected)
{
    CHECK(evaluate("pocSummary=L.join('|');L.length=0"));
    const char *actual = page.navigation.page.script_result.summary;
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "expected: %s\nactual:   %s\n", expected, actual);
        return false;
    }
    return true;
}

static bool value_is(const char *id, const char *expected)
{
    char script[128];
    snprintf(script, sizeof(script),
             "pocSummary=document.getElementById('%s').value", id);
    CHECK(evaluate(script));
    const char *actual = page.navigation.page.script_result.summary;
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "value of %s: expected %s, actual %s\n", id,
                expected, actual);
        return false;
    }
    return true;
}

#define ENTER_DOWN(at) "keydown@" at ":Enter,Enter,13,13,0,false"
#define ENTER_PRESS(at) "keypress@" at ":Enter,Enter,13,13,13,false"
#define ENTER_UP(at) "keyup@" at ":Enter,Enter,13,13,0,false"
#define KEY_CLICK(at) "click@" at ":PointerEvent,0,\"\",false"
#define VIRTUAL_DOWN(at) "keydown@" at ":Unidentified,,229,229,0,false"
#define VIRTUAL_UP(at) "keyup@" at ":Unidentified,,229,229,0,false"

/* X on a focused button: Enter around a keyboard-style click. */
static bool test_button_enter(void)
{
    CHECK(open_page(NULL) && focus("send"));
    ControllerAction action;
    CHECK(controller_activate(&page.controller, &action));
    CHECK(action.type == CONTROLLER_ACTION_FORM_SUBMIT);
    CHECK(log_is(ENTER_DOWN("send") "|" ENTER_PRESS("send") "|"
                 KEY_CLICK("send") "|submit@form|" ENTER_UP("send")));
    return close_page();
}

/* X on a link; a cancelled keydown or keypress cancels the activation, and
   keyup still follows the release. */
static bool test_link_enter(void)
{
    CHECK(open_page(NULL) && focus("link"));
    ControllerAction action;
    CHECK(controller_activate(&page.controller, &action));
    CHECK(action.type == CONTROLLER_ACTION_NAVIGATE
          && strcmp(action.url, "https://input.test/next") == 0);
    CHECK(log_is(ENTER_DOWN("link") "|" ENTER_PRESS("link") "|"
                 KEY_CLICK("link") "|" ENTER_UP("link")));
    CHECK(close_page());

    CHECK(open_page("document.getElementById('link').addEventListener("
                    "'keydown',function(e){e.preventDefault()});")
          && focus("link"));
    CHECK(controller_activate(&page.controller, &action));
    CHECK(action.type == CONTROLLER_ACTION_NONE);
    CHECK(log_is(ENTER_DOWN("link") "|" ENTER_UP("link")));
    CHECK(close_page());

    CHECK(open_page("document.getElementById('link').addEventListener("
                    "'keypress',function(e){e.preventDefault()});")
          && focus("link"));
    CHECK(controller_activate(&page.controller, &action));
    CHECK(action.type == CONTROLLER_ACTION_NONE);
    CHECK(log_is(ENTER_DOWN("link") "|" ENTER_PRESS("link") "|"
                 ENTER_UP("link")));
    return close_page();
}

/* A highlighted element that cannot take focus: the keys go to the body,
   not to a field focused earlier, and the click to the element. */
static bool test_unfocusable_enter(void)
{
    CHECK(open_page(NULL) && focus("loose"));
    const LayoutNodeBox *box = layout_box_for_node(
        &page.navigation.page.layout, node("box"));
    CHECK(box != NULL
          && controller_focus_at(&page.controller, box->x + 2, box->y + 2)
          && page.controller.focus_kind == CONTROLLER_FOCUS_POINTER
          && evaluate("L.length=0"));
    ControllerAction action;
    CHECK(controller_activate(&page.controller, &action));
    CHECK(log_is(ENTER_DOWN("body") "|" ENTER_PRESS("body") "|"
                 KEY_CLICK("box") "|" ENTER_UP("body")));
    return close_page();
}

/* X with the analog cursor: a mouse press and release at the cursor, then
   the pointer's click. No key events. */
static bool test_cursor_press(void)
{
    CHECK(open_page(NULL));
    const LayoutDocument *layout = &page.navigation.page.layout;
    const ControlRegion *button = NULL;
    for (size_t i = 0; i < layout->control_count; i++) {
        if (layout->controls[i].node == node("send"))
            button = &layout->controls[i];
    }
    CHECK(button != NULL);
    int x = button->x + button->width / 2, y = button->y + button->height / 2;
    bool activate = false;
    CHECK(controller_pointer_event(&page.controller, CONTROLLER_POINTER_DOWN,
                                   x, y, &activate));
    CHECK(controller_pointer_event(&page.controller, CONTROLLER_POINTER_UP,
                                   x, y, &activate) && activate);
    CHECK(controller_commit_pointer_click(&page.controller));
    CHECK(log_is("pointerdown@send:mouse|mousedown@send|pointerup@send:mouse"
                 "|mouseup@send|click@send:PointerEvent,1,\"mouse\",true"));
    return close_page();
}

/* The keyboard session's Done: one composition replaces the field's text.
   beforeinput cannot cancel it; the value is in place by compositionend;
   input follows compositionend, as in Chrome. */
static bool test_keyboard_commit(void)
{
    CHECK(open_page(NULL) && focus("field"));
    CHECK(controller_replace_text(&page.controller, "hello", 5));
    CHECK(value_is("field", "hello"));
    CHECK(log_is(VIRTUAL_DOWN("field")
                 "|compositionstart@field:\"\",\"ab\""
                 "|beforeinput@field:insertCompositionText,\"hello\",true,false,\"ab\""
                 "|compositionupdate@field:\"hello\",\"ab\""
                 "|compositionend@field:\"hello\",\"hello\""
                 "|input@field:insertCompositionText,\"hello\",false,false,\"hello\""
                 "|selectionchange@field|" VIRTUAL_UP("field")
                 "|change@field"));
    CHECK(evaluate("var f=document.getElementById('field');"
                   "pocSummary=f.selectionStart+','+f.selectionEnd"));
    CHECK(strcmp(page.navigation.page.script_result.summary, "5,5") == 0);
    /* Done with the text unchanged reports no typing. */
    CHECK(controller_replace_text(&page.controller, "hello", 5));
    CHECK(log_is(""));
    /* A page cannot refuse a phone keyboard's composition. */
    CHECK(evaluate("document.getElementById('field').addEventListener("
                   "'beforeinput',function(e){e.preventDefault()})"));
    CHECK(controller_replace_text(&page.controller, "world", 5));
    CHECK(value_is("field", "world"));
    CHECK(evaluate("L.length=0"));
    return close_page();
}

/* Typed text at the caret: a non-composing keyboard commit. */
static bool test_keyboard_insert(void)
{
    CHECK(open_page(NULL) && focus("field"));
    CHECK(controller_insert_text(&page.controller, "c", 1));
    CHECK(value_is("field", "abc"));
    CHECK(log_is(VIRTUAL_DOWN("field") "|textInput@field:\"c\""
                 "|beforeinput@field:insertText,\"c\",false,true,\"ab\""
                 "|input@field:insertText,\"c\",false,false,\"abc\""
                 "|selectionchange@field|" VIRTUAL_UP("field")));
    /* Refused insertion still releases its key. */
    CHECK(evaluate("document.getElementById('field').addEventListener("
                   "'beforeinput',function(e){e.preventDefault()});L.length=0"));
    CHECK(controller_insert_text(&page.controller, "d", 1));
    CHECK(value_is("field", "abc"));
    CHECK(log_is(VIRTUAL_DOWN("field") "|textInput@field:\"d\""
                 "|beforeinput@field:insertText,\"d\",false,true,\"abc\""
                 "|" VIRTUAL_UP("field")));
    return close_page();
}

/* Backspace is its own key, and its keydown can cancel the deletion. */
static bool test_backspace(void)
{
    CHECK(open_page(NULL) && focus("field"));
    CHECK(controller_backspace(&page.controller));
    CHECK(value_is("field", "a"));
    CHECK(log_is("keydown@field:Backspace,Backspace,8,8,0,false"
                 "|beforeinput@field:deleteContentBackward,null,false,true,\"ab\""
                 "|input@field:deleteContentBackward,null,false,false,\"a\""
                 "|selectionchange@field"
                 "|keyup@field:Backspace,Backspace,8,8,0,false"));
    CHECK(evaluate("document.getElementById('field').addEventListener("
                   "'keydown',function(e){e.preventDefault()})"));
    CHECK(controller_backspace(&page.controller));
    CHECK(value_is("field", "a"));
    CHECK(log_is("keydown@field:Backspace,Backspace,8,8,0,false"
                 "|keyup@field:Backspace,Backspace,8,8,0,false"));
    return close_page();
}

/* Enter in a field (START, or the keyboard's Done on a form's only field):
   keydown and keypress on the field, the implicit submission they default
   to (the default button's click, then submit), keyup. Focus stays in the
   field. A cancelled keydown submits nothing. */
static bool test_field_enter(void)
{
    CHECK(open_page(NULL) && focus("field"));
    ControllerAction action;
    CHECK(controller_activate(&page.controller, &action));
    CHECK(action.type == CONTROLLER_ACTION_FORM_SUBMIT);
    CHECK(log_is(ENTER_DOWN("field") "|" ENTER_PRESS("field") "|"
                 KEY_CLICK("send") "|submit@form|" ENTER_UP("field")));
    CHECK(evaluate("pocSummary=document.activeElement.id"));
    CHECK(strcmp(page.navigation.page.script_result.summary, "field") == 0);
    CHECK(close_page());

    CHECK(open_page("document.getElementById('field').addEventListener("
                    "'keydown',function(e){if(e.key==='Enter')"
                    "e.preventDefault()});")
          && focus("field"));
    CHECK(controller_activate(&page.controller, &action));
    CHECK(action.type == CONTROLLER_ACTION_NONE);
    CHECK(log_is(ENTER_DOWN("field") "|" ENTER_UP("field")));
    CHECK(close_page());

    /* Outside a form, Enter is the keys alone: no click on the field. */
    CHECK(open_page(NULL) && focus("loose"));
    CHECK(controller_activate(&page.controller, &action));
    CHECK(log_is(ENTER_DOWN("loose") "|" ENTER_PRESS("loose") "|"
                 ENTER_UP("loose")));
    return close_page();
}

/* X on a readonly field outside a form stays a press that clicks (a field
   that opens a picker); a keydown handler that removes the pressed link
   leaves nothing to follow. */
static bool test_readonly_and_removed(void)
{
    CHECK(open_page(NULL) && focus("picker"));
    ControllerAction action;
    CHECK(controller_activate(&page.controller, &action));
    CHECK(log_is(ENTER_DOWN("picker") "|" ENTER_PRESS("picker") "|"
                 KEY_CLICK("picker") "|" ENTER_UP("picker")));
    CHECK(close_page());

    CHECK(open_page("document.getElementById('link').addEventListener("
                    "'keydown',function(){this.remove()});")
          && focus("link"));
    CHECK(controller_activate(&page.controller, &action));
    CHECK(action.type == CONTROLLER_ACTION_NONE);
    CHECK(log_is(ENTER_DOWN("link") "|" ENTER_PRESS("body") "|"
                 ENTER_UP("body")));
    return close_page();
}

/* contenteditable: the same composition; the document selection moves. */
static bool test_editable_commit(void)
{
    CHECK(open_page(NULL) && focus("editor"));
    CHECK(controller_replace_text(&page.controller, "hey", 3));
    CHECK(log_is(VIRTUAL_DOWN("editor")
                 "|compositionstart@editor:\"\",\"hi\""
                 "|beforeinput@editor:insertCompositionText,\"hey\",true,false,\"hi\""
                 "|compositionupdate@editor:\"hey\",\"hi\""
                 "|compositionend@editor:\"hey\",\"hey\""
                 "|input@editor:insertCompositionText,\"hey\",false,false,\"hey\""
                 "|selectionchange@doc|" VIRTUAL_UP("editor")
                 "|change@editor"));
    return close_page();
}

/* selectionchange is the browser's own event, also after a script call. */
static bool test_selectionchange_trusted(void)
{
    CHECK(open_page(NULL) && focus("field"));
    CHECK(evaluate("document.getElementById('field').setSelectionRange(0,1);"
                   "getSelection().collapse(document.body,0)"));
    CHECK(log_is("selectionchange@field|selectionchange@doc"));
    return close_page();
}

int main(void)
{
    static const struct {
        const char *name;
        bool (*run)(void);
    } cases[] = {
        { "X on a focused button is Enter", test_button_enter },
        { "X on a link is Enter; cancelled keys cancel it", test_link_enter },
        { "X on an unfocusable element sends keys to the body",
          test_unfocusable_enter },
        { "X with the cursor is a mouse press", test_cursor_press },
        { "keyboard Done commits a composition", test_keyboard_commit },
        { "typed text is a keyboard commit", test_keyboard_insert },
        { "Backspace is its own key", test_backspace },
        { "Enter in a field submits implicitly", test_field_enter },
        { "readonly field and a removed link", test_readonly_and_removed },
        { "contenteditable commit", test_editable_commit },
        { "selectionchange is trusted", test_selectionchange_trusted },
    };
    bool all_ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("test: input events: %s\n", cases[i].name);
        if (!cases[i].run()) {
            printf("FAIL: %s\n", cases[i].name);
            all_ok = false;
        }
    }
    return all_ok ? 0 : 1;
}
