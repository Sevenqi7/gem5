import os

import m5
from m5.objects import *
from m5.params import *

from gem5.components.boards.arm_board import ArmBoard
from gem5.components.boards.riscv_board import RiscvBoard
from gem5.components.cachehierarchies.classic.private_l1_cache_hierarchy import (
    PrivateL1CacheHierarchy,
)
from gem5.components.memory.simple import SimpleMemory
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import *
from gem5.simulate.simulator import Simulator

# from gem5.components.boards.riscv_board


class CheshireBoard(RiscvBoard):
    def __init__(self, clk_freq, processor, memory, cache_hierarchy):
        super().__init__(
            clk_freq=clk_freq,
            processor=processor,
            memory=memory,
            cache_hierarchy=cache_hierarchy,
        )

        def _setup_memory_ranges(self):
            super()._setup_memory_ranges()
            self.mem_ranges_append(AddrRange(start=0x10000000, size="128KiB"))

        def _setup_io_devices(self):
            super()._setup_io_devices()
            self.uart.pio_addr = 0x03002000

        def _setup_board(self):
            super()._setup_board()
            self.spm = SimpleMemory(
                range=AddrRange(start=0x10000000, size="128KiB"),
                latency="1ns",
                bandwidth="64GB/s",
            )

            self.spm.port = self.iobus.mem_side_ports


cache_hierarchy = PrivateL1CacheHierarchy(l1d_size="32KiB", l1i_size="16KiB")

memory = SingleChannelDDR4_2400(size="1GiB")

processor = SimpleProcessor(
    cpu_type=CPUTypes.TIMING, isa=ISA.RISCV, num_cores=1
)

# processor.cores[0].get_isa().

board = CheshireBoard(
    clk_freq="50MHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)


s3k_kernel_path = "/gem5/configs/s3k/s3k.elf"
app_path = "/gem5/configs/s3k/app1.elf"
dummy_image = DiskImageResource(
    local_path="/gem5/configs/s3k/dummy.img", root_partition=""
)

s3k_kernel = KernelResource(local_path=s3k_kernel_path)
board.set_kernel_disk_workload
board.set_workload(
    WorkloadResource(
        function="set_kernel_disk_workload",
        parameters={
            "kernel": s3k_kernel,
            "disk_image": dummy_image,
        },
        local_path=s3k_kernel_path,
    )
)
print(f"{hex(board.workload.entry_point)}")

board.workload = RiscvBareMetal(
    bootloader=s3k_kernel_path, reset_vect=0x10000000, extras=[app_path]
)

# board.workload.entry_point = 0x10000000
# board.workload.kernel_addr = 0x10000000
# board.workload.reset_vect = 0x10000000
# print(f"{hex(board.workload.entry_point)}")


print("Instantiating the Cheshire SoC simulation...")
simulator = Simulator(board=board)

print("Beginning simulation!")
exit_event = simulator.run()
print("Simulation Ends!")

print(
    f"Exited simulation at tick {m5.curTick()} because: {exit_event.getCause()}"
)
