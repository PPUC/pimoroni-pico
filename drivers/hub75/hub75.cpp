#include <cstring>
#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "hardware/clocks.h"

#include "hub75.hpp"

namespace pimoroni {

namespace {

ShiftDriver panel_type_to_shift_driver(PanelType panel_type) {
    switch (panel_type) {
        case PANEL_FM6126A:
            return SHIFT_DRIVER_FM6126A;
        case PANEL_GENERIC:
        default:
            return SHIFT_DRIVER_SHIFTREG;
    }
}

void set_all_data_pins(const Hub75 &hub75, bool value) {
    gpio_put(hub75.pin_r0, value);
    gpio_put(hub75.pin_g0, value);
    gpio_put(hub75.pin_b0, value);
    gpio_put(hub75.pin_r1, value);
    gpio_put(hub75.pin_g1, value);
    gpio_put(hub75.pin_b1, value);
}

void pulse_panel_clock_pin(uint clock_pin, bool clock_polarity) {
    gpio_put(clock_pin, clock_polarity);
    gpio_put(clock_pin, !clock_polarity);
}

uint32_t system_clock_hz() {
    return clock_get_hz(clk_sys);
}

uint32_t latch_cycles_for_system_clock() {
    return std::max<uint32_t>(1u, system_clock_hz() / 4000000u);
}

uint32_t shiftreg_delay_cycles() {
    return std::max<uint32_t>(8u, (uint32_t)(((uint64_t)system_clock_hz() * 8u + 124999999u) / 125000000u));
}

float panel_data_clkdiv(uint panel_width) {
    float base_div = panel_width <= 32 ? 2.0f : 1.0f;
    float clock_scale = (float)system_clock_hz() / 125000000.0f;
    return std::max(base_div, base_div * clock_scale);
}

uint range_min(std::initializer_list<uint> pins) {
    return *std::min_element(pins.begin(), pins.end());
}

uint range_max(std::initializer_list<uint> pins) {
    return *std::max_element(pins.begin(), pins.end());
}

// Small setup/hold delay for GPIO-driven serial row decoder updates.
inline void shiftreg_timing_delay() {
    busy_wait_at_least_cycles(shiftreg_delay_cycles());
}

bool uses_dp3246_type595(const Hub75 &hub75) {
    return hub75.shift_driver == SHIFT_DRIVER_DP3246 && hub75.line_decoder == LINE_DECODER_TYPE595;
}

} // namespace

Hub75::Hub75(uint width, uint height, Pixel *buffer, PanelType panel_type, bool inverted_stb, COLOR_ORDER color_order,
  uint16_t *lut_table, ShiftDriver shift_driver, LineDecoder line_decoder)
 : width(width), height(height), inverted_stb(inverted_stb), color_order(color_order),
  lut_table(lut_table), pio(nullptr)
 {
    this->shift_driver = (shift_driver == SHIFT_DRIVER_SHIFTREG) ? panel_type_to_shift_driver(panel_type) : shift_driver;
    this->line_decoder = line_decoder;
    split_controls = (pin_clk2 != pin_clk) || (pin_stb2 != pin_stb) || (pin_oe2 != pin_oe);

    // Set up allllll the GPIO
    gpio_init(pin_r0); gpio_set_function(pin_r0, GPIO_FUNC_SIO); gpio_set_dir(pin_r0, true); gpio_put(pin_r0, 0);
    gpio_init(pin_g0); gpio_set_function(pin_g0, GPIO_FUNC_SIO); gpio_set_dir(pin_g0, true); gpio_put(pin_g0, 0);
    gpio_init(pin_b0); gpio_set_function(pin_b0, GPIO_FUNC_SIO); gpio_set_dir(pin_b0, true); gpio_put(pin_b0, 0);

    gpio_init(pin_r1); gpio_set_function(pin_r1, GPIO_FUNC_SIO); gpio_set_dir(pin_r1, true); gpio_put(pin_r1, 0);
    gpio_init(pin_g1); gpio_set_function(pin_g1, GPIO_FUNC_SIO); gpio_set_dir(pin_g1, true); gpio_put(pin_g1, 0);
    gpio_init(pin_b1); gpio_set_function(pin_b1, GPIO_FUNC_SIO); gpio_set_dir(pin_b1, true); gpio_put(pin_b1, 0);

    gpio_init(pin_row_a); gpio_set_function(pin_row_a, GPIO_FUNC_SIO); gpio_set_dir(pin_row_a, true); gpio_put(pin_row_a, 0);
    gpio_init(pin_row_b); gpio_set_function(pin_row_b, GPIO_FUNC_SIO); gpio_set_dir(pin_row_b, true); gpio_put(pin_row_b, 0);
    gpio_init(pin_row_c); gpio_set_function(pin_row_c, GPIO_FUNC_SIO); gpio_set_dir(pin_row_c, true); gpio_put(pin_row_c, 0);
    gpio_init(pin_row_d); gpio_set_function(pin_row_d, GPIO_FUNC_SIO); gpio_set_dir(pin_row_d, true); gpio_put(pin_row_d, 0);
    gpio_init(pin_row_e); gpio_set_function(pin_row_e, GPIO_FUNC_SIO); gpio_set_dir(pin_row_e, true); gpio_put(pin_row_e, 0);

    gpio_init(pin_clk); gpio_set_function(pin_clk, GPIO_FUNC_SIO); gpio_set_dir(pin_clk, true); gpio_put(pin_clk, !clk_polarity);
    gpio_init(pin_stb); gpio_set_function(pin_stb, GPIO_FUNC_SIO); gpio_set_dir(pin_stb, true); gpio_put(pin_stb, !stb_polarity);
    gpio_init(pin_oe); gpio_set_function(pin_oe, GPIO_FUNC_SIO); gpio_set_dir(pin_oe, true); gpio_put(pin_oe, !oe_polarity);
    if (split_controls) {
        gpio_init(pin_clk2); gpio_set_function(pin_clk2, GPIO_FUNC_SIO); gpio_set_dir(pin_clk2, true); gpio_put(pin_clk2, !clk_polarity);
        gpio_init(pin_stb2); gpio_set_function(pin_stb2, GPIO_FUNC_SIO); gpio_set_dir(pin_stb2, true); gpio_put(pin_stb2, !stb_polarity);
        gpio_init(pin_oe2); gpio_set_function(pin_oe2, GPIO_FUNC_SIO); gpio_set_dir(pin_oe2, true); gpio_put(pin_oe2, !oe_polarity);
    }

    if (buffer == nullptr) {
        back_buffer1 = new Pixel[width * height];
        back_buffer2 = new Pixel[width * height];
        render_back_buffer = back_buffer1;
        draw_back_buffer = back_buffer2;
        managed_buffer = true;
    } else {
        back_buffer1 = buffer;
        back_buffer2 = nullptr;
        render_back_buffer = back_buffer1;
        draw_back_buffer = back_buffer1;
        managed_buffer = false;
    }

    if (brightness == 0) {
#if PICO_RP2350
        brightness = 6;
#else
        if (width >= 64) brightness = 6;
        if (width >= 96) brightness = 3;
        if (width >= 128) brightness = 2;
        if (width >= 160) brightness = 1;
#endif
    }

    switch (color_order) {
        case COLOR_ORDER::RGB:
            r_shift = 0;
            g_shift = 10;
            b_shift = 20;
            break;
        case COLOR_ORDER::RBG:
            r_shift = 0;
            g_shift = 20;
            b_shift = 10;
            break;
        case COLOR_ORDER::GRB:
            r_shift = 20;
            g_shift = 0;
            b_shift = 10;
            break;
        case COLOR_ORDER::GBR:
            r_shift = 10;
            g_shift = 20;
            b_shift = 0;
            break;
        case COLOR_ORDER::BRG:
            r_shift = 10;
            g_shift = 00;
            b_shift = 20;
            break;
        case COLOR_ORDER::BGR:
            r_shift = 20;
            g_shift = 10;
            b_shift = 0;
            break;

    }
}

void Hub75::set_color(uint x, uint y, Pixel c) {
    if(x >= width || y >= height) return;
    draw_back_buffer[buffer_offset(x, y)] = c;
}

void Hub75::set_pixel(uint x, uint y, uint8_t r, uint8_t g, uint8_t b) {
    if(x >= width || y >= height) return;
    draw_back_buffer[buffer_offset(x, y)] = (lut_table[b] << b_shift) | (lut_table[g] << g_shift) | (lut_table[r] << r_shift);
}

void Hub75::FM6126A_write_register(uint16_t value, uint8_t position) {
    uint setup_width = panel_width();

    auto write_register = [&](uint clock_pin, uint strobe_pin) {
        gpio_put(clock_pin, !clk_polarity);
        gpio_put(strobe_pin, !stb_polarity);

        uint8_t threshold = setup_width - position;
        for(auto i = 0u; i < setup_width; i++) {
            auto j = i % 16;
            bool b = value & (1 << j);

            gpio_put(pin_r0, b);
            gpio_put(pin_g0, b);
            gpio_put(pin_b0, b);
            gpio_put(pin_r1, b);
            gpio_put(pin_g1, b);
            gpio_put(pin_b1, b);

            // Assert strobe/latch if i > threshold.
            gpio_put(strobe_pin, i > threshold);
            gpio_put(clock_pin, clk_polarity);
            sleep_us(10);
            gpio_put(clock_pin, !clk_polarity);
        }
    };

    write_register(pin_clk, pin_stb);
    if (split_controls) {
        write_register(pin_clk2, pin_stb2);
    }
}

void Hub75::FM6126A_setup() {
    // Ridiculous register write nonsense for the FM6126A-based 64x64 matrix
    FM6126A_write_register(0b1111111111111110, 12);
    FM6126A_write_register(0b0000001000000000, 13);
}

void Hub75::DP3246_setup() {
    static constexpr bool REG1[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
    static constexpr bool REG2[16] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    uint setup_width = panel_width();

    auto setup_panel = [&](uint clock_pin, uint strobe_pin, uint oe_pin) {
        gpio_put(clock_pin, !clk_polarity);
        gpio_put(strobe_pin, !stb_polarity);
        gpio_put(oe_pin, !oe_polarity);

        for (uint l = 0; l < setup_width; ++l) {
            if (l == setup_width - 3) {
                gpio_put(strobe_pin, stb_polarity);
            }
            pulse_panel_clock_pin(clock_pin, clk_polarity);
        }

        gpio_put(strobe_pin, !stb_polarity);

        for (uint l = 0; l < setup_width; ++l) {
            set_all_data_pins(*this, REG1[l % 16]);
            if (l == setup_width - 11) {
                gpio_put(strobe_pin, stb_polarity);
            }
            pulse_panel_clock_pin(clock_pin, clk_polarity);
        }

        gpio_put(strobe_pin, !stb_polarity);

        for (uint l = 0; l < setup_width; ++l) {
            set_all_data_pins(*this, REG2[l % 16]);
            if (l == setup_width - 12) {
                gpio_put(strobe_pin, stb_polarity);
            }
            pulse_panel_clock_pin(clock_pin, clk_polarity);
        }

        gpio_put(strobe_pin, !stb_polarity);
        pulse_panel_clock_pin(clock_pin, clk_polarity);

        set_all_data_pins(*this, false);

        for (uint l = 0; l < setup_width; ++l) {
            if (l == setup_width - 3) {
                gpio_put(strobe_pin, stb_polarity);
            }
            pulse_panel_clock_pin(clock_pin, clk_polarity);
        }

        gpio_put(strobe_pin, !stb_polarity);
        gpio_put(oe_pin, oe_polarity);
        pulse_panel_clock_pin(clock_pin, clk_polarity);
    };

    setup_panel(pin_clk, pin_stb, pin_oe);
    if (split_controls) {
        setup_panel(pin_clk2, pin_stb2, pin_oe2);
    }
}

void Hub75::init_shiftreg_rows() {
    // TYPE595-style row decoders use A=row clock, B=BK, C=row data.
    gpio_init(pin_row_a); gpio_set_function(pin_row_a, GPIO_FUNC_SIO); gpio_set_dir(pin_row_a, true);
    gpio_init(pin_row_b); gpio_set_function(pin_row_b, GPIO_FUNC_SIO); gpio_set_dir(pin_row_b, true);
    gpio_init(pin_row_c); gpio_set_function(pin_row_c, GPIO_FUNC_SIO); gpio_set_dir(pin_row_c, true);

    gpio_put(pin_row_a, 0);
    gpio_put(pin_row_b, 0);
    gpio_put(pin_row_c, 0);
}

void Hub75::step_shiftreg_row(uint row) const {
    // Seed the shift register when wrapping back to row 0, otherwise shift in 0s.
    gpio_put(pin_row_b, 1);
    gpio_put(pin_row_c, row == 0);
    shiftreg_timing_delay();
    gpio_put(pin_row_a, 1);
    shiftreg_timing_delay();
    gpio_put(pin_row_a, 0);
    shiftreg_timing_delay();
    gpio_put(pin_row_b, 0);
    if (row == 0) {
        gpio_put(pin_row_c, 0);
    }
}

void Hub75::start(irq_handler_t handler) {
    if(handler) {
        switch (shift_driver) {
            case SHIFT_DRIVER_FM6126A:
                FM6126A_setup();
                break;
            case SHIFT_DRIVER_DP3246:
                DP3246_setup();
                break;
            default:
                break;
        }

        uint latch_cycles = latch_cycles_for_system_clock();

        if (line_decoder == LINE_DECODER_TYPE595) {
            // TYPE595 panels keep row selection outside the row PIO program.
            init_shiftreg_rows();
            step_shiftreg_row(0);
            shiftreg_row_preloaded = true;
        }

        uint data_range_base = DATA_BASE_PIN;
        uint data_range_count = DATA_N_PINS;
        uint row_range_base = line_decoder == LINE_DECODER_TYPE595 ? pin_stb : ROWSEL_BASE_PIN;
        uint row_range_count = line_decoder == LINE_DECODER_TYPE595 ? 2 : ROWSEL_N_PINS;

        if (split_controls) {
            data_range_base = range_min({DATA_BASE_PIN, pin_clk, pin_clk2});
            data_range_count = range_max({DATA_BASE_PIN + DATA_N_PINS - 1, pin_clk, pin_clk2}) - data_range_base + 1;

            if (line_decoder == LINE_DECODER_TYPE595) {
                row_range_base = range_min({pin_stb, pin_stb + 1, pin_stb2, pin_stb2 + 1});
                row_range_count = range_max({pin_stb, pin_stb + 1, pin_stb2, pin_stb2 + 1}) - row_range_base + 1;
            } else {
                row_range_base = range_min({ROWSEL_BASE_PIN, ROWSEL_BASE_PIN + ROWSEL_N_PINS - 1, pin_stb, pin_stb + 1, pin_stb2, pin_stb2 + 1});
                row_range_count = range_max({ROWSEL_BASE_PIN, ROWSEL_BASE_PIN + ROWSEL_N_PINS - 1, pin_stb, pin_stb + 1, pin_stb2, pin_stb2 + 1}) - row_range_base + 1;
            }
        }

        if (shift_driver == SHIFT_DRIVER_DP3246) {
            pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_data_rgb888_invclk_program, &pio, &sm_data,
              &data_prog_offs, data_range_base, data_range_count, true);
        } else {
            pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_data_rgb888_program, &pio, &sm_data,
              &data_prog_offs, data_range_base, data_range_count, true);
        }

        if (line_decoder == LINE_DECODER_TYPE595) {
            if (shift_driver == SHIFT_DRIVER_DP3246) {
                if (inverted_stb) {
                    pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_noaddr_dp3246_inverted_program, &pio, &sm_row,
                      &row_prog_offs, row_range_base, row_range_count, true);
                } else {
                    pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_noaddr_dp3246_program, &pio, &sm_row,
                      &row_prog_offs, row_range_base, row_range_count, true);
                }
            } else if (inverted_stb) {
                pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_noaddr_inverted_program, &pio, &sm_row,
                  &row_prog_offs, row_range_base, row_range_count, true);
            } else {
                pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_noaddr_program, &pio, &sm_row,
                  &row_prog_offs, row_range_base, row_range_count, true);
            }
        } else if (shift_driver == SHIFT_DRIVER_DP3246) {
            if (inverted_stb) {
                pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_dp3246_inverted_program, &pio, &sm_row,
                  &row_prog_offs, row_range_base, row_range_count, true);
            } else {
                pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_dp3246_program, &pio, &sm_row,
                  &row_prog_offs, row_range_base, row_range_count, true);
            }
        } else if (inverted_stb) {
            pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_inverted_program, &pio, &sm_row,
              &row_prog_offs, row_range_base, row_range_count, true);
        } else {
            pio_claim_free_sm_and_add_program_for_gpio_range(&hub75_row_program, &pio, &sm_row,
              &row_prog_offs, row_range_base, row_range_count, true);
        }

        if (split_controls) {
            // The range-aware claims above selected a PIO/GPIO-base combination that can see
            // all pins used by both heads, so the extra SMs can now be claimed directly.
            sm_data_b = pio_claim_unused_sm(pio, true);
            sm_row_b = pio_claim_unused_sm(pio, true);
        }

        if (shift_driver == SHIFT_DRIVER_DP3246) {
            hub75_data_rgb888_invclk_program_init(pio, sm_data, data_prog_offs, DATA_BASE_PIN, pin_clk);
        } else {
            hub75_data_rgb888_program_init(pio, sm_data, data_prog_offs, DATA_BASE_PIN, pin_clk);
        }
        if (line_decoder == LINE_DECODER_TYPE595) {
            if (shift_driver == SHIFT_DRIVER_DP3246) {
                hub75_row_noaddr_dp3246_program_init(pio, sm_row, row_prog_offs, pin_stb, latch_cycles);
            } else {
                hub75_row_noaddr_program_init(pio, sm_row, row_prog_offs, pin_stb, latch_cycles);
            }
        } else {
            hub75_row_program_init(pio, sm_row, row_prog_offs, ROWSEL_BASE_PIN, ROWSEL_N_PINS, pin_stb, latch_cycles);
        }

        if (split_controls) {
            if (shift_driver == SHIFT_DRIVER_DP3246) {
                hub75_data_rgb888_invclk_program_init(pio, sm_data_b, data_prog_offs, DATA_BASE_PIN, pin_clk2);
            } else {
                hub75_data_rgb888_program_init(pio, sm_data_b, data_prog_offs, DATA_BASE_PIN, pin_clk2);
            }
            if (line_decoder == LINE_DECODER_TYPE595) {
                if (shift_driver == SHIFT_DRIVER_DP3246) {
                    hub75_row_noaddr_dp3246_program_init(pio, sm_row_b, row_prog_offs, pin_stb2, latch_cycles);
                } else {
                    hub75_row_noaddr_program_init(pio, sm_row_b, row_prog_offs, pin_stb2, latch_cycles);
                }
            } else {
                hub75_row_program_init(pio, sm_row_b, row_prog_offs, ROWSEL_BASE_PIN, ROWSEL_N_PINS, pin_stb2, latch_cycles);
            }
        }

        // Prevent flicker in Python caused by the smaller dataset just blasting through the PIO too quickly.
        float data_clkdiv = panel_data_clkdiv(panel_width());
        pio_sm_set_clkdiv(pio, sm_data, data_clkdiv);
        if (split_controls) {
            pio_sm_set_clkdiv(pio, sm_data_b, data_clkdiv);
        }

        dma_channel = dma_claim_unused_channel(true);
        dma_channel_config config = dma_channel_get_default_config(dma_channel);
        channel_config_set_transfer_data_size(&config, DMA_SIZE_32);
        channel_config_set_bswap(&config, false);
        channel_config_set_dreq(&config, pio_get_dreq(pio, sm_data, true));
        dma_channel_configure(dma_channel, &config, &pio->txf[sm_data], NULL, 0, false);

        if (split_controls) {
            dma_channel_b = dma_claim_unused_channel(true);
            dma_channel_config config_b = dma_channel_get_default_config(dma_channel_b);
            channel_config_set_transfer_data_size(&config_b, DMA_SIZE_32);
            channel_config_set_bswap(&config_b, false);
            channel_config_set_dreq(&config_b, pio_get_dreq(pio, sm_data_b, true));
            dma_channel_configure(dma_channel_b, &config_b, &pio->txf[sm_data_b], NULL, 0, false);
        }

        irq_add_shared_handler(DMA_IRQ_0, handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
        dma_channel_set_irq0_enabled(dma_channel, true);
        if (split_controls) {
            dma_channel_set_irq0_enabled(dma_channel_b, true);
        }
        irq_set_enabled(DMA_IRQ_0, true);

        row = 0;
        bit = 0;
        if (shift_driver == SHIFT_DRIVER_DP3246) {
            hub75_data_rgb888_invclk_set_shift(pio, sm_data, data_prog_offs, bit);
            if (split_controls) {
                hub75_data_rgb888_invclk_set_shift(pio, sm_data_b, data_prog_offs, bit);
            }
        } else {
            hub75_data_rgb888_set_shift(pio, sm_data, data_prog_offs, bit);
            if (split_controls) {
                hub75_data_rgb888_set_shift(pio, sm_data_b, data_prog_offs, bit);
            }
        }

        dma_channel_set_trans_count(dma_channel, panel_width() * 2, false);
        dma_channel_set_read_addr(dma_channel, row_buffer_ptr(row, 0), true);
    }
}

