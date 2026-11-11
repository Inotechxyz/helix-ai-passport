#!/usr/bin/env python3
"""Static contracts for the terminal deep-sleep shutdown path."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def initializer(source: str, symbol: str) -> str:
    match = re.search(rf"\b{re.escape(symbol)}\s*\[\]\s*=\s*\{{", source)
    if not match:
        raise AssertionError(f"initializer not found: {symbol}")
    end = source.find("};", match.end())
    if end < 0:
        raise AssertionError(f"initializer is unterminated: {symbol}")
    return source[match.end():end]


def strip_comments(source: str) -> str:
    # A comment naming a function (e.g. "ls_app_start() re-checks ...") must not
    # be mistaken for its definition by the signature regex below.
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def function_body(source: str, name: str) -> str:
    source = strip_comments(source)
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


def register_pairs(block: str) -> list[tuple[int, int]]:
    return [
        (int(reg, 16), int(value, 16))
        for reg, value in re.findall(r"\{\s*0x([0-9A-Fa-f]{2})\s*,\s*0x([0-9A-Fa-f]{2})\s*\}", block)
    ]


class DeepSleepContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.audio = read("components/bsp/src/bsp_audio.c")
        cls.battery = read("components/bsp/src/bsp_battery.c")
        cls.display = read("components/bsp/src/bsp_display.c")
        cls.i2c = read("components/bsp/src/bsp_i2c.c")
        cls.demo = read("main/demo_low_power.c")

    def test_es8311_force_sleep_sequence_is_complete_and_ordered(self) -> None:
        expected = [
            (0x32, 0x00), (0x17, 0x00), (0x0E, 0xFF), (0x12, 0x02),
            (0x14, 0x00), (0x0D, 0xFA), (0x15, 0x00), (0x02, 0x10),
            (0x00, 0x00), (0x00, 0x1F), (0x01, 0x30), (0x01, 0x00),
            (0x45, 0x01), (0x0D, 0xFC), (0x02, 0x00),
        ]
        actual = register_pairs(initializer(self.audio, "s_es8311_sleep_sequence"))
        self.assertEqual(actual, expected)

    def test_es8311_critical_registers_are_read_back(self) -> None:
        # The C host test exhausts all readback values against the actual policy.
        # This integration contract makes sure the driver uses it, and that an
        # I2C read failure cannot pass even when the output byte matches.
        body = function_body(self.audio, "es8311_force_sleep_once")
        self.assertIn("s_ctrl->write_reg", body)
        self.assertIn("s_ctrl->read_reg", body)
        self.assertIn("i < bsp_es8311_sleep_check_count", body)
        self.assertIn("&bsp_es8311_sleep_checks[i]", body)
        self.assertRegex(
            body,
            r"read_result == ESP_CODEC_DEV_OK\s*&&\s*"
            r"bsp_es8311_sleep_check_matches\(item, actual\)",
        )

    def test_es8311_force_sleep_retries_once_after_five_ms(self) -> None:
        self.assertRegex(self.audio, r"#define\s+ES8311_SLEEP_ATTEMPTS\s+2\b")
        self.assertRegex(self.audio, r"#define\s+ES8311_SLEEP_RETRY_MS\s+5\b")
        body = function_body(self.audio, "es8311_force_sleep")
        self.assertIn("attempt <= ES8311_SLEEP_ATTEMPTS", body)
        self.assertIn("pdMS_TO_TICKS(ES8311_SLEEP_RETRY_MS)", body)

    def test_audio_suspend_does_not_require_a_silent_open(self) -> None:
        body = function_body(self.audio, "bsp_audio_sleep")
        self.assertIn("es8311_force_sleep()", body)
        self.assertIn("audio_disable_i2s_channels()", body)
        self.assertNotIn("bsp_audio_set_format", body)

    def test_i2s_pins_are_released_only_by_deep_sleep_api(self) -> None:
        body = function_body(self.audio, "bsp_audio_prepare_deep_sleep")
        for pin in ("BSP_I2S_MCLK", "BSP_I2S_BCLK", "BSP_I2S_WS",
                    "BSP_I2S_DOUT", "BSP_I2S_DIN"):
            self.assertIn(pin, body)
        self.assertIn("GPIO_MODE_INPUT", body)
        self.assertIn("GPIO_PULLUP_DISABLE", body)
        self.assertIn("GPIO_PULLDOWN_DISABLE", body)

    def test_cw2017_sleep_is_verified_and_retried(self) -> None:
        body = function_body(self.battery, "bsp_battery_sleep")
        self.assertIn("attempt <= 2", body)
        self.assertIn("cw_write(CW_REG_CONFIG, CW_CONFIG_SLEEP)", body)
        self.assertIn("pdMS_TO_TICKS(5)", body)
        self.assertIn("cw_read(CW_REG_CONFIG, &actual, 1)", body)
        self.assertIn("actual == CW_CONFIG_SLEEP", body)

    def test_shared_i2c_is_released_after_device_transactions(self) -> None:
        body = function_body(self.i2c, "bsp_i2c_prepare_deep_sleep")
        self.assertIn("BSP_I2C_SDA", body)
        self.assertIn("BSP_I2C_SCL", body)
        self.assertIn("GPIO_MODE_INPUT", body)
        self.assertIn("GPIO_PULLUP_DISABLE", body)
        self.assertIn("GPIO_PULLDOWN_DISABLE", body)

    def test_lcd_safe_levels_and_holds_are_configured(self) -> None:
        pins = initializer(self.display, "s_deep_sleep_pins")
        levels = initializer(self.display, "s_deep_sleep_levels")
        self.assertRegex(pins, r"BSP_LCD_CS.*BSP_LCD_SCLK.*BSP_LCD_MOSI.*BSP_LCD_DC.*BSP_LCD_BL")
        self.assertRegex(levels, r"1\s*,\s*0\s*,\s*0\s*,\s*0\s*,\s*0")
        body = function_body(self.display, "bsp_display_prepare_deep_sleep")
        self.assertIn("esp_lcd_panel_disp_on_off(s_panel, false)", body)
        self.assertIn("esp_lcd_panel_disp_sleep(s_panel, true)", body)
        self.assertIn("ledc_stop", body)
        self.assertIn("gpio_hold_en", body)
        self.assertIn("gpio_deep_sleep_hold_en", body)

    def test_lcd_holds_are_released_before_spi_initialization(self) -> None:
        release = function_body(self.display, "display_release_deep_sleep_holds")
        self.assertIn("gpio_deep_sleep_hold_dis", release)
        self.assertIn("gpio_hold_dis", release)
        init = function_body(self.display, "bsp_display_init")
        self.assertLess(init.index("display_release_deep_sleep_holds()"),
                        init.index("spi_bus_initialize"))

    def test_terminal_shutdown_order_precedes_deep_sleep(self) -> None:
        body = function_body(self.demo, "sleep_task")
        calls = [
            "bsp_battery_sleep()",
            "bsp_audio_sleep()",
            "bsp_audio_prepare_deep_sleep()",
            "bsp_i2c_prepare_deep_sleep()",
            "bsp_display_prepare_deep_sleep()",
            "esp_deep_sleep_start()",
        ]
        positions = [body.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))
        self.assertLess(body.index("bsp_lvgl_lock(1000)"),
                        body.index("bsp_display_prepare_deep_sleep()"))

    def test_application_initializes_the_fuel_gauge_before_the_ui(self) -> None:
        app = read("main/langstudy_app.c")
        body = function_body(app, "ls_app_start")
        # bsp_battery_soc() answers -1 until bsp_battery_init() has run, so the
        # header gauge would show an empty outline on every boot without it.
        self.assertIn("bsp_battery_init()", body)
        self.assertLess(body.index("bsp_battery_init()"), body.index("ls_ui_init()"))

    def test_application_rechecks_links_instead_of_restoring_them(self) -> None:
        app = read("main/langstudy_app.c")
        body = function_body(app, "load_state")
        # A boot/wake probe re-checks both links, so the header never shows a
        # value saved before the reset that produced this boot.
        self.assertNotIn("state->wifi_ok = cached.wifi_ok", body)
        self.assertNotIn("state->deepseek_ok = cached.deepseek_ok", body)

    def test_application_starts_the_silent_link_probe_at_boot(self) -> None:
        app = read("main/langstudy_app.c")
        body = function_body(app, "ls_app_start")
        self.assertIn("ls_online_start(LS_ONLINE_PROBE)", body)
        self.assertLess(body.index("ls_online_init()"),
                        body.index("ls_online_start(LS_ONLINE_PROBE)"))

    def test_launch_screen_is_skipped_on_a_deep_sleep_wake(self) -> None:
        app = read("main/langstudy_app.c")
        body = function_body(app, "ls_app_start")
        # The GPIO wake check must precede the splash decision so a key wake
        # returns straight to the study screen.
        self.assertLess(body.index("ESP_SLEEP_WAKEUP_GPIO"),
                        body.index("ls_splash_show("))
        self.assertIn("if (!key_wake)", body)

    def test_application_saves_the_battery_before_sleeping_the_gauge(self) -> None:
        app = read("main/langstudy_app.c")
        body = function_body(app, "enter_deep_sleep")
        self.assertIn("ls_status_cache_set_soc(", body)
        self.assertLess(body.index("ls_status_cache_set_soc("),
                        body.index("bsp_battery_sleep()"))

    def test_ui_caches_the_values_the_next_wake_needs(self) -> None:
        ui = read("main/langstudy_ui.c")
        self.assertIn("ls_status_cache_set_links(", function_body(ui, "handle_online"))
        self.assertIn("ls_status_cache_set_soc(", function_body(ui, "ls_ui_tick"))
        init = function_body(ui, "ls_ui_init")
        self.assertIn("ls_status_cache_get(", init)
        self.assertLess(init.index("ls_status_cache_get("),
                        init.index("update_battery_bar("))

    def test_ui_shutdown_never_deletes_the_active_screen_first(self) -> None:
        ui = read("main/langstudy_ui.c")
        body = function_body(ui, "ls_ui_shutdown")
        # Deleting the active screen leaves LVGL's act_scr NULL; its refresh
        # task then asserts and spins while holding the port mutex, so the
        # deep-sleep path times out and restarts instead of sleeping. A blank
        # screen must be loaded before the old tree is freed.
        self.assertLess(body.index("lv_screen_load("),
                        body.index("lv_obj_delete(s_scr)"))

    def test_battery_ready_check_does_not_trust_a_zero_reading(self) -> None:
        battery = read("components/bsp/src/bsp_battery.c")
        # A restarted CW2017 answers 0 to SOC and VCELL until its first
        # conversion; the old check accepted soc <= 100, i.e. 0, as ready.
        body = function_body(battery, "cw_wait_soc_ready")
        self.assertIn("bsp_battery_mv()", body)
        self.assertIn("reading_is_plausible(", body)
        soc = function_body(battery, "bsp_battery_soc")
        self.assertRegex(soc, r"mv\s*<\s*CW_VCELL_MIN_MV")

    def test_application_sleep_order_matches_bsp_contract(self) -> None:
        app = read("main/langstudy_app.c")
        body = function_body(app, "enter_deep_sleep")
        self.assertIn("BSP_BTN_MV_RELEASED_MIN", body)
        calls = [
            "bsp_button_prepare_deep_sleep(",
            "esp_deep_sleep_enable_gpio_wakeup(",
            "bsp_battery_sleep()",
            "bsp_audio_sleep()",
            "bsp_audio_prepare_deep_sleep()",
            "bsp_i2c_prepare_deep_sleep()",
            "bsp_lvgl_lock(1000)",
            "bsp_display_prepare_deep_sleep()",
            "esp_deep_sleep_start()",
        ]
        positions = [body.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))


if __name__ == "__main__":
    unittest.main()
