# Vendored: Rattlegram COFDMTV modem (aicodix)

Source: https://github.com/aicodix/rattlegram  `app/src/main/cpp/*.hh` (commit recorded in the
HamLink README), which bundles headers from aicodix/dsp and aicodix/code. License: the permissive
"Permission to use, copy, modify, and/or distribute ... with or without fee" grant (0BSD-style);
see LICENSE.* in this directory.

Changes:
* `encoder.hh` `configure()` takes an explicit payload length (was: scan for a NUL byte), because HamLink
  payloads are binary.
* `schmidl_cox.hh`: the sync detector's power floor `min_R` is `1e-9 * symbol_len` (was `0.00001`, about
  -47 dBFS). Below the old floor no packet was ever detected, whatever its signal-to-noise ratio, and audio from a
  radio's speaker output or a laptop microphone often arrives quieter than that. Test:
  `tests/test_ofdm.py::test_ofdm_decodes_quiet_input_as_well_as_loud`. `native-lib.cpp` and the Android app are not included.
