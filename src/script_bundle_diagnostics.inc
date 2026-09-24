/* Trace-gated rewrites of individual minified bundles, used to debug them on
 * the host: each exposes a caught value or records how far an async flow
 * got in a global the trace tooling reads back. The needles match one build
 * of each bundle; a needle that no longer matches is reported and skipped.
 * TILEFINCH_NO_TRACE builds compile only the two no-op entry points, so no
 * needle, bundle path or rewrite reaches a shipping binary.
 * Included by script_loader.c. */

#ifdef TILEFINCH_NO_TRACE

static size_t script_bundle_diagnostics_select(const char *url,
                                               unsigned *selected)
{
    (void) url;
    *selected = 0;
    return 0;
}

static void script_bundle_diagnostics_apply(unsigned selected, char *source,
                                            size_t *length)
{
    (void) selected;
    (void) source;
    (void) length;
}

#else

typedef struct {
    const char *label;
    const char *needle;
    const char *diagnostic;
} ScriptBundleRewrite;

typedef struct {
    const char *name;
    const ScriptBundleRewrite *rewrites;
    size_t count;
} ScriptBundleDiagnostic;

static const ScriptBundleRewrite script_react_error_rewrites[] = {
    { "caught", "stack:Me(t)",
      "stack:(console.error('react-caught',e&&e.name||typeof e,e&&e.message||String(e),e&&e.stack||e),'')" }
};

static const ScriptBundleRewrite script_sentinel_rewrites[] = {
    { "start",
      "async function Ms({requirements:e}){let[t,n]=await Promise.all",
      "async function Ms({requirements:e}){globalThis.__tilefinchSentinelStage='enforcement-start';let[t,n]=await Promise.all" },
    { "done",
      "]);return{proofToken:t,requirements:e,turnstileToken:n}}function Ns",
      "]);globalThis.__tilefinchSentinelStage='enforcement-done';return{proofToken:t,requirements:e,turnstileToken:n}}function Ns" },
    { "finalize",
      "let a=await Is(H.sentinelChatRequirementsFinalize,i);",
      "globalThis.__tilefinchSentinelStage='finalize-start';let a=await Is(H.sentinelChatRequirementsFinalize,i);globalThis.__tilefinchSentinelStage='finalize-done';" }
};

static const ScriptBundleRewrite script_submit_rewrites[] = {
    { "marker",
      "ye.current=S,r().then(async e=>{if(Ie.current)return;",
      "ye.current=S,globalThis.__tilefinchSentinelStage='submit-sentinel-call',r().then(async e=>{globalThis.__tilefinchSentinelStage='submit-sentinel-resolved';if(Ie.current)return;" },
    { "optimistic",
      ").searchParams)),Ze(d),Ye(!0),t.dataset.submitting=``;",
      ").searchParams)),globalThis.__tilefinchSentinelStage='submit-after-optimistic',Ze(d),Ye(!0),t.dataset.submitting=``;" },
    { "store",
      "if(_o({conversationState:mt,",
      "globalThis.__tilefinchSentinelStage='submit-before-store';if(_o({conversationState:mt," },
    { "rollback",
      "let S=()=>{if(!n&&",
      "globalThis.__tilefinchSentinelStage='submit-before-sentinel';let S=()=>{if(!n&&" }
};

static const ScriptBundleRewrite script_dpu_rewrites[] = {
    { "start",
      "Tr=async(e,t,n)=>{let r=e.getReader(),i=$t().getWriter(),a=new TextDecoder",
      "Tr=async(e,t,n)=>{globalThis.__tilefinchDpuStage='reader-start';let r=e.getReader(),i=$t().getWriter(),a=new TextDecoder" },
    { "read",
      "let{done:e,value:t}=await r.read();if(e){",
      "let{done:e,value:t}=await r.read();globalThis.__tilefinchDpuStage=e?'read-done':'read-'+String(t?.byteLength ?? -1);if(e){" },
    { "write",
      "o&&(n(`client_stream_apply`),await i.write(o))",
      "o&&(n(`client_stream_apply`),globalThis.__tilefinchDpuStage='write-'+String(o.length),await i.write(o),globalThis.__tilefinchDpuStage='write-done')" },
    { "catch",
      "}catch(e){throw await i.abort(e).catch(()=>{}),e}finally{",
      "}catch(e){globalThis.__tilefinchDpuStage='error:'+String(e&&e.message||e);console.error('stream-loop-error',e&&e.stack||e);throw await i.abort(e).catch(()=>{}),e}finally{" }
};

