///
/// File: main.c
///
/// Developer: Philip Wyettt as Kathenas Control Technologies.
///
/// License: GPL-2.0-only.
///

/// Standard C headers.
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/// FreeRTOS headers.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/// ESP-IDF headers.
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7796.h"

///
/// Display adaptor (Freenove 4 inch FNK0104S) configuration.
///
#define LCD_H_RES 320
#define LCD_V_RES 480
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIN_MOSI 11
#define LCD_PIN_MISO -1
#define LCD_PIN_SCLK 12
#define LCD_PIN_CS 10
#define LCD_PIN_DC 46
#define LCD_PIN_RST -1
#define LCD_PIN_BK_LIGHT 45
#define LCD_SPI_MODE 0
#define LCD_COLOR_BITS 16
#define LCD_BUFFER_LINES 16
#define LCD_COLOR_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#define LCD_INVERT_COLOR true
#define LCD_SWAP_XY false
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y false
#define LCD_BACKLIGHT_ON 1

static const char *TAG = "freenove_4_inch_st7796s_touch";

/// Display context.
typedef struct
{
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t transfer_done;

    uint16_t *buffer;
    int width;
    int height;
    bool initialized;
} lcd_t;

static lcd_t lcd;

///
/// RGB565 colour definitions
///
/// Useful link: https://colorpicker.uk/
///
#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8) << 8) | \
        (((g) & 0xFC) << 3) | \
        (((b) & 0xF8) >> 3))

#define LCD_BLACK RGB565(0, 0, 0)
#define LCD_WHITE RGB565(255, 255, 255)
#define LCD_RED RGB565(255, 0, 0)
#define LCD_GREEN RGB565(0, 255, 0)
#define LCD_BLUE RGB565(0, 0, 255)

///
/// SPI transfer completion callback.
///
static bool lcd_transfer_callback(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx)
{
    lcd_t *display = (lcd_t *)user_ctx;

    BaseType_t high_task_woken = pdFALSE;

    xSemaphoreGiveFromISR(
        display->transfer_done,
        &high_task_woken
    );

    return high_task_woken == pdTRUE;
}

///
/// Backlight control.
///
static void lcd_backlight_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << LCD_PIN_BK_LIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&config));

    gpio_set_level(
        LCD_PIN_BK_LIGHT,
        !LCD_BACKLIGHT_ON
    );
}

///
/// Backlight enable.
///
static void lcd_backlight_set(bool enabled)
{
    gpio_set_level(
        LCD_PIN_BK_LIGHT,
        enabled ? LCD_BACKLIGHT_ON : !LCD_BACKLIGHT_ON
    );
}

