# Handoff — r8q (Samsung Galaxy S20 FE 5G) Arch Linux via Mu-Silicium + mainline kernel

_Recipes and operational knowledge. Session-persistent. See `status.md` for state._

## Boot chain (planned)
Download mode → `heimdall` flash Mu-Silicium (Android boot image wrapping UEFI FD) →
Mu-Silicium UEFI → GRUB/EFI → mainline `Image` + `sm8250-samsung-r8q.dtb` → initramfs → Arch.

## Golden references
- `~/garnet_linux/handoff.md` — module staging/tsort, initramfs repack, ESP-logging workflow,
  USB gadget saga, Wi-Fi debugging playbook, silent-defer checklist. Read before debugging.
- `~/garnet_linux/status.md` — "Key operational learnings" section.
- Mu-Silicium: github.com/Project-Silicium/Mu-Silicium (local copy: `~/r8q_linux/Mu-Silicium`)
- Guides: github.com/Project-Silicium/Guides

## Recipes

### Build Mu-Silicium UEFI (from scratch, ~20 s after env setup)
```bash
cd ~/r8q_linux/Mu-Silicium
# once: python3 -m venv ../.venv && ../.venv/bin/pip install -r pip-requirements.txt
../.venv/bin/python build_uefi.py -d r8q     # → Mu-r8q-0.img (Android boot image, header v1)
```
DTB embedding (REQUIRED for mainline Linux — else kernel sees only ACPI):
uncomment the "Device Tree" block in `Platforms/Samsung/r8qPkg/r8q.fdf` and put our
kernel-built DTB at `Platforms/Samsung/r8qPkg/FdtBlob/sm8250-samsung-r8q.dtb`.
DtPlatformDxe is already in the DSC via SiliciumPkg.dsc.inc:478.

### Build kernel (Linux 7.1.2 stable, r8q DT upstream)
```bash
~/r8q_linux/build_kernel.sh    # arm64 defconfig + r8q_bringup.config, LLVM=1
# → out/arch/arm64/boot/Image (EFI-stub PE; M1: embedded initramfs from ~/r8q_linux/irfs
#   + CMDLINE_FORCE "console=tty0 loglevel=7 panic=30")
# → out/arch/arm64/boot/dts/qcom/sm8250-samsung-r8q.dtb
```
Re-pack external initramfs after editing irfs/: 
`cd irfs && find . | cpio -o -H newc | gzip -9 > ../initramfs-m1.gz`
(busybox: static aarch64 from ALARM extra/busybox-1.36.1-4 pkg — NOT from garnet artifacts)

### Flash (heimdall, phone in download mode)
```bash
heimdall detect
heimdall print-pit --no-reboot > pit.txt          # session check + partition names
heimdall flash --VBMETA vbmeta_disabled.img --RECOVERY Mu-Silicium/Mu-r8q-0.img --resume --no-reboot
```
vbmeta_disabled.img: `avbtool make_vbmeta_image --flags 2 --padding_size 4096 --output …`
Boot UEFI: unplug, then hold **VolUp+Power with USB cable connected** (recovery boot combo).
Download mode again: power off, hold VolUp+VolDown, plug USB.

