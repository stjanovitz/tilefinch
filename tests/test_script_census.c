#include "tilefinch/script_census.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); return 1; } } while (0)
static uint32_t parent_function;
static JSValue probe(JSContext *ctx, JSValueConst receiver, int argc, JSValueConst *argv)
{
    (void)receiver; (void)argc; (void)argv;
    uintptr_t address;
    unsigned zone = JS_ReadExecutionCensus(&address);
    uint32_t function = JS_ReadExecutionCensusFunction();
    int step = 0;
    if (argc && JS_ToInt32(ctx, &step, argv[0]) < 0) return JS_EXCEPTION;
    bool identity = true;
    if (step == 1) parent_function = function;
    else if (step == 2) identity = function != parent_function;
    else if (step == 3) identity = function == parent_function;
    return JS_NewBool(ctx, zone == JS_CENSUS_NATIVE && address == (uintptr_t)probe
        && function != 0 && identity);
}
int main(int argc, char **argv)
{
    Budget budget;
    budget_init(&budget, 2u * 1024u * 1024u);
    JSRuntime *rt = JS_NewRuntime();
    CHECK(rt);
    JSContext *ctx = JS_NewContext(rt);
    CHECK(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx, global, "probe", JS_NewCFunction(ctx, probe, "probe", 0)) >= 0);
    CHECK(setenv("TILEFINCH_EXECUTION_CENSUS", "2", 1) == 0);
    script_census_attach(rt, &budget);
    CHECK(budget.current > 0);
    if (argc > 1 && strcmp(argv[1], "--negative-control") == 0)
        JS_SetExecutionCensus(rt, 0);
    script_split_set_enabled(true);
    unsigned token = script_split_enter(SCRIPT_SPLIT_JS);
    const char *source = "let ok=probe(1);let p={get x(){return probe(2)}};ok=ok&&p.x&&probe(3);"
        "let inherited=Object.create({z:1});ok=ok&&inherited.z===1;"
        "let exotic=new Proxy({y:1},{});ok=ok&&exotic.y===1;"
        "function nested(){return probe()}ok=ok&&nested.bind(null)();"
        "function pad(a,b){return a||b}ok=ok&&pad(true);"
        "function capture(x){let v=x;return()=>v}ok=ok&&capture(true)();"
        "function bad(){if(!probe(2))throw 1;throw Error('x')}"
        "try{bad()}catch(e){};ok&&probe(3)";
    JSValue value = JS_Eval(ctx, source, strlen(source), "census.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(value) && JS_ToBool(ctx, value) == 1);
    JS_FreeValue(ctx, value);
    uintptr_t address;
    CHECK(JS_ReadExecutionCensus(&address) == JS_CENSUS_OUTSIDE);
    CHECK(JS_ReadExecutionCensusFunction() == 0);
    uint64_t paths[JS_CENSUS_COUNT];
    CHECK(JS_ReadExecutionCensusPaths(paths, JS_CENSUS_COUNT) == JS_CENSUS_COUNT);
    CHECK(paths[JS_CENSUS_GET_OWN]);
    CHECK(paths[JS_CENSUS_GET_PROTOTYPE]);
    CHECK(paths[JS_CENSUS_GET_GETTER]);
    CHECK(paths[JS_CENSUS_GET_EXOTIC]);
    CHECK(paths[JS_CENSUS_CALL_BYTECODE]);
    CHECK(paths[JS_CENSUS_CALL_NATIVE]);
    CHECK(paths[JS_CENSUS_CALL_BOUND]);
    uint64_t frames[5] = {0};
    const char *frame_names[] = {"call-frame-setup", "call-frame-arguments",
        "call-frame-initialize", "call-frame-cleanup", "call-frame-simple"};
    for (unsigned z = 256; z < JS_CENSUS_COUNT; z++)
        for (unsigned f = 0; f < 5; f++)
            if (strcmp(JS_ExecutionCensusName(z), frame_names[f]) == 0)
                frames[f] = paths[z];
    CHECK(frames[0] && frames[1] && frames[2] && frames[3] && frames[4]);
    CHECK(frames[0] == frames[2] && frames[0] == frames[3]);
    CHECK(frames[4] <= frames[0] && frames[1] <= frames[0]);
    uint64_t references[3];
    JS_ReadExecutionCensusReferences(UINT32_MAX, references);
#ifdef CONFIG_TILEFINCH_REFCOUNT_CENSUS
    CHECK(references[0] && references[1] && references[2]);
#else
    CHECK(!references[0] && !references[1] && !references[2]);
#endif
    CHECK(JS_ReadExecutionCensusPaths(paths, 1) == 0);
    JS_ResetExecutionCensusPaths();
    const char *constructor_source = "function C(){} new C();";
    value = JS_Eval(ctx, constructor_source, strlen(constructor_source),
                    "census-constructor.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(value));
    JS_FreeValue(ctx, value);
    CHECK(JS_ReadExecutionCensusPaths(paths, JS_CENSUS_COUNT) == JS_CENSUS_COUNT);
    CHECK(paths[JS_CENSUS_FRAME_SETUP] == 2);
    /* The script entry is ordinary; the constructor needs special semantics. */
    CHECK(paths[JS_CENSUS_FRAME_SIMPLE] == 1);
    JS_ResetExecutionCensusPaths();
    const char *generator_source =
        "function* g(){yield 1;return 2}const it=g();it.next();it.next();";
    value = JS_Eval(ctx, generator_source, strlen(generator_source),
                    "census-generator.js", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(value));
    JS_FreeValue(ctx, value);
    CHECK(JS_ReadExecutionCensusPaths(paths, JS_CENSUS_COUNT) == JS_CENSUS_COUNT);
    /* Resumes reuse the generator's state; only the script creates a frame. */
    CHECK(paths[JS_CENSUS_FRAME_SETUP] == 1);
    CHECK(paths[JS_CENSUS_FRAME_CLEANUP] == 1);
    CHECK(JS_ReadExecutionCensus(&address) == JS_CENSUS_OUTSIDE);
    uint32_t job = script_census_job_begin(100);
    CHECK(job && script_census_job_begin(101) == 0);
    script_census_job_end(job, 200);
    CHECK(script_census_job_begin(300) != 0);
    script_split_leave(token);
    script_census_detach(rt);
    CHECK(budget.current == 0);
    /* Repeated attachment and admission refusal must release all ownership. */
    script_census_attach(rt, &budget);
    script_census_detach(rt);
    CHECK(budget.current == 0);
    Budget small;
    budget_init(&small, 1);
    script_census_attach(rt, &small);
    CHECK(small.current == 0);
    script_census_detach(rt);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    CHECK(unsetenv("TILEFINCH_EXECUTION_CENSUS") == 0);
    return 0;
}
