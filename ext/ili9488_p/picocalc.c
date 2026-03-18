#include <stdint.h>
#include "picocalc.h"
#include "picocalc_c.h"
#include "picocalc.pio.h"
#include "mono8x16.h"
#include "i2ckbd.h"
#include <stdio.h>

static int sys_clock_hz;
static direction_t curr_dir = EMPTY;
static int curr_speed = 10 * MHZ;

static PIO spi_pio;
static uint spi_sm;
static uint spi_offset;
static uint dma_tx;

// SPI push byte
static void pio_push(uint8_t data);

// SPI pop byte
static uint8_t pio_pop();

// wait for SPI to idle
static void pio_wait_idle();

// setup SPI direction and speed
static void setup_pio(direction_t new_dir, int new_speed);

// set SPI direction
static void set_spi_direction(direction_t new_dir);

void init(int sys_clk_hz)
{
    sys_clock_hz = sys_clk_hz;

    spi_pio = pio0;
    spi_sm = 0;

    spi_offset = pio_add_program(spi_pio, &picocalc_program);
    pio_gpio_init(spi_pio, PIN_MISO);
    pio_gpio_init(spi_pio, PIN_MOSI);
    pio_gpio_init(spi_pio, PIN_SCK);
    pio_sm_set_consecutive_pindirs(spi_pio, spi_sm, PIN_MISO, 1, false);
    pio_sm_set_consecutive_pindirs(spi_pio, spi_sm, PIN_MOSI, 1, true);
    pio_sm_set_consecutive_pindirs(spi_pio, spi_sm, PIN_SCK, 1, true);

    dma_tx = dma_claim_unused_channel(true);

    gpio_init(PIN_RST);
    gpio_init(PIN_DC);
    gpio_init(PIN_LCD_CS);
    gpio_init(PIN_RAM_CS);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_set_dir(PIN_RAM_CS, GPIO_OUT);
    gpio_set_dir(PIN_LCD_CS, GPIO_OUT);

    gpio_put(PIN_RST, 1);
    gpio_put(PIN_LCD_CS, 1);
    gpio_put(PIN_RAM_CS, 1);

    // hardware reset
    gpio_put(PIN_RST, 1);
    sleep_ms(1);
    gpio_put(PIN_RST, 0);
    sleep_ms(10);
    gpio_put(PIN_RST, 1);
    sleep_ms(10);

    // Software reset - reset the commands and parameters to their S/W Reset default values
    write_command(0x01, NULL, 0); // SWRESET
    sleep_ms(10);                 // required to wait at least 5ms

    // Pixel format set - 16 bit/pixel (RGB565)
    {
        uint8_t data[] = {0x55};
        write_command(0x3A, data, sizeof(data)); // COLMOD
    }

    // Memory access control - BGR colour filter panel, top to bottom, left to right
    {
        uint8_t data[] = {0x48};
        write_command(0x36, data, sizeof(data)); // MADCTL
    }

    // Display inversion on - this is critical for BGR displays
    write_command(0x21, NULL, 0); // INVON

    // Entry mode set - normal display, 16-bit (RGB) to 18-bit (rgb) colour conversion
    {
        uint8_t data[] = {0xC6};
        write_command(0xB7, data, sizeof(data)); // EMS
    }

    // Vertical scroll definition
    {
        uint8_t data[] = {0x00, 0x00, 0x01, 0x40, 0x00, 0x00}; // top fixed: 0, scroll: 320, bottom fixed: 0
        write_command(0x33, data, sizeof(data));               // VSCRDEF
    }

    // Sleep out
    write_command(0x11, NULL, 0); // SLPOUT
    sleep_ms(10);                 // required to wait at least 5ms

    // Display on
    write_command(0x29, NULL, 0); // DISPON

    {
        uint8_t data[] = {0x00, 0x00, 0x01, 0x3F};
        write_command(0x2A, data, sizeof(data));
    }

    {
        uint8_t data[] = {0x00, 0x00, 0x01, 0x3F};
        write_command(0x2B, data, sizeof(data));
    }
    write_command(0x2C, NULL, 0);
}

