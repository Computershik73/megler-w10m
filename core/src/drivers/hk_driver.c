#include "hk_driver.h"

#include <string.h>

static const hk_driver *const g_drivers[] = {
    &hk_driver_native_activity,
    &hk_driver_unity,
    &hk_driver_java,
};

const hk_driver *hk_driver_find(const char *engine)
{
    size_t i;
    for (i = 0; i < sizeof(g_drivers) / sizeof(g_drivers[0]); i++)
        if (!strcmp(g_drivers[i]->name, engine)) return g_drivers[i];
    return NULL;
}

bool hk_driver_supported(const char *engine) { return hk_driver_find(engine) != NULL; }
