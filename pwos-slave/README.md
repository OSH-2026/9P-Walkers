# PWOS STM32F407 Slave

`pwos-slave` is the STM32F407 slave firmware template. It combines the
CubeMX-generated board layer with the shared Mini9P, mesh, storage, and VFS
modules used by the rest of the repository.

## Layout

- `Core/` contains STM32CubeMX generated HAL startup, peripheral init, and
  `main.c`.
- `User/app/` contains board bring-up glue such as `mesh_node_mini9p_init.c`,
  `fs_selftest.c`, and optional mesh diagnostics.
- `User/mesh/` contains the slave mesh service/runtime integration. It binds
  shared mesh code to STM32 UART transports.
- `User/backend/` contains node-local VFS backends exposed through Mini9P.
- `User/fs/` and `User/drivers/storage/` contain littlefs and storage ports.
- Shared protocol and routing code is pulled from `../pwos-shared/`.

## Build Presets

Use the CMake presets from this directory:

```sh
cmake --preset Debug
cmake --build --preset Debug
```

`Debug` is the normal local firmware configuration. It does not start the
Mini9P mesh serial service by default.

For the ZGT6 board Mini9P/mesh bring-up:

```sh
cmake --preset ZGT6Debug
cmake --build --preset ZGT6Debug
```

`ZGT6Debug` enables:

- `PWOS_BOARD_ZGT6=ON`
- `PWOS_ENABLE_MINI9P_SERIAL=ON`
- `PWOS_SKIP_LFS_MOUNT=ON`

The helper script defaults to `ZGT6Debug`:

```sh
pwos-slave/build.sh build
pwos-slave/build.sh flash
pwos-slave/build.sh flash-test
```

`flash-test` flashes the board, starts `tools/pc_master_emulator`, then expects
you to reset the board after the emulator prints its waiting message.

## Mesh Serial Mode

When `PWOS_ENABLE_MINI9P_SERIAL` is set, `main.c` starts
`mesh_node_mini9p_init()` and then polls `mesh_node_service_poll_once()` in the
main loop. The normal VOFA/firewater and filesystem self-test loop is disabled.

Current F407 mesh UART configuration is:

- `port0`: `USART2` (`PA2 TX`, `PA3 RX`) for the host/PC upstream link.
- `port1`: `USART1` (`PA9 TX`, `PA10 RX`) for a downstream slave link.

Both ports run at `1000000` baud. For a single-node PC smoke test, connect the
PC USB-UART adapter to `USART2`.

## PC Master Emulator

Build and run the PC host emulator from the repository root:

```sh
cmake -S tools/pc_master_emulator -B tools/pc_master_emulator/build
cmake --build tools/pc_master_emulator/build
tools/pc_master_emulator/build/pc_master_emulator /dev/ttyUSB0 1000000 1
```

The third argument is the number of nodes to wait for. Use `2` when testing a
two-slave chain.

## Mesh Diagnostics

Mesh diagnostics are compiled out by default. Enable them only when debugging
board bring-up:

```sh
cmake --preset ZGT6Debug -DPWOS_ENABLE_MESH_DIAG=ON
cmake --build --preset ZGT6Debug
```

Diagnostics print blocking text through `USART1`. Because `USART1` is also
`mesh port1`, this can disturb timing and mix text with binary mesh frames.
Keep `PWOS_ENABLE_MESH_DIAG=OFF` for normal mesh tests and relay-chain tests.

## Notes

- Do not add static neighbor address configuration to the slave. Slave
  `addr -> port` mappings are learned at runtime.
- Shared mesh or Mini9P protocol changes should be tested with the PC tests
  under `pwos-master-esp32p4/vfs_bridge/test/` and
  `pwos-shared/mesh/module test(on PC)/`.