void Hub75::stop(irq_handler_t handler) {
    shiftreg_row_preloaded = false;

    irq_set_enabled(DMA_IRQ_0, false);

    if(dma_channel != -1 &&  dma_channel_is_claimed(dma_channel)) {
        dma_channel_set_irq0_enabled(dma_channel, false);
        //dma_channel_wait_for_finish_blocking(dma_channel);
        dma_channel_abort(dma_channel);
        dma_channel_acknowledge_irq0(dma_channel);
        dma_channel_unclaim(dma_channel);
    }
    if(dma_channel_b != -1 && dma_channel_is_claimed(dma_channel_b)) {
        dma_channel_set_irq0_enabled(dma_channel_b, false);
        dma_channel_abort(dma_channel_b);
        dma_channel_acknowledge_irq0(dma_channel_b);
        dma_channel_unclaim(dma_channel_b);
    }

    if (handler) {
        irq_remove_handler(DMA_IRQ_0, handler);
    }

    if(pio_sm_is_claimed(pio, sm_data)) {
        pio_sm_set_enabled(pio, sm_data, false);
        pio_sm_drain_tx_fifo(pio, sm_data);
        if (shift_driver == SHIFT_DRIVER_DP3246) {
            pio_remove_program_and_unclaim_sm(&hub75_data_rgb888_invclk_program, pio, sm_data, data_prog_offs);
        } else {
            pio_remove_program_and_unclaim_sm(&hub75_data_rgb888_program, pio, sm_data, data_prog_offs);
        }
    }
    if(split_controls && pio_sm_is_claimed(pio, sm_data_b)) {
        pio_sm_set_enabled(pio, sm_data_b, false);
        pio_sm_drain_tx_fifo(pio, sm_data_b);
        pio_sm_unclaim(pio, sm_data_b);
    }

    if(pio_sm_is_claimed(pio, sm_row)) {
        pio_sm_set_enabled(pio, sm_row, false);
        pio_sm_drain_tx_fifo(pio, sm_row);
        if (line_decoder == LINE_DECODER_TYPE595) {
          if (shift_driver == SHIFT_DRIVER_DP3246) {
                if (inverted_stb) {
                    pio_remove_program_and_unclaim_sm(&hub75_row_noaddr_dp3246_inverted_program, pio, sm_row, row_prog_offs);
                } else {
                    pio_remove_program_and_unclaim_sm(&hub75_row_noaddr_dp3246_program, pio, sm_row, row_prog_offs);
                }
            } else if (inverted_stb) {
                pio_remove_program_and_unclaim_sm(&hub75_row_noaddr_inverted_program, pio, sm_row, row_prog_offs);
            } else {
                pio_remove_program_and_unclaim_sm(&hub75_row_noaddr_program, pio, sm_row, row_prog_offs);
            }
        } else if (shift_driver == SHIFT_DRIVER_DP3246) {
            if (inverted_stb) {
                pio_remove_program_and_unclaim_sm(&hub75_row_dp3246_inverted_program, pio, sm_row, row_prog_offs);
            } else {
                pio_remove_program_and_unclaim_sm(&hub75_row_dp3246_program, pio, sm_row, row_prog_offs);
            }
        } else if (inverted_stb) {
            pio_remove_program_and_unclaim_sm(&hub75_row_inverted_program, pio, sm_row, row_prog_offs);
        } else {
            pio_remove_program_and_unclaim_sm(&hub75_row_program, pio, sm_row, row_prog_offs);
        }
    }
    if(split_controls && pio_sm_is_claimed(pio, sm_row_b)) {
        pio_sm_set_enabled(pio, sm_row_b, false);
        pio_sm_drain_tx_fifo(pio, sm_row_b);
        pio_sm_unclaim(pio, sm_row_b);
    }

    // Make sure the GPIO is in a known good state
    // since we don't know what the PIO might have done with it
    for (int i = 0; i < 6; i++) {
        gpio_put(pin_r0 + i, 0);
    }
    for (int i = 0; i < 5; i++) {
        gpio_put(pin_row_a + i, 0);
    }
    gpio_put(pin_clk, !clk_polarity);
    gpio_put(pin_stb, !stb_polarity);
    gpio_put(pin_oe, !oe_polarity);
    if (split_controls) {
        gpio_put(pin_clk2, !clk_polarity);
        gpio_put(pin_stb2, !stb_polarity);
        gpio_put(pin_oe2, !oe_polarity);
    }
}

