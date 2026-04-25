import os

import m5
from m5.objects import *

# ==========================================
# 1. Basic system and clock configuration
# ==========================================
system = System()

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "50MHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"

# ==========================================
# 2. Physical address map
# ==========================================
system.mem_ranges = [
    AddrRange(start=0x80000000, size="1GiB"),
    AddrRange(start=0x10000000, size="64KiB"),
    AddrRange(start=0x2040000, size="64KiB"),
    AddrRange(start=0x3002000, size="8"),
]

# ==========================================
# 3. CPU and interconnect
# ==========================================
system.cpu = RiscvTimingSimpleCPU()
system.cpu.mmu.clb = CLB(enable=False, clb_entries=8)
system.platform = HiFive()

system.platform.rtc = RiscvRTC(frequency=Frequency("1MHz"))

system.membus = SystemXBar()

system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports

system.cpu.createThreads()
system.cpu.createInterruptController()

system.system_port = system.membus.cpu_side_ports

# ==========================================
# 4. Memory devices
# ==========================================
app_path = os.environ.get(
    "S3K_APP_PATH",
    "/gem5/configs/s3k/cheshire_clb/uclb_get_test-cheshire-uart1.bin",
)

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_8x8(
    range=system.mem_ranges[0], image_file=app_path
)
system.mem_ctrl.port = system.membus.mem_side_ports

system.spm = SimpleMemory(
    range=system.mem_ranges[1], latency="1ns", bandwidth="64GiB/s"
)
system.spm.port = system.membus.mem_side_ports

system.platform.pci_host.pio = system.membus.mem_side_ports
system.platform.plic.pio_addr = 0xFF000000
system.platform.plic.n_src = 1
system.platform.plic.pio = system.membus.mem_side_ports

system.platform.clint = Clint(pio_addr=0x2040000, num_threads=1)
system.platform.clint.pio_addr = 0x2040000
system.platform.clint.num_threads = 1
system.platform.clint.pio = system.membus.mem_side_ports
system.platform.clint.int_pin = system.platform.rtc.int_pin

system.platform.uart = Uart8250(pio_addr=0x03002000)
system.platform.uart.pio = system.membus.mem_side_ports
system.platform.terminal = Terminal(outfile="file")
system.platform.uart.device = system.platform.terminal

# ==========================================
# 5. Bare-metal workload
# ==========================================
s3k_kernel_path = os.environ.get(
    "S3K_KERNEL_PATH",
    "/gem5/configs/s3k/cheshire_clb/s3k.elf",
)

system.workload = RiscvBareMetal(
    bootloader=s3k_kernel_path, reset_vect=0x10000000, bare_metal=True
)

# ==========================================
# 6. Instantiate and run
# ==========================================
root = Root(full_system=True, system=system)

print("Instantiating the Cheshire SoC classic simulation...")
m5.instantiate()

print(
    f"Verified Entry Point / Reset Vector: {hex(system.workload.reset_vect)}"
)
print("Beginning simulation!")

exit_event = m5.simulate()

print("Simulation Ends!")
print(
    f"Exited simulation at tick {m5.curTick()} because: {exit_event.getCause()}"
)
