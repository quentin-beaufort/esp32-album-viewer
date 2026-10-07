#pragma once

#include <stdbool.h>

/* Diagnostic screen shown from boot until the first image (or /off): reset reason, Wi-Fi,
 * HTTP server, last request, memory and the latest log lines. Call right after
 * display_init, so that the rest of the start-up shows in the log lines. */
void diag_start(void);

void diag_http_ready(bool ready);

/* Records a request answered by the HTTP server; status 0 means the connection broke. */
void diag_note_request(const char *method, const char *uri, int status);

/* Reset reason of this boot, in French. */
const char *diag_reset_reason(void);
