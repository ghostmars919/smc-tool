# smc-tool - Configurator for Intel MacBooks

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0)

Command-line utility to read and write Apple SMC (System Management
Controller) keys directly through I/O ports on MacBooks running Linux.

## Description

`smc-tool` is a small C program that talks to the SMC of Intel Macs without
going through the kernel `applesmc` module: it accesses the I/O ports
`0x300-0x31F` directly, using the same protocol implemented by the Linux
kernel `applesmc` driver.

It can:

- **read** the value of an SMC key (`get`);
- **write** a numeric value (0-100) into an SMC key, with automatic
  verification via read-back (`set`).

Typical use case: setting the minimum fan speed (keys like `F0Mn`) on
MacBooks where the standard drivers are not enough.

> **WARNING**: writing incorrect SMC keys may destabilize your machine or,
> in extreme cases, make it unbootable. Use this tool only if you know
> exactly what you are doing, and always at your own risk.

## Requirements

- Linux on an Intel Mac (the code uses `ioperm()`, `inb()`, `outb()`: x86 only)
- Root privileges (direct port I/O access)
- GCC or clang; no dependencies beyond the standard C library

## Build

With make:

```sh
make
```

Or directly:

```sh
gcc -O2 -Wall -Wextra -o smc src/smc-tool.c
```

## Install

```sh
sudo make install                 # installs to /usr/local/bin/smc
sudo make uninstall               # removes it
make install PREFIX=$HOME/.local  # custom location
```

## Usage

```sh
sudo ./smc get F0Mn      # reads the key (prints decimal and hex value)
sudo ./smc set F0Mn 40   # writes 40 into the key and verifies by re-reading
./smc --help             # full usage
./smc --version          # version information
```

- The key must be **exactly 4 characters** long.
- The value for `set` must be between **0 and 100**
  (`strtol` with base 0: hex notation such as `0x28` is accepted).
- After a write the value is read back: if it differs, the warning
  `[ALERT: read value not equal]` is printed.
- Exit status: `0` success, `1` runtime or permission error, `2` usage error.

## How it works (protocol)

The SMC answers on these ports:

| Port    | Role                              |
|---------|-----------------------------------|
| `0x300` | data port (`DATA_PORT`)           |
| `0x304` | command/status port (`CMD_PORT`)  |

Status bits read from `0x304`:

| Bit  | Name                | Meaning                                       |
|------|---------------------|-----------------------------------------------|
| 0x01 | `ST_AWAITING_DATA`  | a byte is ready to be read                    |
| 0x02 | `ST_IB_CLOSED`      | input buffer closed: sending is allowed       |
| 0x04 | `ST_BUSY`           | the SMC is processing a command               |

Supported commands: `0x10` (READ) and `0x11` (WRITE).

Key read sequence:

1. wait until the SMC is idle (`smc_sane`; if still busy, send a READ flush);
2. send the READ command;
3. send the key, one byte at a time;
4. send the expected length;
5. read bytes while `AWAITING_DATA | BUSY` is active;
6. drain any leftover bytes and wait for completion.

Writes follow the same pattern with the WRITE command.
Every wait uses exponential backoff (starting at 8 us, up to 24 attempts),
for a total timeout in the order of ~100 ms.

## Code structure (`src/smc-tool.c`)

| Function        | Description |
|-----------------|-------------|
| `wait_status`   | Polls the status until `(status & mask) == val`, with exponential backoff; returns `-ETIMEDOUT` on timeout |
| `send_byte`     | Waits for IB closed then BUSY active, writes a byte to the given port |
| `send_command`  | Waits for IB closed and sends a command byte to `0x304` |
| `smc_sane`      | Ensures the SMC is not busy; sends a READ flush command if needed |
| `send_argument` | Sends the 4 key characters to the data port |
| `read_smc`      | Full read of `len` bytes from the given key, with final drain |
| `write_smc`     | Writes `len` bytes into the given key |
| `main`          | Argument parsing (`get`/`set`), root check, `ioperm(0x300, 32)`, execution and output |

Current limitations:

- handles only **1-byte** keys, values **0-100**;
- ignores the key type descriptor (SMC flags/format);
- no whitelist of keys considered safe;
- no tests or packaging yet.

## Roadmap

See [docs/roadmap.md](docs/roadmap.md).

## License

Released under the **GPL-2.0-only** license — see [LICENSE](LICENSE).
