# Fix 3: The display is SOLVED — persistent live mainline framebuffer on r8q

**Device:** Samsung Galaxy S20 FE 5G Snapdragon (samsung-r8q, SM8250 / Snapdragon 865)
**Stack:** mainline Linux (7.1.2) booted via Mu-Silicium UEFI, upstream `simple-framebuffer`
**Companion docs:** `fix.md` (early-boot stride-mismatch garble — not our failure mode),
`fix2.md` (the false "msm modeset" lead + the real clock-controller insight in §5).
**Status:** ✅ **WORKING.** Live-updating fbcon console and `/dev/fb0` writes appear on the
panel and persist for the whole session. This is the first known working Linux display on r8q
via Mu-Silicium (Mu-Silicium's own r8q page lists "Linux Boot ❌").

> **TL;DR — the fix is two DT changes + the right cmdline:**
> 1. Keep `dispcc` **enabled** but add `protected-clocks = <0 … 57>` to it, so the qcom clock
>    driver registers the controller (no `fw_devlink` hang) yet never touches the display
>    clocks that Samsung's firmware left driving the live DSI scanout.
> 2. Add `power-domains = <&dispcc MDSS_GDSC>` (and **no clocks**) to the
>    `simple-framebuffer@9c000000` node, so `simpledrm` becomes an active consumer of the MDSS
>    GDSC and Linux never powers it off — the DPU keeps pushing frames.
> 3. Boot with `simpledrm` **enabled** and cmdline
>    `clk_ignore_unused pd_ignore_unused arm-smmu.disable_bypass=0` (plus
>    `earlycon=efifb keep_bootcon console=tty0`). **No** dispcc/simpledrm/msm blacklists.

---

## 1. The setup (what makes r8q's display weird)

- Mu-Silicium's firmware runs **two** things that touch the screen: Samsung's **binary
  DisplayDxe** (programs the real DPU/DSI and leaves it scanning out of the splash buffer at
  `0x9C000000`) and SiliciumPkg's **SimpleFbDxe** (a UEFI GOP at the same buffer).
- The panel (EA8076A / AMS646UJ10 FHD AMOLED) is **DSI command-mode**: it only displays frames
  that are actively *pushed* to it. After ExitBootServices the DPU keeps pushing (self-refresh
  from `0x9C000000`), so the upstream `simple-framebuffer@9c000000` node shows a live image —
  *as long as nothing disturbs the DPU's clocks or power*.
- We embed our own kernel-built DTB inside the Mu-Silicium firmware (via `DtPlatformDxe` +
  a FREEFORM DTB in `r8q.fdf`), so UEFI is the single source of truth for both the GOP
  framebuffer and the FDT. Geometry agrees end-to-end (1080×2400, stride 4320, `a8r8g8b8`).
  That means the `fix.md` stride/format garble is **not** our failure mode.

So the display hardware is already alive at kernel entry. The whole problem was: **mainline's
power management tears down the DPU's clocks and power domain a few seconds into boot**, and the
command-mode panel freezes on its last frame.

---

## 2. The failure, bisected

We had a live `/dev/fb0` and a telnet shell over USB the entire time (the *system* never
crashed — only the *scanout* died), which let us pin each step exactly:

| Config | Scanout dies at | Cause (proven) |
|---|---|---|
| plain upstream DT | **~11.6 s** | `dispcc` (display clock controller) probes and reconfigures the byte/pixel/MDP clocks feeding the live DSI scanout |
| + `protected-clocks` on `dispcc` | **~13.3 s** | Linux powers off the **MDSS GDSC** power domain (`pm_genpd_summary` showed `mdss_gdsc → off`); `pd_ignore_unused` does **not** prevent this |
| + `power-domains = <&dispcc MDSS_GDSC>` on the fb node | **never** | `simpledrm` holds `mdss_gdsc` **on** → DPU keeps pushing frames indefinitely |

The SMMU was **exonerated**: with `arm-smmu.disable_bypass=0` the scanout sails through the
`arm-smmu` probe at ~11.9 s (`preserved 0 boot mappings`) without dying, so the SMMU takeover is
not the killer once bypass is allowed.

---

## 3. The fix (exact changes)

### 3a. Embedded DTB — `sm8250-samsung-common.dtsi`

Keep `dispcc` enabled, protect all its clocks:

```dts
&dispcc {
	/* dispcc stays ENABLED (probes -> fw_devlink satisfied -> no boot hang),
	 * but the qcom clk driver skips registering these clocks
	 * (qcom_cc_drop_protected -> rclks[i]=NULL), so it never reconfigures the
	 * display clocks Samsung UEFI left driving the live DSI scanout. */
	protected-clocks = <0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22
			    23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41
			    42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57>;
};
```

Hold the MDSS GDSC on via the framebuffer node — **note: `power-domains` only, NO `clocks`**
(adding clocks to this node hangs boot):

```dts
chosen {
	#address-cells = <2>;
	#size-cells = <2>;
	ranges;

	framebuffer: framebuffer@9c000000 {
		compatible = "simple-framebuffer";
		reg = <0x0 0x9c000000 0x0 0x2300000>;
		width = <1080>;
		height = <2400>;
		stride = <(1080 * 4)>;
		format = "a8r8g8b8";
		/* simpledrm becomes an active consumer of MDSS_GDSC, so genpd
		 * never powers it off and the DPU keeps pushing frames. */
		power-domains = <&dispcc MDSS_GDSC>;   /* MDSS_GDSC = 0 */
	};
};
```

(`MDSS_GDSC` is defined in `include/dt-bindings/clock/qcom,dispcc-sm8250.h`, which
`sm8250.dtsi` already pulls in.)

### 3b. Kernel cmdline (embedded via `CONFIG_CMDLINE` + `CONFIG_CMDLINE_FORCE`)

```
earlycon=efifb keep_bootcon console=tty0 loglevel=8 ignore_loglevel \
clk_ignore_unused pd_ignore_unused arm-smmu.disable_bypass=0 panic=30
```

- `simpledrm` is **built-in and enabled** — do **not** blacklist it.
- `clk_ignore_unused pd_ignore_unused` stop the late-init "unused" cleanup (necessary alongside
  the DT holds above).
- `arm-smmu.disable_bypass=0` lets the DPU's unmatched scanout stream fall through to bypass
  instead of faulting when the SMMU takes over.

---

## 4. Dead ends — do NOT re-chase

- **Disabling the `dispcc` node** (`status = "disabled"`) → **hangs boot** before userspace.
  Keep it enabled; use `protected-clocks` instead.
- **`initcall_blacklist=disp_cc_sm8250_driver_init`** → hangs (`fw_devlink` consumers wait
  forever on the never-binding controller).
- **`initcall_blacklist=simpledrm_platform_driver_init`** → destabilizes boot (with
  `keep_bootcon` and a non-functional `efifb` bootconsole there's no real fb console to hand off
  to); no logs, no USB gadget.
- **Adding display clocks to the framebuffer node** (the `sm8250-sony-xperia-edo.dtsi`
  clock-hold pattern) → total system hang at ~11.6 s on r8q. Use `protected-clocks` on `dispcc`
  for the clocks and only `power-domains` on the fb node.
- **MMIO-reading the DPU/DSI block** (`0xae0xxxx` via devmem etc.) → instant hard reset.
- **`initcall_blacklist=msm_drm_register` / `modprobe.blacklist=msm` / `nomodeset`** — pointless
  here; the MDSS display-subsystem is `status="disabled"` upstream so `msm`/DPU never runs (see
  `fix2.md`).

---

## 5. How to verify it's working

Over the telnet shell (USB NCM gadget, `172.16.42.1`):

```sh
# MDSS power domain must read "on":
grep mdss_gdsc /sys/kernel/debug/pm_genpd/pm_genpd_summary     # -> mdss_gdsc  on

# Paint the framebuffer — the panel should change instantly:
head -c 10368000 /dev/zero | tr '\000' '\377' > /dev/fb0       # solid white
cat /dev/zero > /dev/fb0                                        # clear to black

# Console text must appear live on the panel:
echo "R8Q MAINLINE DISPLAY ALIVE" > /dev/tty0

cat /proc/fb        # -> 0 simpledrmdrmfb
```

If `/dev/fb0` writes show up on the physical panel and keep working past ~15 s of uptime, the
fix is in.

---

## 6. Build / flash recap (for the future repo)

- **DTB lives in firmware.** Rebuild the DTB, drop it in
  `Mu-Silicium/Platforms/Samsung/r8qPkg/FdtBlob/sm8250-samsung-r8q.dtb`, rebuild the UEFI
  (`build_uefi.py -d r8q`), and flash to `BOOT` in download mode
  (`heimdall flash --BOOT Mu-Silicium/Mu-r8q-0.img`).
- **Kernel Image (cmdline) lives on the ESP.** Rebuild `Image`, copy to
  `EFI/BOOT/BOOTAA64.EFI` on the R8QESP partition (reachable via Mu-Silicium mass-storage mode).
- Working firmware image saved as `~/r8q_linux/Mu-Silicium/Mu-r8q-protclk-gdsc.img`.

---

## 7. One-paragraph summary

On r8q the panel is DSI command-mode and Samsung's UEFI leaves the DPU self-refreshing from the
`0x9C000000` splash buffer, so the upstream `simple-framebuffer` shows a live image at boot. The
only problem was mainline's power management: `dispcc` reconfigures the display clocks at ~11.6 s
(kills the scanout), and Linux powers off the MDSS GDSC at ~13.3 s (`pd_ignore_unused` doesn't
cover it). Fixing both with pure DT — `protected-clocks` on `dispcc` (keep it enabled but hands
off the clocks) plus `power-domains = <&dispcc MDSS_GDSC>` on the framebuffer node (keep the GDSC
on), with `simpledrm` enabled and `clk_ignore_unused pd_ignore_unused arm-smmu.disable_bypass=0`
on the cmdline — gives a persistent, live-updating mainline framebuffer console. No panel driver,
no msm-drm, no MMIO to the display block.
