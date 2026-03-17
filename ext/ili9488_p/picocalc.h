#pragma once

#include "stdint.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"

#define WIDTH 320
#define HEIGHT 320 // LCD SCREEN HEIGHT,not logical Height
#define PIN_DC 14
#define PIN_LCD_CS 13

// static constexpr uint PIN_TP_CS = 16;
#define PIN_RAM_CS 21
#define PIN_SCK 10
#define PIN_MOSI 11
#define PIN_MISO 12
#define PIN_RST 15
// static constexpr uint PIN_BL = 13;

typedef enum
{
    EMPTY,
    TX,
    RX
} direction_t;

#define KEY_A 0
#define KEY_B 1
#define KEY_ENTER 9
#define KEY_START 3
#define KEY_SELECT 2
#define KEY_RIGHT 7
#define KEY_DOWN 5
#define KEY_LEFT 6
#define KEY_UP 4
#define KEY_F1 4
#define KEY_F2 4
#define KEY_F3 4
#define KEY_F4 4

static const uint8_t KEYCHECKTIME = 16;
static const uint TICKSPERSEC   = 1000;   /* Ticks per second */
static const int PIN_PAD_A      = 0;
static const int PIN_PAD_B      = 0;
static const int PIN_PAD_START  = 0;
static const int PIN_PAD_SELECT = 0;
static const int PIN_PAD_RIGHT  = 0;
static const int PIN_PAD_DOWN   = 0;
static const int PIN_PAD_LEFT   = 0;
static const int PIN_PAD_UP     = 0;
extern int input_pins[];

// setup GPIO, PIO, LCD
void init(int sys_clk_hz);

// set SPI clock frequency
void set_spi_speed(int new_speed);

// clear display
void clear(uint16_t color);

// start DMA
void start_write_data(int x0, int y0, int w, int h, uint8_t *data);
void start_window(int x0, int y0, int w, int h);
void start_game();

// wait DMA to finish
void finish_write_data(bool end);

// write DMA data
void write_data(uint8_t *data, uint16_t length);

// write LCD command
void write_command(uint8_t cmd, const uint8_t *data, int len);

// write LCD command
// void write_command(uint8_t cmd);

// SPI write
void write_blocking(const uint8_t *data, int len);

// SPI read
void read_blocking(uint8_t tx_repeat, uint8_t *buff, int len);


void kbd_interrupt();
void set_kdb_key(uint8_t pin_offset, uint8_t key_status);
void device_init();
int wait_key();