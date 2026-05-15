#include "joystick2.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "joystick2";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

// Calibrated centre values
static uint16_t s_cx = 32767;
static uint16_t s_cy = 32767;

// ─── Low-level helper ─────────────────────────────────────────────────────────
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    // Write register address then read data in one I2C transaction
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len,
                                       pdMS_TO_TICKS(100));
}

// ─── Public API ───────────────────────────────────────────────────────────────
esp_err_t joystick2_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = I2C_NUM_1,
        .sda_io_num          = JOYSTICK2_I2C_SDA,
        .scl_io_num          = JOYSTICK2_I2C_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = JOYSTICK2_ADDR,
        .scl_speed_hz    = JOYSTICK2_I2C_FREQ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    // Verify device is present
    uint8_t probe[5] = {0};
    esp_err_t ret = read_regs(JOYSTICK2_REG_DATA, probe, 5);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Joystick2 not found at 0x%02X (err 0x%x)", JOYSTICK2_ADDR, ret);
        return ret;
    }

    s_cx = (uint16_t)(probe[0] | (probe[1] << 8));
    s_cy = (uint16_t)(probe[2] | (probe[3] << 8));
    ESP_LOGI(TAG, "Joystick2 ready. Centre X=%u Y=%u", s_cx, s_cy);
    return ESP_OK;
}

esp_err_t joystick2_calibrate_centre(void)
{
    joystick2_data_t tmp;
    esp_err_t ret = joystick2_read(&tmp);
    if (ret == ESP_OK) {
        s_cx = tmp.x_raw;
        s_cy = tmp.y_raw;
        ESP_LOGI(TAG, "Centre calibrated: X=%u Y=%u", s_cx, s_cy);
    }
    return ret;
}

esp_err_t joystick2_read(joystick2_data_t *out)
{
    uint8_t buf[5] = {0};
    esp_err_t ret = read_regs(JOYSTICK2_REG_DATA, buf, 5);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read failed: 0x%x", ret);
        return ret;
    }

    out->x_raw = (uint16_t)(buf[0] | (buf[1] << 8));
    out->y_raw = (uint16_t)(buf[2] | (buf[3] << 8));
    // btn byte: 0 = pressed, 1 = released (active-low from Joystick2 firmware)
    out->btn = (buf[4] == 0);

    // Normalise relative to calibrated centre → −100 … +100
    int32_t dx = (int32_t)out->x_raw - s_cx;
    int32_t dy = (int32_t)out->y_raw - s_cy;
    out->x = (int16_t)((dx * 100) / 32767);
    out->y = (int16_t)((dy * 100) / 32767);
    if (out->x >  100) out->x =  100;
    if (out->x < -100) out->x = -100;
    if (out->y >  100) out->y =  100;
    if (out->y < -100) out->y = -100;

    return ESP_OK;
}
