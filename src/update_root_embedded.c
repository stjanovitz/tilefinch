#include "tilefinch/update.h"

#include "tilefinch_update_root_data.h"

bool tilefinch_update_root_is_configured(void)
{
    return tilefinch_update_root_v1_length != 0;
}

bool tilefinch_update_embedded_root(TilefinchUpdateRoot *root)
{
    return root != NULL
        && tilefinch_update_root_v1_length != 0
        && tilefinch_update_parse_root(
               tilefinch_update_root_v1, tilefinch_update_root_v1_length,
               root) == TILEFINCH_UPDATE_OK;
}
