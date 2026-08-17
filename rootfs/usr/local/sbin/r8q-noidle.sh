#!/bin/bash
# r8q: keep every CPU out of the idle loop.
#
# WHY. On this device a core can enter idle and never come back out. RCU then reports it as
# idle forever and it does not answer an IPI:
#
#   rcu: INFO: rcu_preempt detected stalls on CPUs/tasks:
#   rcu:   5-...0: (1 GPs behind) idle=2344/1/0x4000000000000000 softirq=59911/59914
#   Sending NMI from CPU 6 to CPUs 5:      <- no backtrace comes back
#
# (An even 'idle=' counter means RCU is not watching that CPU, i.e. it believes it is idle.)
# Everything that needs a global IPI then blocks behind it, userspace stops being scheduled,
# and the box is gone while the kernel still prints. Session 5h blamed the deep
# 'cpu-sleep-1-0' power-collapse state and disabled it on cpus 4-7; that made the failure much
# rarer but did NOT fix it — cores have since been lost with every power-collapse state
# disabled, i.e. while they were only ever allowed to sit in plain WFI (cpu5 twice, cpu2 once).
# So it is idle ENTRY itself that is unsafe here, not one particular idle state. Suspected
# firmware side: Mu-Silicium's PSCI reports 'OSI mode supported' and then refuses to be put
# into Platform-Coordinated mode ('[Firmware Bug]: failed to set PC mode: -3').
#
# WHAT THIS DOES. A SCHED_IDLE task is only picked when a CPU would otherwise have nothing to
# run, so one pinned per core means the idle loop is never entered, while real work keeps
# 100% of the CPU. Measured: WFI entries across all 8 cores drop from ~300/s to ~4/s.
#
# COST. No core ever sleeps -> constant power draw and heat. Run it for heavy work (a
# `makepkg` of anything Rust reliably wedged the phone within a minute without it, and
# completed a 220-crate build with it), not as a permanent boot service.
#
# The kernel-side equivalent, if you would rather pay this cost permanently and drop the
# spinners, is `nohlt` on the cmdline (arm64 selects GENERIC_IDLE_POLL_SETUP, so the generic
# polling idle loop is available) — that needs an Image rebuild, CONFIG_CMDLINE_FORCE is set.
set -u

ncpu=$(nproc)
pids=()

cleanup() {
	[ ${#pids[@]} -gt 0 ] && kill "${pids[@]}" 2>/dev/null
	exit 0
}
trap cleanup EXIT INT TERM

for ((c = 0; c < ncpu; c++)); do
	chrt --idle 0 taskset -c "$c" bash -c 'while :; do :; done' &
	pids+=($!)
done

echo "r8q-noidle: $ncpu SCHED_IDLE spinners pinned (cpu0-$((ncpu - 1)))"
wait
