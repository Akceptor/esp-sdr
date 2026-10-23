#!/usr/bin/env python3
"""Publish the CI-built esp32-elrs firmware into web/flash (stdlib only).

--artifact-dir is the directory that holds manifest.json, build-info.json and
esp32-elrs/*.bin: the firmware-esp32-elrs CI artifact, or DIR/esp32-elrs when
DIR was given to tools/build_firmware.py --output. Without it the artifact is
fetched with `gh run download`.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PROFILE = 'esp32-elrs'
REPO = 'https://github.com/Akceptor/esp-sdr'
PARTS = [('0-bootloader.bin', 0x1000), ('1-partition-table.bin', 0x8000), ('2-esp_sdr.bin', 0x10000)]
BEGIN, END = '<!-- elrs-firmware:begin -->', '<!-- elrs-firmware:end -->'


def verify(artifact_dir, commit):
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('commit must be a full 40-hex revision')
    info = json.loads((artifact_dir / 'build-info.json').read_text())
    if info.get('profile') != PROFILE:
        raise ValueError(f"build-info profile is {info.get('profile')!r}, expected {PROFILE!r}")
    if info.get('version') != commit:
        raise ValueError(f"build-info version {info.get('version')!r} differs from commit {commit}")
    variant = json.loads((artifact_dir / 'manifest.json').read_text())['variants'][PROFILE]
    if variant.get('chip') != 'ESP32':
        raise ValueError(f"chip is {variant.get('chip')!r}, expected 'ESP32'")
    parts = variant['parts']
    if [(p['name'], p['offset']) for p in parts] != PARTS:
        raise ValueError('parts must be exactly ' + ', '.join(f'{n} at {o:#x}' for n, o in PARTS) + ' (offset/name mismatch)')
    payloads = []
    for part in parts:
        data = (artifact_dir / PROFILE / part['name']).read_bytes()
        if len(data) != part['size']:
            raise ValueError(f"{part['name']}: size differs from manifest")
        if hashlib.sha256(data).hexdigest() != part['sha256']:
            raise ValueError(f"{part['name']}: SHA-256 differs from manifest")
        if hashlib.md5(data).hexdigest() != part['md5']:
            raise ValueError(f"{part['name']}: MD5 differs from manifest")
        payloads.append((part, data))
    return payloads


def render_block(commit, payloads):
    rows = ''.join(
        f'  <tr><td><a href="firmware/{PROFILE}/{p["name"]}">{p["name"]}</a></td>'
        f'<td>{p["offset"]:#x}</td><td><code>{p["sha256"]}</code></td></tr>\n'
        for p, _ in payloads)
    return (f'{BEGIN}\n'
            f'<p>\n  ELRS receiver build of commit <a href="{REPO}/commit/{commit}"><code>{commit[:7]}</code></a>, '
            f'built by CI. Source: <a href="{REPO}">Akceptor/esp-sdr</a>. Licensed under GPL-3.0.\n</p>\n'
            f'<table>\n  <tr><th>File</th><th>Offset</th><th>SHA-256</th></tr>\n{rows}</table>\n'
            f'{END}')


def publish(artifact_dir, web_root, commit):
    payloads = verify(artifact_dir, commit)
    flash = web_root / 'flash'
    page = (flash / 'index.html').read_text()
    start, stop = page.find(BEGIN), page.find(END)
    if start < 0 or stop < start or page.count(BEGIN) != 1 or page.count(END) != 1:
        raise ValueError('index.html needs exactly one elrs-firmware begin/end marker pair')
    page = page[:start] + render_block(commit, payloads) + page[stop + len(END):]
    manifest = {'name': 'ESP-SDR ELRS receiver', 'version': commit[:7], 'new_install_prompt_erase': True,
                'builds': [{'chipFamily': 'ESP32', 'parts': [
                    {'path': f'firmware/{PROFILE}/{p["name"]}', 'offset': p['offset']} for p, _ in payloads]}]}
    images = flash / 'firmware' / PROFILE
    images.mkdir(parents=True, exist_ok=True)
    for part, data in payloads:
        (images / part['name']).write_bytes(data)
    (flash / 'manifest-elrs.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (flash / 'index.html').write_text(page)
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run_id', help='GitHub Actions run id of the green Build firmware workflow')
    parser.add_argument('--artifact-dir', type=Path,
                        help='already-downloaded artifact directory (skips gh run download)')
    args = parser.parse_args()
    commit = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], check=True,
                            capture_output=True, text=True).stdout.strip()
    if args.artifact_dir:
        publish(args.artifact_dir, ROOT / 'web', commit)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(['gh', 'run', 'download', args.run_id, '-n', 'firmware-' + PROFILE, '-D', tmp], check=True)
            publish(Path(tmp), ROOT / 'web', commit)
    print('Published', commit[:7])
