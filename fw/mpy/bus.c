#include "bus.h"

/* ========================================================================= */
/*  INTERNAL FUNCTIONS                                                       */
/* ========================================================================= */

bool bus_attached;
uint32_t bus_delay_loops;

// BUS_DELAY: delay for a number of "loops" to allow RPI/FPGA to ready for next stage
// of sending/recieving data from the bus.
static void bus_delay(void) {
    uint32_t loops = bus_get_delay();
    uint32_t i;

    // The nop is inside asm volatile so -Os cannot delete the loop.
    // Each nop is 1 clock cycle
    for (i = 0; i < loops; i++) {
        __asm__ volatile ("nop");
    }
}

// BUS_ATTACH: setup required pins for RPI-FPGA communication
void bus_attach(void) {
    uint32_t pin;

     // 1. Obtain pins 0-15, 21, and 22, and then assign them to registers.
    for (pin = 0; pin < 32u; pin++) {
        if (pin >= BUS_DATA_PINS && pin != BUS_CLK_PIN && pin != BUS_WR_PIN) {
            continue;
        }

        reg_write(IO_BANK0_CTRL(pin), IO_FUNCSEL_SIO);
        reg_write(PADS_BANK0(pin), PAD_IE | PAD_DRIVE_8MA | PAD_SCHMITT);
    }

    // 2. Park the bus, everything is set low and ready for the beats
    reg_write(SIO_GPIO_OUT_CLR, BUS_ALL_MASK);
    reg_write(SIO_GPIO_OE_SET, BUS_CLK_MASK | BUS_WR_MASK);
    reg_write(SIO_GPIO_OE_CLR, BUS_DATA_MASK);

    // 3. Flag bus is ready for transport
    bus_attached = true;
}

// BUS_DETACH: If User wants to disconnect the bus, use this function.
void bus_detach(void) {
    reg_write(SIO_GPIO_OE_CLR, BUS_DATA_MASK);
    reg_write(SIO_GPIO_OUT_CLR, BUS_CLK_MASK | BUS_WR_MASK);

    bus_attached = false;
}

// BUS_SET_DELAY: user can call this to set there own delay based on the FPGA frequency
bool bus_set_delay(uint32_t loops) {
    if (loops < BUS_DELAY_MIN || loops > BUS_DELAY_MAX) {
        return false;
    }

    bus_delay_loops = loops;

    // Note: each loop is roughly 47 ns
    // FPGA requires roughly 60 ns per delay.
    // default is set to 12 loops == 560 ns
    return true;
}

// BUS_GET_DELAY: Used by the BUS_DELAY() function. User can call to check for debugging.
uint32_t bus_get_delay(void) {
    // If User does not specify bus delay, then use default 12 loops
    if (bus_delay_loops == 0u) {
        return BUS_DELAY_DEFAULT;
    }

    return bus_delay_loops;
}

/* ========================================================================= */
/*  Write                                                                    */
/* ========================================================================= */

void bus_write(uint32_t address, uint32_t value) {
    uint16_t beat[BUS_BEATS];
    uint32_t i;

    // 1. Attach/setup bus pins if needed.
    if (!bus_attached) {
        bus_attach();
    }

    // 2. Process inputs into 4 16'bit segments, 1 for each beat.
    beat[0] = (uint16_t)(address);
    beat[1] = (uint16_t)(address >> 16);
    beat[2] = (uint16_t)(value);
    beat[3] = (uint16_t)(value >> 16);

        // set wr = 1
    reg_write(SIO_GPIO_OUT_SET, BUS_WR_MASK);
    bus_delay();

        // set mask to high to reveal pin values
    reg_write(SIO_GPIO_OE_SET, BUS_DATA_MASK);


    // 3. For each beat: apply data to beat, raise clock pin, lower clock pin, repeat. 
    for (i = 0; i < BUS_BEATS; i++) {

        // clear the bus, then set next beat values
        reg_write(SIO_GPIO_OUT_CLR, BUS_DATA_MASK & ~(uint32_t)beat[i]);
        reg_write(SIO_GPIO_OUT_SET, (uint32_t)beat[i]);
        bus_delay();

        // clk: low -> high
        reg_write(SIO_GPIO_OUT_SET, BUS_CLK_MASK);
        bus_delay();

        // clk: high -> low
        reg_write(SIO_GPIO_OUT_CLR, BUS_CLK_MASK);
        bus_delay();
    }

    // 4. Send pin values low and wait for next function call/transfer.
    // Note: the FPGA only reads wr on edges, which is why we don't touch the clock here.
    reg_write(SIO_GPIO_OE_CLR, BUS_DATA_MASK);
    reg_write(SIO_GPIO_OUT_CLR, BUS_WR_MASK);
    bus_delay();
}

/* ========================================================================= */
/*  Read                                                                     */
/* ========================================================================= */

uint32_t bus_read(uint32_t address) {
    uint16_t beat[BUS_BEATS];
    uint32_t i;

    // 1. Attach/setup bus pins if needed.
    if (!bus_attached) {
        bus_attach();
    }

    // 2. Process inputs into 2 16'bit segments, then set beats 2 and 3 to 0 as
    // default values. These will be returned.
    beat[0] = (uint16_t)(address);
    beat[1] = (uint16_t)(address >> 16);
    beat[2] = 0;
    beat[3] = 0;

        // set wr = 0, for read operation
    reg_write(SIO_GPIO_OUT_CLR, BUS_WR_MASK);
    bus_delay();

        // start with control of bus, as we send first 32'bits.
    reg_write(SIO_GPIO_OE_SET, BUS_DATA_MASK);


    // 3. For first two beats, send address like write operation.
    // Then for second two beats recieve from the FPGA.
    for (i = 0; i < BUS_BEATS; i++) {

        if (i < 2u) {
            // clear the bus of prior data, then set the beat value
            reg_write(SIO_GPIO_OUT_CLR, BUS_DATA_MASK & ~(uint32_t)beat[i]);
            reg_write(SIO_GPIO_OUT_SET, (uint32_t)beat[i]);
            bus_delay();

        } else if (i == 2u) {
            // stop driving the bus pins
            reg_write(SIO_GPIO_OE_CLR, BUS_DATA_MASK);
            bus_delay();
        }

        // clk: low -> high.
        reg_write(SIO_GPIO_OUT_SET, BUS_CLK_MASK);
        bus_delay();

        if (i >= 2u) {
            // store the current value on the bus
            beat[i] = (uint16_t)(reg_read(SIO_GPIO_IN) & BUS_DATA_MASK);
        }

        // clk: high -> low.
        reg_write(SIO_GPIO_OUT_CLR, BUS_CLK_MASK);
        bus_delay();
    }

    // 4. Send data lines low
    reg_write(SIO_GPIO_OE_CLR, BUS_DATA_MASK);

    // 5. Return 32'bit value recieved from FPGA
    return ((uint32_t)beat[3] << 16) | (uint32_t)beat[2];
}
