#pragma once

#include "esp_err.h"

/* Start the HTTP server on port 80 (routes in docs/protocol.md). */
esp_err_t api_start(void);
