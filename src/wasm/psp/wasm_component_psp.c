#include "tilefinch/wasm_component.h"

#include <pspkernel.h>
#include <string.h>

PSP_MODULE_INFO("tilefinch_wasm", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

int module_start(SceSize argument_size, void *argument_data)
{
    if (argument_data == NULL
        || argument_size < (int)sizeof(TilefinchWasmComponentStart)) return -1;
    TilefinchWasmComponentStart *start = argument_data;
    if (start->magic != TILEFINCH_WASM_COMPONENT_MAGIC
        || start->abi_version != TILEFINCH_WASM_COMPONENT_ABI_VERSION
        || start->struct_size < sizeof(*start) || start->api == NULL) return -2;
    const TilefinchWasmComponentApi api = {
        .magic = TILEFINCH_WASM_COMPONENT_MAGIC,
        .abi_version = TILEFINCH_WASM_COMPONENT_ABI_VERSION,
        .struct_size = sizeof(api),
        .runtime_full_init = wasm_runtime_full_init,
        .runtime_destroy = wasm_runtime_destroy,
        .runtime_set_log_level = wasm_runtime_set_log_level,
        .runtime_load = wasm_runtime_load,
        .runtime_load_ex = wasm_runtime_load_ex,
        .runtime_unload = wasm_runtime_unload,
        .runtime_instantiate_ex = wasm_runtime_instantiate_ex,
        .runtime_deinstantiate = wasm_runtime_deinstantiate,
        .runtime_create_exec_env = wasm_runtime_create_exec_env,
        .runtime_destroy_exec_env = wasm_runtime_destroy_exec_env,
        .runtime_set_instruction_count_limit = wasm_runtime_set_instruction_count_limit,
        .runtime_get_import_count = wasm_runtime_get_import_count,
        .runtime_get_import_type = wasm_runtime_get_import_type,
        .runtime_get_export_count = wasm_runtime_get_export_count,
        .runtime_get_export_type = wasm_runtime_get_export_type,
        .module_get_export_function_index =
            tilefinch_wasm_export_function_index,
        .module_get_imported_start_function_index =
            tilefinch_wasm_imported_start_function_index,
        .runtime_register_natives_raw = wasm_runtime_register_natives_raw,
        .runtime_unregister_natives = wasm_runtime_unregister_natives,
        .runtime_lookup_function = wasm_runtime_lookup_function,
        .runtime_lookup_memory = wasm_runtime_lookup_memory,
        .runtime_get_export_global_inst =
            wasm_runtime_get_export_global_inst,
        .func_get_param_count = wasm_func_get_param_count,
        .func_get_result_count = wasm_func_get_result_count,
        .func_get_param_types = wasm_func_get_param_types,
        .func_get_result_types = wasm_func_get_result_types,
        .runtime_call_wasm_a = wasm_runtime_call_wasm_a,
        .runtime_get_exception = wasm_runtime_get_exception,
        .runtime_set_exception = wasm_runtime_set_exception,
        .runtime_get_function_attachment = wasm_runtime_get_function_attachment,
        .func_type_get_param_count = wasm_func_type_get_param_count,
        .func_type_get_result_count = wasm_func_type_get_result_count,
        .func_type_get_param_valkind = wasm_func_type_get_param_valkind,
        .func_type_get_result_valkind = wasm_func_type_get_result_valkind,
        .memory_get_base_address = wasm_memory_get_base_address,
        .memory_get_bytes_per_page = wasm_memory_get_bytes_per_page,
        .memory_get_cur_page_count = wasm_memory_get_cur_page_count,
        .memory_get_max_page_count = wasm_memory_get_max_page_count,
        .memory_enlarge = wasm_memory_enlarge
    };
    memcpy(start->api, &api, sizeof(api));
    return 0;
}
