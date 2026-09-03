#ifndef TILEFINCH_WASM_RUNTIME_H
#define TILEFINCH_WASM_RUNTIME_H

/* Configure, but do not load, the PSP's bounded WebAssembly component. This
   is a no-op on host builds and on builds without the component. */
void js_wasm_component_configure(const char *program_directory);

#endif
