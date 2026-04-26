#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/sio.h"

#define PIN_A0 8
#define PIN_A1 7
#define PIN_A2 6
#define PIN_A3 5
#define PIN_A4 4
#define PIN_A5 3
#define PIN_A6 2
#define PIN_A7 1
#define PIN_A8 27
#define PIN_A9 26
#define PIN_A10 23
#define PIN_A11 25
#define PIN_A12 0
#define PIN_A13 28
#define PIN_A14 29

#define PIN_D0 9
#define PIN_D1 10
#define PIN_D2 11
#define PIN_D3 17
#define PIN_D4 18
#define PIN_D5 19
#define PIN_D6 20
#define PIN_D7 21

#define PIN_CE_N 24 // active low, labeled G on schematic
#define PIN_OE_N 22 // active low, labeled E on schematic
#define PIN_LED 12

#define ROM_SIZE (32u * 1024u) // M27C256 = 32KB

// Flash tail 36KB: first 4KB for metadata, remaining 32KB for ROM data
#define FLASH_RESERVED_SIZE  (9u * FLASH_SECTOR_SIZE)
#define FLASH_META_OFFSET    (PICO_FLASH_SIZE_BYTES - FLASH_RESERVED_SIZE)
#define FLASH_ROM_OFFSET     (FLASH_META_OFFSET + FLASH_SECTOR_SIZE)
#define FLASH_META_ADDR      (XIP_BASE + FLASH_META_OFFSET)
#define FLASH_ROM_ADDR       (XIP_BASE + FLASH_ROM_OFFSET)
#define FLASH_MAGIC          0x52504F4Du // 'RPOM'

#define DATA_PIN_MASK ((1u << PIN_D0) | (1u << PIN_D1) | (1u << PIN_D2) | (1u << PIN_D3) | (1u << PIN_D4) | (1u << PIN_D5) | (1u << PIN_D6) | (1u << PIN_D7))

static uint8_t rom_data[ROM_SIZE];
static volatile bool rom_updating = false;
static volatile bool core1_paused = false;

// gpio_lo_lut[i]: i = GPIO[8:0] (9-bit), result = partial address from A0,A1-A7,A12
//   GPIO0=A12, GPIO1=A7, GPIO2=A6, GPIO3=A5, GPIO4=A4, GPIO5=A3, GPIO6=A2, GPIO7=A1, GPIO8=A0
// GPIO8(A0) is folded in to eliminate the separate | ((g>>8)&1) op in the hot loop
static uint16_t gpio_lo_lut[512];

// gpio_hi_lut[i]: i = GPIO[29:23] packed into 7 bits, result = partial address from A8-A14
//   bit0(GPIO23)=A10, bit1(GPIO24)=CE#(skip), bit2(GPIO25)=A11,
//   bit3(GPIO26)=A9,  bit4(GPIO27)=A8, bit5(GPIO28)=A13, bit6(GPIO29)=A14
static uint16_t gpio_hi_lut[128];

// addr_out_lut[addr]: combines rom_data lookup and data pin mapping into one SRAM access.
// 32KB * 4B = 128KB; total SRAM use stays well within 264KB.
static uint32_t addr_out_lut[ROM_SIZE];

// byte value -> GPIO bitmask for data pins; used only during LUT rebuild
static uint32_t data_out_lut[256];

