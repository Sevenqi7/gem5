import os

import m5
from m5.objects import *

# ==========================================
# 1. 基础系统与时钟配置
# ==========================================
system = System()

# 设置时钟和电压域 (50MHz)
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "50MHz"
system.clk_domain.voltage_domain = VoltageDomain()

# 使用 timing 内存模式
system.mem_mode = "timing"

# ==========================================
# 2. 内存物理地址映射 (核心)
# ==========================================
# 必须在这里显式声明所有内存组件的范围，供系统总线路由使用
system.mem_ranges = [
    AddrRange(start=0x80000000, size="1GiB"),  # DDR4 主存范围
    AddrRange(start=0x10000000, size="64KiB"),  # SPM 范围
    AddrRange(start=0x2040000, size="64KiB"),  # CLINT
    AddrRange(start=0x3002000, size="8"),
]
# system.badaddr = BadAddr(range=AddrRange(0x0, 0x10000000))

# ==========================================
# 3. 处理器与系统总线
# ==========================================
# 对应 SimpleProcessor(cpu_type=CPUTypes.TIMING)
system.cpu = RiscvTimingSimpleCPU()
system.platform = HiFive()

# Match the Cheshire kernel's build-time RTC_HZ value.
system.platform.rtc = RiscvRTC(frequency=Frequency("1MHz"))

# 创建系统主交叉开关 (System XBar)
system.membus = SystemXBar()

# 为最简起见，这里先不连接 Cache，直接将 CPU 连到总线
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports

# RISC-V 必须配置中断控制器，否则无法处理异常
system.cpu.createThreads()
system.cpu.createInterruptController()

# 系统端口（用于加载 ELF 等初始操作）
system.system_port = system.membus.cpu_side_ports

# ==========================================
# 4. 内存设备实例化与连线
# ==========================================
# 4.1 主存 (DDR4)
# Clean S3K baseline: no CLB SimObject is instantiated in this configuration.
# Allow command-line overrides through the environment for quick benchmark
# switching.
app_path = os.environ.get(
    "S3K_APP_PATH",
    "/gem5/configs/s3k/cheshire/hello-cheshire-uart1.bin",
)

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR4_2400_8x8(
    range=system.mem_ranges[0], image_file=app_path
)
system.mem_ctrl.port = system.membus.mem_side_ports

# 4.2 Scratchpad Memory (SPM)
system.spm = SimpleMemory(
    range=system.mem_ranges[1], latency="1ns", bandwidth="64GiB/s"
)
system.spm.port = system.membus.mem_side_ports


# 4.3 Configuration of HiFive peripherals
# Unused in s3k

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
# system.platform.uart.pio_size = 0x20
system.platform.uart.pio = system.membus.mem_side_ports
system.platform.terminal = Terminal(outfile="file")
system.platform.uart.device = system.platform.terminal

# ==========================================
# 5. 纯裸机 Workload 配置 (摆脱所有束缚)
# ==========================================
s3k_kernel_path = os.environ.get(
    "S3K_KERNEL_PATH",
    "/gem5/configs/s3k/cheshire/s3k.elf",
)


# 直接使用 RiscvBareMetal，不会有任何多余的 kernel_addr 检查
system.workload = RiscvBareMetal(
    bootloader=s3k_kernel_path, reset_vect=0x10000000, bare_metal=True
)
# system.workload.wait_for_remote_gdb = True


# ==========================================
# 6. 实例化与运行
# ==========================================
# 必须指定 full_system=True
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
