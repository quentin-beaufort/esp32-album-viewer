#include "api.h"
#include "diag.h"
#include "display.h"
#include "esp_check.h"
#include "net.h"
#include "nvs_flash.h"
#include "player.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(display_init());
    diag_start();
    ESP_ERROR_CHECK(player_init());
    ESP_ERROR_CHECK(net_start());
    ESP_ERROR_CHECK(api_start());
}
