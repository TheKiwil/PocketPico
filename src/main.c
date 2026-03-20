/**
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 * Copyright (C) 2024 by Vlastimil Slintak <slintak@uart.cz>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * PocketPico - Game Boy Emulator for Raspberry Pi Pico
 * 
 * This file contains the main program for the PocketPico Game Boy emulator.
 * It handles ROM loading, display rendering, input processing, save file management,
 * and audio playback using the Walnut-CGB emulator core.
 */

// Walnut-CGB emulator settings
/* Core emulator feature configuration */
#define ENABLE_LCD 1                  // Enable LCD display output
#define ENABLE_SOUND 1                // Enable sound output
#define ENABLE_SDCARD 1               // Enable SD card for ROM and save storage
#define WALNUT_GB_HIGH_LCD_ACCURACY 1 // Use high accuracy LCD emulation
#define WALNUT_FULL_GBC_SUPPORT 1     // Enable full Game Boy support
#define PEANUT_FULL_GBC_SUPPORT 1     // Enable full Game Boy Color support
#if PICO_RP2040
    #define VREG_VOLT VREG_VOLTAGE_1_15
    #define SYS_CLK_FREQ 300 * MHZ        // Set system clock to 300 MHz
#elif PICO_RP2350
    #define VREG_VOLT VREG_VOLTAGE_1_30
    #define SYS_CLK_FREQ 360 * MHZ        // Set system clock to 360 MHz
#endif

#define ENABLE_DEBUG 0                // Enable debug output

/* Display hardware configuration */
#define USE_ILI9225 0                 // Disable ILI9225 display driver
#define USE_ILI9488 1                 // Enable ILI9488 display driver
#define LCD_BAUDRATE 80000000         // Set fast SPI baud rate for LCD

/* Standard C Headers */
#include <stdlib.h>
#include <string.h>

/* Raspberry Pi Pico SDK Headers */
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/spi.h>
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <pico/bootrom.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include "pico/util/queue.h"
#include <sys/unistd.h>
#include <hardware/irq.h>

#include "boot/picobin.h"
#include "hardware/watchdog.h"

/* Project Headers */
#include "debug.h"             // Debug output functionality
#include "hedley.h"            // Cross-platform compiler macros
#include "minigb_apu.h"        // Game Boy audio processing unit emulation
#include "sdcard.h"            // SD card interface
#include "gbcolors.h"          // Game Boy color palette definitions

/* External Component Headers */
#include "../ext/ili9488_p/mono8x16.h" // Font for text display
#include "i2ckbd.h"                    // I2C keyboard interface
#include "picocalc.h"                  // PocketPico hardware interface

#if ENABLE_SOUND

/**
 * Audio Command Enumeration
 * 
 * Defines the possible commands that can be sent to the audio processing core.
 */
typedef enum
{
    AUDIO_CMD_IDLE = 0,        // No operation
    AUDIO_CMD_PLAYBACK,        // Play audio samples
    AUDIO_CMD_VOLUME_UP,       // Increase volume
    AUDIO_CMD_VOLUME_DOWN,     // Decrease volume
    AUDIO_CMD_INVALID          // Invalid command
} audio_commands_e;

queue_t call_queue;            // Queue for communication between cores

#define audio_read(a) audio_read(&apu_ctx, (a))
#define audio_write(a, v) audio_write(&apu_ctx, (a), (v));

/**
 * Global Variables for Audio Processing
 * 
 * stream: Contains N=AUDIO_SAMPLES samples
 * Each sample is 32 bits (16 bits for left channel + 16 bits for right channel)
 * in stereo interleaved format. This is played at AUDIO_SAMPLE_RATE Hz.
 */
int16_t *stream;
struct minigb_apu_ctx apu_ctx = {0};

/* Audio Hardware Configuration */
#define AUDIO_DATA_PIN 26      // I2S data pin
#define AUDIO_CLOCK_PIN 27     // I2S clock pin
#define AUDIO_PWM_PIN 26       // PWM output pin (same as data pin)
#define PIN_SPEAKER 26         // Speaker connection pin
#define SPK_LATENCY 256        // Audio buffer latency
#define SPK_PWM_FREQ 22050     // PWM frequency for audio output

#include "audio.h"
struct gb_s;
#include "walnut_cgb.h"
#undef audio_read
#undef audio_write
#else
struct gb_s;
#include "walnut_cgb.h"
#endif

/**
 * ROM Storage Configuration
 * 
 * Defines a region in flash memory to store the Game Boy ROM.
 * We erase and reprogram a region 1MB from the start of flash memory.
 * Once done, we access this at XIP_BASE + 1MB.
 * Game Boy DMG ROM sizes range from 32KB (e.g. Tetris) to 1MB (e.g. Pokemod Red)
 */
#define FLASH_TARGET_OFFSET (1024 * 1024)  // 1MB offset from flash start
const uint8_t *rom = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
static unsigned char rom_bank0[65536];     // 64KB buffer for ROM bank 0

#if PICO_RP2040
static uint8_t ram[0x1000];                  // 4KB buffer for cartridge RAM
#elif PICO_RP2350
static uint8_t ram[0x40000];                 // 256KB buffer for cartridge RAM
#endif

