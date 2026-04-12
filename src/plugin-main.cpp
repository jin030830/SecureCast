#include <obs-module.h>
#include <obs-frontend-api.h>

// Forward declaration from securecast-filter.cpp
extern struct obs_source_info securecast_filter_info;

OBS_DECLARE_MODULE()

// Module entry point
bool obs_module_load(void)
{
    obs_log(LOG_INFO, "[SecureCast] Initializing SecureCast v5.7 plugin (Single-Process Native C++)");

    // Register our main filter
    obs_register_source(&securecast_filter_info);

    return true;
}

// Module cleanup
void obs_module_unload(void)
{
    obs_log(LOG_INFO, "[SecureCast] Unloading plugin");
}
