from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class CLB(SimObject):
    type = "CLB"
    cxx_header = "arch/riscv/clb.hh"
    cxx_class = "gem5::RiscvISA::CLB"

    # Functional enable for CLB-based access control. The S3K kernel can
    # still override this at runtime through the custom control CSR.
    enable = Param.Bool(False, "Enable capability lookaside buffer checks")
    clb_entries = Param.Int(8, "Number of CLB entries")

    # Instruction-level timing model. MinorCPU additionally uses
    # access_latency as a pre-LSQ delay for ordinary data accesses.
    # Local fixed-latency CLB maintenance operations such as fill/flush/read
    # rely on the core pipeline's own execution latency instead of an extra
    # CLB-side stall.
    access_latency = Param.Cycles(
        1,
        "Total latency of a CLB access-control lookup for an ordinary data access",
    )
    get_hit_latency = Param.Cycles(2, "Total latency of uclb.get on a CLB hit")
    get_ctable_latency = Param.Cycles(
        6, "Total latency of uclb.get when it reads the capability table"
    )
    delete_hit_latency = Param.Cycles(
        3, "Total latency of uclb.delete when the capability hits in the CLB"
    )
    delete_miss_latency = Param.Cycles(
        5,
        "Total latency of uclb.delete when it falls back to the capability table",
    )
    revoke_lookup_latency = Param.Cycles(
        2, "Total latency of the CLB lookup phase of uclb.revoke"
    )
    revoke_parent_ctable_latency = Param.Cycles(
        4,
        "Additional total latency when uclb.revoke must read parent metadata from the capability table",
    )
    revoke_per_child_latency = Param.Cycles(
        3,
        "Additional total latency per child capability revoked by uclb.revoke",
    )
