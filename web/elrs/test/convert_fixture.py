"""One-off: convert tools/elrs/testdata/real_dk500.npz (branch elrs-spike) to the int8 fixture used by decode.test.js.

Usage: python3 convert_fixture.py path/to/real_dk500.npz
"""
import json
import os
import sys

import numpy as np

z = np.load(sys.argv[1])
iq = z['iq']
out = np.empty((iq.shape[0], iq.shape[1] * 2), dtype=np.int8)
out[:, 0::2] = np.rint(iq.real * 128)
out[:, 1::2] = np.rint(iq.imag * 128)
here = os.path.dirname(os.path.abspath(__file__))
out.tofile(os.path.join(here, 'real_dk500.bin'))
with open(os.path.join(here, 'real_dk500.json'), 'w') as f:
    json.dump(dict(snapshots=int(iq.shape[0]), samples=int(iq.shape[1]), fs=float(z['fs']),
                   freqMhz=int(z['freq_mhz']), format='int8 interleaved I,Q (adc/128)'), f, indent=1)
