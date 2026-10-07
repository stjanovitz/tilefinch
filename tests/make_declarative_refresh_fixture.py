"""Regenerates tests/fixtures/http-declarative-refresh, the response-keyed
replay used by the declarative refresh tests.

usage: make_declarative_refresh_fixture.py tests/fixtures/http-declarative-refresh
"""
import os, sys, shutil
OUT = sys.argv[1]
H = "https://refresh.test"
PAGES = [
    # (path, body, extra headers)
    ("/vk-away",
     "<!doctype html><html><head><meta name=\"referrer\" content=\"origin\">"
     "<title></title></head><body><script>location.replace("
     "'https://refresh.test/target?via=script')</script><noscript>"
     "<META http-equiv=\"refresh\" content=\"0;URL='https://refresh.test/target'\">"
     "<form action=\"https://refresh.test/target\" method=\"POST\">"
     "<input type=\"submit\" value=\"Continue\"></form></noscript></body></html>", []),
    ("/target", "<!doctype html><title>Target</title><p>Arrived at the target.</p>", []),
    ("/target?via=script", "<!doctype html><title>Script target</title><p>Arrived by script.</p>", []),
    ("/header",
     "<!doctype html><title>Header</title><meta http-equiv=\"refresh\" "
     "content=\"0;url=/target-meta\"><p>Header page.</p>",
     [("refresh", "0; url=/target-header")]),
    ("/target-header", "<!doctype html><title>Header target</title><p>From the header.</p>", []),
    ("/target-meta", "<!doctype html><title>Meta target</title><p>Wrong: meta beat the header.</p>", []),
    ("/delayed",
     "<!doctype html><title>Delayed</title><meta http-equiv=\"Refresh\" "
     "content=\"2; URL=later\"><p>Moving in two seconds.</p>", []),
    ("/later", "<!doctype html><title>Later</title><p>Later page.</p>", []),
    ("/self", "<!doctype html><title>Self</title><meta http-equiv=\"refresh\" content=\"0\"><p>Reloading.</p>", []),
    ("/reload", "<!doctype html><title>Reload</title><meta http-equiv=\"refresh\" content=\"1\"><p>News.</p>", []),
    ("/cancel",
     "<!doctype html><title>Cancel</title><meta http-equiv=\"refresh\" "
     "content=\"1;url=/target\"><p>Leave before one second.</p>", []),
    ("/other", "<!doctype html><title>Other</title><p>The user went here.</p>", []),
    ("/javascript",
     "<!doctype html><title>Javascript</title><meta http-equiv=\"refresh\" "
     "content=\"0;url=javascript:document.title='owned'\"><meta http-equiv=\"refresh\" "
     "content=\"0;url=/target\"><p>Never navigates.</p>", []),
    ("/data",
     "<!doctype html><title>Data</title><meta http-equiv=\"refresh\" "
     "content=\"0; URL='data:text/html,owned'\"><p>Never navigates.</p>", []),
    ("/sandbox-top",
     "<!doctype html><title>Sandbox top</title><p>Top document.</p>"
     "<iframe sandbox src=\"https://refresh.test/frame-refresh\"></iframe>", []),
    ("/frame-top",
     "<!doctype html><title>Frame top</title><p>Top document.</p>"
     "<iframe src=\"https://refresh.test/frame-refresh\"></iframe>", []),
    ("/frame-refresh",
     "<!doctype html><title>Frame</title><meta http-equiv=\"refresh\" "
     "content=\"0;url=https://refresh.test/escaped\"><p>Inside a frame.</p>", []),
    ("/escaped", "<!doctype html><title>Escaped</title><p>Wrong: a frame moved the top.</p>", []),
    ("/noscript",
     "<!doctype html><title>Noscript</title><noscript><meta http-equiv=\"refresh\" "
     "content=\"0;url=/target\"></noscript><p>Script-capable fallback.</p>", []),
    ("/dynamic",
     "<!doctype html><title>Dynamic</title><p>Script adds a refresh.</p><script>"
     "setTimeout(function(){var m=document.createElement('meta');"
     "m.httpEquiv='refresh';m.content='0;url=/target';"
     "document.head.appendChild(m);},50);</script>", []),
    ("/body-meta",
     "<!doctype html><title>Body meta</title><p>Body.</p><meta http-equiv=\"refresh\" "
     "content=\"0,url=/target\">", []),
    ("/template",
     "<!doctype html><title>Template</title><template><meta http-equiv=\"refresh\" "
     "content=\"0;url=/target\"></template><p>Inert.</p>", []),
    ("/base",
     "<!doctype html><title>Base</title><base href=\"https://refresh.test/sub/\">"
     "<meta http-equiv=\"refresh\" content=\" 0 ; url = 'page' \"><p>Base.</p>", []),
    ("/sub/page", "<!doctype html><title>Sub page</title><p>Resolved against the base.</p>", []),
    # Page-script navigations: only location.replace() replaces the entry.
    ("/assign",
     "<!doctype html><title>Assign</title><p>Assigning.</p><script>"
     "setTimeout(function(){location.assign('/target')},0)</script>", []),
    ("/href",
     "<!doctype html><title>Href</title><p>Setting href.</p><script>"
     "setTimeout(function(){location.href='/target'},0)</script>", []),
    ("/replace-state",
     "<!doctype html><title>Replace state</title><p>Same document.</p>"
     "<script>history.replaceState(null,'','/replaced-state')</script>", []),
    ("/form",
     "<!doctype html><title>Form</title><meta http-equiv=\"refresh\" "
     "content=\"1;url=/target\"><input id=\"q\" type=\"text\"><p>Type here.</p>", []),
    # A refresh whose URL is longer than a URL may be declares no refresh,
    # from the header as from a meta.
    ("/long-header", "<!doctype html><title>Long header</title><p>Stays.</p>",
     [("refresh", "0; url=" + H + "/target?" + "a" * 2100)]),
    ("/long-meta",
     "<!doctype html><title>Long meta</title><meta http-equiv=\"refresh\" "
     "content=\"0; url=" + H + "/target?" + "a" * 2100 + "\"><p>Stays.</p>", []),
]
if os.path.isdir(OUT):
    shutil.rmtree(OUT)
os.makedirs(OUT)
for index, (path, body, headers) in enumerate(PAGES):
    data = body.encode()
    url = H + path
    lines = ["psp-http-trace=1", "method=GET", "url=" + url, "success=1",
             "async-delay-pumps=1", "external-cancel=0", "transport-timeout=0",
             "error=", "status=200", "length=%d" % len(data),
             "effective-url=" + url, "content-type=text/html; charset=utf-8",
             "server=fixture", "set-cookie-count=0"]
    all_headers = [("content-type", "text/html; charset=utf-8")] + headers
    lines.append("response-header-count=%d" % len(all_headers))
    for at, (name, value) in enumerate(all_headers):
        lines.append("response-header-%d=%s: %s" % (at, name, value))
    with open(os.path.join(OUT, "%04d.meta" % index), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(OUT, "%04d.body" % index), "wb") as f:
        f.write(data)
with open(os.path.join(OUT, "trace.meta"), "w") as f:
    f.write("psp-http-trace-clock=1\norigin-ms=1700000000000\n"
            "capture-complete=yes\nrecord-count=%d\n" % len(PAGES))