#define SCRIPT_BUNDLE_DIAGNOSTIC(name, rewrites) \
    { name, rewrites, sizeof(rewrites) / sizeof(rewrites[0]) }
static const ScriptBundleDiagnostic script_bundle_diagnostics[] = {
    SCRIPT_BUNDLE_DIAGNOSTIC("react-error", script_react_error_rewrites),
    SCRIPT_BUNDLE_DIAGNOSTIC("sentinel", script_sentinel_rewrites),
    SCRIPT_BUNDLE_DIAGNOSTIC("submit sentinel", script_submit_rewrites),
    SCRIPT_BUNDLE_DIAGNOSTIC("DPU", script_dpu_rewrites)
};
#undef SCRIPT_BUNDLE_DIAGNOSTIC

static bool script_bundle_diagnostic_wanted(size_t index, const char *url)
{
    bool shared = strstr(url, "/client-shared-") != NULL;
    bool client = strstr(url, "/client-C") != NULL && !shared;
    switch (index) {
    case 0:
        return tilefinch_trace_react_error()
            && strstr(url, "/react-stable-") != NULL;
    case 1: return tilefinch_trace_sentinel() && shared;
    case 2: return tilefinch_trace_sentinel() && client;
    case 3: return tilefinch_trace_dpu() && client;
    default: return false;
    }
}

/* Selects the rewrites that apply to url (a bit per diagnostic) and returns
   how many bytes they may add, so the caller can size one copy. */
static size_t script_bundle_diagnostics_select(const char *url,
                                               unsigned *selected)
{
    size_t extra = 0;
    *selected = 0;
    for (size_t i = 0; i < sizeof(script_bundle_diagnostics)
                               / sizeof(script_bundle_diagnostics[0]); i++) {
        if (!script_bundle_diagnostic_wanted(i, url)) continue;
        *selected |= 1u << i;
        const ScriptBundleDiagnostic *diagnostic =
            &script_bundle_diagnostics[i];
        for (size_t r = 0; r < diagnostic->count; r++) {
            extra += strlen(diagnostic->rewrites[r].diagnostic)
                - strlen(diagnostic->rewrites[r].needle);
        }
    }
    return extra;
}

static bool replace_first(char *buffer, size_t *length,
                          const char *needle, const char *replacement)
{
    char *at = strstr(buffer, needle);
    if (at == NULL) return false;
    size_t offset = (size_t) (at - buffer);
    size_t needle_length = strlen(needle);
    size_t replacement_length = strlen(replacement);
    memmove(buffer + offset + replacement_length,
            buffer + offset + needle_length,
            *length - offset - needle_length + 1);
    memcpy(buffer + offset, replacement, replacement_length);
    *length += replacement_length - needle_length;
    return true;
}

/* Applies the selected rewrites in place; source has room for the extra
   bytes script_bundle_diagnostics_select reported. */
static void script_bundle_diagnostics_apply(unsigned selected, char *source,
                                            size_t *length)
{
    for (size_t i = 0; i < sizeof(script_bundle_diagnostics)
                               / sizeof(script_bundle_diagnostics[0]); i++) {
        if ((selected & (1u << i)) == 0) continue;
        const ScriptBundleDiagnostic *diagnostic =
            &script_bundle_diagnostics[i];
        fprintf(stderr, "%s diagnostic:", diagnostic->name);
        for (size_t r = 0; r < diagnostic->count; r++) {
            bool applied = replace_first(
                source, length, diagnostic->rewrites[r].needle,
                diagnostic->rewrites[r].diagnostic);
            fprintf(stderr, " %s=%d", diagnostic->rewrites[r].label,
                    applied);
        }
        fputc('\n', stderr);
    }
}

#endif
