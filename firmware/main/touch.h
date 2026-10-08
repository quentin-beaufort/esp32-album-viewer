#pragma once

#include "esp_err.h"

/* Watches the GT911 touch controller: each new touch shows or hides the diagnostic screen.
 * A missing touch controller is logged, not fatal. */
esp_err_t touch_start(void);