static palette_t palette;                  // Current color palette
static uint8_t manual_palette_selected = 0; // Index of manually selected palette

/**
 * Previous Joypad State
 * 
 * Stores the previous state of all joypad buttons to detect button press events.
 * Each field is a 1-bit flag indicating whether the button was pressed.
 */
static struct
{
    unsigned a : 1;        // A button
    unsigned b : 1;        // B button
    unsigned select : 1;   // Select button
    unsigned start : 1;    // Start button
    unsigned right : 1;    // Right direction
    unsigned left : 1;     // Left direction
    unsigned up : 1;       // Up direction
    unsigned down : 1;     // Down direction
    unsigned F1 : 1;       // F1 button
    unsigned F2 : 1;       // F2 button
    unsigned F3 : 1;       // F3 button
    unsigned F4 : 1;       // F4 button
} prev_joypad_bits;

static struct
{
    unsigned F1 : 1;       // F1 button (if used)
    unsigned F2 : 1;       // F2 button (if used)
    unsigned F3 : 1;       // F3 button (if used)
    unsigned F4 : 1;       // F4 button (if used)
} other_joypad_bits;

/* Display Buffer Configuration */
#define FRAME_BUFF_WIDTH 320                  // Width of frame buffer
#define FRAME_BUFF_STRIDE (FRAME_BUFF_WIDTH * 2) // Stride of frame buffer (bytes per row)
#define FRAME_BUFF_HEIGHT 320                 // Height of frame buffer

/* Line buffer for rendering Game Boy LCD output */
static uint8_t pixels_buffer[WIDTH*2] = {0};

/**
 * ROM Read Callback
 * 
 * Returns a byte from the ROM file at the given address.
 * This function is called by the Game Boy emulator when it needs to read from ROM.
 * 
 * @param gb Pointer to the Game Boy emulator context
 * @param addr Address to read from
 * @return The byte at the specified address
 */
uint8_t gb_rom_read_8bit(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr < sizeof(rom_bank0))
        return rom_bank0[addr];
    else
        return rom[addr];
}

uint16_t gb_rom_read_16bit(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    uint16_t val;

    if (addr < sizeof(rom_bank0))
        memcpy(&val, &rom_bank0[addr], sizeof(val));
    else
        memcpy(&val, &rom[addr], sizeof(val));

    return val;
}

uint32_t gb_rom_read_32bit(struct gb_s *gb, const uint_fast32_t addr) {
    (void)gb;
    uint32_t val;

    if (addr < sizeof(rom_bank0))
        memcpy(&val, &rom_bank0[addr], sizeof(val));
    else
        memcpy(&val, &rom[addr], sizeof(val));

    return val;
}

/**
 * Cartridge RAM Read Callback
 * 
 * Returns a byte from the cartridge RAM at the given address.
 * This function is called by the Game Boy emulator when it needs to read from cartridge RAM.
 * 
 * @param gb Pointer to the Game Boy emulator context
 * @param addr Address to read from
 * @return The byte at the specified address
 */
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    return ram[addr];
}

/**
 * Cartridge RAM Write Callback
 * 
 * Writes a given byte to the cartridge RAM at the given address.
 * This function is called by the Game Boy emulator when it needs to write to cartridge RAM.
 * 
 * @param gb Pointer to the Game Boy emulator context
 * @param addr Address to write to
 * @param val Value to write
 */
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                       const uint8_t val)
{
    ram[addr] = val;
}

/**
 * Error Handling Callback
 * 
 * Handles errors that occur during emulation.
 * Currently logs the error but allows emulation to continue.
 * 
 * @param gb Pointer to the Game Boy emulator context
 * @param gb_err Type of error that occurred
 * @param addr Address where the error occurred
 */
