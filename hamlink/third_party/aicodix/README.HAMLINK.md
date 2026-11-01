# Vendored: Rattlegram COFDMTV modem (aicodix)

Source: https://github.com/aicodix/rattlegram  `app/src/main/cpp/*.hh` (commit recorded in the
HamLink README), which bundles headers from aicodix/dsp and aicodix/code. License: the permissive
"Permission to use, copy, modify, and/or distribute ... with or without fee" grant (0BSD-style);
see LICENSE.* in this directory.

Only change: `encoder.hh` `configure()` now takes an explicit payload length (was: scan for a NUL
byte), because HamLink payloads are binary. `native-lib.cpp` and the Android app are not included.
