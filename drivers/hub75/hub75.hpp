#include <stdint.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#ifndef NO_PICO_GRAPHICS
#include "libraries/pico_graphics/pico_graphics.hpp"
#endif

#ifndef NO_QSTR
#include "hub75.pio.h"
#endif

#ifndef HUB75_R0
#define HUB75_R0 0
#endif
#ifndef HUB75_G0
#define HUB75_G0 1
#endif
#ifndef HUB75_B0
#define HUB75_B0 2
#endif
#ifndef HUB75_R1
#define HUB75_R1 3
#endif
#ifndef HUB75_G1
#define HUB75_G1 4
#endif
#ifndef HUB75_B1
#define HUB75_B1 5
#endif
#ifndef HUB75_A
#define HUB75_A 6
#endif
#ifndef HUB75_B
#define HUB75_B 7
#endif
#ifndef HUB75_C
#define HUB75_C 8
#endif
#ifndef HUB75_D
#define HUB75_D 9
#endif
#ifndef HUB75_E
#define HUB75_E 10
#endif
#ifndef HUB75_CLK
#define HUB75_CLK 11
#endif
#ifndef HUB75_LAT
#define HUB75_LAT 12
#endif
#ifndef HUB75_OE
#define HUB75_OE 13
#endif
#ifndef HUB75_CLK2
#define HUB75_CLK2 HUB75_CLK
#endif
#ifndef HUB75_LAT2
#define HUB75_LAT2 HUB75_LAT
#endif
#ifndef HUB75_OE2
#define HUB75_OE2 HUB75_OE
#endif

// OE blanking cycles subtracted from each bit-plane pulse.
// Increase to hide latch transitions on panels that ghost around LAT/OE.
#ifndef HUB75_LATCH_BLANKING
#define HUB75_LATCH_BLANKING 0
#endif

namespace ZeDMD {
const uint DATA_BASE_PIN = HUB75_R0;
const uint DATA_N_PINS = 6;
const uint ROWSEL_BASE_PIN = HUB75_A;
const uint ROWSEL_N_PINS = 5;
const uint BIT_DEPTH = 10;

/*
10-bit gamma table, allowing us to gamma correct our 8-bit colour values up
to 10-bit without losing dynamic range.

Calculated with the following Python code:

gamma_lut = [int(round(1024 * (x / (256 - 1)) ** 2.2)) for x in range(256)]

Post-processed to enforce a minimum difference of 1 between adjacent values,
and no leading zeros:

for i in range(1, len(gamma_lut)):
	if gamma_lut[i] <= gamma_lut[i - 1]:
		gamma_lut[i] = gamma_lut[i - 1] + 1
*/
static inline uint16_t GAMMA_10BIT[256] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    80, 82, 84, 87, 89, 91, 94, 96, 98, 101, 103, 106, 109, 111, 114, 117,
    119, 122, 125, 128, 130, 133, 136, 139, 142, 145, 148, 151, 155, 158, 161, 164,
    167, 171, 174, 177, 181, 184, 188, 191, 195, 198, 202, 206, 209, 213, 217, 221,
    225, 228, 232, 236, 240, 244, 248, 252, 257, 261, 265, 269, 274, 278, 282, 287,
    291, 295, 300, 304, 309, 314, 318, 323, 328, 333, 337, 342, 347, 352, 357, 362,
    367, 372, 377, 382, 387, 393, 398, 403, 408, 414, 419, 425, 430, 436, 441, 447,
    452, 458, 464, 470, 475, 481, 487, 493, 499, 505, 511, 517, 523, 529, 535, 542,
    548, 554, 561, 567, 573, 580, 586, 593, 599, 606, 613, 619, 626, 633, 640, 647,
    653, 660, 667, 674, 681, 689, 696, 703, 710, 717, 725, 732, 739, 747, 754, 762,
    769, 777, 784, 792, 800, 807, 815, 823, 831, 839, 847, 855, 863, 871, 879, 887,
    895, 903, 912, 920, 928, 937, 945, 954, 962, 971, 979, 988, 997, 1005, 1014, 1023
};


struct Pixel {
    uint32_t color;
    constexpr Pixel() : color(0) {};
    constexpr Pixel(uint32_t color) : color(color) {};
};

enum ShiftDriver {
    SHIFT_DRIVER_SHIFTREG = 0,
    SHIFT_DRIVER_FM6124,
    SHIFT_DRIVER_FM6126A,
    SHIFT_DRIVER_ICN2038S,
    SHIFT_DRIVER_MBI5124,
    SHIFT_DRIVER_DP3246,
    SHIFT_DRIVER_RUL6024,
    SHIFT_DRIVER_ICND2153,
};

enum LineDecoder {
    LINE_DECODER_TYPE138 = 0,
    LINE_DECODER_TYPE595,
    LINE_DECODER_TYPE_DIRECT,
    LINE_DECODER_SM5266P,
    LINE_DECODER_SM5368P,
    LINE_DECODER_TC7559E,
};

Pixel hsv_to_rgb(float h, float s, float v);

class Hub75 {
    public:
    enum class COLOR_ORDER {
        RGB,
        RBG,
        GRB,
        GBR,
        BRG,
        BGR
    };
    uint width;
    uint height;
    uint r_shift = 0;
    uint g_shift = 10;
    uint b_shift = 20;
    Pixel *back_buffer1;
    Pixel *back_buffer2;
    Pixel *render_back_buffer = nullptr;
    Pixel *draw_back_buffer = nullptr;
    bool managed_buffer = false;
    ShiftDriver shift_driver;
    LineDecoder line_decoder;
    bool inverted_stb = false;
    COLOR_ORDER color_order;
    uint16_t *lut_table;
    Pixel background = 0;