void Hub75::render() {
    if (back_buffer2 != nullptr) {
        memcpy(render_back_buffer, draw_back_buffer, width * height * sizeof(Pixel));
    }
}

Hub75::~Hub75() {
    if (managed_buffer) {
        delete[] back_buffer1;
        delete[] back_buffer2;
    }
}

void Hub75::clear() {
    for(auto x = 0u; x < width; x++) {
        for(auto y = 0u; y < height; y++) {
            set_pixel(x, y, 0, 0, 0);
        }
    }
}


void Hub75::dma_complete() {
    if (!split_controls && dma_channel_get_irq0_status(dma_channel)) {
        dma_channel_acknowledge_irq0(dma_channel);

        // Check that previous OEn pulse is finished, else things WILL get out of sequence
        hub75_wait_tx_stall(pio, sm_row);

        if (line_decoder == LINE_DECODER_TYPE595) {
            if (shiftreg_row_preloaded) {
                // Row 0 was already seeded during startup.
                shiftreg_row_preloaded = false;
            } else {
                step_shiftreg_row(row);
            }
        }

        if (uses_dp3246_type595(*this)) {
            // DP3246 wants LAT held while the final clocks are still being shifted.
            pio_sm_put_blocking(pio, sm_row, encode_row_payload(row, bit));
        }

        // Fully flush the pixel shifter before latching the next row.
        for (uint i = 0; i < end_of_row_dummy_pixels(); ++i) {
            pio_sm_put_blocking(pio, sm_data, 0);
        }

        // SM is finished when it stalls on empty TX FIFO
        hub75_wait_tx_stall(pio, sm_data);

        if (!uses_dp3246_type595(*this)) {
            // Latch row data, pulse output enable for new row.
            pio_sm_put_blocking(pio, sm_row, encode_row_payload(row, bit));
        }

        row++;

        if(row == height / 2) {
            row = 0;
            bit++;
            if (bit == BIT_DEPTH) {
                bit = 0;
            }
            if (shift_driver == SHIFT_DRIVER_DP3246) {
                hub75_data_rgb888_invclk_set_shift(pio, sm_data, data_prog_offs, bit);
            } else {
                hub75_data_rgb888_set_shift(pio, sm_data, data_prog_offs, bit);
            }
        }

        dma_channel_set_trans_count(dma_channel, panel_width() * 2, false);
        dma_channel_set_read_addr(dma_channel, row_buffer_ptr(row, 0), true);
    }

    if (split_controls && dma_channel_get_irq0_status(dma_channel)) {
        dma_channel_acknowledge_irq0(dma_channel);

        // Phase A clocks the shared row data into the left panel, then arms the right panel.
        hub75_wait_tx_stall(pio, sm_row);

        if (line_decoder == LINE_DECODER_TYPE595) {
            if (shiftreg_row_preloaded) {
                shiftreg_row_preloaded = false;
            } else {
                step_shiftreg_row(row);
            }
        }

        if (uses_dp3246_type595(*this)) {
            pio_sm_put_blocking(pio, sm_row, encode_row_payload(row, bit));
        }

        for (uint i = 0; i < end_of_row_dummy_pixels(); ++i) {
            pio_sm_put_blocking(pio, sm_data, 0);
        }

        hub75_wait_tx_stall(pio, sm_data);

        if (!uses_dp3246_type595(*this)) {
            pio_sm_put_blocking(pio, sm_row, encode_row_payload(row, bit));
        }

        dma_channel_set_trans_count(dma_channel_b, panel_width() * 2, false);
        dma_channel_set_read_addr(dma_channel_b, row_buffer_ptr(row, 1), true);
    }

    if (split_controls && dma_channel_get_irq0_status(dma_channel_b)) {
        dma_channel_acknowledge_irq0(dma_channel_b);

        // Phase B replays the same logical row on the right panel before advancing the scan.
        hub75_wait_tx_stall(pio, sm_row_b);

        if (uses_dp3246_type595(*this)) {
            pio_sm_put_blocking(pio, sm_row_b, encode_row_payload(row, bit));
        }

        for (uint i = 0; i < end_of_row_dummy_pixels(); ++i) {
            pio_sm_put_blocking(pio, sm_data_b, 0);
        }

        hub75_wait_tx_stall(pio, sm_data_b);

        if (!uses_dp3246_type595(*this)) {
            pio_sm_put_blocking(pio, sm_row_b, encode_row_payload(row, bit));
        }

        row++;

        if(row == height / 2) {
            row = 0;
            bit++;
            if (bit == BIT_DEPTH) {
                bit = 0;
            }
            if (shift_driver == SHIFT_DRIVER_DP3246) {
                hub75_data_rgb888_invclk_set_shift(pio, sm_data, data_prog_offs, bit);
                hub75_data_rgb888_invclk_set_shift(pio, sm_data_b, data_prog_offs, bit);
            } else {
                hub75_data_rgb888_set_shift(pio, sm_data, data_prog_offs, bit);
                hub75_data_rgb888_set_shift(pio, sm_data_b, data_prog_offs, bit);
            }
        }

        dma_channel_set_trans_count(dma_channel, panel_width() * 2, false);
        dma_channel_set_read_addr(dma_channel, row_buffer_ptr(row, 0), true);
    }
}

