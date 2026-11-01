# SPDX-License-Identifier: GPL-3.0-or-later
"""Hand-operated PTT: the core must not give up on ACKs while a human is getting ready to key the radio."""
import ctypes as C
import numpy as np
from test_api import A, N


def burst(n, t_ms):
    k = C.c_int()
    p = A.hl_poll_tx(n.c, t_ms, 0.5, C.byref(k))
    return k.value if p else 0


def test_pending_counts_queued_frames():
    a = N("W1AAA", seed=1)
    assert A.hl_pending_tx(a.c) == 0
    A.hl_send(a.c, b"W2BBB", 1, b"x" * 300, 300, 1, 0)       # 4 fragments of 96 bytes
    assert A.hl_pending_tx(a.c) == 4
    assert burst(a, 0) > 0
    assert A.hl_pending_tx(a.c) == 0


def retries_after(manual, seconds):
    a = N("W1AAA", seed=1)
    if manual: A.hl_set_manual_ptt(a.c, 1)
    A.hl_send(a.c, b"W2BBB", 1, b"hello", 5, 1, 0)
    assert burst(a, 0) > 0                                   # first transmission; nobody ever ACKs
    for t in range(0, seconds * 1000, 100):
        A.hl_process_audio(a.c, None, 0, t, 0.004)
        burst(a, t)
    return a.stats()["retries"]


def test_manual_mode_waits_much_longer_before_retrying():
    assert retries_after(manual=False, seconds=30) >= 2      # automatic PTT: gives up quickly
    assert retries_after(manual=True, seconds=30) == 0       # a human needs time to key an ACK
    assert retries_after(manual=True, seconds=70) >= 1       # but it does still retry eventually


def test_manual_flag_survives_modem_and_link_changes():
    a = N("W1AAA", seed=1)
    A.hl_set_manual_ptt(a.c, 1)
    assert A.hl_set_modem(a.c, 1) == 0                       # switching modem must not reset the long timeout
    A.hl_send(a.c, b"W2BBB", 1, b"hello", 5, 1, 0)
    assert burst(a, 0) > 0
    for t in range(0, 30_000, 100):
        A.hl_process_audio(a.c, None, 0, t, 0.004)
        burst(a, t)
    assert a.stats()["retries"] == 0
