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
/*  Beats                                                                    */
/* ========================================================================= */

// BUS_CRC16: CRC-16/CCITT over one 16-bit word, the same as cpu_bus.sv.
static uint16_t bus_crc16(uint16_t crc, uint16_t word) {
    uint32_t i;

    crc ^= word;

    for (i = 0; i < 16u; i++) {
        if (crc & 0x8000u) {
            crc = (uint16_t)((crc << 1) ^ 0x1021u);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }

    return crc;
}

// BUS_FRAME: raise or lower wr, which frames a whole transaction.
static void bus_frame(bool high) {
    if (high) {
        reg_write(SIO_GPIO_OUT_SET, BUS_WR_MASK);
    } else {
        reg_write(SIO_GPIO_OUT_CLR, BUS_WR_MASK);
    }

    bus_delay();
}

// BUS_SEND: put a word on the bus and pulse the clock. For the last word the
// lines are released while clk is still high, so the FPGA can start answering
// on the falling edge without both sides ever driving at once.
static void bus_send(uint16_t word, bool last) {
    reg_write(SIO_GPIO_OUT_CLR, BUS_DATA_MASK & ~(uint32_t)word);
    reg_write(SIO_GPIO_OUT_SET, (uint32_t)word);
    reg_write(SIO_GPIO_OE_SET, BUS_DATA_MASK);
    bus_delay();

    // clk: low -> high, the FPGA takes the word
    reg_write(SIO_GPIO_OUT_SET, BUS_CLK_MASK);
    bus_delay();

    if (last) {
        reg_write(SIO_GPIO_OE_CLR, BUS_DATA_MASK);
    }

    // clk: high -> low
    reg_write(SIO_GPIO_OUT_CLR, BUS_CLK_MASK);
    bus_delay();
}

// BUS_RECEIVE: pulse the clock and take the word the FPGA is driving.
static uint16_t bus_receive(void) {
    uint16_t word;

    reg_write(SIO_GPIO_OUT_SET, BUS_CLK_MASK);
    bus_delay();

    word = (uint16_t)(reg_read(SIO_GPIO_IN) & BUS_DATA_MASK);

    reg_write(SIO_GPIO_OUT_CLR, BUS_CLK_MASK);
    bus_delay();

    return word;
}

// BUS_WAIT_READY: keep pulsing until the FPGA answers READY.
static bus_result_t bus_wait_ready(void) {
    uint32_t i;
    uint16_t status;

    for (i = 0; i < BUS_STATUS_POLLS; i++) {
        status = bus_receive();

        if (status == BUS_STATUS_READY) {
            return BUS_OK;
        }
        if (status == BUS_STATUS_ERROR) {
            return BUS_ERR_REFUSED;
        }
        if (status != BUS_STATUS_BUSY) {
            return BUS_ERR_NO_RESPONSE;
        }
    }

    return BUS_ERR_TIMEOUT;
}

/* ========================================================================= */
/*  Write                                                                    */
/* ========================================================================= */

bus_result_t bus_write(uint32_t address, uint32_t value) {
    uint16_t word[5];
    uint16_t crc = BUS_CRC_INIT;
    bus_result_t result;
    uint32_t i;

    // 1. Attach/setup bus pins if needed.
    if (!bus_attached) {
        bus_attach();
    }

    // 2. The request: command, address, value.
    word[0] = BUS_CMD_WRITE;
    word[1] = (uint16_t)(address);
    word[2] = (uint16_t)(address >> 16);
    word[3] = (uint16_t)(value);
    word[4] = (uint16_t)(value >> 16);

    // 3. Open the frame, send the request, then its CHECK.
    bus_frame(true);

    for (i = 0; i < 5u; i++) {
        bus_send(word[i], false);
        crc = bus_crc16(crc, word[i]);
    }
    bus_send(crc, true);

    // 4. Wait for the FPGA to confirm downstream took the value.
    result = bus_wait_ready();

    // 5. Close the frame. Whatever happened, the next frame starts clean.
    bus_frame(false);

    return result;
}

/* ========================================================================= */
/*  Read                                                                     */
/* ========================================================================= */

bus_result_t bus_read(uint32_t address, uint32_t *value) {
    uint16_t word[3];
    uint16_t crc = BUS_CRC_INIT;
    uint16_t low;
    uint16_t high;
    uint16_t check;
    bus_result_t result;
    uint32_t i;

    // 1. Attach/setup bus pins if needed.
    if (!bus_attached) {
        bus_attach();
    }

    // 2. The request: command, address.
    word[0] = BUS_CMD_READ;
    word[1] = (uint16_t)(address);
    word[2] = (uint16_t)(address >> 16);

    // 3. Open the frame, send the request, then its CHECK.
    bus_frame(true);

    for (i = 0; i < 3u; i++) {
        bus_send(word[i], false);
        crc = bus_crc16(crc, word[i]);
    }
    bus_send(crc, true);

    // 4. Wait for READY, then take the value and its CHECK.
    result = bus_wait_ready();

    if (result == BUS_OK) {
        low   = bus_receive();
        high  = bus_receive();
        check = bus_receive();

        if (check != bus_crc16(bus_crc16(BUS_CRC_INIT, low), high)) {
            result = BUS_ERR_CHECK;
        } else {
            *value = ((uint32_t)high << 16) | (uint32_t)low;
        }
    }

    // 5. Close the frame. Whatever happened, the next frame starts clean.
    bus_frame(false);

    return result;
}