uint32_t Hub75::encode_row_payload(uint row, uint bit) const {
    uint32_t oe_width = brightness << bit;

    if (line_decoder == LINE_DECODER_TYPE595) {
        return oe_width << 5;
    }

    return row | (oe_width << 5);
}

PanelType Hub75::legacy_panel_type() const {
    switch (shift_driver) {
        case SHIFT_DRIVER_FM6126A:
            return PANEL_FM6126A;
        default:
            return PANEL_GENERIC;
    }
}

int Hub75::buffer_offset(uint x, uint y) const {
    if(y >= height / 2) {
        y -= height / 2;
        return (y * width + x) * 2 + 1;
    }
    return (y * width + x) * 2;
}

uint Hub75::panel_width() const {
    return split_controls ? width / 2 : width;
}

Pixel *Hub75::row_buffer_ptr(uint row, uint phase) const {
    return &render_back_buffer[row * width * 2 + phase * panel_width() * 2];
}

uint Hub75::end_of_row_dummy_pixels() const {
    if (shift_driver == SHIFT_DRIVER_DP3246 && line_decoder == LINE_DECODER_TYPE595) {
        // DP3246 + TYPE595 needs a longer tail so LAT overlaps the final clocks cleanly.
        return 6;
    }
    return 2;
}

void Hub75::copy_to_back_buffer(void *data, size_t len, int start_x, int start_y, int g_width, int g_height) {
    uint8_t *p = (uint8_t *)data;

    if(g_width == int32_t(width / 2) && g_height == int32_t(height * 2)) {
        for(int y = start_y; y < g_height; y++) {
            int offsety = 0;
            int sy = y;
            int basex = 0;

            // Assuming our canvas is 128x128 and our display is 256x64,
            // consisting of 2x128x64 panels, remap the bottom half
            // of the canvas to the right-half of the display,
            // This gives us an optional square arrangement.
            if (sy >= int(height)) {
                sy -= height;
                basex = width / 2;
            }

            // Interlace the top and bottom halves of the panel.
            // Since these are scanned out simultaneously to two chains
            // of shift registers we need each pair of rows
            // (N and N + height / 2) to be adjacent in the buffer.
            offsety = width * 2;
            if(sy >= int(height / 2)) {
                sy -= height / 2;
                offsety *= sy;
                offsety += 1;
            } else {
                offsety *= sy;
            }

            for(int x = start_x; x < g_width; x++) {
                int sx = x;
                uint8_t b = *p++;
                uint8_t g = *p++;
                uint8_t r = *p++;

                // Assumes width / 2 is even.
                if (basex & 1) {
                    sx = basex - sx;
                } else {
                    sx += basex;
                }
                int offset = offsety + sx * 2;

                draw_back_buffer[offset] = (lut_table[b] << b_shift) | (lut_table[g] << g_shift) | (lut_table[r] << r_shift);

                // Skip the empty byte in out 32-bit aligned 24-bit colour.
                p++;

                len -= 4;

                if(len == 0) {
                    return;
                }
            }
        }
    } else {
        for(uint y = start_y; y < height; y++) {
            for(uint x = start_x; x < width; x++) {
                int offset = 0;
                int sy = y;
                int sx = x;
                uint8_t b = *p++;
                uint8_t g = *p++;
                uint8_t r = *p++;

                offset = buffer_offset(sx, sy);

                draw_back_buffer[offset] = (lut_table[b] << b_shift) | (lut_table[g] << g_shift) | (lut_table[r] << r_shift);

                // Skip the empty byte in out 32-bit aligned 24-bit colour.
                p++;

                len -= 4;

                if(len == 0) {
                    return;
                }
            }
        }
    }
}

#ifndef NO_PICO_GRAPHICS
void Hub75::update(PicoGraphics *graphics) {
    if(graphics->pen_type == PicoGraphics::PEN_RGB888) {
        copy_to_back_buffer(graphics->frame_buffer, width * height * sizeof(RGB888), 0, 0, graphics->bounds.w, graphics->bounds.h);
    } else {
        unsigned int offset = 0;
        graphics->frame_convert(PicoGraphics::PEN_RGB888, [this, &offset, &graphics](void *data, size_t length) {
            if (length > 0) {
                int offset_y = offset / graphics->bounds.w;
                int offset_x = offset - (offset_y * graphics->bounds.w);
                copy_to_back_buffer(data, length, offset_x, offset_y, graphics->bounds.w, graphics->bounds.h);
                offset += length / sizeof(RGB888);
            }
        });
    }
}
#endif
}
