import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
SPEC = importlib.util.spec_from_file_location('builder', ROOT / 'tools/build_firmware.py')
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)

profiles = {x['id']: x for x in json.loads((ROOT / 'firmware-targets.json').read_text())['profiles']}


class ElrsVariant(unittest.TestCase):
    def test_catalog_profile(self):
        self.assertEqual(profiles['esp32-elrs'], {
            "id": "esp32-elrs", "target": "esp32", "label": "ESP32 · ELRS DK500 stick receiver (UART)",
            "idf_ref": "25fe69f946311abdaf9ad56591f25fedbc20ac98", "allow_larger_flash": True, "preview": True,
            "variant": "elrs", "sdkconfig": "sdkconfig.defaults.esp32-elrs"})

    def test_build_command_flags(self):
        elrs = builder.build_command(profiles['esp32-elrs'], Path('/b'), '/sdk')
        stock = builder.build_command(profiles['esp32'], Path('/b'), '/sdk')
        self.assertIn('-DESP_SDR_ELRS=ON', elrs)
        self.assertIn('-DESP_SDR_ELRS=OFF', stock)
        self.assertIn('-DSDKCONFIG_DEFAULTS=' + str(ROOT / 'sdkconfig.defaults.esp32-elrs'), elrs)
        self.assertIn('-DIDF_TARGET=esp32', elrs)

    def test_cmake_branch_is_early_and_returns(self):
        text = (ROOT / 'main/CMakeLists.txt').read_text()
        lines = [x for x in text.splitlines() if x.strip() and not x.strip().startswith('#')]
        self.assertEqual(lines[0], 'idf_build_get_property(elrs ESP_SDR_ELRS)')
        end = text.index('set(esp_sdr_supported_targets')
        block = text[:end]
        for needle in ('generate_version.py', '--profile', 'esp32-elrs', 'sram_guard.ld',
                       '--wrap=chip_v7_set_chan_ana', 'rx_calibration_link.cmake'):
            self.assertIn(needle, block)
        self.assertIn('return()', block)


if __name__ == '__main__':
    unittest.main()
