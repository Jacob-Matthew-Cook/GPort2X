# GPort2X documentation

| document | what it covers |
|---|---|
| [STATUS.md](STATUS.md) | what works today, area by area, and the check behind each |
| [ARCHITECTURE.md](ARCHITECTURE.md) | the design: one environment behind two engines, the modules and their layers, the guest memory model and clock, processes, verification |
| [JIT.md](JIT.md) | the interpreter's just-in-time compiler for x86-64 and AArch64: design, measurements, tiering, register caching |
| [NATIVE_ENGINE.md](NATIVE_ENGINE.md) | running the guest's ARM code directly on an AArch64 CPU: what the host needs, the address space, trapping syscalls and faults, processes and threads |
| [../tests/README.md](../tests/README.md) | how the tests are built, what they compare against, and the state diffs |
| [../port/gport2x/README.md](../port/gport2x/README.md) | the players' guide for the PortMaster port on handhelds |

The design documents cite the behavioural specification GPort2X was written
from as "spec N" (its section N). The specification, `HARNESS_SPEC.md`, is
kept with the reverse-engineering project it came out of and is not part of
this repository; [LEGAL.md](../LEGAL.md) says what may and may not come from
there.

"The reference" or "the oracle" in these documents is an independent native
build of the game's logic, used only to compare frames and game state; none
of its code is in this repository.
