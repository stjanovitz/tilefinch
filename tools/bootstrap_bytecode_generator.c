#include <quickjs.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#define tilefinch_process_id _getpid
#else
#include <unistd.h>
#define tilefinch_process_id getpid
#endif

typedef struct {
    const char *source_symbol;
    const char *file_name;
    const char *source_name;
    const char *bytecode_symbol;
    char *source;
    size_t source_length;
} BootstrapInput;

#define BOOTSTRAP_SOURCE(source_symbol, file_name, source_name,              \
                         bytecode_symbol)                                   \
    {#source_symbol, file_name, source_name, #bytecode_symbol, NULL, 0},
#define BOOTSTRAP_DIAGNOSTIC(source_symbol, file_name, source_name)           \
    {#source_symbol, file_name, source_name, NULL, NULL, 0},
static BootstrapInput inputs[] = {
#include "../src/bootstrap/sources.def"
};
#undef BOOTSTRAP_DIAGNOSTIC
#undef BOOTSTRAP_SOURCE

static const size_t input_count = sizeof(inputs) / sizeof(inputs[0]);

static uint64_t source_hash(const char *source, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < length; i++) {
        hash ^= (unsigned char) source[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int make_path(char *output, size_t capacity, const char *directory,
                     const char *file_name)
{
    int length = snprintf(output, capacity, "%s/%s", directory, file_name);
    return length > 0 && (size_t) length < capacity;
}

static int make_suffix_path(char *output, size_t capacity, const char *path,
                            const char *suffix)
{
    int length = snprintf(output, capacity, "%s%s", path, suffix);
    return length > 0 && (size_t) length < capacity;
}

static int read_source(BootstrapInput *input, const char *source_directory)
{
    char path[PATH_MAX];
    if (!make_path(path, sizeof(path), source_directory, input->file_name)) {
        fprintf(stderr, "bootstrap source path is too long: %s\n",
                input->file_name);
        return 0;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "could not open %s: %s\n", path, strerror(errno));
        return 0;
    }
    int ok = fseek(file, 0, SEEK_END) == 0;
    long end = ok ? ftell(file) : -1;
    ok = ok && end >= 0 && fseek(file, 0, SEEK_SET) == 0;
    if (!ok || (uintmax_t) end > SIZE_MAX - 1) {
        fprintf(stderr, "could not size %s\n", path);
        fclose(file);
        return 0;
    }
    input->source_length = (size_t) end;
    input->source = malloc(input->source_length + 1);
    ok = input->source != NULL
        && fread(input->source, 1, input->source_length, file)
               == input->source_length
        && !ferror(file);
    if (fclose(file) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "could not read %s\n", path);
        free(input->source);
        input->source = NULL;
        input->source_length = 0;
        return 0;
    }
    input->source[input->source_length] = '\0';
    return 1;
}

static int load_sources(const char *source_directory)
{
    for (size_t i = 0; i < input_count; i++) {
        if (!read_source(&inputs[i], source_directory)) return 0;
    }
    return 1;
}

static void free_sources(void)
{
    for (size_t i = 0; i < input_count; i++) {
        free(inputs[i].source);
        inputs[i].source = NULL;
        inputs[i].source_length = 0;
    }
}

static int emit_c_string(FILE *output, const char *source, size_t length)
{
    fputs("    \"", output);
    for (size_t i = 0; i < length; i++) {
        unsigned char byte = (unsigned char) source[i];
        switch (byte) {
        case '\n':
            fputs("\\n\"\n    \"", output);
            break;
        case '\r':
            fputs("\\r", output);
            break;
        case '\t':
            fputs("\\t", output);
            break;
        case '\f':
            fputs("\\f", output);
            break;
        case '\b':
            fputs("\\b", output);
            break;
        case '\\':
            fputs("\\\\", output);
            break;
        case '"':
            fputs("\\\"", output);
            break;
        case '?':
            /* Avoid C trigraph interpretation in older cross-compilers. */
            fputs("\\?", output);
            break;
        default:
            if (byte >= 0x20 && byte <= 0x7e) {
                fputc(byte, output);
            } else {
                fprintf(output, "\\%03o", byte);
            }
            break;
        }
    }
    fputs("\"", output);
    return !ferror(output);
}

static int emit_source_file(FILE *output)
{
    fputs("/* Generated from src/bootstrap/sources.def. Do not edit. */\n"
          "#include \"js_runtime_internal.h\"\n\n", output);
    for (size_t i = 0; i < input_count; i++) {
        const BootstrapInput *input = &inputs[i];
        fprintf(output, "const char %s[] =\n", input->source_symbol);
        if (!emit_c_string(output, input->source, input->source_length)) {
            return 0;
        }
        fprintf(output,
                ";\nconst size_t %s_length = sizeof(%s) - 1;\n"
                "_Static_assert(sizeof(%s) - 1 <= "
                "SCRIPT_PSP_MAXIMUM_HOST_COMPILE_BYTES,\n"
                "               \"%s exceeds PSP bootstrap source "
                "ceiling\");\n",
                input->source_symbol, input->source_symbol,
                input->source_symbol, input->source_symbol);
        if (i + 1 != input_count) fputc('\n', output);
    }
    return !ferror(output);
}

static int emit_bytecode(FILE *output, JSContext *context,
                         const BootstrapInput *input)
{
    JSValue function = JS_Eval(
        context, input->source, input->source_length, input->source_name,
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(function)) {
        JSValue exception = JS_GetException(context);
        const char *message = JS_ToCString(context, exception);
        fprintf(stderr, "could not compile %s%s%s\n", input->source_name,
                message == NULL ? "" : ": ",
                message == NULL ? "" : message);
        if (message != NULL) JS_FreeCString(context, message);
        JS_FreeValue(context, exception);
        return 0;
    }
    size_t bytecode_length = 0;
    uint8_t *bytecode = JS_WriteObject(
        context, &bytecode_length, function, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(context, function);
    if (bytecode == NULL) return 0;
    fprintf(output, "static const unsigned char %s_data[] = {\n    ",
            input->bytecode_symbol);
    for (size_t i = 0; i < bytecode_length; i++) {
        fprintf(output, "0x%02x%s", bytecode[i],
                i + 1 == bytecode_length ? "" : ",");
        if (i + 1 != bytecode_length && (i + 1) % 12 == 0) {
            fputs("\n    ", output);
        }
    }
    fprintf(output,
            "\n};\nconst BrowserBootstrapBytecode %s = {\n"
            "    %s_data, sizeof(%s_data), %zu, "
            "UINT64_C(0x%016" PRIx64 ")\n};\n\n",
            input->bytecode_symbol, input->bytecode_symbol,
            input->bytecode_symbol, input->source_length,
            source_hash(input->source, input->source_length));
    js_free(context, bytecode);
    return !ferror(output);
}

static int emit_bytecode_file(FILE *output)
{
    JSRuntime *runtime = JS_NewRuntime();
    /* The authored source is embedded separately for the checked fallback
       path. Do not duplicate it inside every bytecode object; retain line
       tables so bootstrap exceptions still identify useful source lines. */
    if (runtime != NULL) JS_SetStripInfo(runtime, JS_STRIP_SOURCE);
    JSContext *context = runtime == NULL ? NULL : JS_NewContext(runtime);
    if (context == NULL) {
        if (runtime != NULL) JS_FreeRuntime(runtime);
        fputs("could not create QuickJS generator context\n", stderr);
        return 0;
    }
    fputs("/* Generated from src/bootstrap/sources.def. Do not edit. */\n"
          "#include \"js_runtime_internal.h\"\n\n", output);
    int ok = 1;
    for (size_t i = 0; i < input_count; i++) {
        if (inputs[i].bytecode_symbol != NULL
            && !emit_bytecode(output, context, &inputs[i])) {
            ok = 0;
            break;
        }
    }
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return ok && !ferror(output);
}

static int generate_file(const char *path, int (*emit)(FILE *))
{
    char temporary_path[PATH_MAX];
    if (!make_suffix_path(temporary_path, sizeof(temporary_path), path,
                          ".tmp")) {
        fprintf(stderr, "generated output path is too long: %s\n", path);
        return 0;
    }
    FILE *output = fopen(temporary_path, "wb");
    int ok = output != NULL && emit(output);
    if (output != NULL && fclose(output) != 0) ok = 0;
    if (ok && rename(temporary_path, path) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "could not generate %s\n", path);
        remove(temporary_path);
    }
    return ok;
}

static int generate(const char *source_directory, const char *source_output,
                    const char *bytecode_output)
{
    int ok = load_sources(source_directory)
        && generate_file(source_output, emit_source_file)
        && generate_file(bytecode_output, emit_bytecode_file);
    free_sources();
    return ok;
}

static int files_equal(const char *left_path, const char *right_path)
{
    FILE *left = fopen(left_path, "rb");
    FILE *right = fopen(right_path, "rb");
    if (left == NULL || right == NULL) {
        if (left != NULL) fclose(left);
        if (right != NULL) fclose(right);
        return 0;
    }
    int equal = 1;
    for (;;) {
        unsigned char left_buffer[8192];
        unsigned char right_buffer[8192];
        size_t left_length = fread(left_buffer, 1, sizeof(left_buffer), left);
        size_t right_length =
            fread(right_buffer, 1, sizeof(right_buffer), right);
        if (left_length != right_length
            || memcmp(left_buffer, right_buffer, left_length) != 0) {
            equal = 0;
            break;
        }
        if (left_length < sizeof(left_buffer)) {
            if (ferror(left) || ferror(right)) equal = 0;
            break;
        }
    }
    if (fclose(left) != 0 || fclose(right) != 0) equal = 0;
    return equal;
}

static int check_generated(const char *source_directory,
                           const char *source_output,
                           const char *bytecode_output,
                           const char *scratch_directory)
{
    char source_check[PATH_MAX];
    char bytecode_check[PATH_MAX];
    long process_id = (long) tilefinch_process_id();
    int source_length = snprintf(
        source_check, sizeof(source_check),
        "%s/tilefinch-bootstrap-source-%ld.check", scratch_directory,
        process_id);
    int bytecode_length = snprintf(
        bytecode_check, sizeof(bytecode_check),
        "%s/tilefinch-bootstrap-bytecode-%ld.check", scratch_directory,
        process_id);
    if (source_length <= 0 || (size_t) source_length >= sizeof(source_check)
        || bytecode_length <= 0
        || (size_t) bytecode_length >= sizeof(bytecode_check)) {
        fputs("generated check path is too long\n", stderr);
        return 0;
    }
    int generated =
        generate(source_directory, source_check, bytecode_check);
    int source_equal = generated && files_equal(source_output, source_check);
    int bytecode_equal =
        generated && files_equal(bytecode_output, bytecode_check);
    remove(source_check);
    remove(bytecode_check);
    if (!source_equal || !bytecode_equal) {
        fputs("bootstrap generated files are stale; build target "
              "regenerate_tilefinch_bootstrap\n", stderr);
        return 0;
    }
    return 1;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s SOURCE_DIR OUTPUT_SOURCE_C OUTPUT_BYTECODE_C\n"
            "       %s --check SOURCE_DIR SOURCE_C BYTECODE_C SCRATCH_DIR\n",
            program, program);
}

int main(int argc, char **argv)
{
    if (argc == 6 && strcmp(argv[1], "--check") == 0) {
        return check_generated(argv[2], argv[3], argv[4], argv[5]) ? 0 : 1;
    }
    if (argc == 4) {
        return generate(argv[1], argv[2], argv[3]) ? 0 : 1;
    }
    usage(argv[0]);
    return 2;
}
