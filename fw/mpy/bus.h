#ifndef EPM11_BUS_H
#define EPM11_BUS_H

#include <stdbool.h>
#include <stdint.h>

/* ========================================================================= */
/*  The pins                                                                 */
/* ========================================================================= */

// Data lines are GPIO 0-15 and contiguous, so a whole beat is one masked write.
#define BUS_DATA_PINS       (16u)
#define BUS_DATA_MASK       (0x0000FFFFu)

// NOTE: clk is 22 and wr is 21, the opposite of what the numbering suggests.
// Measured on hardware. Swapping them fails silently, no transfer completes.
#define BUS_CLK_PIN         (22u)
#define BUS_WR_PIN          (21u)

#define BUS_CLK_MASK        (1u << BUS_CLK_PIN)
#define BUS_WR_MASK         (1u << BUS_WR_PIN)

// Every pin this module touches, and nothing else is ever touched.
#define BUS_ALL_MASK        (BUS_DATA_MASK | BUS_CLK_MASK | BUS_WR_MASK)

// Four beats per transfer: addr low, addr high, value low, value high.
#define BUS_BEATS           (4u)

/* ========================================================================= */
/*  How long to hold each level                                              */
/* ========================================================================= */

// Loops held after every pin change. One loop is roughly 47 ns, the FPGA
// needs roughly 60 ns. Too long is always safe, too short drops beats.
#define BUS_DELAY_DEFAULT   (12u)
#define BUS_DELAY_MIN       (1u)
#define BUS_DELAY_MAX       (100000u)

/* ========================================================================= */
/*  RP2350 registers                                                         */
/* ========================================================================= */

// NOTE: hand-typed from the datasheet, not yet checked against the pico-sdk.
// A wrong address here passes every host test and only fails on real silicon.
// NOTE: the RP2350 SIO layout is not the RP2040 one, offsets differ.

#define SIO_BASE            (0xd0000000u)

#define SIO_GPIO_IN         (SIO_BASE + 0x004u) // the level on each pin
#define SIO_GPIO_OUT        (SIO_BASE + 0x010u) // the level we drive
#define SIO_GPIO_OUT_SET    (SIO_BASE + 0x018u) // drive these pins high
#define SIO_GPIO_OUT_CLR    (SIO_BASE + 0x020u) // drive these pins low
#define SIO_GPIO_OE         (SIO_BASE + 0x030u) // which pins we drive at all
#define SIO_GPIO_OE_SET     (SIO_BASE + 0x038u) // start driving these pins
#define SIO_GPIO_OE_CLR     (SIO_BASE + 0x040u) // stop driving these pins

#define IO_BANK0_BASE       (0x40028000u)

// One control register per pin: GPIO0 at +0x004, then every 8 bytes.
#define IO_BANK0_CTRL(pin) (IO_BANK0_BASE + 0x004u + 0x008u * (uint32_t)(pin))

// Function 5 is SIO on every GPIO, so GPIO_OUT / OE / IN control the pin.
#define IO_FUNCSEL_SIO      (5u)

#define PADS_BANK0_BASE     (0x40038000u)

// One pad register per pin: GPIO0 at +0x004, then every 4 bytes.
#define PADS_BANK0(pin)     (PADS_BANK0_BASE + 0x004u + 0x004u * (uint32_t)(pin))

#define PAD_ISO             (1u << 8) // pad disconnected, SET out of reset
#define PAD_IE              (1u << 6) // input enable
#define PAD_DRIVE_8MA       (2u << 4) // drive strength, bits 5..4
#define PAD_SCHMITT         (1u << 1) // schmitt trigger on the input



#ifdef BUS_HOST_TEST
    // NOTE: REG_READ / REG_WRITE: the only way this module touches hardware.
    // Functions not macros, so the host test can swap in its own GPIO model.
    // On the board they compile to a single load or store.

    uint32_t bus_test_reg_read(uint32_t address);
    void bus_test_reg_write(uint32_t address, uint32_t value);

    static inline uint32_t reg_read(uint32_t address) {
        return bus_test_reg_read(address);
    }

    static inline void reg_write(uint32_t address, uint32_t value) {
        bus_test_reg_write(address, value);
    }

    #else

    static inline uint32_t reg_read(uint32_t address) {
        return *(volatile uint32_t *)(uintptr_t)address;
    }

    static inline void reg_write(uint32_t address, uint32_t value) {
        *(volatile uint32_t *)(uintptr_t)address = value;
    }

#endif

/* ========================================================================= */
/*  Functions                                                                */
/* ========================================================================= */

// True once the pins are claimed. Read it, do not write it.
// A plain global because mpy_ld.py rejects static bss, so a getter guards nothing.
extern bool bus_attached;

// BUS_ATTACH: claim the 18 pins and park the bus low. Called automatically on
// first use, so only call it directly if you want to pick the moment.
void bus_attach(void);

// BUS_DETACH: let go of the data lines and leave clk and wr low.
void bus_detach(void);

// BUS_SET_DELAY: set the hold time in loops. Returns false, and changes
// nothing, if the value is outside BUS_DELAY_MIN..BUS_DELAY_MAX.
bool bus_set_delay(uint32_t loops);

// BUS_GET_DELAY: the hold time in use, or the default if none was set.
uint32_t bus_get_delay(void);

// WRITE: send a 32-bit value to a 32-bit address. Four beats, all ours.
void bus_write(uint32_t address, uint32_t value);

// READ: fetch the 32-bit value at a 32-bit address. Two beats out, two back.
// NOTE: an address the FPGA does not decode returns 0xDEADBEEF, which is its
// own miss marker, not an error from this code.
uint32_t bus_read(uint32_t address);


#endif /* EPM11_BUS_H */
