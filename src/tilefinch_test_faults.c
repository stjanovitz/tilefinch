#include "tilefinch_test_faults.h"

#ifndef __PSP__
static TilefinchTestFaults installed_faults;

TilefinchTestFaults *tilefinch_test_faults(void)
{
    return &installed_faults;
}
#else
typedef int tilefinch_test_faults_translation_unit_is_not_empty;
#endif