void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr)
{
#if 1
    const char *gb_err_str[4] = {
        "UNKNOWN",
        "INVALID OPCODE",
        "INVALID READ",
        "INVALID WRITE"};
    DBG_INFO("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
//  abort();
#endif
}

#if ENABLE_LCD
/**
 * Direct Character Rendering
 * 
 * Draws a character directly on the screen without using an intermediate buffer.
 * 
 * @param x0 X-coordinate (relative to frame buffer)
 * @param y0 Y-coordinate (relative to frame buffer)
 * @param c Character to draw
 * @param color Color of the character in RGB565 format
 */
void draw_char_direct_rgb565(int x0, int y0, const char c, uint16_t color)
{
    if (c <= 0x20 || 0x80 <= c) return;
    
    // Calcul de la position absolue sur l'écran
    int x_screen = (WIDTH - FRAME_BUFF_WIDTH) / 2 + x0;
    int y_screen = (HEIGHT - FRAME_BUFF_HEIGHT) / 2 + y0;
    
    // Buffer temporaire juste pour un caractère (8×16 pixels × 2 octets par pixel)
    uint8_t char_buffer[8 * 16 * 2] = {0};
    
    // Récupérer le bitmap du caractère
    const uint8_t *rd_ptr = bmp + (c - 0x20) * ((8 * 16 + 7) / 8);
    
    // Remplir le buffer temporaire avec les pixels du caractère
    for (int iy = 0; iy < 16; iy++) {
        uint8_t pattern = *(rd_ptr++);
        for (int ix = 0; ix < 8; ix++) {
            if (pattern & 1) {
                int wr_index = iy * 16 + ix * 2;
                char_buffer[wr_index] = (uint8_t)(color >> 8);      // high byte
                char_buffer[wr_index + 1] = (uint8_t)(color & 0xFF); // low byte
            }
            pattern >>= 1;
        }
    }
    
    // Définir la fenêtre d'affichage pour ce caractère
    start_window(x_screen, y_screen, 8, 16);
    
    // Envoyer les données du caractère
    for (int y = 0; y < 16; y++) {
        write_data(&char_buffer[y * 16], 8);
        finish_write_data(false);
    }
    
    // Finaliser l'écriture
    finish_write_data(true);
}

/**
 * Draw String Helper Function
 * 
 * Wrapper function to draw a white string at the specified position.
 * 
 * @param x X-coordinate (relative to frame buffer)
 * @param y Y-coordinate (relative to frame buffer)
 * @param str String to draw
 */
void draw_string(int x, int y, const char *str)
{
    char c;
    int curr_x = x;
    uint16_t color = 0xffff; // White color

    while ((c = *(str++)) != '\0') {
        if (curr_x + 8 > FRAME_BUFF_WIDTH) break;
        draw_char_direct_rgb565(curr_x, y, c, color);
        curr_x += 8 + 1; // 8 pixels width + 1 space pixel
    }
}

/**
 * Clear Entire Screen
 * 
 * Clears the entire screen, not just the frame buffer area.
 */
void clear_screen()
{
    // Set window to entire screen
    start_window(0, 0, WIDTH, HEIGHT);
    
    // Temporary buffer for a clear line
    uint8_t clear_line[WIDTH * 2] = {0};
    
    // Clear the screen line by line
    for (int y = 0; y < HEIGHT; y++) {
        write_data(clear_line, WIDTH);
        finish_write_data(false);
    }
    
    finish_write_data(true);
}

/**
 * LCD Draw Line Callback
 * 
 * Callback function used by the Game Boy emulator to draw a line of the LCD.
 * This function converts Game Boy pixels to RGB565 format and sends them to the display.
 * Each pixel is duplicated horizontally and vertically to scale the 160x144 Game Boy
 * screen to 320x288 pixels on the physical display.
 * 
 * @param gb Pointer to the Game Boy emulator context
 * @param pixels Array of pixels for the current line
 * @param line Line number to draw
 */
void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[LCD_WIDTH],
                       const uint_fast8_t line)
{
    // Duplicate each pixel horizontally (160 -> 320 pixels)
#if PEANUT_FULL_GBC_SUPPORT || WALNUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode)
    {
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            // Convert RGB555 to RGB565 properly
            uint16_t color555 = gb->cgb.fixPalette[pixels[x]];
            uint16_t r = (color555 >> 10) & 0x1F;
            uint16_t g = (color555 >> 5) & 0x1F;
            uint16_t b = color555 & 0x1F;
            uint16_t pixel = (r << 11) | ((g << 1) << 5) | b;
            // Duplicate each pixel twice in the buffer with correct byte order
            pixels_buffer[x * 4] = (uint8_t)(pixel >> 8);       // high byte of first pixel
            pixels_buffer[x * 4 + 1] = (uint8_t)(pixel & 0xFF); // low byte of first pixel
            pixels_buffer[x * 4 + 2] = (uint8_t)(pixel >> 8);   // high byte of second pixel
            pixels_buffer[x * 4 + 3] = (uint8_t)(pixel & 0xFF); // low byte of second pixel
        }
    }
    else
    {
#endif
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            // Duplicate each pixel twice in the buffer with correct byte order
            uint16_t pixel = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
            pixels_buffer[x * 4] = (uint8_t)(pixel >> 8);       // high byte of first pixel
            pixels_buffer[x * 4 + 1] = (uint8_t)(pixel & 0xFF); // low byte of first pixel
            pixels_buffer[x * 4 + 2] = (uint8_t)(pixel >> 8);   // high byte of second pixel
            pixels_buffer[x * 4 + 3] = (uint8_t)(pixel & 0xFF); // low byte of second pixel
        }
#if PEANUT_FULL_GBC_SUPPORT || WALNUT_FULL_GBC_SUPPORT
    }
