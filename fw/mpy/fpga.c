// This is the micropython wrapper for the bus.h header file
#include "py/dynruntime.h"

#include "bus.h"


/* ========================================================================= */
/*  INTERNAL                                                                 */
/* ========================================================================= */

// fpga.FPGAError, raised when the bus cannot do what was asked.
// Not static: mpy_ld.py rejects static bss, same rule as the globals in bus.c.
mp_obj_full_type_t fpga_error_type;

// ARG_TO_U32: turn a python int into a 32-bit bus word.
// A negative number is a mistake, so report it as a ValueError.
// A number too big for a small int is taken modulo 2**32, the usual
// convention for register work, so 0xFFFFFFFF needs no sign juggling.
// Note: anything that is not an int raises TypeError from micropython below.
static uint32_t arg_to_u32(mp_obj_t obj) {
    if (mp_obj_is_small_int(obj) && mp_obj_get_int(obj) < 0) {
        mp_raise_ValueError("address and value must not be negative");
    }

    return (uint32_t)mp_obj_get_int_truncated(obj);
}

/* ========================================================================= */
/*  PYTHON FUNCTIONS                                                         */
/* ========================================================================= */

// WRITE: fpga.write(address, value) -> None
static mp_obj_t fpga_write(mp_obj_t address_in, mp_obj_t value_in) {
    uint32_t address = arg_to_u32(address_in);
    uint32_t value = arg_to_u32(value_in);

    bus_write(address, value);

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(fpga_write_obj, fpga_write);

// READ: fpga.read(address) -> int
static mp_obj_t fpga_read(mp_obj_t address_in) {
    uint32_t address = arg_to_u32(address_in);

    return mp_obj_new_int_from_uint(bus_read(address));
}
static MP_DEFINE_CONST_FUN_OBJ_1(fpga_read_obj, fpga_read);

// INIT: fpga.init() -> None
// Note: the pins are claimed on first use anyway, this only picks the moment.
static mp_obj_t fpga_init(void) {
    bus_attach();

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fpga_init_obj, fpga_init);

// DEINIT: fpga.deinit() -> None
static mp_obj_t fpga_deinit(void) {
    bus_detach();

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(fpga_deinit_obj, fpga_deinit);

// SET_PULSE_DELAY: fpga.set_pulse_delay(loops) -> None
// Raise it if beats are being missed. Lower it only with a scope or the
// FPGA logic analyser watching the bus.
static mp_obj_t fpga_set_pulse_delay(mp_obj_t loops_in) {
    uint32_t loops = arg_to_u32(loops_in);

    if (!bus_set_delay(loops)) {
        mp_raise_msg((mp_obj_type_t *)&fpga_error_type, "pulse delay out of range");
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(fpga_set_pulse_delay_obj, fpga_set_pulse_delay);

// GET_PULSE_DELAY: fpga.get_pulse_delay() -> int
static mp_obj_t fpga_get_pulse_delay(void) {
    return mp_obj_new_int_from_uint(bus_get_delay());
}
static MP_DEFINE_CONST_FUN_OBJ_0(fpga_get_pulse_delay_obj, fpga_get_pulse_delay);



/* ========================================================================= */
/*  MODULE TABLE                                                             */
/* ========================================================================= */

// MPY_INIT: runs on "import fpga". Everything the module exports is listed
// here, and nothing not listed here is reachable from python.
mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY

    mp_obj_exception_init(&fpga_error_type, MP_QSTR_FPGAError, &mp_type_Exception);
    mp_store_global(MP_QSTR_FPGAError, MP_OBJ_FROM_PTR(&fpga_error_type));

    mp_store_global(MP_QSTR_write, MP_OBJ_FROM_PTR(&fpga_write_obj));
    mp_store_global(MP_QSTR_read, MP_OBJ_FROM_PTR(&fpga_read_obj));

    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&fpga_init_obj));
    mp_store_global(MP_QSTR_deinit, MP_OBJ_FROM_PTR(&fpga_deinit_obj));

    mp_store_global(MP_QSTR_set_pulse_delay, MP_OBJ_FROM_PTR(&fpga_set_pulse_delay_obj));
    mp_store_global(MP_QSTR_get_pulse_delay, MP_OBJ_FROM_PTR(&fpga_get_pulse_delay_obj));

    MP_DYNRUNTIME_INIT_EXIT
}