    // DMA & PIO
    int dma_channel = -1;
    int dma_channel_b = -1;
    uint bit = 0;
    uint row = 0;

    PIO pio;
    uint sm_data = 0;
    uint sm_row = 1;
    uint sm_data_b = 2;
    uint sm_row_b = 3;

    uint data_prog_offs = 0;
    uint row_prog_offs = 0;
    bool shiftreg_row_preloaded = false;
    bool split_phase_b_active = false;
    volatile bool software_icnd2153_active = false;

    uint brightness = 6;


    // Top half of display - 16 rows on a 32x32 panel
    unsigned int pin_r0 = HUB75_R0;
    unsigned int pin_g0 = HUB75_G0;
    unsigned int pin_b0 = HUB75_B0;

    // Bottom half of display - 16 rows on a 64x64 panel
    unsigned int pin_r1 = HUB75_R1;
    unsigned int pin_g1 = HUB75_G1;
    unsigned int pin_b1 = HUB75_B1;

    // Address pins, 5 lines = 2^5 = 32 values (max 64x64 display)
    unsigned int pin_row_a = HUB75_A;
    unsigned int pin_row_b = HUB75_B;
    unsigned int pin_row_c = HUB75_C;
    unsigned int pin_row_d = HUB75_D;
    unsigned int pin_row_e = HUB75_E;

    // Sundry things
    unsigned int pin_clk = HUB75_CLK;    // Clock
    unsigned int pin_stb = HUB75_LAT;    // Strobe/Latch
    unsigned int pin_oe = HUB75_OE;      // Output Enable
    unsigned int pin_clk2 = HUB75_CLK2;  // Clock for panel 2
    unsigned int pin_stb2 = HUB75_LAT2;  // Strobe/Latch for panel 2
    unsigned int pin_oe2 = HUB75_OE2;    // Output Enable for panel 2
    bool split_controls = false;

    const bool clk_polarity = 1;
    const bool stb_polarity = 1;
    const bool oe_polarity = 0;

    // User buttons and status LED
    //unsigned int pin_sw_a = 14;
    //unsigned int pin_sw_user = 23;

    //unsigned int pin_led_r = 16;
    //unsigned int pin_led_g = 17;
    //unsigned int pin_led_b = 18;

    Hub75(uint width, uint height) : Hub75(width, height, nullptr) {};
    Hub75(uint width, uint height, Pixel *buffer)
      : Hub75(width, height, buffer, SHIFT_DRIVER_SHIFTREG, LINE_DECODER_TYPE138, false) {};
    Hub75(uint width, uint height, Pixel *buffer, ShiftDriver shift_driver, LineDecoder line_decoder, bool inverted_stb,
      COLOR_ORDER color_order=COLOR_ORDER::RGB, uint16_t *lut_table = GAMMA_10BIT)
      : Hub75(width, height, buffer, inverted_stb, color_order, lut_table, shift_driver, line_decoder) {};
    Hub75(uint width, uint height, Pixel *buffer, bool inverted_stb,
      COLOR_ORDER color_order=COLOR_ORDER::RGB, uint16_t *lut_table = GAMMA_10BIT,
      ShiftDriver shift_driver = SHIFT_DRIVER_SHIFTREG, LineDecoder line_decoder = LINE_DECODER_TYPE138);
    ~Hub75();

    void FM6126A_write_register(uint16_t value, uint8_t position);
    void FM6126A_setup();
    void DP3246_setup();
    void RUL6024_setup();
    void ICND2153_setup();
    void set_color(uint x, uint y, Pixel c);
    void render();
    void set_pixel(uint x, uint y, uint8_t r, uint8_t g, uint8_t b);
    void copy_to_back_buffer(void *data, size_t len, int start_x, int start_y, int g_width, int g_height);
    void clear();
    void start(irq_handler_t handler);
    void stop(irq_handler_t handler);
    void dma_complete();
    uint32_t encode_row_payload(uint row, uint bit) const;
    int buffer_offset(uint x, uint y) const;
    uint panel_width() const;
    Pixel *row_buffer_ptr(uint row, uint phase) const;
    // Number of dummy pixels needed after each row before latching the next row.
    // Some shift-driver / line-decoder combinations need more trailing clocks.
    uint end_of_row_dummy_pixels() const;
    // SM5266P/SM5368PF panels use a GPIO-stepped serial row decoder.
    void init_shiftreg_rows();
    void step_shiftreg_row(uint row) const;
    // TC7559E-style row selectors use A=row clock and C=row data.
    void init_tc7559e_rows();
    void step_tc7559e_row(uint row) const;
    bool uses_icnd2153_software_scan() const;
    void icnd2153_run_loop();
    void icnd2153_refresh_row(uint row);
    void icnd2153_set_phase_data(uint row, uint phase, uint x, uint bit_index) const;
    void icnd2153_shift_row_phase(uint row, uint phase, uint bit_index, bool final_plane);
    void icnd2153_pulse_data(uint clock_pin, uint oe_pin) const;
#ifndef NO_PICO_GRAPHICS
    void update(PicoGraphics *graphics);
#endif
    };
}
