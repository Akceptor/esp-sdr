# ESP-SDR web tools

Static site deployed to GitHub Pages from `web/` (see `.github/workflows/pages.yml`).

- `index.html` landing page
- `elrs/` ELRS stick viewer: Web Serial to an ESP-SDR board, DK500 packets decoded in JavaScript
  (`dk500.js` protocol, `demod.js` DSP, `worker.js` decode thread, `serial.js` link, `app.js` UI)
- `flash/` ESP Web Tools flasher with the official firmware build of commit `f86da81`

Run locally (Web Serial works on `localhost`):

    python3 -m http.server -d web 8000

Tests (Node 20+; `node --test web/elrs/test/` also works):

    node --test web/elrs/test/*.test.js

`web/elrs/test/real_dk500.bin` is a real capture (6 snapshots, int8 I/Q, 16 MS/s, centre 2436 MHz) converted by
`convert_fixture.py` from `tools/elrs/testdata/real_dk500.npz` on branch `elrs-spike`.

GPL-3.0, like the rest of the repository.