static void build_luts(void)
{
    for (int i = 0; i < 512; i++)
    {
        uint16_t a = 0;
        a |= (uint16_t)(((i >> 0) & 1) << 12); // GPIO0 -> A12
        a |= (uint16_t)(((i >> 1) & 1) << 7);  // GPIO1 -> A7
        a |= (uint16_t)(((i >> 2) & 1) << 6);  // GPIO2 -> A6
        a |= (uint16_t)(((i >> 3) & 1) << 5);  // GPIO3 -> A5
        a |= (uint16_t)(((i >> 4) & 1) << 4);  // GPIO4 -> A4
        a |= (uint16_t)(((i >> 5) & 1) << 3);  // GPIO5 -> A3
        a |= (uint16_t)(((i >> 6) & 1) << 2);  // GPIO6 -> A2
        a |= (uint16_t)(((i >> 7) & 1) << 1);  // GPIO7 -> A1
        a |= (uint16_t)(((i >> 8) & 1) << 0);  // GPIO8 -> A0
        gpio_lo_lut[i] = a;
    }
    for (int i = 0; i < 128; i++)
    {
        uint16_t a = 0;
        a |= (uint16_t)(((i >> 0) & 1) << 10); // bit0 (GPIO23) -> A10
        // bit1 (GPIO24) = CE#, skip
        a |= (uint16_t)(((i >> 2) & 1) << 11); // bit2 (GPIO25) -> A11
        a |= (uint16_t)(((i >> 3) & 1) << 9);  // bit3 (GPIO26) -> A9
        a |= (uint16_t)(((i >> 4) & 1) << 8);  // bit4 (GPIO27) -> A8
        a |= (uint16_t)(((i >> 5) & 1) << 13); // bit5 (GPIO28) -> A13
        a |= (uint16_t)(((i >> 6) & 1) << 14); // bit6 (GPIO29) -> A14
        gpio_hi_lut[i] = a;
    }
    for (int i = 0; i < 256; i++)
    {
        uint32_t m = 0;
        m |= (uint32_t)(((i >> 0) & 1) << PIN_D0);
        m |= (uint32_t)(((i >> 1) & 1) << PIN_D1);
        m |= (uint32_t)(((i >> 2) & 1) << PIN_D2);
        m |= (uint32_t)(((i >> 3) & 1) << PIN_D3);
        m |= (uint32_t)(((i >> 4) & 1) << PIN_D4);
        m |= (uint32_t)(((i >> 5) & 1) << PIN_D5);
        m |= (uint32_t)(((i >> 6) & 1) << PIN_D6);
        m |= (uint32_t)(((i >> 7) & 1) << PIN_D7);
        data_out_lut[i] = m;
    }
}

// Call whenever rom_data changes.
static void build_addr_out_lut(void)
{
    for (uint32_t i = 0; i < ROM_SIZE; i++)
        addr_out_lut[i] = data_out_lut[rom_data[i]];
}

// Runs from SRAM; gpio_lo_lut covers GPIO[8:0] so A0 is already included.
static inline uint16_t __not_in_flash_func(gpio_to_addr)(uint32_t g)
{
    return (uint16_t)(gpio_lo_lut[g & 0x1FFu] | gpio_hi_lut[(g >> 23) & 0x7Fu]);
}

// Core 1 ROM emulation tight loop (executes from SRAM)
static void __not_in_flash_func(rom_emulator_core1)(void)
{
    const uint32_t CE_MASK = (1u << PIN_CE_N);
    const uint32_t OE_MASK = (1u << PIN_OE_N);
    const uint32_t ACTIVE_MASK = CE_MASK | OE_MASK;

    while (true)
    {
        if (rom_updating)
        {
            sio_hw->gpio_oe_clr = DATA_PIN_MASK;
            core1_paused = true;
            while (rom_updating)
                tight_loop_contents();
            core1_paused = false;
            __dmb(); // acquire barrier: ensure fresh rom_data[] / addr_out_lut[] visible
        }

        // address is already stable when CE# asserts
        while (sio_hw->gpio_in & CE_MASK)
            tight_loop_contents();

        // capture GPIO snapshot atomically when OE# asserts
        uint32_t g;
        while ((g = sio_hw->gpio_in) & OE_MASK)
            tight_loop_contents();

        uint32_t out = addr_out_lut[gpio_to_addr(g)];
        sio_hw->gpio_set = out;
        sio_hw->gpio_clr = DATA_PIN_MASK ^ out; // out ⊆ DATA_PIN_MASK so XOR = AND NOT
        sio_hw->gpio_oe_set = DATA_PIN_MASK;

        while (!(sio_hw->gpio_in & ACTIVE_MASK))
            tight_loop_contents();

        sio_hw->gpio_oe_clr = DATA_PIN_MASK;
    }
}

