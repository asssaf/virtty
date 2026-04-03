# virtty

`virtty` is a Linux kernel module that provides a virtual TTY device proxying data to and from an arbitrary "slave" TTY (e.g., `ttyS0`). It allows users to create virtual TTYs configurable via the kernel command line, supporting both standard TTY operations and system console output.

## Features

- **1:1 Proxying:** Directly links a virtual device (e.g., `virtty0`) to a physical slave TTY (e.g., `ttyS0`).
- **Command-line Configuration:** Configurable during boot using the `virttyN=slave,options` syntax.
- **Console Support:** Can be used as a system console via the `console=virttyN` kernel parameter.
- **Raw Configuration Passing:** Slave options are passed directly to the underlying slave console's setup function, supporting diverse TTY types and options without parsing.
- **Line Discipline 29:** Uses the `N_DEVELOPMENT` line discipline (29) for efficient input interception from the slave device.
- **DKMS Support:** Ready for building against modern Linux kernels (e.g., 6.12.25) using DKMS.

## Usage

`virtty` can be configured as a built-in driver or a loadable module.

### Built-in Usage

When built into the kernel, use the following syntax in the kernel command line:

```
virtty0=ttyS0,115200 console=virtty0
```

This will:
1. Initialize `virtty0` as a proxy for `ttyS0`.
2. Pass the `115200` option to the `ttyS` console setup function.
3. Register `virtty0` as a system console.

### Module Usage

When loaded as a module, use module parameters to configure instances:

```bash
sudo modprobe virtty v0=ttyS0,115200
```

Or via the kernel command line:

```
virtty.v0=ttyS0,115200 console=virtty0
```

**Note:** For `console=virttyN` to capture early boot messages (before modules are loaded), the `virtty` module must be built into the kernel image. As a loadable module, it will start proxying console output as soon as it is loaded and initialized.

## Building

### Using the Makefile

To build the module for your currently running kernel:

```bash
cd virtty
make
```

To build for a specific kernel version:

```bash
make KVERSION=6.12.25
```

### Using DKMS

To install the module using DKMS:

```bash
sudo dkms add ./virtty
sudo dkms install virtty/1.0
```

## Technical Details

- **Device Name:** `virtty`
- **Major Number:** Dynamically allocated.
- **Line Discipline:** 29 (`N_DEVELOPMENT`).
- **Architecture:** The module creates a TTY driver for `virtty` devices and attaches a custom line discipline to the configured slave device to intercept incoming data. Console output is proxied directly to the slave console to ensure safety across different execution contexts.

## License

GPL
