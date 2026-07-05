# Fix 2: The "Second" Garble — the late-boot / msm-DPU modeset theory

**Device:** Samsung Galaxy S20 FE 5G Snapdragon (samsung-r8q, SM8250 / Snapdragon 865)
**Stack:** mainline Linux 6.17 (postmarketOS kernel) booted via Mu-Silicium UEFI
**Companion doc:** see `fix.md` for the primary (early-boot stride-mismatch) garble.

> **TL;DR:** There appeared to be a *second, different* garble that showed up much
> deeper into boot, and for a while we believed it was the kernel's real display driver
> (`msm` / MDSS DPU) doing a **modeset** that reprogrammed the panel and trashed the
> efifb console. We built a fix for that theory (`initcall_blacklist=msm_drm_register`).
> **That theory turned out to be WRONG.** The display stack was never running. The
> "second garble" was the **same stride-mismatch garble from `fix.md`**, just observed
> at a different time/loglevel — and it was cured by the same embedded-FDT fix. This doc
> records the investigation so the false lead doesn't get re-chased.

---

## 1. Why it looked like a second, different garble

Across the June 21–22 sessions the garble seemed to behave inconsistently, which made it
look like two separate problems:

- At **`loglevel=4`**, the screen garbled almost immediately (~0.6 s) and the device
  reset by ~10 s.
- At **`loglevel=8`**, the console stayed **readable for ~33 s of wall-time**, scrolling
  a huge flood of CoreSight `Fixed dependency cycle(s)` lines (from `fw_devlink`), and
  only *then* turned into colored static right before the reset.

That difference — clean-then-garbles-late vs garbles-immediately — is what made it look
like a distinct "late-boot" garble with its own trigger. The last readable kernel-time
before the static was ~9.2 s, right around driver init, which fed the idea that some
**driver** was doing something at that point to corrupt the framebuffer.

The natural suspect: the **`msm` DRM / MDSS DPU display driver** performing a **modeset**.
A modeset re-initializes the display controller (clocks, DSI, scanout timing). If the
kernel's real display driver "takes over" the panel that UEFI had set up for the simplefb
console, it would reprogram the hardware mid-boot and the efifb console would dissolve
into static exactly like we saw. This is a *real* and common phenomenon on Qualcomm
devices, so it was a reasonable hypothesis.

---

## 2. The fix we built for that theory (and how to reproduce it)

The plan was: stop the msm DRM driver from binding, so no modeset happens, so the efifb
console survives — letting us finally read past the garble point.

**Cmdline approach (no kernel rebuild needed):**
```fish
printf 'earlycon=efifb keep_bootcon loglevel=8 ignore_loglevel initcall_blacklist=msm_drm_register panic=0 clk_ignore_unused pd_ignore_unused' > ~/cmdline.txt
truncate -s 266 ~/cmdline.txt
# then rebuild the UKI (objcopy --update-section=.cmdline=...) and deploy to ESP
```

- `initcall_blacklist=msm_drm_register` — stops the `msm` DRM driver's initcall from
  running, so it never does its modeset.
- Escalation ladder if that name didn't catch it (msm is often **built-in**, not a
  module, so `modprobe.blacklist` alone won't work):
  - `modprobe.blacklist=msm`
  - blacklisting the specific DPU/MDSS initcall symbol
  - or disabling the `mdss` / `dpu` DT node directly.

---

## 3. Why the theory was WRONG (this is the important part)

Two independent findings killed the msm/DPU-modeset explanation:

### (a) The display controller was already disabled upstream
`display-subsystem@ae00000` (`qcom,sm8250-mdss`) ships with **`status = "disabled";`** in
the upstream DTS. With the MDSS parent disabled, the DPU/DSI never probe — **there is no
`msm` display driver running at all** to perform a modeset. There was nothing to
blacklist; `initcall_blacklist=msm_drm_register` was blocking a driver that was never
going to bind in the first place. The "display driver taking over" could not be happening.

### (b) After the embedded-FDT fix, the garble was simply gone — everywhere
Once we applied the real fix from `fix.md` (embed the DTB in UEFI so the firmware owns
both the GOP framebuffer and the FDT, and strip `.dtb` from the UKI), the boot came up as
a **clean, readable text console the entire time** — no static at 0.6 s, none at 33 s,
none "at the end." If a late-boot modeset had been the cause, fixing an *early-boot*
stride mismatch would not have made the *late* garble disappear. It disappeared because
there was only ever **one** garble.

### So what actually explained the "late" appearance?
It was the **same stride/format mismatch** described in `fix.md`, and the *timing* was an
artifact of logging, not a second cause:
- At `loglevel=8`, the CoreSight `fw_devlink` flood kept the console scrolling readable
  text for ~33 s of wall-time before the mis-strided region of the framebuffer filled the
  visible area — making the garble *look* like a late event.
- At `loglevel=4`, without that flood, the mismatch was visible almost immediately (~0.6 s).

Same root cause (UEFI GOP ↔ injected-DTB stride disagreement), two different apparent
onset times. One garble, not two.

---

## 4. What to take away / do NOT re-chase

- **Do not** add `initcall_blacklist=msm_drm_register`, `modprobe.blacklist=msm`, or
  `nomodeset` to "fix the garble." They target a driver that never runs on this DT and
  will not help. (They're harmless, but they're cargo-cult here.)
- **Do not** disable the `mdss`/`dpu` node for garble reasons — it's already disabled
  upstream, and it was never the cause.
- The garble was **structurally** fixed by making UEFI the single source of truth for the
  framebuffer geometry (embedded FDT + no-DTB UKI). See `fix.md`. There is no separate
  "second garble" fix to apply.

---

## 5. Related but SEPARATE issue this investigation surfaced (don't confuse it)

While chasing the "late garble," the same ~9 s reset was also being investigated. That
reset is a **genuinely different problem** from the garble and was NOT solved by any of
the above:
- The reset was traced to the **APSS watchdog** biting (re-enabling `qcom_wdt` /keeping
  the watchdog node enabled stopped the early reset loop), and then to a chain of
  **Qualcomm clock-controller probe stalls** (`camcc` at `ad00000`, `dispcc` at
  `af00000`, …) that spin ~50 ms and trip the watchdog — fixed one-by-one by disabling
  each offending clock-controller node in the embedded DTB.
- That work is tracked in `handoff.md` (§6 "the loop" and §8 "diagnostic history"), not
  here. Keep the **garble** (a *display-geometry* problem, solved) mentally separate from
  the **reset** (a *watchdog/clock-controller* problem, in progress).

---

## 6. One-paragraph summary

The apparent "second garble" — colored static appearing deep into boot — was blamed on
the `msm`/MDSS DPU display driver doing a modeset and stealing the panel from the efifb
console. We prepared to disable it (`initcall_blacklist=msm_drm_register` and escalations).
But the MDSS node is `status="disabled"` upstream, so the display driver never runs, and
after the embedded-FDT fix from `fix.md` the garble vanished at *every* timestamp. The
conclusion: there was only ever **one** garble — the UEFI-GOP-vs-injected-DTB stride
mismatch — and its "late" appearance at `loglevel=8` was just the CoreSight devlink flood
delaying when the mis-strided region scrolled into view. No separate fix is needed or
correct; do not re-chase the msm/DPU modeset theory.
