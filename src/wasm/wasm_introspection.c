#include <stdint.h>
#include <string.h>

#include <wasm_export.h>
#include "wasm.h"

uint32_t tilefinch_wasm_export_function_index(
    wasm_module_t common, const char *name)
{
    if (common == NULL || name == NULL) return UINT32_MAX;
    WASMModule *module = (WASMModule *)common;
    if (module->module_type != Wasm_Module_Bytecode) return UINT32_MAX;
    for (uint32_t i = 0; i < module->export_count; i++) {
        const WASMExport *exported = &module->exports[i];
        if (exported->kind == EXPORT_KIND_FUNC
            && exported->name != NULL
            && strcmp(exported->name, name) == 0) return exported->index;
    }
    return UINT32_MAX;
}

uint32_t tilefinch_wasm_imported_start_function_index(wasm_module_t common)
{
    if (common == NULL) return UINT32_MAX;
    WASMModule *module = (WASMModule *)common;
    if (module->module_type != Wasm_Module_Bytecode
        || module->start_function >= module->import_function_count) {
        return UINT32_MAX;
    }
    return module->start_function;
}
