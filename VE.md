# NEC SX-Aurora TSUBASA Vector Engine (VE) in NVTOP

This fork of [Syllo/nvtop](https://github.com/Syllo/nvtop) adds NEC SX-Aurora TSUBASA Vector Engine support.
It is not affiliated with NEC and the change has not been submitted upstream. The difference from upstream `master` is
[here](https://github.com/Syllo/nvtop/compare/master...tka3220:nvtop-ve:ve-support).

## Requirements

The NEC VEOS packages must be installed. The backend runs these tools from `/opt/nec/ve/bin` at runtime and needs no
additional library at build time:

| Tool | Used for |
|---|---|
| `vecmd info` | clocks, temperature, fan, 12 V voltage and current, memory size, PCIe link |
| `ve-mpstat` | device utilization |
| `ve-top` | memory in use, process list |

VEs are discovered in `/sys/class/ve`. Without it no VE is shown. The backend is built by default on Linux; use
`-DVE_SUPPORT=OFF` to leave it out.

## What is shown

| NVTOP item | Value for a VE |
|---|---|
| Clock (`GPU`, `MEM`) | `Core Clock` and `Memory Clock`; the maximum shown is the same value |
| Utilization | average over all cores (`ve-mpstat`); one busy core of eight is 12.5 % |
| Temperature | highest `Chip Core` temperature |
| Fan | RPM, and a percentage of the rated 10600 RPM (`ve_hw_spec.yaml`). This is not the PWM duty |
| Power | AUX 12 V and Edge 12 V rails, voltage times current |
| Memory | total from `Memory Size`, used from `ve-top` |
| PCIe | current link generation and width |
| Processes | the VE processes listed by `ve-top`, with user, command and `%CPU` as usage and `RES` as memory |

## Not shown

The power limit, temperature thresholds, memory utilization, PCIe RX/TX throughput and encoder/decoder usage are not
reported. The VE tools, the NEC operation guide and `/sys/class/ve` do not expose these values.

## Notes

- The process usage is the `%CPU` from `ve-top`: 100 % is one fully used core. The device utilization is the average
  over all cores, so the two scales differ.
- The `PID` of a process is the PID of the `ve_exec` process on the host.
- `F9` sends the signal to that host PID. Choosing `15 SIGTERM` for a VE process of the same user ended the VE job
  (tested with a busy loop on VE 0). NVTOP does not check the result of `kill()`, so a signal that the kernel refuses,
  for example to a process of another user, fails without a message. The refusal itself was checked with a host
  process of another user, not with another user's VE job.
- `ve-mpstat` and `ve-top` select the VE with the environment variable `VE_NODE_NUMBER`. The `-N` option did not select
  VE 1 in the tested environment. This variable is not described in NEC's guide.
- The fan percentage uses RPM divided by the rated maximum. The PWM duty of the fan controller is not available.

## Tested environment

Two VE Type 10C cards (VE driver 3.6.1, VMCFW 1.5.18, `vecmd` 1.3.92) on a RHEL 8 compatible host. Other VE models and
VEOS versions have not been tested.

## Build

Build this fork like NVTOP (see the [build section of the README](README.md#nvtop-build)), cloning
`https://github.com/tka3220/nvtop-ve.git` instead of the upstream repository.
