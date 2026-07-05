# Project Status — Arch Linux on Samsung Galaxy S20 FE 5G (r8q) via Mu-Silicium + MAINLINE kernel

_Last updated: 2026-07-04 (session 1, ~00:30)_

## 🎉🎉🎉 DISPLAY SOLVED (2026-07-05, session 2) — persistent live mainline framebuffer on the r8q panel
First known working Linux display on Samsung Galaxy S20 FE 5G via Mu-Silicium (Mu-Silicium's own
r8q status said "Linux Boot ❌"). Live-updating fbcon console + `/dev/fb0` writes appear on the
panel and STAY (verified: painted colors show, `echo > /dev/tty0` shows, uptime 185 s+ stable).
**The winning combination = v17:**
1. **Kernel Image (on ESP), simpledrm ENABLED**, cmdline (NO simpledrm/dispcc blacklist):
   `earlycon=efifb keep_bootcon console=tty0 loglevel=8 ignore_loglevel clk_ignore_unused
   pd_ignore_unused arm-smmu.disable_bypass=0 panic=30`
2. **Embedded DTB (in firmware), `sm8250-samsung-common.dtsi`:**
   - `&dispcc { protected-clocks = <0 1 … 57>; };` (dispcc stays ENABLED so it probes → no
     fw_devlink hang; protected-clocks → qcom clk driver registers the controller but NEVER
     touches the display clocks → firmware's live DSI scanout survives dispcc probe).
   - framebuffer@9c000000 node: `power-domains = <&dispcc MDSS_GDSC>;` and **NO clocks** (holds
     mdss_gdsc ON via simpledrm as active consumer → DPU keeps pushing frames; adding clocks
     to this node HANGS boot).
- **Why each piece (the bisect that got here):** plain DT → scanout dies 11.6 s (dispcc probe
  reconfigures display clocks). protected-clocks → survives to 13.3 s, then dies (Linux powers
  off mdss_gdsc; pd_ignore_unused does NOT cover it). + MDSS_GDSC hold → survives indefinitely.
  SMMU exonerated (`arm-smmu.disable_bypass=0` lets the scanout survive the 11.9 s SMMU probe).
  Dead ends: disabling dispcc NODE (v14) hangs boot; `initcall_blacklist=simpledrm…` (v13)
  destabilizes boot (keep_bootcon + broken efifb, no fb handoff) — both ABANDONED.
- **Firmware flashed = `Mu-Silicium/Mu-r8q-0.img` (== `Mu-r8q-protclk-gdsc.img`). ESP Image = v16
  (simpledrm on).** System fully healthy: telnet 172.16.42.1, gadget @19 s, zero deferred.
- **NEXT: this is a great upstream patch candidate for samsung-r8q** (protected-clocks + fb
  power-domain is the sanctioned "no panel driver yet" pattern, cf. sony-xperia-edo which does
  the clock-hold variant). Also: proceed to Arch install on userdata (M2/M3), display no longer
  blocking.

## 🎉 ARCH LINUX RUNNING (2026-07-06, session 2) — M2 + M3 COMPLETE
Verified over SSH: `Linux r8q 7.1.2 aarch64`, Arch Linux ARM, stable uptime (bootloop fixed),
`ssh root@172.16.42.1` (pw `root`) works, tty1 autologin root shell live on the panel, display
still up (`simpledrmdrmfb`, `mdss_gdsc on`), rootfs `/dev/sda36` 105G (3.4G used). Services
active: r8q-usb-gadget, systemd-networkd, sshd, getty@tty1. **Bootloop fix CONFIRMED = moving the
1556-module tree aside (`/lib/modules/7.1.2` -> `.disabled`) so udev cold-plugs nothing (the
`qcom_wdt`=m + DSP remoteproc coldplug was hard-resetting the SoC).**
Host access without sshpass: `python3 ~/r8q_linux/../scratchpad/ssh_r8q.py '<cmd>'` (pexpect).
Next comforts: pruned module allowlist (for wifi etc.), Wi-Fi, USB host-mode keyboard (see
handoff — mainline dwc3, likely needs a powered hub / pm8150b vbus regulator), RTC.

### USB tethering + pacman WORKING (2026-07-06)
- Phone has internet over the NCM gadget (verified ping 1.1.1.1 + archlinux.org, pacman installs).
- HOST NAT (runtime): `net.ipv4.ip_forward=1` + iptables MASQUERADE 172.16.42.0/24 out `enp3s0`
  (PC uplink) + FORWARD accepts. Re-apply after a HOST reboot with `~/r8q_linux/host-tether.sh`.
- PHONE (persistent): networkd `20-usb0.network` has `Gateway=172.16.42.14` + `DNS=1.1.1.1`;
  static `/etc/resolv.conf` (1.1.1.1/8.8.8.8). So phone reboots keep internet as long as host NAT is up.
- **pacman gotchas fixed on-device:** (1) kernel lacks Landlock → pacman 7.x sandbox errored →
  added `DisableSandbox` to `/etc/pacman.conf` (proper fix later = `CONFIG_SECURITY_LANDLOCK=y`
  in kernel). (2) fresh keyring → ran `pacman-key --init && pacman-key --populate archlinuxarm`.
  Now `pacman -S` works (htop installed as proof).
- **TODO kernel comfort:** add `CONFIG_SECURITY_LANDLOCK=y` next rebuild so DisableSandbox isn't needed.
- **Next: Wi-Fi** (the untethered goal) — remoteproc (WLAN/WPSS) + firmware + ath11k/qca path,
  delicate on Samsung; do it now that pacman/internet exist.

## ARCH INSTALL detail (2026-07-06, session 2)
- **Arch Linux ARM aarch64 installed on `userdata` (sda36, ext4 LABEL=archroot, GPT PARTLABEL
  stays userdata).** Wiped the old pmOS. Tarball: `~/r8q_linux/dl/ArchLinuxARM-aarch64-latest.tar.gz`.
- **Boot = switch_root initramfs** (irfs `/init` rewritten; old M1 heartbeat saved as
  `irfs/init.m1-heartbeat.bak`). UFS+ext4 built-in → no module stage. /init mounts userdata,
  `switch_root` → systemd; on failure: NCM gadget + telnet + screen heartbeat + ESP log
  (`logs/switchroot*.txt`). Image on ESP = this (cmdline unchanged v16). Firmware = v17.
- **rootfs config:** root pw = `root`; sshd enabled + PermitRootLogin/PasswordAuth yes (ALARM
  default); `r8q-usb-gadget.service` (NCM, usb0=172.16.42.1 via networkd 20-usb0.network) +
  systemd-networkd enabled; hostname r8q; fstab LABEL=archroot. Overlay: `~/r8q_linux/rootfs-overlay/`.
- **✅ Arch reached the `login:` prompt on the PANEL (switch_root + full systemd stack works).**
  Then BOOTLOOPED. Journal (persistent, machine ac629746) shows each boot cutting off ~4 s in
  right after journal-flush = abrupt hard reset, no panic logged (later logs were volatile).
- **Root cause = udev cold-plugging the full 1556-module tree.** Same kernel ran 185 s under the
  busybox initramfs (no modules). Prime trigger: `CONFIG_QCOM_WDT=m` — the watchdog module loads,
  takes over the firmware-armed APSS watchdog, bite lands ~30 s later; also DSP remoteproc
  (adsp/cdsp/slpi enabled in r8q.dts) coldplug is delicate. **This is garnet's "never give udev
  the full module set" lesson.**
- **FIXES applied to rootfs (2026-07-06, phone in mass storage):**
  1. `mv /lib/modules/7.1.2 -> 7.1.2.disabled` — udev cold-plugs nothing (all bring-up drivers
     are built-in). Reintroduce a pruned allowlist later if modules are needed (wifi etc.).
  2. Autologin root on tty1: `etc/systemd/system/getty@tty1.service.d/autologin.conf`
     (`agetty --autologin root`).
  3. `etc/systemd/journald.conf.d/r8q-sync.conf` (Storage=persistent, SyncIntervalSec=1s) so any
     future reset is diagnosable past the 4 s cutoff. `/var/log/journal` present.
- **NEXT: reboot out of mass storage → expect stable Arch, autologin root on panel + NCM gadget
  → `ssh root@172.16.42.1` (pw root). If it STILL reboots, read the now-aggressively-synced
  journal (`journalctl -D <ud>/var/log/journal -b -1`).**

## CURRENT STATE (read this first) — updated 2026-07-05 (session 2)
- **Firmware on BOOT:** Mu-Silicium RELEASE with embedded PLAIN upstream DTB (framebuffer node
  without clocks/power-domains — the clock/GDSC experiments are REVERTED). Flashed 2026-07-04.
- **v12 RESOLVED = confirmed dead end.** ESP logs pulled (session 2, boots 12–17 all present).
  Boots 12–17 all ran the **v10** cmdline and reached **+62 s** cleanly (NCM gadget enumerated
  `configfs-gadget.g1 HOST MAC` @19.5 s, ZERO deferred devices). The v12 Image
  (`initcall_blacklist=disp_cc_sm8250_driver_init` + `iommu.passthrough=1`) was staged but
  produced **NO log** (no boot-18) → it hangs before the +13 s dump = fw_devlink starvation from
  the never-binding dispcc, exactly as predicted. **disp_cc blacklist ABANDONED.**
- **KEY REFRAME (fix2.md + logs): the screen death lands exactly on
  `Console: switching to colour frame buffer device` @11.90 s** — the fbcon→simpledrm handoff.
  `dispcc`/`af00000` NEVER appears in any log (silent; not our killer). MDSS node is
  `status="disabled"` upstream so msm-drm never runs (fix2.md §3a). So the garble is Linux
  WRITING the framebuffer, surfacing at the console switch — NOT a driver modeset/takeover.
- **Kernel on ESP now (v13, staged+verified 2026-07-05):** 7.1.2, cmdline =
  `earlycon=efifb keep_bootcon console=tty0 loglevel=8 ignore_loglevel clk_ignore_unused
  pd_ignore_unused arm-smmu.disable_bypass=0 iommu.passthrough=1
  initcall_blacklist=simpledrm_platform_driver_init panic=30`. Drops the hang-causing disp_cc
  blacklist; keeps all v10/v11 flags; **disables simpledrm** so Linux never touches 9c000000,
  leaving the firmware splash untouched. (simpledrm's fb node has no consumers → no fw_devlink
  starvation, unlike disp_cc, so this will NOT hang like v12.)
- **v13 RESULT (user, 2026-07-05): screen STILL died at ~11.6 s with simpledrm disabled.**
  → Linux's fb writes / stride-format theory is RULED OUT for r8q. The killer is a HARDWARE
  event at ~11.6 s, exactly fix2.md §5 (Qualcomm clock-controller probe touching the live
  display). fbcon/simpledrm exonerated.
- **v14 = fix2.md §5 applied (dispcc NODE disabled in embedded DTB). FIRMWARE BUILT, AWAITING
  DOWNLOAD-MODE FLASH.**
  - Change: `sm8250-samsung-common.dtsi` adds `&dispcc { status = "disabled"; };` → dispcc
    (af00000) never probes → can't reconfigure the live display clock tree at 11.6 s.
    Verified `status="disabled"` in built DTB; UEFI rebuilt (`Mu-Silicium/Mu-r8q-0.img`,
    2026-07-05 23:15, cmp-verified embeds the disabled DTB).
  - SAFE (no v12-style hang): all `&dispcc` consumers live under the upstream-disabled
    `mdss@ae00000`, so nothing enabled needs dispcc → fw_devlink drops the links. This is the
    NODE-disable route, categorically different from v12's driver-initcall blacklist.
  - ESP Image left at **v13** (simpledrm still disabled) on purpose → single-variable change
    vs the v13 test. If the splash now SURVIVES to +62 s → dispcc was the killer (fix2.md
    confirmed); next cycle re-enables simpledrm (mass storage) for a live Linux console.
    If it STILL dies → dispcc exonerated too; next suspect = arm-smmu takeover (11.6–11.8 s,
    "preserved 0 boot mappings") despite disable_bypass=0, or genpd.
  - v14 was flashed 2026-07-05 ~23:18.
- **v14 RESULT (user): REGRESSION — screen "instant black" (no splash-then-garble) AND phone
  does NOT enumerate on USB at all** (no NCM gadget, no mass-storage disk; 119.1G disk gone
  from host). Worse than v13 (which reached +62 s + gadget @19.5 s). dispcc-node-disable was
  the ONLY change → treat as the cause: **disabling the dispcc NODE broke the boot** (mechanism
  unconfirmed — no enabled consumer found; possibly early hang/panic). **dispcc-disable ABANDONED.**
- **RECOVERY IMAGES BUILT (2026-07-05 23:25):**
  - `Mu-Silicium/Mu-r8q-0.img` = **dispcc-ON known-good** (== v13 firmware DTB). Flash this to
    restore the working state. dtsi reverted (dispcc override removed).
  - `Mu-Silicium/Mu-r8q-dispcc-OFF.img` = the bad v14 (kept for reference).
  - `Mu-Silicium/Mu-r8q-dispcc-ON-known-good.img` = backup copy of the good one.
- **NEXT ACTION (user), pick one:**
  - **Mass storage** (if reachable) → host pulls `ESP:/logs/boot-N.txt`: a NEW boot-N = kernel
    booted (dispcc-disable was display/gadget-only, not a full hang); NO new boot-N = early hang.
  - **Download mode** (power off; VolUp+VolDown; plug USB) → host reflashes
    `heimdall flash --BOOT Mu-Silicium/Mu-r8q-0.img` to restore the known-good v13 firmware.
- **v14 CONFIRMED early hang: mass-storage pull shows NO new boot log (still boot-17). Kernel
  died before the +13 s /init dump. dispcc NODE-disable abandoned.**
- **v15 = `protected-clocks` on dispcc (the surgical fix status.md always named). BUILT, AWAITING
  DOWNLOAD-MODE FLASH.** dispcc stays ENABLED (probes → no fw_devlink hang) but
  `protected-clocks = <0..57>` → `qcom_cc_drop_protected` sets rclks[i]=NULL → driver registers
  the controller but NEVER touches the display clocks → firmware's live DSI scanout should
  survive the 11.6 s burst. Verified in DTB; UEFI = `Mu-Silicium/Mu-r8q-0.img` (== `Mu-r8q-protclk.img`).
  Also serves as boot-restore (dispcc enabled again). ESP Image left at v13 (simpledrm OFF) for
  a clean single-variable test vs v13.
  - **NEXT (user): DOWNLOAD MODE → `heimdall flash --BOOT Mu-Silicium/Mu-r8q-0.img` → watch screen.**
    - Splash SURVIVES to +62 s → protected-clocks is the fix (dispcc probe was the 11.6 s killer).
      Next: re-enable simpledrm (mass storage, Image rebuild) for a live Linux console.
    - Still dies ~11.6 s → dispcc probe exonerated; killer = arm-smmu takeover (11.6–11.8 s) or
      RPMh regulator. Those are cmdline-bisectable (mass storage, no reflash).
    - Boots headless fine either way (dispcc enabled = v13-class healthy) → telnet 172.16.42.1.
- **v15 RESULT (user, 2026-07-05): 🎉 DISPLAY SURVIVED 11.6 s → ~17–18 s.** protected-clocks
  works — dispcc's clock reconfiguration WAS part of the 11.6 s killer; leaving those clocks
  untouched pushed the death ~6 s later. A SECOND killer now hits at ~17–18 s.
- **CONFOUND FOUND: `initcall_blacklist=simpledrm_platform_driver_init` (v13 Image, in play all
  session) destabilizes boot.** Evidence: prev-session boots 12–17 (simpledrm ENABLED) reached
  +62 s with logs+gadget; this session v13/v14/v15 (simpledrm DISABLED) wrote NO log (still
  boot-17) and never enumerated the NCM gadget → all died before /init's ~21 s log write.
  Likely `keep_bootcon` holding the broken `efifb` bootconsole (I/O port 0x0) with no real fb
  console to hand off to. **simpledrm-disable ABANDONED as an Image option.**
- **v16 = protected-clocks firmware (v15, unchanged) + simpledrm-ENABLED Image (built, awaiting
  mass-storage stage).** cmdline = EXACT boot-17 proven-healthy string
  `earlycon=efifb keep_bootcon console=tty0 loglevel=8 ignore_loglevel clk_ignore_unused
  pd_ignore_unused arm-smmu.disable_bypass=0 panic=30` (no simpledrm blacklist, no
  iommu.passthrough). Goal: living system to +62 s → LOGS + telnet + gadget back, AND an honest
  look at the display with protected clocks (fbcon draws @11.9 s onto the — hopefully still
  live — scanout). Then diagnose the ~17–18 s second killer from the log.
  - **NEXT (user): MASS STORAGE → host stages v16 Image to ESP → reboot → watch screen + pull logs.**
- **v16 RESULT (2026-07-05 ~23:45): 🎉 LIVE MAINLINE CONSOLE ON THE PANEL for ~1.3 s.** System
  fully healthy (uptime 100s+, gadget @19s, telnet WORKS, zero deferred, no faults). User:
  "display stopped updating after 13.3096 s, frozen frame (colorful static rectangle)." So the
  display renders fine 12.03 s (Console switch) → 13.31 s, then FREEZES. fb0 paint at 114 s did
  NOT appear → the DPU stopped pushing frames (command-mode panel holds last frame).
  - **ROOT CAUSE PINNED via telnet `pm_genpd_summary`: `mdss_gdsc → off-0`.** Linux powered off
    the MDSS GDSC power domain at ~13.3 s. `pd_ignore_unused` does NOT cover it (that only skips
    the late_initcall; the message "Not disabling unused power domains" printed yet mdss_gdsc is
    off — powered off via a different genpd path). SMMU EXONERATED (display survived the 11.9 s
    arm-smmu probe → `disable_bypass=0` works). protected-clocks EXONERATED as sufficient-alone.
- **v17 = protected-clocks + MDSS_GDSC hold (BUILT `Mu-Silicium/Mu-r8q-0.img` == `Mu-r8q-protclk-gdsc.img`,
  AWAITING DOWNLOAD-MODE FLASH).** Added `power-domains = <&dispcc MDSS_GDSC>` to the
  simple-framebuffer node so simpledrm is an active consumer → genpd keeps mdss_gdsc ON. NO
  clocks in the fb node (protected-clocks handles those; adding clocks hangs boot per history).
  dispcc phandle=0x02 verified. ESP Image stays v16 (simpledrm ON, healthy).
  - **NEXT (user): DOWNLOAD MODE → `heimdall flash --BOOT Mu-Silicium/Mu-r8q-0.img` → watch screen.**
    - Console stays live past 13.3 s to +60 s → DISPLAY SOLVED (persistent mainline console).
    - Boot hangs (no gadget/log) → GDSC-in-fb-node is lethal like the clocks were → fall back;
      real fix = full msm-drm + panel driver. (Restore images: `Mu-r8q-protclk.img` = v15 boots
      healthy but freezes @13.3; `Mu-r8q-dispcc-ON-known-good.img` = plain.)
- **Firmware image inventory (`~/r8q_linux/Mu-Silicium/`):**
  `Mu-r8q-0.img` = v15 protected-clocks (current flash target) · `Mu-r8q-protclk.img` = same ·
  `Mu-r8q-dispcc-ON-known-good.img` = plain dispcc-on safe restore (no experiment) ·
  `Mu-r8q-dispcc-OFF.img` = bad v14 (do not flash).
- Working and verified: UEFI boot, DT mode, UFS (all LUNs), ESP logging, USB NCM gadget +
  telnet shell (v9–v11), zero deferred devices. Arch install on userdata is user-approved,
  not yet started; display prioritized first (user decision).

## Goal
Run **Arch Linux ARM** on the Samsung Galaxy S20 FE 5G (**codename `r8q`**, SoC **SM8250 "kona"**)
using **Mu-Silicium UEFI** as firmware layer and the **MAINLINE Linux kernel** (not downstream).
Milestones mirror the garnet project (`~/garnet_linux/status.md`):
1. Mainline kernel + DTB boots to initramfs with on-screen output.
2. UFS storage up; Arch rootfs on a repurposed partition.
3. systemd boot to console; SSH (USB gadget / Wi-Fi).
4. Input, Wi-Fi, comforts.

## Ground rules (user decisions, session 1)
- **Start from scratch** — ignore all previous attempts present on the phone and any prebuilt
  artifacts (e.g. `garnet_linux/Mu-Silicium/Mu-r8q-0.img`, `Build/kernel-r8q`). Sources may be
  reused from `~/garnet_linux`.
- Phone is currently in **download mode**; `heimdall detect` → "Device detected" ✅.
- User decides destructive steps (repartitioning, wiping Android).

## Device facts
| Item | Value |
|------|-------|
| Codename | `r8q` |
| Model | Samsung Galaxy S20 FE 5G (Snapdragon variant, likely SM-G781B — verify from device) |
| SoC | SM8250 "kona" (Snapdragon 865) |
| Flash path | Download mode + Odin protocol (`heimdall` on Linux) — no fastboot |
| Partition scheme | non-A/B expected (verify from PIT/GPT) |
| Bootloader | assumed unlocked (verify: flash acceptance will tell) |

## Key discoveries (session 1)
- **Mu-Silicium supports r8q upstream**: `Platforms/Samsung/r8qPkg` exists in the repo
  (prompt.md's "no Mu-Silicium port" assumption is FALSE). Repo also carries
  `Resources/DTBs/r8q.dts` and `r8qPkg/FdtBlob/sm8250-samsung-r8q.dtb` — i.e. a
  **mainline-style r8q DTB already exists** in the Mu-Silicium tree. Prime prior art.
- Previous attempt (found in `~/garnet_linux/Mu-Silicium`, discarded per ground rules) had
  exactly one source change: `r8q.fdf` un-commented the `DtPlatformDxe` + embedded
  `sm8250-samsung-r8q.dtb` FREEFORM section (exposes DT to the OS via EFI). We reset to
  pristine and will re-derive this deliberately when we get to kernel boot.
- r8q build config (`Resources/Configs/r8q.toml`): FD base `0x9FC00000`, size `0x300000`;
  boot image: gzip kernel, `dtb_location = "kernel"`, `header_version = 1`, boot shim
  requires kernel header.
- Host tools present: heimdall, mkbootimg, dtc, aarch64-linux-gnu-gcc, clang. Network OK.

## Flash procedure (researched, from LineageOS r8q docs + samsung_qcom template)
- Install method: Odin protocol; `heimdall` v2.2.2 (Grimler fork — handles 2020+ Samsungs) detects the phone ✅.
- **AVB gate:** custom images on `recovery` only boot after flashing a **verification-disabled
  vbmeta** (`avbtool make_vbmeta_image --flags 2 --padding_size 4096`). avbtool present ✅.
  Side effect: first Android boot after that demands a userdata format (Android is being
  sacrificed anyway per project goal — but this makes the vbmeta flash a DESTRUCTIVE step).
- Partition: `recovery` (r8q is non-A/B, has a dedicated recovery partition).
- Key combos: download mode = VolUp+VolDown + plug USB (powered off);
  boot recovery (= our UEFI) = VolUp+Power with USB connected.
- Bootloader unlock state: unknown from host; flash acceptance will tell (locked → clean reject).

## Kernel prior art (researched)
- pmaports has **no device-samsung-r8q package** (not merged), but the community sm8250 mainline
  kernel is `gitlab.postmarketos.org/soc/qualcomm-sm8250/linux`, current tag **sm8250-6.17.0**
  (pmaports `device/testing/linux-postmarketos-qcom-sm8250` builds it with LLVM=1).
  Cloned (shallow) to `~/r8q_linux/kernel` — checking for an r8q DTS in-tree.
- Previous attempt's `sm8250-samsung-r8q.dtb` (in garnet copy, discarded) decompiles to a real
  mainline DT: `compatible = "samsung,r8q", "qcom,sm8250"`, model "Samsung Galaxy S20 FE" —
  proves a source DTS exists somewhere; find it rather than reuse the blob.

## Done (session 1)
- ✅ Mu-Silicium copied to `~/r8q_linux/Mu-Silicium`, reset to pristine (previous attempt's
  changes dropped), **built from scratch**: `Mu-Silicium/Mu-r8q-0.img` (1.8 MB, valid Android
  boot image, header v1, page 2048). Build log: `build_uefi_r8q.log`. Python venv for edk2
  pytools: `~/r8q_linux/.venv` (recreate with `python3 -m venv .venv && .venv/bin/pip install
  -r Mu-Silicium/pip-requirements.txt`).
- ✅ `vbmeta_disabled.img` generated from scratch (`avbtool make_vbmeta_image --flags 2
  --padding_size 4096`).
- ✅ **The r8q mainline DT is UPSTREAM since v6.18**: `sm8250-samsung-r8q.dts` +
  `sm8250-samsung-common.dtsi` (merged 2025-09, commit 6657fe9e9f23 + 194c7636faf8).
  Covers: simple-framebuffer@9c000000 (1080x2400 ARGB), gpio-keys (VolUp), reserved-memory
  (splash + ramoops@9fa00000), PM8150 RPMh regulators, pon pwrkey/resin (VolDown), USB
  peripheral (HS only), **UFS with supplies**. Milestone 1+2 hardware is upstream!
- ✅ Kernel source: **Linux 7.1.2 stable** (git.kernel.org snapshot; cdn.kernel.org v7.x
  tarball URL 404s — use https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/snapshot/linux-7.1.2.tar.gz).
  → `~/r8q_linux/kernel/`. Config fragment `r8q_bringup.config` + `build_kernel.sh`
  (arm64 defconfig + fragment, LLVM=1, bring-up drivers built-in per garnet lesson).
- Reference config saved: `config-postmarketos-qcom-sm8250.aarch64.ref` (pmOS 6.17 sm8250).

## Boot design (session 1 decisions + rationale)
- **Mu-Silicium must expose the mainline DTB as an EFI config table** (`DtPlatformDxe` +
  FREEFORM DTB section in `r8q.fdf` — the exact change the previous attempt made, re-derived):
  without a DT table the kernel falls back to Mu-Silicium's WoA-style ACPI tables = no devices.
  We embed OUR OWN `sm8250-samsung-r8q.dtb` built from Linux 7.1.2 source.
- **Milestone 1 = single-file EFI boot:** `BOOTAA64.EFI` = uncompressed `Image` (EFI stub PE)
  with initramfs embedded (`CONFIG_INITRAMFS_SOURCE=~/r8q_linux/irfs`) and
  `CONFIG_CMDLINE_FORCE="console=tty0 loglevel=7 panic=30"`. Reason: the phone has no keyboard
  for the UEFI shell, so no way to pass cmdline/initrd args; Mu-Silicium's boot manager just
  launches the default boot path. GRUB volume-key menu comes later (garnet workflow).
- Initramfs `~/r8q_linux/irfs/` (busybox 1.36.1 static aarch64 from official ALARM repo,
  extra/busybox-1.36.1-4): /init = banner + 20 s heartbeat (block devs, devices_deferred) +
  one-shot `dump_logs` writing dmesg/partitions to the first mountable vfat partition.
  Packed copy: `initramfs-m1.gz` (797 KB) for later external-initrd use.

## 🎉 Milestone 0 (2026-07-03 17:43): Mu-Silicium UEFI FLASHED to BOOT
- Kernel 7.1.2 built: `out/arch/arm64/boot/Image` (52 MB, EFI stub, embedded busybox initramfs,
  forced cmdline `console=tty0 loglevel=7 panic=30`) + `sm8250-samsung-r8q.dtb` (modules still
  compiling in background, not needed for M1).
- DTB copied to `Mu-Silicium/Platforms/Samsung/r8qPkg/FdtBlob/`, `r8q.fdf` Device Tree block
  re-enabled (user-confirmed approach), UEFI rebuilt — DTB FREEFORM GUID verified inside
  FVMAIN at 0x45C3A0. DtPlatformDxe default pref = DT (PcdDefaultDtPref TRUE); NOTE: a stale
  `DtAcpiPref` NVRAM var from old attempts (UEFIVARSTORE partition) could force ACPI — check if
  kernel says "ACPI" instead of DT.
- `heimdall flash --BOOT Mu-Silicium/Mu-r8q-0.img --resume` → **upload successful**, device
  rebooted into UEFI.

## 🎉🎉 MILESTONE 1 COMPLETE (2026-07-03 ~17:48) — AND UFS ALREADY WORKS (M2 storage half)
**Mainline Linux 7.1.2 boots on r8q via Mu-Silicium on the FIRST attempt.** Evidence:
`R8QESP:/logs/boot-{0,1}.txt` (two clean boots, pulled to `logs_pull/`):
- `efi: EFI v2.7 by Project Silicium`, **`Machine model: Samsung Galaxy S20 FE`** (DT mode,
  ACPI interpreter disabled — DtPlatformDxe DTB table worked).
- **simpledrm bound to 9c000000.framebuffer at 0.16 s** (DT fb node; GOP addr matched).
- **Full UFS at 0.57 s** — sda (119.1 GiB, all 36 partitions) + all LUNs, first try; the
  garnet 9-layer UFS onion simply doesn't exist on mainline+sm8250.
- Initramfs ran, mounted the ESP by marker, wrote logs, heartbeats looped. Total to logs: 20.7 s.
- **Exactly ONE deferred device**: `a600000.usb — dwc3: failed to initialize core` (M4 work).
- Benign: `psci: [Firmware Bug]: failed to set PC mode: -3`.
- **On-screen reality check (user videos): text appeared then display died** — one boot showed
  colored static, one "instant power-off" (screen only; system kept running and logged).
  Cause: fbcon rendered fine (`switching to colour frame buffer 135x150` @0.18 s) but
  **`clk_disable_unused` + genpd powered off the unclaimed MDSS clocks/domains** right after
  boot (no msm-drm driver owns them). Fix (user's prediction from session start):
  `clk_ignore_unused pd_ignore_unused` added to CONFIG_CMDLINE → **Image v3 on ESP**.
- **v3 boot result (boots 2–3, video 2): flags active, system clean, BUT display now shows
  splash → colored horizontal garbage bands → black.** Diagnosis: **the panel is DSI
  command-mode** — it only shows frames that are *pushed*; Samsung's binary UEFI display
  driver stops pushing at ExitBootServices (and possibly tears down the pipe). fbcon renders
  into RAM nobody transfers. The upstream DT simple-framebuffer works under stock ABL because
  ABL leaves DPU **autorefresh** enabled; Mu-Silicium does not.
- **Mu-Silicium's own r8q status: "Linux Boot ❌"** — no one has done this before; the display
  handoff is the unsolved part. Display via real msm-drm needs a panel driver (none upstream
  for r8q; downstream DT + panel-driver-generator is the long-term path).
- **Image v4 on ESP (awaiting boot):** (a) fixed `PHY_QCOM_USB_SNPS_FEMTO_V2=y` (was =m →
  missing in initramfs → THE dwc3 "failed to initialize core" defer; symbol was RENAMED from
  garnet-era PHY_QCOM_SNPS_FEMTO_V2 — fragment updated); (b) /init v4 adds a guarded DPU
  probe after log dump: reads INTF1 tear regs (0xae6ba80..) / INTF1 AR cfg (0xae6bab4) /
  PP0+PP1 AR (0xae71030/0xae71830) / DSI0 ctrl (0xae94000/4), then tries
  autorefresh-enable (0x80000001) on INTF1 then PP0, logging to `logs/dpu-N.txt`; then
  urandom test-paint to fb0. If DSI regs read 0 → pipe torn down → autorefresh won't help,
  panel driver route required.
- **v4 boot (boot-4): 🎉 ZERO deferred devices — dwc3 probed with the built-in PHY.**
  DPU probe didn't run: ALARM busybox lacks the `devmem` applet ("devmem: not found" in
  dpu-0.txt). Fixed: static aarch64 `devmem` cross-compiled from `tools/devmem.c` into
  `irfs/bin/`. (CONFIG_DEVMEM=y, STRICT_DEVMEM=y is fine for MMIO, IO_STRICT off.)
- **Image v5 on ESP (awaiting boot):** devmem in place; /init v5 additionally sets up a
  **configfs NCM gadget (self-powered ids 1d6b:0104) + usb0 172.16.42.1 + busybox telnetd**
  → interactive shell over USB with no display. Host monitor armed: auto-assigns
  172.16.42.14/24 to the new netdev and pings the phone. UDC name now logged in boot-N.txt.
- **v5/v6 boots: phone HARD-RESETS ~10 s after the handoff mess** (user observation).
  Root cause proven by absence of dpu-1.txt + presence of boot-5/6: **the FIRST devmem read
  of the DPU/INTF block hard-resets the SoC** — Samsung's binary UEFI display driver leaves
  the display block UNPOWERED after ExitBootServices; MMIO to it = bus stall = watchdog.
  (v4 "survived" the probe only because devmem was missing. Garnet lesson 11 replayed.)
  **Autorefresh shortcut is DEAD. Display route = mainline msm-drm DPU/DSI + a new panel
  driver** (downstream DT panel timings + linux-mdss-dsi-panel-driver-generator).
- **boot-5.txt (v5) confirms `udc: a600000.usb` EXISTS** — dwc3 gadget-ready.
- **Image v7 on ESP (awaiting boot): dpu_probe retired; setup_gadget runs FIRST (+5 s),
  then dump_logs.** Expect NCM enumeration on the PC + telnet 172.16.42.1 within ~15 s of
  power-on, and the phone should STAY UP. Host netdev monitor armed.

## 🎉 Milestone (2026-07-03 ~18:45): USB NETWORKING WORKS — phone pings at 172.16.42.1
- v7 boot: **no crash, NCM gadget enumerated (host netdev enp0s20f0u7), ping 3.6–5.7 ms.**
  Phone stays up. Mainline dwc3 peripheral mode needed ZERO extcon hacks (vs garnet's saga).
- telnet: **connection reset** — busybox telnetd can't open a pty: initramfs never mounts
  **devpts**. Fix in v8: `mount -t devpts devpts /dev/pts` before telnetd + fallback raw
  `nc -ll -p 24 -e /bin/sh`. (CONFIG_UNIX98_PTYS=y already.) v8 built, awaiting one more
  mass-storage cycle to stage; after that, iteration should go OVER THE NETWORK.

## DISPLAY ROOT CAUSE #3 (the real one?) — dispcc probe kills the scanout; Edo pattern applied
- v11 (`iommu.passthrough=1`) still scrambled at the same ledtrig-cpu/+11.6 s point → SMMU
  mostly exonerated. The 11.6 s window = regulators + serial + SMMU + **clock controllers**.
- **Upstream reference found: `sm8250-sony-xperia-edo.dtsi`** (mainline-booting sm8250 phone,
  no panel driver) — its framebuffer node HOLDS 9 display clocks (dispcc AHB/VSYNC/MDP/BYTE0/
  BYTE0_INTF/PCLK0/ESC0 + gcc DISP HF/SF AXI) **+ `power-domains = <&dispcc MDSS_GDSC>`**,
  with the comment "necessary due to unused clk cleanup & no panel driver yet". Our samsung
  common dtsi holds NOTHING → when built-in dispcc probes (~11.6 s), the display clock tree
  is re-parented/reset under the running DPU → scramble → death. `clk_ignore_unused` can't
  prevent controller takeover, only late cleanup. simpledrm claims DT clocks/power-domains,
  so with the Edo block copied in, the scanout survives.
- **Change applied to `sm8250-samsung-common.dtsi`** (kernel tree, upstream-style — good
  future upstream patch candidate for samsung-r8q!), DTB rebuilt, **UEFI rebuilt with new
  embedded DTB (`Mu-Silicium/Mu-r8q-0.img`, 2026-07-03 23:31)**.
- **NEEDS: heimdall flash to BOOT (download mode) — the DTB lives in firmware.**
  `heimdall flash --BOOT Mu-Silicium/Mu-r8q-0.img` (+ `--resume` if a session was used).
- Panel identified from vendor DT: **EA8076A / AMS646UJ10** FHD AMOLED, vdd3 = GPIO-fixed
  regulator (tlmm 93), reset-seq <0 10 1 15>, TE gpio 66. Mainline has s6e3fc2x01 (close
  DDIC family) — panel-driver milestone groundwork.

## Display bisect round 2 (2026-07-03 ~23:55)
- **Edo clock-hold pattern FAILED on r8q — twice:** (a) clocks+MDSS_GDSC power-domain →
  TOTAL SYSTEM HANG at 11.6 s (no +13 s log dump, no gadget — died before /init payload);
  (b) clocks-only (no power-domains) → same hang. Conclusion: **the display block tolerates
  ZERO kernel interaction** on r8q (MMIO read = hard reset; GDSC attach = hang; clk enable =
  hang). Samsung firmware likely keeps the domain TZ/xPU-protected. DT reverted to the plain
  framebuffer node; firmware rebuilt (23:55 image).
- **v12 experiment (staged):** `initcall_blacklist=disp_cc_sm8250_driver_init` on cmdline —
  dispcc never registers → nothing re-parents display clocks at 11.6 s. If screen survives:
  killer proven = dispcc registration; permanent fix = `protected-clocks` property on the
  dispcc DT node (supported by qcom clk common.c). If still dies: next suspect = RPMh
  regulator constraint application (bisect via initcall_blacklist=rpmh_regulator_driver_init —
  NOTE that also defers UFS/USB supplies, so expect no shell in that test).
- Combined watcher stages v12 (mass storage) + flashes reverted firmware (download mode).

## Display bisect round 3 (2026-07-04 ~00:00) — v12 dispcc blacklist, outcome pending
- v12 = reverted plain DTB + `initcall_blacklist=disp_cc_sm8250_driver_init`. User: same
  11.6 s screen death AND no shell. Not yet log-verified (see CURRENT STATE).
- **Theories checked and RULED OUT this round:**
  - *Reserved GPIO* (Mu-Silicium Discord suggestion): upstream r8q DT reserves tlmm 20–23
    (fingerprint SPI) + 40–43; we never added GPIO usage; pinctrl probes at 0.3 s without
    incident and v9–v11 booted fully with the same DT. (Edo reserves 40–43 + 52–55 for
    comparison.) Could matter later if a driver claims a TZ-protected pin — keep in mind.
  - *"Naumov DSP" (Discord)*: all remoteproc drivers (QCOM_Q6V5_PAS/ADSP/MSS) are **=m**,
    initramfs ships no modules, logs show zero remoteproc/adsp lines — the kernel never
    touches the DSPs. Revisit when remoteprocs get enabled for real (Wi-Fi/audio milestone);
    on Samsung that bring-up is known-delicate.
- **Question drafted for Mu-Silicium Discord** (asked 2026-07-04): what state does Samsung's
  binary DisplayDxe leave the DPU/DSI in at ExitBootServices on kona; is cont-splash takeover
  viable at all or is the display domain TZ/xPU-protected (any MMIO read = hard reset, any
  clk/GDSC touch = hang, in our experiments) so only full msm-drm re-init works?

## Display facts accumulated (the hard-won list)
| Experiment | Result |
|---|---|
| v3–v8: plain DT, console=tty0 | text renders 0.2–11.6 s (proven by logs), screen scramble→black at ~11.6 s; system + shell fine |
| v9: + fix.md cmdline (earlycon=efifb keep_bootcon) | same; earlycon got NO fb address ("at I/O port 0x0" — stub gave no screen_info) |
| v10: + arm-smmu.disable_bypass=0 | same (SMMU "preserved 0 boot mappings" at 11.7 s; no fault lines ever) |
| v11: + iommu.passthrough=1 | same |
| DPU MMIO read (devmem, INTF1 tear regs) | **instant hard reset** (dpu-1.txt never written) |
| DT fb node + 9 display clocks + MDSS_GDSC (Edo pattern) | **total system hang ~11.6 s** (no +13 s log, no gadget) |
| DT fb node + clocks only (no GDSC) | **same total hang** |
| v12: dispcc driver blacklisted, plain DT | pending log verification |
- Panel: EA8076A / AMS646UJ10 FHD AMOLED, DSI command-mode; vdd3 = GPIO-fixed (tlmm 93);
  reset seq <0 10 1 15>; TE gpio 66. Closest mainline driver family: s6e3fc2x01.
- fb geometry verified live: simpledrm on DT node, 1080x2400 stride 4320 — matches SimpleFbDxe
  GOP (PixelsPerScanLine=width). r8q firmware runs Samsung binary DisplayDxe (APRIORI, owns
  real DPU) + SiliciumPkg SimpleFbDxe (GOP consumers). fix.md's stride-mismatch mechanism is
  therefore NOT our failure mode (geometry agrees end to end).
- 11.6 s killer is in the built-in probe burst: dispcc + RPMh regulator constraints + arm-smmu
  reset all land within ~0.15 s of the last readable line (`ledtrig-cpu`).

## User's fix.md applied (v9, on ESP) — the garble fix from their earlier pmOS attempt
User's prior pmOS-6.17 attempt hit the same "mess of colors" and FIXED it (fix.md, 2026-06-23):
stride/format mismatch between the GOP owner and the kernel's DT-fed simple-framebuffer;
structural fix = UEFI owns both GOP and FDT (DtPlatformDxe embedded DTB, RELEASE build, no
loader-injected DTB) + boot with `earlycon=efifb keep_bootcon` so the console paints with
GOP geometry. Our build already had the DtPlatformDxe/RELEASE architecture; the missing part
was the cmdline. **v9 = v8 (devpts/telnet fix) + fix.md cmdline:**
`earlycon=efifb keep_bootcon console=tty0 loglevel=8 ignore_loglevel clk_ignore_unused
pd_ignore_unused panic=30`.
Key architecture facts learned: r8q firmware runs BOTH Samsung's binary DisplayDxe (programs
the real DPU scanout) AND SiliciumPkg SimpleFbDxe (GOP at "Display Reserved" 0x9C000000,
PixelsPerScanLine = width = 1080 → stride 4320 == DT node). If the screen shows clean
earlycon text but sheared fbcon overwrites, the DPU's true stride ≠ 4320 → next step:
align the embedded DTB's framebuffer stride to the GOP-reported linelength (from the
efifb dmesg line in the ESP log) and reflash BOOT.

## 🎉 Milestone (2026-07-03 ~21:30): INTERACTIVE SHELL over USB (telnet) + garble root-caused
- **v9 boot: telnet shell WORKS** (`tools/tsh.py`, an option-refusing telnet client for busybox
  telnetd; raw `ncat` fails on IAC negotiation). Live `uname`, dmesg, fb sysfs, /dev/fb0 paint
  all confirmed. Iteration is now OVER THE WIRE — no reboot-per-experiment.
- **fb geometry is CORRECT**: `/proc/fb` = simpledrmdrmfb, `virtual_size 1080,2400`,
  **`stride 4320`** — matches GOP. So the garble is NOT a stride mismatch (fix.md's stride
  theory doesn't apply to *our* build — we embed the DTB in firmware, so GOP and DT agree).
- **USER on-screen report (v9): text READABLE for ~seconds, then color garbage, then black —
  but shell stayed alive.** ROOT CAUSE FOUND in boot-9.txt: **`arm-smmu 3da0000.iommu:
  preserved 0 boot mappings`** at ~11.7 s. `CONFIG_ARM_SMMU_DISABLE_BYPASS_BY_DEFAULT=y` →
  when apps_smmu resets, the still-running DPU scanout stream (set up by Samsung DisplayDxe,
  no SMR to preserve) is FAULTED instead of bypassed → scanout reads fault → garbage → black.
  Timeline fits exactly (readable until SMMU probe, then dead).
- **FIX = `arm-smmu.disable_bypass=0` on cmdline** (Image v10, staging to ESP): unmatched
  streams fall through to identity/bypass, so the boot display DMA survives the SMMU takeover.
  This is the standard mainline fix for "framebuffer dies when SMMU probes." Awaiting boot.
- Housekeeping: ESP (sd*32) fsck'd clean from host (dirty bit from crash boots).
- **Bug found+fixed post-boot: userspace was blind** — `Warning: unable to open an initial
  console`: the cpio (packed from a plain dir) had no `/dev/console` node, so /init's
  echo/banner went nowhere (kernel printk to fbcon was unaffected). Fix: `irfs.devnodes`
  file-list (console/null/tty0) appended to CONFIG_INITRAMFS_SOURCE; Image v2 rebuilt and
  restaged on ESP. External `initramfs-m1.gz` has the same flaw — regenerate with devnodes
  when GRUB-era comes (needs fakeroot or the kbuild list trick).

## Milestone 1 history — ESP staging
- ✅ Mu-Silicium mass storage works (UFS = host sdb..sde; LUN0 main 119.1 GiB, 36 partitions).
- ✅ **ESP = `cache` (sda32 on phone / sdb32 on host, 600 MB)**, reformatted
  `mkfs.vfat -F 32 -S 4096 -n R8QESP` (UFS logical block 4096, garnet lesson).
  Userdata (107 GiB, LOS ext4) left untouched for now.
- ✅ Staged: `EFI/BOOT/BOOTAA64.EFI` (= our Image, 52 MB), `initramfs-m1.gz`,
  `sm8250-samsung-r8q.dtb` (spare for future GRUB), `logs/` dir.
- ✅ /init fixed BEFORE first boot: log-dump now only writes to the partition carrying
  `EFI/BOOT/BOOTAA64.EFI` (would have polluted `apnhlos` modem vfat otherwise). Image relinked
  with fixed embedded initramfs, restaged. Full module build also completed (`out/`).
- **NEXT (user): reboot the phone out of mass-storage mode.** UEFI BDS should auto-boot
  `\EFI\BOOT\BOOTAA64.EFI` from R8QESP → EFI stub → fbcon banner + 20 s heartbeats on screen;
  logs land in R8QESP:/logs/boot-0.txt (readable over mass storage afterwards).
- Expected-failure knowledge: black screen w/ progressing logs → DT-fb vs GOP addr issue;
  hang mid-init → try `clk_ignore_unused pd_ignore_unused` (user tip, requires kernel rebuild
  due to CMDLINE_FORCE); kernel says ACPI → stale DtAcpiPref NVRAM var.

## Next
- Initramfs (busybox aarch64 from Arch ARM repo) + GRUB EFI + boot media layout.
- Flash (blocked on download-mode re-entry, see Blockers): one heimdall session,
  `--VBMETA vbmeta_disabled.img --RECOVERY Mu-r8q-0.img`; boot UEFI = VolUp+Power with USB.

## User decisions (2026-07-03, recorded)
- **Phone currently runs LineageOS; its verification-disabled VBMETA is already flashed —
  do NOT flash vbmeta.** (Our generated `vbmeta_disabled.img` kept as spare.)
- **Flash Mu-Silicium to `BOOT`, not RECOVERY** — every power-on boots UEFI directly
  (no key combo needed; Android/LOS boot is sacrificed on that partition).
- Bootloader confirmed unlocked.
- ESP/rootfs partition choice: decide after mass-storage view of the GPT.

## Device facts (updated from PIT, 87 entries: `pit.txt`)
- PIT partition names are UPPERCASE: `BOOT`, `RECOVERY`, `VBMETA`, `VBMETA_SAMSUNG`,
  `USERDATA`, `SUPER`, `CACHE` (exists!), `OMR`, `DTBO`, `PARAM`, `UEFIVARSTORE`, …
- Non-A/B confirmed (single BOOT/RECOVERY).

## Blockers
- (was: heimdall session wedged — RESOLVED by physically re-entering download mode;
  `heimdall detect` alone does NOT prove sessions work; a phone sitting at the download-mode
  warning screen enumerates but talks no Odin protocol.)
