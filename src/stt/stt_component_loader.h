#ifndef TILEFINCH_STT_COMPONENT_LOADER_H
#define TILEFINCH_STT_COMPONENT_LOADER_H
#include "stt_component.h"
#include "tilefinch/budget.h"

extern TilefinchSttApi tilefinch_stt_api;
void psp_voice_component_configure(const char *program_directory);
bool psp_voice_component_load(Budget *budget);
void psp_voice_component_unload(void);
/* Process-lifetime containment after an externally killed decoder. No PRX
 * callbacks or libc cleanup are safe; its reservation remains charged. */
void psp_voice_component_quarantine(void);
bool psp_voice_component_is_quarantined(void);

#define stt_engine_config_init tilefinch_stt_api.config_init
#define stt_engine_config_set_preset tilefinch_stt_api.config_set_preset
#define stt_engine_create tilefinch_stt_api.create
#define stt_engine_destroy tilefinch_stt_api.destroy
#define stt_engine_init_microseconds tilefinch_stt_api.init_microseconds
#define stt_engine_preset tilefinch_stt_api.preset
#define stt_engine_decode_capture_pcm_cancelable tilefinch_stt_api.decode
#define stt_status_name tilefinch_stt_api.status_name
#define stt_input_status_name tilefinch_stt_api.input_status_name
#define stt_preset_name tilefinch_stt_api.preset_name
#endif
