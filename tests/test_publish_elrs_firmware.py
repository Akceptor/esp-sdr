import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'publisher', Path(__file__).resolve().parents[1] / 'tools/elrs/publish_web_firmware.py')
publisher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(publisher)

COMMIT = 'c' * 40
NAMES = ['0-bootloader.bin', '1-partition-table.bin', '2-esp_sdr.bin']
OFFSETS = [0x1000, 0x8000, 0x10000]
STOCK_MANIFEST = b'{"name": "ESP-SDR"}\n'
PAGE = ('<main>\n<!-- elrs-firmware:begin -->\nnot published yet\n<!-- elrs-firmware:end -->\n'
        '<p>stock section</p>\n</main>\n')


class PublishElrs(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        self.art = root / 'art'
        self.web = root / 'web'
        (self.art / 'esp32-elrs').mkdir(parents=True)
        (self.web / 'flash').mkdir(parents=True)
        (self.web / 'flash/manifest.json').write_bytes(STOCK_MANIFEST)
        (self.web / 'flash/index.html').write_text(PAGE)
        self.payloads = [b'boot-' + bytes([i]) * 40 for i in range(3)]
        self.parts = []
        for name, offset, data in zip(NAMES, OFFSETS, self.payloads):
            (self.art / 'esp32-elrs' / name).write_bytes(data)
            self.parts.append(dict(name=name, offset=offset, size=len(data),
                                   sha256=hashlib.sha256(data).hexdigest(),
                                   md5=hashlib.md5(data).hexdigest()))
        self.chip = 'ESP32'
        self.version = COMMIT
        self.save()

    def save(self):
        manifest = dict(schema_version=1, version=COMMIT, variants={'esp32-elrs': dict(
            chip=self.chip, parts=self.parts)})
        (self.art / 'manifest.json').write_text(json.dumps(manifest))
        (self.art / 'build-info.json').write_text(json.dumps(
            dict(profile='esp32-elrs', version=self.version, idf_commit='x')))

    def snapshot(self):
        return {str(p.relative_to(self.web)): p.read_bytes()
                for p in sorted(self.web.rglob('*')) if p.is_file()}

    def publish(self):
        return publisher.publish(self.art, self.web, COMMIT)

    def assert_untouched(self, error):
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, error):
            self.publish()
        self.assertEqual(self.snapshot(), before)

    def test_publishes_manifest_and_table(self):
        result = self.publish()
        flash = self.web / 'flash'
        expected = {'name': 'ESP-SDR ELRS receiver', 'version': 'c' * 7,
                    'new_install_prompt_erase': True,
                    'builds': [{'chipFamily': 'ESP32', 'parts': [
                        {'path': 'firmware/esp32-elrs/' + n, 'offset': o}
                        for n, o in zip(NAMES, OFFSETS)]}]}
        self.assertEqual(json.loads((flash / 'manifest-elrs.json').read_text()), expected)
        self.assertEqual(result, expected)
        for name, data in zip(NAMES, self.payloads):
            self.assertEqual((flash / 'firmware/esp32-elrs' / name).read_bytes(), data)
        html = (flash / 'index.html').read_text()
        for part in self.parts:
            self.assertIn(part['sha256'], html)
        self.assertIn('/commit/' + COMMIT, html)
        self.assertNotIn('not published yet', html)
        self.assertTrue(html.endswith('<p>stock section</p>\n</main>\n'))
        self.assertEqual((flash / 'manifest.json').read_bytes(), STOCK_MANIFEST)

    def test_republish_replaces_block_only(self):
        self.publish()
        first = (self.web / 'flash/index.html').read_text()
        self.publish()
        self.assertEqual((self.web / 'flash/index.html').read_text(), first)
        self.assertEqual(first.count('elrs-firmware:begin'), 1)

    def test_version_mismatch_writes_nothing(self):
        self.version = 'd' * 40
        self.save()
        self.assert_untouched('version')

    def test_short_version_rejected(self):
        self.assertRaisesRegex(ValueError, 'commit', publisher.publish, self.art, self.web, 'ccccccc')

    def test_hash_mismatch_writes_nothing(self):
        (self.art / 'esp32-elrs' / NAMES[2]).write_bytes(b'tampered' * 5)
        self.assert_untouched('size|SHA-256')
        (self.art / 'esp32-elrs' / NAMES[2]).write_bytes(b'x' * len(self.payloads[2]))
        self.assert_untouched('SHA-256')

    def test_md5_mismatch_writes_nothing(self):
        self.parts[0]['md5'] = '0' * 32
        self.save()
        self.assert_untouched('MD5')

    def test_wrong_offsets_rejected(self):
        self.parts[1]['offset'] = 0x9000
        self.save()
        self.assert_untouched('offset')

    def test_wrong_chip_rejected(self):
        self.chip = 'ESP32-S3'
        self.save()
        self.assert_untouched('chip')

    def test_missing_marker_writes_nothing(self):
        (self.web / 'flash/index.html').write_text('<main></main>')
        self.assert_untouched('marker')
