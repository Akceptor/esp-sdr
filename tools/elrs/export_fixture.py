"""Convert a capture .npz (iq complex, fs, freq_mhz) to the int8 interleaved I,Q fixture format.

Usage: export_fixture.py <npz> <out.bin> [--snapshots i,j,...]
"""
import argparse

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('npz')
    ap.add_argument('out')
    ap.add_argument('--snapshots', help='comma-separated snapshot indices to keep, in order')
    a = ap.parse_args()
    iq = np.load(a.npz)['iq']
    if a.snapshots:
        iq = iq[[int(s) for s in a.snapshots.split(',')]]
    out = np.empty((iq.shape[0], iq.shape[1] * 2), dtype=np.int8)
    out[:, 0::2] = np.rint(iq.real * 128)
    out[:, 1::2] = np.rint(iq.imag * 128)
    out.tofile(a.out)
    print(f'{iq.shape[0]} snapshots x {iq.shape[1]} samples -> {a.out}')


if __name__ == '__main__':
    main()
