#import <Foundation/Foundation.h>
#import <JavaScriptCore/JavaScriptCore.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    (void) clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t) value.tv_sec * 1000u
           + (uint64_t) value.tv_nsec / 1000000u;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --script FILE [--prelude FILE] [--timeout-ms N]\n",
            program);
}

static NSString *read_source(const char *path)
{
    NSError *error = nil;
    NSString *value = [NSString stringWithContentsOfFile:
        [NSString stringWithUTF8String:path]
        encoding:NSUTF8StringEncoding error:&error];
    if (value == nil) {
        fprintf(stderr, "jsc-spike read failure path=\"%s\" error=\"%s\"\n",
                path, error.localizedDescription.UTF8String);
    }
    return value;
}

static int evaluate(const char *script_path, const char *prelude_path)
{
    @autoreleasepool {
        JSContext *context = [[JSContext alloc] init];
        if (context == nil) return 1;
        __block bool failed = false;
        context.exceptionHandler = ^(JSContext *inner, JSValue *exception) {
            (void) inner;
            failed = true;
            fprintf(stderr, "jsc-spike exception=\"%s\"\n",
                    exception.toString.UTF8String);
            JSValue *stack = exception[@"stack"];
            if (stack != nil && !stack.isUndefined) {
                fprintf(stderr, "jsc-spike stack=\"%s\"\n",
                        stack.toString.UTF8String);
            }
        };
        context[@"console"] = @{ @"log": ^(JSValue *value) {
            printf("jsc-console %s\n", value.toString.UTF8String);
        }};
        context[@"print"] = ^(JSValue *value) {
            printf("jsc-print %s\n", value.toString.UTF8String);
        };
        uint64_t started = monotonic_ms();
        if (prelude_path != NULL) {
            NSString *prelude = read_source(prelude_path);
            if (prelude == nil) return 1;
            [context evaluateScript:prelude withSourceURL:
                [NSURL fileURLWithPath:[NSString stringWithUTF8String:
                    prelude_path]]];
            if (failed) return 1;
        }
        NSString *script = read_source(script_path);
        if (script == nil) return 1;
        JSValue *result = [context evaluateScript:script withSourceURL:
            [NSURL fileURLWithPath:[NSString stringWithUTF8String:
                script_path]]];
        uint64_t elapsed = monotonic_ms() - started;
        if (failed) return 1;
        JSValue *summary = context.globalObject[@"pocSummary"];
        JSValue *iterations = context.globalObject[@"vmIterations"];
        printf("jsc-spike status=completed elapsed-ms=%llu result=\"%s\" "
               "summary=\"%s\" iterations=\"%s\"\n",
               (unsigned long long) elapsed,
               result.isUndefined ? "undefined" : result.toString.UTF8String,
               summary.isUndefined ? "" : summary.toString.UTF8String,
               iterations.isUndefined ? "" : iterations.toString.UTF8String);
        fflush(stdout);
        return 0;
    }
}

int main(int argc, char **argv)
{
    const char *script = NULL;
    const char *prelude = NULL;
    unsigned long timeout_ms = 20000;
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "--script") == 0) script = argv[++i];
        else if (strcmp(argv[i], "--prelude") == 0) prelude = argv[++i];
        else if (strcmp(argv[i], "--timeout-ms") == 0) {
            char *end = NULL;
            errno = 0;
            timeout_ms = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == NULL || *end != '\0'
                || timeout_ms < 1 || timeout_ms > 300000) {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (script == NULL) {
        usage(argv[0]);
        return 2;
    }
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) _exit(evaluate(script, prelude));
    uint64_t started = monotonic_ms();
    int status = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return 1;
        }
        if (result < 0) {
            perror("waitpid");
            return 1;
        }
        if (monotonic_ms() - started >= timeout_ms) {
            (void) kill(child, SIGKILL);
            (void) waitpid(child, &status, 0);
            fprintf(stderr, "jsc-spike status=timeout elapsed-ms=%lu\n",
                    timeout_ms);
            return 124;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        (void) nanosleep(&pause, NULL);
    }
}
