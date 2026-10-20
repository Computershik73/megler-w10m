#include "mg_driver.h"

#include <string.h>

static const mg_driver *const g_drivers[] = {
    &mg_driver_native_activity,
    &mg_driver_unity,
    &mg_driver_java,
};

const mg_driver *mg_driver_find(const char *engine)
{
    size_t i;
    for (i = 0; i < sizeof(g_drivers) / sizeof(g_drivers[0]); i++)
        if (!strcmp(g_drivers[i]->name, engine)) return g_drivers[i];
    return NULL;
}

bool mg_driver_supported(const char *engine) { return mg_driver_find(engine) != NULL; }