///
/// LCD initialisation.
///
static esp_err_t lcd_init(void)
{
    esp_err_t ret;

    memset(&lcd, 0, sizeof(lcd));

    lcd.width = LCD_H_RES;
    lcd.height = LCD_V_RES;

    /// Create transfer completion semaphore.
    lcd.transfer_done = xSemaphoreCreateBinary();

    if(lcd.transfer_done == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    /// Configure SPI bus.
    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,

        .max_transfer_sz =
            LCD_H_RES * LCD_BUFFER_LINES * sizeof(uint16_t),

        .flags = SPICOMMON_BUSFLAG_MASTER
    };

    ret = spi_bus_initialize(
        LCD_SPI_HOST,
        &bus_config,
        SPI_DMA_CH_AUTO
    );

    if(ret != ESP_OK)
    {
        return ret;
    }

    /// Configure LCD SPI interface.
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,

        .spi_mode = LCD_SPI_MODE,

        .pclk_hz = LCD_PIXEL_CLOCK_HZ,

        .trans_queue_depth = 1,

        .on_color_trans_done = lcd_transfer_callback,
        .user_ctx = &lcd,

        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,

        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0
        }
    };

    ret = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
        &io_config,
        &lcd.io
    );

    if(ret != ESP_OK)
    {
        return ret;
    }

    /// Configure ST7796 panel.
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,

        .rgb_ele_order = LCD_COLOR_ORDER,

        .bits_per_pixel = LCD_COLOR_BITS
    };

    ret = esp_lcd_new_panel_st7796(
        lcd.io,
        &panel_config,
        &lcd.panel
    );

    if(ret != ESP_OK)
    {
        return ret;
    }

    /// Reset and initialise display.
    ESP_ERROR_CHECK(
        esp_lcd_panel_reset(lcd.panel)
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_init(lcd.panel)
    );

    /// Configure orientation.
    ESP_ERROR_CHECK(
        esp_lcd_panel_swap_xy(
            lcd.panel,
            LCD_SWAP_XY
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_mirror(
            lcd.panel,
            LCD_MIRROR_X,
            LCD_MIRROR_Y
        )
    );

    /// Configure colour inversion.
    ESP_ERROR_CHECK(
        esp_lcd_panel_invert_color(
            lcd.panel,
            LCD_INVERT_COLOR
        )
    );

    /// Turn display on.
    ESP_ERROR_CHECK(
        esp_lcd_panel_disp_on_off(lcd.panel, true)
    );

    /// Allocate DMA-capable drawing buffer.
    lcd.buffer = heap_caps_malloc(
        LCD_H_RES * LCD_BUFFER_LINES * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
    );

    if(lcd.buffer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    lcd_backlight_init();
    lcd_backlight_set(true);

    lcd.initialized = true;

    ESP_LOGI(TAG, "LCD initialised: %dx%d",
             lcd.width, lcd.height);

    return ESP_OK;
}

///
/// Draw rectangular bitmap
///
/// Coordinates use:
///
/// x1, y1 = top-left
/// x2, y2 = bottom-right (exclusive)
///
/// Buffer is RGB565.
///
static esp_err_t lcd_draw_bitmap(
    int x1,
    int y1,
    int x2,
    int y2,
    const uint16_t *pixels)
{
    if(!lcd.initialized || pixels == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(x1 < 0 || y1 < 0 ||
        x2 > lcd.width || y2 > lcd.height ||
        x1 >= x2 || y1 >= y2)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int width = x2 - x1;
    int height = y2 - y1;

    int lines_per_transfer = LCD_BUFFER_LINES;

    for(int y = 0; y < height; )
    {
        int lines = height - y;

        if(lines > lines_per_transfer)
        {
            lines = lines_per_transfer;
        }

        size_t pixel_count = width * lines;

        /// Copy data into DMA-capable buffer.
        for(size_t i = 0; i < pixel_count; i++)
        {
            lcd.buffer[i] = pixels[(size_t)y * width + i];
        }

        /// Submit LCD transfer.
        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            lcd.panel,
            x1,
            y1 + y,
            x2,
            y1 + y + lines,
            lcd.buffer
        );

        if(ret != ESP_OK)
        {
            return ret;
        }

        /// Wait until DMA transfer completes before reusing the buffer.
        if(xSemaphoreTake(
            lcd.transfer_done,
            portMAX_DELAY) != pdTRUE)
        {
            return ESP_FAIL;
        }

        y += lines;
    }

    return ESP_OK;
}

///
/// Fill screen.
///
static esp_err_t lcd_fill(uint16_t colour)
{
    if(!lcd.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    for(int i = 0;
         i < LCD_H_RES * LCD_BUFFER_LINES;
         i++)
    {
        lcd.buffer[i] = colour;
    }

    for(int y = 0; y < LCD_V_RES; )
    {
        int lines = LCD_BUFFER_LINES;

        if(y + lines > LCD_V_RES)
        {
            lines = LCD_V_RES - y;
        }

        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            lcd.panel,
            0,
            y,
            LCD_H_RES,
            y + lines,
            lcd.buffer
        );

        if(ret != ESP_OK)
        {
            return ret;
        }

        if(xSemaphoreTake(
            lcd.transfer_done,
            portMAX_DELAY) != pdTRUE)
        {
            return ESP_FAIL;
        }

        y += lines;
    }

    return ESP_OK;
}

///
/// Draw solid rectangle
///
static esp_err_t lcd_fill_rect(
    int x,
    int y,
    int width,
    int height,
    uint16_t colour)
{
    if(!lcd.initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if(x < 0 || y < 0 ||
        width <= 0 || height <= 0 ||
        x + width > LCD_H_RES ||
        y + height > LCD_V_RES)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for(int i = 0;
         i < LCD_H_RES * LCD_BUFFER_LINES;
         i++)
    {
        lcd.buffer[i] = colour;
    }

    for(int row = 0; row < height; )
    {
        int lines = height - row;

        if(lines > LCD_BUFFER_LINES)
        {
            lines = LCD_BUFFER_LINES;
        }

        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            lcd.panel,
            x,
            y + row,
            x + width,
            y + row + lines,
            lcd.buffer
        );

        if(ret != ESP_OK)
        {
            return ret;
        }

        if(xSemaphoreTake(
            lcd.transfer_done,
            portMAX_DELAY) != pdTRUE)
        {
            return ESP_FAIL;
        }

        row += lines;
    }

    return ESP_OK;
}

///
/// Main function with demo code.
///
void app_main(void)
{
    ESP_LOGI(TAG, "Starting ST7796 display");

    ESP_ERROR_CHECK(lcd_init());

    while (1)
    {
        /// Fill screen with red.
        ESP_LOGI(TAG, "RED");
        ESP_ERROR_CHECK(lcd_fill(LCD_RED));
        vTaskDelay(pdMS_TO_TICKS(1000));

        /// Fill screen with green.
        ESP_LOGI(TAG, "GREEN");
        ESP_ERROR_CHECK(lcd_fill(LCD_GREEN));
        vTaskDelay(pdMS_TO_TICKS(1000));

        /// Fill screen with blue.
        ESP_LOGI(TAG, "BLUE");
        ESP_ERROR_CHECK(lcd_fill(LCD_BLUE));
        vTaskDelay(pdMS_TO_TICKS(1000));

        /// Fill screen with white.
        ESP_LOGI(TAG, "WHITE");
        ESP_ERROR_CHECK(lcd_fill(LCD_WHITE));
        vTaskDelay(pdMS_TO_TICKS(1000));

        /// Let's draw some boxes.
        ESP_LOGI(TAG, "Drawing rectangles");
        ESP_ERROR_CHECK(lcd_fill(LCD_BLACK)); // Fill screen background with black.
        ESP_ERROR_CHECK(lcd_fill_rect(20, 20, 100, 100, LCD_RED)); /// Red square.
        ESP_ERROR_CHECK(lcd_fill_rect(130, 20, 100, 100, LCD_GREEN)); /// Green square.
        ESP_ERROR_CHECK(lcd_fill_rect(20, 140, 100, 100, LCD_BLUE)); /// Blue square.

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// End of file.