void setup_pio(direction_t new_dir, int new_speed)
{
    if (new_dir == curr_dir && new_speed == curr_speed)
        return;

    float div = sys_clock_hz / 2.f / new_speed;
    // printf("SPI: %d MHz, %d\n", new_speed / 1000000, (int)div);
    pio_sm_set_enabled(spi_pio, spi_sm, false);

    // load new PIO
    if (new_dir == TX)
    {
        pio_sm_config pio_cfg = picocalc_program_get_default_config(spi_offset);
        sm_config_set_out_pins(&pio_cfg, PIN_MOSI, 1);
        sm_config_set_sideset_pins(&pio_cfg, PIN_SCK);
        sm_config_set_fifo_join(&pio_cfg, PIO_FIFO_JOIN_TX);
        sm_config_set_out_shift(&pio_cfg, false, true, 8);
        sm_config_set_clkdiv(&pio_cfg, div);
        pio_sm_init(spi_pio, spi_sm, spi_offset, &pio_cfg);
    }
    else if (new_dir == RX)
    {
        pio_sm_config pio_cfg = picocalc_program_get_default_config(spi_offset);
        sm_config_set_out_pins(&pio_cfg, PIN_MOSI, 1);
        sm_config_set_in_pins(&pio_cfg, PIN_MISO);
        sm_config_set_sideset_pins(&pio_cfg, PIN_SCK);
        sm_config_set_out_shift(&pio_cfg, false, true, 8);
        sm_config_set_in_shift(&pio_cfg, false, true, 8);
        sm_config_set_clkdiv(&pio_cfg, div);
        pio_sm_init(spi_pio, spi_sm, spi_offset, &pio_cfg);
    }

    hw_set_bits(&spi_pio->input_sync_bypass, 1u << PIN_MISO);
    pio_sm_set_enabled(spi_pio, spi_sm, true);

    curr_dir = new_dir;
    curr_speed = new_speed;
}

void set_spi_direction(direction_t new_dir)
{
    setup_pio(new_dir, curr_speed);
}

void set_spi_speed(int new_speed)
{
    setup_pio(curr_dir, new_speed);
}

void clear(uint16_t color)
{
    uint8_t data[WIDTH * 2];
    // Use color directly - display hardware handles BGR conversion via MADCTL
    for (int x = 0; x < WIDTH * 2; x += 2)
    {
        data[x] = (uint8_t)(color >> 8);       // high byte
        data[x + 1] = (uint8_t)(color & 0xFF); // low byte
    }
    start_window(0, 0, WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++)
    {
        write_data(data, WIDTH);
        // start_write_data(0, y, WIDTH, 1, data);
        // finish_write_data(true);
    }
    finish_write_data(true);
}

void start_write_data(int x0, int y0, int w, int h, uint8_t *data)
{
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;
    {
        uint8_t xcoord[] = {
            (uint8_t)(x0 >> 8),
            (uint8_t)(x0 & 0xff),
            (uint8_t)(x1 >> 8),
            (uint8_t)(x1 & 0xff)};
        write_command(0x2a, xcoord, sizeof(xcoord));
    }
    {
        uint8_t ycoord[] = {
            (uint8_t)(y0 >> 8),
            (uint8_t)(y0 & 0xff),
            (uint8_t)(y1 >> 8),
            (uint8_t)(y1 & 0xff)};
        write_command(0x2b, ycoord, sizeof(ycoord));
    }
    // write_command(0x2c, data, w * h * 3 / 2);

    {
        gpio_put(PIN_DC, 0);
        gpio_put(PIN_LCD_CS, 0);
        uint8_t cmd = 0x2c;
        write_blocking(&cmd, 1);
        gpio_put(PIN_DC, 1);

        /*
        if (h > 1)
        {

            dma_channel_config dma_cfg = dma_channel_get_default_config(dma_tx);
            channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
            channel_config_set_dreq(&dma_cfg, pio_get_dreq(spi_pio, spi_sm, true));
            dma_channel_configure(dma_tx, &dma_cfg,
                                  &spi_pio->txf[spi_sm], // write address
                                  data,                  // read address
                                  w * h * 2,             // element count (each element is of size transfer_data_size)
                                  false);                // don't start yet

            dma_start_channel_mask(1u << dma_tx);
        }*/

        dma_channel_transfer_from_buffer_now(dma_tx, data, (w * h * 2));
    }
}