// Runs from SRAM; Core 1 must be paused beforehand.
static void __not_in_flash_func(flash_save_rom)(void)
{
    while (!core1_paused)
        tight_loop_contents();

    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(FLASH_META_OFFSET, FLASH_RESERVED_SIZE);

    uint8_t meta[FLASH_PAGE_SIZE];
    memset(meta, 0xFF, sizeof(meta));
    *(uint32_t *)meta = FLASH_MAGIC;
    flash_range_program(FLASH_META_OFFSET, meta, FLASH_PAGE_SIZE);

    flash_range_program(FLASH_ROM_OFFSET, rom_data, ROM_SIZE);

    restore_interrupts(ints);
}

// Commands (single byte): W=write 32768 bytes, R=read 32768 bytes, E=erase (fill 0xFF), I=info
static void __not_in_flash_func(handle_usb)(void)
{
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT)
        return;

    if (c == 'W')
    {
        rom_updating = true;
        __dmb();

        bool ok = true;
        for (uint32_t i = 0; i < ROM_SIZE; i++)
        {
            int b = PICO_ERROR_TIMEOUT;
            for (int retry = 0; retry < 10 && b == PICO_ERROR_TIMEOUT; retry++)
                b = getchar_timeout_us(500000);
            if (b == PICO_ERROR_TIMEOUT)
            {
                ok = false;
                break;
            }
            rom_data[i] = (uint8_t)b;
        }
        if (ok)
        {
            build_addr_out_lut();
            flash_save_rom();
        }

        __dmb();
        rom_updating = false;

        putchar_raw(ok ? 'K' : 'X');
    }
    else if (c == 'R')
    {
        for (uint32_t i = 0; i < ROM_SIZE; i++)
            putchar_raw(rom_data[i]);
    }
    else if (c == 'E')
    {
        rom_updating = true;
        __dmb();
        memset(rom_data, 0xFF, ROM_SIZE);
        build_addr_out_lut();
        __dmb();
        rom_updating = false;
        putchar_raw('K');
    }
    else if (c == 'I')
    {
        printf("RP-ROM EPROM Emulator (M27C256 compatible)\r\n");
        printf("Clock : %lu Hz\r\n", (unsigned long)clock_get_hz(clk_sys));
        printf("ROM   : %u bytes\r\n", ROM_SIZE);
        printf("Cmds  : W=write R=read E=erase I=info\r\n");
    }
}

static void setup_gpio(void)
{
    const uint addr_pins[] = {
        PIN_A0, PIN_A1, PIN_A2, PIN_A3, PIN_A4, PIN_A5, PIN_A6, PIN_A7,
        PIN_A8, PIN_A9, PIN_A10, PIN_A11, PIN_A12, PIN_A13, PIN_A14};
    for (int i = 0; i < 15; i++)
    {
        gpio_init(addr_pins[i]);
        gpio_set_dir(addr_pins[i], GPIO_IN);
        gpio_disable_pulls(addr_pins[i]);
    }

    gpio_init(PIN_CE_N);
    gpio_set_dir(PIN_CE_N, GPIO_IN);
    gpio_disable_pulls(PIN_CE_N);
    gpio_init(PIN_OE_N);
    gpio_set_dir(PIN_OE_N, GPIO_IN);
    gpio_disable_pulls(PIN_OE_N);

    const uint data_pins[] = {
        PIN_D0, PIN_D1, PIN_D2, PIN_D3, PIN_D4, PIN_D5, PIN_D6, PIN_D7};
    for (int i = 0; i < 8; i++)
    {
        gpio_init(data_pins[i]);
        gpio_set_dir(data_pins[i], GPIO_IN);
        gpio_disable_pulls(data_pins[i]);
    }

    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);
}

int main(void)
{
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    set_sys_clock_khz(360000, true);
    sleep_ms(10);

    stdio_init_all();

    if (*(const uint32_t *)FLASH_META_ADDR == FLASH_MAGIC)
        memcpy(rom_data, (const uint8_t *)FLASH_ROM_ADDR, ROM_SIZE);
    else
        memset(rom_data, 0xFF, ROM_SIZE);

    build_luts();
    build_addr_out_lut();
    setup_gpio();

    multicore_launch_core1(rom_emulator_core1);

    gpio_put(PIN_LED, 1);

    while (true)
    {
        handle_usb();
    }
}
