# Power, boot and signal integrity

## Instruments and their limits

- Scope: AIN0/AIN1, 0..3.3 V, 12 bit, up to 500 kS/s aggregate (2 us per
  sample on one channel, 4 us each on two). Good for supplies, ramps,
  brown-outs, PWM up to a few kHz, slow analog signals. It cannot resolve
  edges or glitches faster than a few microseconds.
- Logic analyzer: 8 DIOs, up to 150 MS/s, so ~7 ns resolution. Use it for
  timing: pulse widths, delays between signals, glitches on digital lines.
- AIN above 3.3 V must go through a divider. Ask the user what is wired
  before measuring a rail above 3.3 V.

## Target does not boot / keeps resetting

1. **Rail ramp.** Wire the target's VCC to AIN0. `scope_capture(trigger="rising",
   level_v=1.0, pre=200, n=4096, wait=false)`, then `power(true)`, then
   `scope_read`. Check that `high` reaches ~3.3 V and stays there, that the
   ramp is monotonic (`scope_image`), and that there are no dips. A dip
   back down after the first rise means inrush or the protection tripping:
   the target draws too much, or has a short.
2. **Brown-out / periodic resets.** Long capture at a low rate
   (`rate_hz=10000`, `n=16384`, 1.6 s) of the rail while the target runs.
   Dips that line up with radio TX, motors or LEDs mean the supply is
   insufficient or decoupling is missing. Correlate with UART: a boot banner
   repeating = reset loop.
3. **Reset and boot-mode pins.** `dio_read` or `adc_read` on RESET/NRST (must
   be high after power-up) and on boot straps (BOOT0 on STM32, GPIO0 on
   ESP32, BOOTSEL on RP2040/2350). A strap at the wrong level boots the ROM
   bootloader instead of the application: the UART shows a ROM banner or
   nothing.
4. **Clock.** A crystal can't be probed meaningfully with these
   instruments (the probe loads it). Instead, have the firmware output a
   clock or toggle a pin at a known rate and measure `freq_hz` with the logic
   analyzer. A ratio error (e.g. 16/8, or HSI vs HSE) is the classic wrong
   PLL configuration.

## Driving reset / boot pins

To reset the target from a DIO (only if the user confirms which pin goes
to RESET and that the target doesn't drive it), do it in one
`sequence_run`: `dio_dir` output, `dio_write` low, `delay_us`, `dio_write`
high, then the checks that depend on timing (`wait_dio` on a pin that
moves at boot, a bus read) and `dio_dir` input at the end. Separate tool
calls give pulse widths and delays that vary by milliseconds. Reset lines
are usually open-drain with a pull-up on the target. Driving high is fine
with a 3.3 V target, but release the pin as an input afterwards.

If reset and boot are on the pins `uart_ctrl` drives, `uart_set_lines(...,
pulse_ms=...)` pulses them too.

**Measuring start-up:** `timeline_start(watch_dios=[<reset>, <status
pin>])` with the UART open, then the reset sequence. The `dio` and `uart`
lines give release-to-first-output, release-to-LED, etc. For µs precision
put a `wait_dio` on the status pin in the sequence.

## Signal looks wrong

- **Slow edges** (`rise_time_s` large): weak pull-up on an open-drain line,
  too much capacitance, or a pin configured as input with pull-up instead of
  push-pull output.
- **Overshoot / ringing** (`overshoot_pct` > 20-30 %): long wires, no series
  termination. At the scope's rate ringing is usually under-sampled. If edges
  are fast, check for glitches on the logic analyzer instead (extra edges
  in `first_edges`, or decode errors).
- **Wrong frequency / duty**: compare `freq_hz` and `duty_pct` with what the
  firmware should produce. A constant ratio error points to the clock
  configuration. A drifting one points to a software-timed signal (bit-banging,
  interrupts delayed).
- **Mid-level voltage** (`low`/`high` around 1-2 V, or a digital pin reading
  unstable): two outputs fighting, a floating input, or a missing ground.
- **Analog stimulus**: `awg_set`, `awg_dc` and `awg_step` drive AOUT0 (PWM +
  RC, bandwidth limited to ~100 kHz). With `awg_step` and the scope triggered
  on the step you can check an ADC input's settling or a comparator threshold.
  Sweep `awg_dc` to find the voltage where a digital input flips (`dio_read`).