void start_window(int x0, int y0, int w, int h)
{
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;
    {
        uint8_t xcoord[] = {
            (uint8_t)(x0 >> 8),
            (uint8_t)(x0 & 0xff),
            (uint8_t)(x1 >> 8),
            (uint8_t)(x1 & 0xff)};
        write_command(0x2a, xcoord, sizeof(xcoord));
    }
    {
        uint8_t ycoord[] = {
            (uint8_t)(y0 >> 8),
            (uint8_t)(y0 & 0xff),
            (uint8_t)(y1 >> 8),
            (uint8_t)(y1 & 0xff)};
        write_command(0x2b, ycoord, sizeof(ycoord));
    }

    {
        gpio_put(PIN_DC, 0);
        gpio_put(PIN_LCD_CS, 0);
        uint8_t cmd = 0x2c;
        write_blocking(&cmd, 1);
        gpio_put(PIN_DC, 1);
    }
}

void write_data(uint8_t *data, uint16_t length)
{
    dma_channel_transfer_from_buffer_now(dma_tx, data, length * 2);
    /* Start the DMA transfer
    dma_channel_config dma_cfg = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(spi_pio, spi_sm, true));
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    dma_channel_configure(dma_tx, &dma_cfg,
                          &spi_pio->txf[spi_sm], // write address
                          data,                  // read address
                          length * 2,            // element count (each element is of size transfer_data_size)
                          true);                 // Start immediately */
}

void start_game()
{
    // Setup DMA
    dma_channel_config dma_cfg = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(spi_pio, spi_sm, true));
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    dma_channel_configure(dma_tx, &dma_cfg,
                          &spi_pio->txf[spi_sm], // write address
                          NULL,                  // read address
                          0,                     // element count (each element is of size transfer_data_size)
                          false);                // Start immediately
}

void finish_write_data(bool end)
{
    pio_wait_idle();
    if (end)
        gpio_put(PIN_LCD_CS, 1);
}

void write_command(uint8_t cmd, const uint8_t *data, int len)
{
    set_spi_direction(TX);
    gpio_put(PIN_DC, 0); // command mode
    gpio_put(PIN_LCD_CS, 0);
    write_blocking(&cmd, 1);
    if (data)
    {
        gpio_put(PIN_DC, 1); // data mode
        write_blocking(data, len);
    }
    gpio_put(PIN_LCD_CS, 1);
}

void write_blocking(const uint8_t *data, int len)
{
    set_spi_direction(TX);
    for (int i = 0; i < len; i++)
    {
        pio_push(data[i]);
    }
    pio_wait_idle();
}

void read_blocking(uint8_t tx_repeat, uint8_t *buff, int len)
{
    set_spi_direction(RX);
    int tx_remain = len, rx_remain = len;
    io_rw_8 *txfifo = (io_rw_8 *)&spi_pio->txf[spi_sm];
    io_rw_8 *rxfifo = (io_rw_8 *)&spi_pio->rxf[spi_sm];
    while (tx_remain || rx_remain)
    {
        if (tx_remain && !pio_sm_is_tx_fifo_full(spi_pio, spi_sm))
        {
            *txfifo = tx_repeat;
            --tx_remain;
        }
        if (rx_remain && !pio_sm_is_rx_fifo_empty(spi_pio, spi_sm))
        {
            *buff++ = *rxfifo;
            --rx_remain;
        }
    }
}

static void pio_push(uint8_t data)
{
    while (pio_sm_is_tx_fifo_full(spi_pio, spi_sm))
    {
    }
    *(volatile uint8_t *)&spi_pio->txf[spi_sm] = data;
}

static uint8_t pio_pop()
{
    while (pio_sm_is_rx_fifo_empty(spi_pio, spi_sm))
    {
    }
    return *(volatile uint8_t *)&spi_pio->rxf[spi_sm];
}

static void pio_wait_idle()
{
    uint32_t stall_mask = 1u << (spi_sm + PIO_FDEBUG_TXSTALL_LSB);
    spi_pio->fdebug = stall_mask;
    while (!(spi_pio->fdebug & stall_mask))
    {
    }
}

void ws19804_write_blocking(const uint8_t *buff, int len)
{
    write_blocking(buff, len);
}

void ws19804_read_blocking(uint8_t tx_repeat, uint8_t *buff, int len)
{
    read_blocking(tx_repeat, buff, len);
}

// Keyboard I2C
static absolute_time_t now;
uint8_t keycheck = 0;
uint8_t keyread = 0;
int input_pins[] = {
    PIN_PAD_A, PIN_PAD_B, PIN_PAD_SELECT, PIN_PAD_START,
    PIN_PAD_UP, PIN_PAD_DOWN, PIN_PAD_LEFT, PIN_PAD_RIGHT,
    PIN_PAD_ENTER, PIN_PAD_F1, PIN_PAD_F2, PIN_PAD_F3, PIN_PAD_F4};
    
