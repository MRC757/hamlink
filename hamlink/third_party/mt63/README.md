# MT63 (from fldigi)

The MT63 modem by Pawel Jalocha, SP9VRC, as maintained in fldigi by Dave Freese, W1HKJ.
Source: https://github.com/w1hkj/fldigi, `src/mt63/` and `src/include/`, commit
61b97f4133c488063f3de1795c894d22d5032e8a. Licence: GPL-3.0-or-later (see the file headers), the same as HamLink.

Files: `dsp.cxx`, `dsp.h`, `mt63base.cxx`, `mt63base.h`, `symbol.dat` (symbol shape), `mt63intl.dat` (interleave
patterns). `config.h` is ours: a stand-in for fldigi's generated header. HamLink's wrapper is `core/mt63.cpp`.

## HamLink changes (marked "HamLink change" in the code)
These fix initialisation bugs that fldigi never hits because it keeps long-lived objects and calls `Preset` again
before each use. HamLink creates fresh objects, where they made the transmitter's output depend on leftover memory and
reception fail.
* `dsp.h`: `dspRedspAllocArray` / `dspAllocArray` zero-initialise the arrays they allocate.
* `dsp.h`: `dspSeq::EnsureSpace` keeps the existing contents when it grows (callers append after growing; growth used
  to lose them), frees the old buffer (it leaked) and zero-fills; the destructor uses `delete[]` to match `new[]`.
* `mt63base.cxx`: `MT63tx::Preset` recomputes the FFT index mask after `FFT.Preset` (it was taken from an uninitialised
  FFT size on first use).

The wrapper also constructs `MT63tx` / `MT63rx` in zeroed memory, because many scalar members are not set by their
constructors, and uses fldigi's default receiver integration of 16 symbols: the optional 32 failed to decode even a
clean signal in our tests.