#endif

    finish_write_data(false);
    if (line == 0)
    {
        // Double the width when starting the window
        start_window((WIDTH - (LCD_WIDTH * 2)) / 2, ((HEIGHT - (LCD_HEIGHT * 2)) / 2), LCD_WIDTH * 2, LCD_HEIGHT * 2);
        write_data(pixels_buffer, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(pixels_buffer, LCD_WIDTH * 2);
    }
    else if (line == LCD_HEIGHT)
    {
        finish_write_data(true);
    }
    else
    {
        // Write double-width line twice to create vertical duplication
        write_data(pixels_buffer, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(pixels_buffer, LCD_WIDTH * 2);
    }
}
#endif

#if ENABLE_SDCARD
/**
 * Load Save File
 * 
 * Loads a save file (cartridge RAM) from the SD card.
 * The filename is derived from the ROM name.
 * 
 * @param gb Pointer to the Game Boy emulator context
 */
void read_cart_ram_file(struct gb_s *gb)
{
    char filename[16];
    size_t save_size = 0;
    UINT br;

    gb_get_rom_name(gb, filename);
    gb_get_save_size_s(gb, &save_size);
    if (save_size > 0)
    {
        sd_card_t *pSD = sd_get_by_num(0);
        FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
        if (FR_OK != fr)
        {
            DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }

        FIL fil;
        fr = f_open(&fil, filename, FA_READ);
        if (fr == FR_OK)
        {
            f_read(&fil, ram, f_size(&fil), &br);
        }
        else
        {
            DBG_INFO("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK)
        {
            DBG_INFO("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
        f_unmount(pSD->pcName);
        DBG_INFO("I read_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename, save_size);
    }
    else
    {
        DBG_INFO("I read_cart_ram_file(%s) SKIPPED\n", filename);
    }
}

/**
 * Save Game Data
 * 
 * Writes the cartridge RAM to a save file on the SD card.
 * The filename is derived from the ROM name.
 * 
 * @param gb Pointer to the Game Boy emulator context
 */
void write_cart_ram_file(struct gb_s *gb)
{
    char filename[16];
    size_t save_size = 0;
    UINT bw;

    gb_get_rom_name(gb, filename);
    gb_get_save_size_s(gb, &save_size);
    if (save_size > 0)
    {
        sd_card_t *sd = sd_get_by_num(0);
        FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);
        if (FR_OK != fr)
        {
            DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }

        FIL fil;
        fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK)
        {
            f_write(&fil, ram, save_size, &bw);
        }
        else
        {
            DBG_INFO("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK)
        {
            DBG_INFO("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }

        f_unmount(sd->pcName);
    }

    DBG_INFO("I write_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename, bw);
}

/**
 * Load Emulator State
 * 
 * Reads the internal Game Boy emulator state from the SD card.
 * This allows resuming the game from where it was last played.
 * 
 * @param gb Pointer to the Game Boy emulator context
 */
void read_gb_emulator_state(struct gb_s *gb)
{
    char filename[16];
    char filename_state[32];
    UINT br = 0;
    FIL fil;

    sd_card_t *sd = sd_get_by_num(0);
    FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);

    gb_get_rom_name(gb, filename);
    sprintf(filename_state, "%s_state.bin", filename);
    fr = f_open(&fil, filename_state, FA_READ);

    if (fr == FR_OK)
    {
        f_read(&fil, (uint8_t *)gb, sizeof(struct gb_s), &br);
    }
    else
    {
        DBG_INFO("W read_gb_emulator_state(%s): SKIPPED (no previous state)\n", filename_state);
        goto finish;
    }

    DBG_INFO("I read_gb_emulator_state(%s) COMPLETED (%lu bytes)\n", filename_state, br);

finish:
    fr = f_close(&fil);
    if (fr != FR_OK)
    {
        DBG_INFO("W f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }

    f_unmount(sd->pcName);
}

/**
 * Save Emulator State
 * 
 * Writes the internal Game Boy emulator state to the SD card.
 * This allows resuming the game from this exact state later.
 * 
 * @param gb Pointer to the Game Boy emulator context
 */
void write_gb_emulator_state(struct gb_s *gb)
{
    char filename[16];
    char filename_state[32];
    UINT bw;
    FIL fil;

    sd_card_t *sd = sd_get_by_num(0);
    FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);

    gb_get_rom_name(gb, filename);
    sprintf(filename_state, "%s_state.bin", filename);
    fr = f_open(&fil, filename_state, FA_CREATE_ALWAYS | FA_WRITE);

    if (fr == FR_OK)
    {
        f_write(&fil, (uint8_t *)gb, sizeof(struct gb_s), &bw);
    }
    else
    {
        DBG_INFO("E write_gb_emulator_state(%s) FAILED (%s)\n", filename_state, FRESULT_str(fr));
        goto finish;
    }

    DBG_INFO("I write_gb_emulator_state(%s) COMPLETED (%lu bytes)\n", filename, bw);

finish:
    fr = f_close(&fil);
    if (fr != FR_OK)
    {
        DBG_INFO("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }
    f_unmount(sd->pcName);
}

/**
 * Erase Flash Memory
 * 
 * Erases a section of flash memory at the specified address.
 * This function supports both RP2040 and RP2350 chips.
 * 
 * @param address Address to erase (offset from start of flash)
 * @param size_bytes Size of area to erase in bytes
 * @return 0 on success, error code on failure
 */
int flash_erase(uintptr_t address, uint32_t size_bytes)
{
#if PICO_RP2040
    flash_range_erase(address, size_bytes);
    return 0;
#elif PICO_RP2350
    cflash_flags_t cflash_flags = {(CFLASH_OP_VALUE_ERASE << CFLASH_OP_LSB) |
                                   (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                                   (CFLASH_ASPACE_VALUE_RUNTIME << CFLASH_ASPACE_LSB)};

    // Round up size_bytes or rom_flash_op will throw an alignment error
    uint32_t size_aligned = (size_bytes + 0x1FFF) & -FLASH_SECTOR_SIZE;

    int ret = rom_flash_op(cflash_flags, address + XIP_BASE, size_aligned, NULL);

    if (ret != PICO_OK)
    {   
        DBG_INFO("E FLASH_ERASE error: %d, address %08x\n", ret, address + XIP_BASE);
        // need to debug all of these
        while(1);
    }

    rom_flash_flush_cache();

    return ret;
#endif
}

/**
 * Program Flash Memory
 * 
 * Writes data to flash memory at the specified address.
 * This function supports both RP2040 and RP2350 chips.
 * 
 * @param address Address to write to (offset from start of flash)
 * @param buf Buffer containing data to write
 * @param size_bytes Size of data to write in bytes
 * @return 0 on success, error code on failure
 */
int flash_program(uintptr_t address, const void *buf, uint32_t size_bytes)
{
#if PICO_RP2040
    flash_range_program(address, buf, size_bytes);
    return 0;

#elif PICO_RP2350
    cflash_flags_t cflash_flags = {(CFLASH_OP_VALUE_PROGRAM << CFLASH_OP_LSB) |
                                   (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                                   (CFLASH_ASPACE_VALUE_RUNTIME << CFLASH_ASPACE_LSB)};

    int ret = rom_flash_op(cflash_flags, address + XIP_BASE, size_bytes, (void *)buf);
    if (ret != PICO_OK)
    {   
        DBG_INFO("E FLASH_PROG error: %d, address %08x\n", ret, address + XIP_BASE);
        // need to debug all of these
        while(1);
    }

    rom_flash_flush_cache();

    return ret;
#endif
}

/**
 * Load ROM File
 * 
 * Loads a Game Boy ROM file from the SD card into flash memory.
 * This makes the ROM available for the emulator to run.
 * 
 * @param filename Name of the ROM file to load
 */
void load_cart_rom_file(char *filename)
{
    UINT br;
    uint8_t buffer[FLASH_SECTOR_SIZE];
    bool mismatch = false;
    sd_card_t *pSD = sd_get_by_num(0);
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (FR_OK != fr)
    {
        DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        return;
    }
    FIL fil;
    fr = f_open(&fil, filename, FA_READ);
    if (fr == FR_OK)
    {
        uint32_t flash_target_offset = FLASH_TARGET_OFFSET;
        uint32_t ctl_flash = 0;
        for (;;)
        {
            f_read(&fil, buffer, sizeof buffer, &br);
            if (br == 0)
                break; /* end of file */

            DBG_INFO("I Erasing target region...\n");
            flash_erase(flash_target_offset, FLASH_SECTOR_SIZE);
            DBG_INFO("I Programming target region...\n");
            flash_program(flash_target_offset, buffer, FLASH_SECTOR_SIZE);
            
            /* Read back target region and check programming */
            DBG_INFO("I Done. Reading back target region...\n");
            for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; i++)
            {
                if (rom[ctl_flash + i] != buffer[i])
                {   
                    DBG_INFO("E Mismatch at address 0x%08X: read 0x%02X, expected 0x%02X\n",
                             (unsigned)(flash_target_offset + i),
                             rom[ctl_flash + i], buffer[i]);
                    mismatch = true;
                }
            }

            /* Next sector */
            ctl_flash += FLASH_SECTOR_SIZE;
            flash_target_offset += FLASH_SECTOR_SIZE;
        }
        if (!mismatch)
        {
            DBG_INFO("I Programming successful!\n");
        }
        else
        {
            DBG_INFO("E Programming failed!\n");
        }
    }
    else
    {
        DBG_INFO("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
    }

    fr = f_close(&fil);
    if (fr != FR_OK)
    {
        DBG_INFO("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }
    f_unmount(pSD->pcName);

    DBG_INFO("I load_cart_rom_file(%s) COMPLETE (%lu bytes)\n", filename, br);
}

/**
 * Display ROM Selection Page
 * 
 * Displays one page of Game Boy ROM files found on the SD card.
 * Used by the ROM file selector interface.
 * 
 * @param filename Array to store found filenames
 * @param num_page Page number to display (each page shows up to 22 files)
 * @return Number of files found on the page
 */
uint16_t rom_file_selector_display_page(char filename[22][256], uint16_t num_page)
{
    sd_card_t *pSD = sd_get_by_num(0);
    DIR dj;
    FILINFO fno;
    FRESULT fr;

    fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (FR_OK != fr)
    {
        DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        return 0;
    }

    /* clear the filenames array */
    for (uint8_t ifile = 0; ifile < 22; ifile++)
    {
        strcpy(filename[ifile], "");
    }

    /* search *.gb files */
    uint16_t num_file = 0;
    fr = f_findfirst(&dj, &fno, ".", "?*.gb*");

    /* skip the first N pages */
    if (num_page > 0)
    {
        while (num_file < num_page * 22 && fr == FR_OK && fno.fname[0])
        {
            num_file++;
            fr = f_findnext(&dj, &fno);
        }
    }

    /* store the filenames of this page */
    num_file = 0;
    while (num_file < 22 && fr == FR_OK && fno.fname[0])
    {
        if (fno.fname[0] != '.')
        {
            /* Skip any file starting with dot. These are hidden files. */
            strcpy(filename[num_file], fno.fname);
            num_file++;
        }

        fr = f_findnext(&dj, &fno);
    }
    f_closedir(&dj);
    f_unmount(pSD->pcName);

    /* display *.gb rom files on screen */
    clear_screen();
    for (uint8_t ifile = 0; ifile < num_file; ifile++)
    {
        DBG_INFO("Game: %s\n", filename[ifile]);
        draw_string(20, ifile * 20, filename[ifile]);
    }
    
    /* display version on bottom right corner */
    char ver_buf[16];
    snprintf(ver_buf, sizeof(ver_buf), "v%d.%d.%d", PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH);
    draw_string(FRAME_BUFF_WIDTH - (strlen(ver_buf) * 9), FRAME_BUFF_HEIGHT - 20, ver_buf);
    
    return num_file;
}

/**
 * ROM File Selector
 * 
 * Presents a user interface to select a Game Boy ROM file to play.
 * Displays pages of up to 22 ROM files and allows navigation between them.
 * ROM files (.gb) should be placed in the root directory of the SD card.
 * The selected ROM will be loaded into flash memory for execution.
 */
void rom_file_selector()
{
    DBG_INFO("ROM File Selector: Starting...\n");
    uint16_t num_page = 0;
    char filename[22][256];
    uint16_t num_file;
    char buf[256];
    bool break_outer = false;

    /* display the first page with up to 22 rom files */
    num_file = rom_file_selector_display_page(filename, num_page);
    DBG_INFO("ROM File Selector: Found %d files on first page\n", num_file);

    /* select the first rom */
    uint8_t selected = 0;
    DBG_INFO("ROM File Selector: Highlighting first ROM: %s\n", filename[selected]);
    sprintf(buf, "%02d", selected + 1);
    draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
    draw_string(0, (selected % 22) * 20, "=>");

    /* get user's input */
    while (true)
    {
        switch (wait_key())
        {
        case KEY_ENTER:
        case KEY_A:
        case KEY_B:
            DBG_INFO("ROM File Selector: A/B button pressed - loading ROM: %s\n", filename[selected]);

            rom_file_selector_display_page(filename, num_page);
            sprintf(buf, "Loading %s", filename[selected]);
            draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
            sleep_ms(150);

            load_cart_rom_file(filename[selected]);
            break_outer = true;
            break;

        case KEY_SELECT:
        case KEY_START:
            DBG_INFO("ROM File Selector: Start button pressed - resuming last game\n");
            break_outer = true;
            break;

        case KEY_UP:
            DBG_INFO("ROM File Selector: Up button - selecting previous ROM\n");
            rom_file_selector_display_page(filename, num_page);
            draw_string(0, (selected % 22) * 20, "");
            if (selected == 0)
            {
                selected = num_file - 1;
            }
            else
            {
                selected--;
            }
            DBG_INFO("ROM File Selector: Selected ROM: %s\n", filename[selected]);
            sprintf(buf, "%02d", selected + 1);
            draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
            draw_string(0, (selected % 22) * 20, "=>");
            sleep_ms(150);
            break;

        case KEY_DOWN:

            DBG_INFO("ROM File Selector: Down button - selecting next ROM\n");
            rom_file_selector_display_page(filename, num_page);
            selected++;
            if (selected >= num_file)
                selected = 0;
            DBG_INFO("ROM File Selector: Selected ROM: %s\n", filename[selected]);
            sprintf(buf, "%02d", selected + 1);
            draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
            draw_string(0, (selected % 22) * 20, "=>");
            sleep_ms(150);
            break;
        }

        if (break_outer)
            break;
    }

    DBG_INFO("ROM File Selector: Exiting selector\n");
}

#endif

#if ENABLE_SOUND

/**
 * Core 1 Audio Processing Function
 * 
 * This function runs on core 1 and handles all audio processing.
 * It initializes the audio hardware, processes audio samples from the
 * Game Boy APU, and sends them to the I2S audio output.
 * Communication with core 0 happens via a queue for commands.
 */
void core1_audio(void)
{   
#if PICO_RP2350
    flash_safe_execute_core_init();
#endif

    /* Allocate memory for the stream buffer */
    stream = malloc(AUDIO_SAMPLES_TOTAL * sizeof(int16_t));
    assert(stream != NULL);
    memset(stream, 0, AUDIO_SAMPLES_TOTAL * sizeof(int16_t));

    /* Initialize I2S sound driver (using PIO0) */
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 4);
    i2s_init(&i2s_config);

    /* Initialize audio emulation. */
    audio_init(&apu_ctx);

    DBG_INFO("I Audio ready on core1.\n");

    while (1)
    {   
        audio_commands_e cmd;
        queue_remove_blocking(&call_queue, &cmd);
        switch (cmd)
        {
        case AUDIO_CMD_PLAYBACK:
            audio_callback(&apu_ctx, stream);
            i2s_dma_write(&i2s_config, stream);
            break;

        case AUDIO_CMD_VOLUME_UP:
            //flash_safe_execute_core_deinit();
            i2s_increase_volume(&i2s_config);
            break;

        case AUDIO_CMD_VOLUME_DOWN:
            i2s_decrease_volume(&i2s_config);
            break;

        default:
            break;
        }
    }

    DBG_INFO("I Audio stop on core1.\n");
    HEDLEY_UNREACHABLE();
}
#endif

/**
 * Main Function
 * 
 * Entry point of the program. Initializes hardware, sets up the Game Boy
 * emulator, and runs the main application loop.
 * 
 * @return Should never return (runs in an infinite loop)
 */
int main(void)
{
    static struct gb_s gb;         // Game Boy emulator context
    enum gb_init_error_e ret;      // Initialization error code

    /* Initialize system hardware */
    vreg_set_voltage(VREG_VOLT);                  // Set voltage for overclocking
    sleep_ms(100);                                // Wait for voltage to stabilize
    set_sys_clock_khz(SYS_CLK_FREQ / 1000, true); // Overclock 
    
    stdio_init_all();                        // Initialize standard I/O
    DBG_INIT();                              // Initialize debug output
    DBG_INFO("INIT: Version %d.%d.%d\n", PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH); // Print initialization message

    /* Initialize subsystems */
#if ENABLE_SOUND
    audio_commands_e q_audio = AUDIO_CMD_IDLE;
    queue_init(&call_queue, sizeof(audio_commands_e), 1); // Initialize communication queue
    multicore_launch_core1(core1_audio);                  // Start audio processing on core 1
#endif

#if ENABLE_LCD
    init(SYS_CLK_FREQ);     // Initialize display with system clock frequency
    start_game();           // Initialize display for game rendering
#endif

    init_i2c_kbd();         // Initialize I2C keyboard interface
    device_init();          // Initialize PocketPico device hardware

    while (true)
    {
#if ENABLE_SDCARD
        /* ROM File selector */
        rom_file_selector();
#endif

#if ENABLE_LCD
        set_spi_speed(LCD_BAUDRATE);
        clear_screen();
#endif
        /* Initialize Game Boy emulator */
        memcpy(rom_bank0, rom, sizeof(rom_bank0));  // Copy ROM bank 0 to RAM for faster access
        ret = gb_init(&gb,                          // Initialize Game Boy context
                     &gb_rom_read_8bit,             // 8-bit ROM read callback
                     &gb_rom_read_16bit,            // 16-bit ROM read callback
                     &gb_rom_read_32bit,            // 32-bit ROM read callback
                     &gb_cart_ram_read,             // RAM read callback
                     &gb_cart_ram_write,            // RAM write callback
                     &gb_error,                     // Error handling callback
                     NULL);                         // No custom context
        
        if (ret != GB_INIT_NO_ERROR)
        {
            DBG_INFO("Error: %d\n", ret);
            goto out;
        }

#if ENABLE_SDCARD
        /* Load saved emulator state */
        read_gb_emulator_state(&gb);         // Try to load last saved emulator state
#endif

        /* Set up display colors */
        char rom_title[16];
        auto_assign_palette(palette,         // Automatically assign a color palette
                           gb_colour_hash(&gb), 
                           gb_get_rom_name(&gb, rom_title));

#if ENABLE_LCD
        gb_init_lcd(&gb, &lcd_draw_line);    // Initialize LCD with draw line callback
        DBG_INFO("LCD ");
#endif

        DBG_INFO("\n> ");
#if ENABLE_DEBUG
        uint_fast32_t frames = 0;
        uint64_t start_time = time_us_64();
        int input;
#endif
        while (1)
        {
            /* Execute CPU cycles until the screen has to be redrawn. */
            gb_run_frame_dualfetch(&gb);
#if ENABLE_DEBUG
            frames++;
#endif

#if ENABLE_SOUND
            q_audio = AUDIO_CMD_PLAYBACK;
            queue_add_blocking(&call_queue, &q_audio);
#endif
            /* Update buttons state */
            prev_joypad_bits.up = gb.direct.joypad_bits.up;
            prev_joypad_bits.down = gb.direct.joypad_bits.down;
            prev_joypad_bits.left = gb.direct.joypad_bits.left;
            prev_joypad_bits.right = gb.direct.joypad_bits.right;
            prev_joypad_bits.a = gb.direct.joypad_bits.a;
            prev_joypad_bits.b = gb.direct.joypad_bits.b;
            prev_joypad_bits.select = gb.direct.joypad_bits.select;
            prev_joypad_bits.start = gb.direct.joypad_bits.start;
            prev_joypad_bits.F1 = other_joypad_bits.F1;
            prev_joypad_bits.F2 = other_joypad_bits.F2;
            prev_joypad_bits.F3 = other_joypad_bits.F3;
            prev_joypad_bits.F4 = other_joypad_bits.F4;

            gb.direct.joypad_bits.up = input_pins[KEY_UP] == 0 ? 1 : 0;
            gb.direct.joypad_bits.down = input_pins[KEY_DOWN] == 0 ? 1 : 0;
            gb.direct.joypad_bits.left = input_pins[KEY_LEFT] == 0 ? 1 : 0;
            gb.direct.joypad_bits.right = input_pins[KEY_RIGHT] == 0 ? 1 : 0;
            gb.direct.joypad_bits.a = input_pins[KEY_A] == 0 ? 1 : 0;
            gb.direct.joypad_bits.b = input_pins[KEY_B] == 0 ? 1 : 0;
            gb.direct.joypad_bits.select = input_pins[KEY_SELECT] == 0 ? 1 : 0;
            gb.direct.joypad_bits.start = input_pins[KEY_START] == 0 ? 1 : 0;
            other_joypad_bits.F1 = input_pins[KEY_F1] == 0 ? 1 : 0;
            other_joypad_bits.F2 = input_pins[KEY_F2] == 0 ? 1 : 0;
            other_joypad_bits.F3 = input_pins[KEY_F3] == 0 ? 1 : 0;
            other_joypad_bits.F4 = input_pins[KEY_F4] == 0 ? 1 : 0;

            /* F2 + UP/DOWN, volume */
            if (!other_joypad_bits.F2)
            {
#if ENABLE_SOUND
                if (!gb.direct.joypad_bits.up && prev_joypad_bits.up)
                {
                    /* select + up: increase sound volume */
                    q_audio = AUDIO_CMD_VOLUME_UP;
                    queue_add_blocking(&call_queue, &q_audio);
                }
                if (!gb.direct.joypad_bits.down && prev_joypad_bits.down)
                {
                    /* select + down: decrease sound volume */
                    q_audio = AUDIO_CMD_VOLUME_DOWN;
                    queue_add_blocking(&call_queue, &q_audio);
                }
#endif
            }

            /* F3 + UP/DOWN, manual color palette */
            if (!other_joypad_bits.F3)
            {
                if (!gb.direct.joypad_bits.up && prev_joypad_bits.up)
                {
                    if (manual_palette_selected < 12)
                    {
                        manual_palette_selected++;
                        manual_assign_palette(palette, manual_palette_selected);
                    }
                }
                if (!gb.direct.joypad_bits.down && prev_joypad_bits.down)
                {
                    if (manual_palette_selected > 0)
                    {
                        manual_palette_selected--;
                        manual_assign_palette(palette, manual_palette_selected);
                    }
                }
            }

            /* F1, save ram and resets to the game selection menu */
            if (!other_joypad_bits.F1 && prev_joypad_bits.F1)
            {
#if ENABLE_SDCARD
                write_cart_ram_file(&gb);
                /* Try to save the emulator state for this game. */
                write_gb_emulator_state(&gb);
#endif
                goto out;
            }
            
            /* F4, enable/disable frame-skip => fast-forward */
            if (!other_joypad_bits.F4 && prev_joypad_bits.F4)
            {
                gb.direct.frame_skip = !gb.direct.frame_skip;
                DBG_INFO("I gb.direct.frame_skip = %d\n", gb.direct.frame_skip);
            }
            
#if ENABLE_DEBUG
            /* Serial monitor commands */
            input = getchar_timeout_us(0);
            if (input == PICO_ERROR_TIMEOUT)
                continue;

            switch (input)
            {
#if 0
        static bool invert = false;
        static bool sleep = false;
        static uint8_t freq = 1;
        static ili9225_color_mode_e colour = ILI9225_COLOR_MODE_FULL;

        case 'i':
            invert = !invert;
            ili9225_display_control(invert, colour);
            break;

        case 'f':
            freq++;
            freq &= 0x0F;
            ili9225_set_drive_freq(freq);
            DBG_INFO("Freq %u\n", freq);
            break;
#endif
            case 'i':
                gb.direct.interlace = !gb.direct.interlace;
                break;

            case 'f':
                gb.direct.frame_skip = !gb.direct.frame_skip;
                break;

            case 'b':
            {
                uint64_t end_time;
                uint32_t diff;
                uint32_t fps;

                end_time = time_us_64();
                diff = end_time - start_time;
                fps = ((uint64_t)frames * 1000 * 1000) / diff;
                DBG_INFO("Frames: %u\n"
                         "Time: %lu us\n"
                         "FPS: %lu\n",
                         frames, diff, fps);
                stdio_flush();
                frames = 0;
                start_time = time_us_64();
                break;
            }

            case '\n':
            case '\r':
            {
                gb.direct.joypad_bits.start = 0;
                break;
            }

            case '\b':
            {
                gb.direct.joypad_bits.select = 0;
                break;
            }

            case '8':
            {
                gb.direct.joypad_bits.up = 0;
                break;
            }

            case '2':
            {
                gb.direct.joypad_bits.down = 0;
                break;
            }

            case '4':
            {
                gb.direct.joypad_bits.left = 0;
                break;
            }

            case '6':
            {
                gb.direct.joypad_bits.right = 0;
                break;
            }

            case 'z':
            case 'w':
            {
                gb.direct.joypad_bits.a = 0;
                break;
            }

            case 'x':
            {
                gb.direct.joypad_bits.b = 0;
                break;
            }

            case 'q':
                goto out;

            default:
                break;
            }
#endif /* ENABLE_DEBUG */
        }

    out:
        DBG_INFO("\nEmulation Ended");
    }
}