#define NUM_INPUT_PINS (sizeof(input_pins) / sizeof(input_pins[0]))

static void __attribute__((optimize("-Os"))) __not_in_flash_func(timer_tick_cb)(unsigned alarm)
{
    absolute_time_t next;
    update_us_since_boot(&next, to_us_since_boot(now) + (TICKSPERSEC));
    if (hardware_alarm_set_target(0, next))
    {
        update_us_since_boot(&next, time_us_64() + (TICKSPERSEC));
        hardware_alarm_set_target(0, next);
    }

    kbd_interrupt();
    if (keycheck)
    {
        keycheck--;
    }
}

void device_init()
{
    hardware_alarm_claim(0);
    update_us_since_boot(&now, time_us_64());
    hardware_alarm_set_callback(0, timer_tick_cb);
    hardware_alarm_force_irq(0);
}

void kbd_interrupt()
{
    int kbd_ret = -1;
    int c;
    static int ctrlheld = 0;
    uint8_t key_stat = 0; // press,release, or hold
    if (keycheck == 0)
    {
        if (keyread == 0)
        {
            kbd_ret = write_i2c_kbd();
            keyread = 1;
        }
        else
        {
            kbd_ret = read_i2c_kbd();
            keyread = 0;
        }
        keycheck = KEYCHECKTIME;
    }

    if (kbd_ret < 0)
    {
        if (check_if_failed() > 0)
        {
            printf("try to reset i2c\n");
            reset_failed();
            init_i2c_kbd(); // re-init
        }
    }

    if (kbd_ret)
    {
        if (kbd_ret == 0xA503)
            ctrlheld = 0;
        else if (kbd_ret == 0xA502)
        {
            ctrlheld = 1;
        }
        else if ((kbd_ret & 0xff) == 1)
        { // pressed
            key_stat = 1;
        }
        else if ((kbd_ret & 0xff) == 3)
        {
            key_stat = 3;
        }

        c = kbd_ret >> 8;
        int realc = -1;
        switch (c)
        {
        case 0xA1:
        case 0xA2:
        case 0xA3:
        case 0xA4:
        case 0xA5:
            realc = -1; // skip shift alt ctrl keys
            break;
        default:
            realc = c;
            break;
        }

        c = realc;
        if (c >= 'a' && c <= 'z' && ctrlheld)
            c = c - 'a' + 1;

        switch (c)
        {
        case 0xb5: // UP
            set_kdb_key(4, key_stat);
            break;
        case 0xb6: // DOWN
            set_kdb_key(5, key_stat);
            break;
        case 0xb4: // LEFT
            set_kdb_key(6, key_stat);
            break;
        case 0xb7: // RIGHT
            set_kdb_key(7, key_stat);
            break;
        case '-': // select
            set_kdb_key(2, key_stat);
            break;
        case '=': // start
            set_kdb_key(3, key_stat);
            break;
        case '[': // B
            set_kdb_key(1, key_stat);
            break;
        case ']': // A
            set_kdb_key(0, key_stat);
            break;
        case 0x0A: // ENTER
            set_kdb_key(8, key_stat);
            break;
        case 0x81: // F1
            set_kdb_key(9, key_stat);
            break;
        case 0x82: // F2
            set_kdb_key(10, key_stat);
            break;
        case 0x83: // F3
            set_kdb_key(11, key_stat);
            break;
        case 0x84: // F4
            set_kdb_key(12, key_stat);
            break;
        default:
            break;
        }
    }
}

// keyboard key status to input_pins map
void set_kdb_key(uint8_t pin_offset, uint8_t key_status)
{

    if (key_status == 1)
    {
        input_pins[pin_offset] = 1;
    }
    else if (key_status == 3)
    {
        input_pins[pin_offset] = 0;
    }
}

int wait_key()
{
    // wait any button pushed
    int i = -1;
    for (;;)
    {
        sleep_ms(10);
        for (i = 0; i < NUM_INPUT_PINS; i++)
        {
            if (input_pins[i] == 1)
            {
                input_pins[i] = 0;
                printf("key %d pressed\n", i);
                return i;
            }
        }
    }
    return i;
}