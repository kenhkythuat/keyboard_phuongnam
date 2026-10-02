#include "firmware_version.h"

#include "esp_app_desc.h"

const char *firmware_version_get(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    return description != NULL ? description->version : "0.0.0";
}
