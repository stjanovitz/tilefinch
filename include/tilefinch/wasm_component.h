#ifndef TILEFINCH_WASM_COMPONENT_H
#define TILEFINCH_WASM_COMPONENT_H

#include <stdint.h>
#include <wasm_export.h>

#define TILEFINCH_WASM_COMPONENT_MAGIC UINT32_C(0x5446574d)
#define TILEFINCH_WASM_COMPONENT_ABI_VERSION 7u

uint32_t tilefinch_wasm_export_function_index(
    wasm_module_t module, const char *name);
uint32_t tilefinch_wasm_imported_start_function_index(wasm_module_t module);

typedef struct TilefinchWasmComponentApi {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t struct_size;
    bool (*runtime_full_init)(RuntimeInitArgs *);
    void (*runtime_destroy)(void);
    void (*runtime_set_log_level)(log_level_t);
    wasm_module_t (*runtime_load)(uint8_t *, uint32_t, char *, uint32_t);
    wasm_module_t (*runtime_load_ex)(uint8_t *, uint32_t, const LoadArgs *,
                                    char *, uint32_t);
    void (*runtime_unload)(wasm_module_t);
    wasm_module_inst_t (*runtime_instantiate_ex)(
        const wasm_module_t, const InstantiationArgs *, char *, uint32_t);
    void (*runtime_deinstantiate)(wasm_module_inst_t);
    wasm_exec_env_t (*runtime_create_exec_env)(wasm_module_inst_t, uint32_t);
    void (*runtime_destroy_exec_env)(wasm_exec_env_t);
    void (*runtime_set_instruction_count_limit)(wasm_exec_env_t, int);
    int32_t (*runtime_get_import_count)(const wasm_module_t);
    void (*runtime_get_import_type)(const wasm_module_t, int32_t,
                                    wasm_import_t *);
    int32_t (*runtime_get_export_count)(const wasm_module_t);
    void (*runtime_get_export_type)(const wasm_module_t, int32_t,
                                    wasm_export_t *);
    uint32_t (*module_get_export_function_index)(wasm_module_t, const char *);
    uint32_t (*module_get_imported_start_function_index)(wasm_module_t);
    bool (*runtime_register_natives_raw)(const char *, NativeSymbol *, uint32_t);
    bool (*runtime_unregister_natives)(const char *, NativeSymbol *);
    wasm_function_inst_t (*runtime_lookup_function)(const wasm_module_inst_t,
                                                    const char *);
    wasm_memory_inst_t (*runtime_lookup_memory)(const wasm_module_inst_t,
                                                const char *);
    bool (*runtime_get_export_global_inst)(const wasm_module_inst_t,
                                           const char *,
                                           wasm_global_inst_t *);
    uint32_t (*func_get_param_count)(const wasm_function_inst_t,
                                     const wasm_module_inst_t);
    uint32_t (*func_get_result_count)(const wasm_function_inst_t,
                                      const wasm_module_inst_t);
    void (*func_get_param_types)(const wasm_function_inst_t,
                                 const wasm_module_inst_t, wasm_valkind_t *);
    void (*func_get_result_types)(const wasm_function_inst_t,
                                  const wasm_module_inst_t, wasm_valkind_t *);
    bool (*runtime_call_wasm_a)(wasm_exec_env_t, wasm_function_inst_t,
                                uint32_t, wasm_val_t *, uint32_t, wasm_val_t *);
    const char *(*runtime_get_exception)(wasm_module_inst_t);
    void (*runtime_set_exception)(wasm_module_inst_t, const char *);
    void *(*runtime_get_function_attachment)(wasm_exec_env_t);
    uint32_t (*func_type_get_param_count)(const wasm_func_type_t);
    uint32_t (*func_type_get_result_count)(const wasm_func_type_t);
    wasm_valkind_t (*func_type_get_param_valkind)(const wasm_func_type_t,
                                                  uint32_t);
    wasm_valkind_t (*func_type_get_result_valkind)(const wasm_func_type_t,
                                                   uint32_t);
    void *(*memory_get_base_address)(const wasm_memory_inst_t);
    uint64_t (*memory_get_bytes_per_page)(const wasm_memory_inst_t);
    uint64_t (*memory_get_cur_page_count)(const wasm_memory_inst_t);
    uint64_t (*memory_get_max_page_count)(const wasm_memory_inst_t);
    bool (*memory_enlarge)(wasm_memory_inst_t, uint64_t);
    /* ABI 5: link a module loaded with LoadArgs::no_resolve. */
    bool (*runtime_resolve_symbols)(wasm_module_t);
    /* ABI 6: whether a module loaded with wasm_binary_freeable no longer
       references its binary, so the caller may free it. */
    bool (*runtime_is_underlying_binary_freeable)(const wasm_module_t);
    /* ABI 7: trap a start-function import before Instance publication. */
    wasm_module_inst_t (*runtime_get_module_inst)(wasm_exec_env_t);
} TilefinchWasmComponentApi;

typedef struct {
    uint32_t magic;
    uint16_t abi_version;
    uint16_t struct_size;
    TilefinchWasmComponentApi *api;
} TilefinchWasmComponentStart;

#endif