## Known risks / first-boot suspects
- **`clk_ignore_unused pd_ignore_unused` (user tip, held in reserve):** if boot hangs or the
  screen dies part-way through init, add these to CONFIG_CMDLINE and rebuild — mainline's
  late-boot disabling of "unused" clocks/power-domains can collapse the firmware-lit display
  path (same failure class as garnet's mm-GDSC hard reset). pmOS ships these for simplefb
  devices. Deliberately NOT in the first-boot cmdline so we learn whether the clean config works.
- **DT framebuffer@9c000000 vs UEFI GOP**: the upstream DT hardcodes the Samsung ABL splash
  buffer. Mu-Silicium FD base = 0x9FC00000, so GOP buffer is *probably* the same 0x9c000000
  region — but if the screen is dark while logs advance, delete the `chosen/framebuffer` node
  (fdtput) and rely on sysfb/GOP, or vice versa.
- DtPlatformDxe DT-vs-ACPI preference variable: if kernel reports ACPI boot, flip the
  DtAcpiPref default (PCD in EmbeddedPkg) or delete ACPI tables from the FDF.
- heimdall sessions: `detect` succeeding does NOT mean sessions work — a wedged download mode
  times out on handshake; physically re-enter download mode.

## Established iteration workflow (as of 2026-07-04)
- **Kernel iterate (no flash):** edit → `make O=../out ARCH=arm64 LLVM=1 -j$(nproc) Image`
  (cmdline via `scripts/config --file ../out/.config --set-str CMDLINE "…"`; initramfs
  embedded from `~/r8q_linux/irfs` + `irfs.devnodes`) → phone to UEFI **Mass Storage** →
  mount `/dev/<119.1G-disk>32` → cp Image to `EFI/BOOT/BOOTAA64.EFI` → umount+sync → reboot.
  Device letter CHANGES between sessions — always re-find by size:
  `lsblk -rno NAME,SIZE,TYPE,TRAN | awk '$2=="119.1G" && $4=="usb"{print $1}'`.
- **DTB iterate (needs flash):** DTB lives IN the firmware. dtsi edit → `make … qcom/
  sm8250-samsung-r8q.dtb` → cp to `Mu-Silicium/Platforms/Samsung/r8qPkg/FdtBlob/` →
  `.venv/bin/python build_uefi.py -d r8q` (~20 s) → phone to download mode →
  `heimdall flash --BOOT Mu-Silicium/Mu-r8q-0.img` (heimdall reboots it).
- **Monitors pattern (host):** background watchers auto-stage the Image when mass storage
  appears / auto-flash when download mode appears / auto-configure 172.16.42.14 on the new
  NCM netdev and ping. Saves every round-trip.
- **Shell on phone:** after ~15 s of boot, telnet 172.16.42.1. Use
  `python3 tools/tsh.py 'cmd1; cmd2'` — plain ncat/nc chokes on telnet IAC negotiation.
  Fallback raw shell may listen on :24 (busybox `nc -ll -e`, if that applet supports -e).
- **Logs:** `/init` writes `ESP:/logs/boot-N.txt` at ~+13 s uptime and again ~+53 s.
  A missing boot-N.txt after a boot attempt = the system died before ~13 s (that's how the
  clock-hold hangs were proven). dpu-N.txt = retired DPU probe (crashes, see below).

## DISPLAY: hard-earned DO-NOTs (r8q + Mu-Silicium specifically)
1. **Never MMIO-read the DPU/DSI block** (0xae0xxxx via devmem or anything else) — instant
   PMIC-level hard reset. The display domain is unpowered/protected after ExitBootServices.
2. **Never put display clocks or MDSS_GDSC power-domain in the framebuffer DT node** (the
   sm8250-sony-xperia-edo.dtsi pattern) — total system hang at ~11.6 s on r8q. Samsung's
   firmware state differs from Sony's; any kernel-side clk enable/GDSC attach on the display
   domain is lethal here.
3. The screen dying at ~11.6 s does NOT mean the phone crashed — check for the NCM gadget /
   telnet before concluding anything. Film at 120 fps if the transition matters.
4. `initcall_blacklist=disp_cc_sm8250_driver_init` is the cmdline way to keep dispcc from
   registering; if it side-effects (fw_devlink starvation), the surgical alternative is the
   `protected-clocks` DT property on the dispcc node (qcom clk common.c honors it).
5. Long-term display = mainline msm-drm DPU+DSI + panel driver for EA8076A/AMS646UJ10
   (vendor DT: reset <0 10 1 15>, TE gpio 66, vdd3 = tlmm 93 fixed-regulator; nearest
   mainline relative: panel-samsung-s6e3fc2x01). Expect the TZ/xPU protection question to
   apply there too — ask Mu-Silicium folks what DisplayDxe leaves behind at EBS.

## Gotchas carried over from garnet (will apply here too)
- busybox modprobe doesn't resolve deps → insmod in tsort order.
- `/sys/kernel/debug/devices_deferred` + `waiting_for_supplier` + initcall_debug for silent defers.
- Never give udev a full module set; deliberate load order for bring-up modules.
- Film the screen at 120 fps for crash dmesg.
- Build a log-to-storage (ESP) path as early as possible; no serial exists.
