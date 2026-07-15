# Linux on the Samsung Galaxy S20 FE 5G (`r8q`)

Mainline Linux + **Arch Linux ARM** on the Samsung Galaxy S20 FE 5G Snapdragon
(codename **`r8q`**, SoC **SM8250 "kona" / Snapdragon 865**), booted through
**[Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) UEFI** — not a
downstream Android kernel, the **real mainline kernel**.

Everything here exists so that **anyone with their own r8q** can reproduce it
from clean upstream sources: the device-tree change that lights the panel, the
kernel config, the switch-root initramfs, the systemd services, and a small set
of scripts that take you from a UEFI flash to a booting Arch install you can SSH
into.

> **Read [`PREREQUISITES.md`](PREREQUISITES.md) first** (unlocked bootloader,
> host packages, and the fact that this **wipes your userdata**), then follow
> **[`INSTALLATION.md`](INSTALLATION.md)**.

> **No proprietary firmware is shipped.** Storage, USB and display run on
> built-in mainline drivers with no firmware at all. GPU acceleration needs the
> Adreno 650 firmware from **linux-firmware** (`a650_sqe.fw`, `a650_gmu.bin`)
> plus the **Samsung-signed zap shader from your own device's stock firmware**
> (Samsung's TrustZone only accepts Samsung's signature) — see
> [`INSTALLATION.md` §9](INSTALLATION.md).

---

## What works today

| # | Milestone | State |
|---|-----------|-------|
| 1 | Mainline kernel boots — BusyBox/switch-root initramfs, output on the phone screen | ✅ |
| 2 | **UFS storage** — all LUNs and the full GPT show up as `/dev/sd*` (first try, no hacks) | ✅ |
| 3 | **Display** — a *persistent, live* mainline framebuffer console on the AMOLED panel | ✅ |
| 4 | **Arch Linux ARM + systemd** on userdata, root autologin on the panel (`tty1`) | ✅ |
| 5 | **SSH over USB** — `ssh root@172.16.42.1`, plus **USB tethering** (internet + `pacman`) | ✅ |
| 6 | **GPU acceleration** — Adreno 650 via freedreno/turnip (GLES 3.2 + Vulkan 1.3) | ✅ |
| 7 | **GNOME 50** on the panel — gdm autologin, mutter rendering on the Adreno (sway still available as a fallback) | ✅ |
| 8 | **Touchscreen** — STM FTS5CU56A multitouch, working under GNOME | ✅ |

See the [**Roadmap**](#roadmap) below for what's next (Wi-Fi, USB host mode,
battery, Bluetooth, audio, the Volume-Up key, a greeter/lock screen, faster boot).

Mu-Silicium is
flashed to `BOOT`; the ESP is the `cache` partition reformatted vfat (`R8QESP`);
the rootfs is the `userdata` partition; the kernel is mainline **Linux 7.1.2**.

---

## Repository layout

```
r8q-arch/
├── README.md                     ← you are here
├── PREREQUISITES.md              ← read first (bootloader, host packages, data wipe)
├── INSTALLATION.md               ← the step-by-step
├── dts/                          ← the display fix + touchscreen node (sm8250-samsung-common.dtsi + r8q.dts)
├── patches/                      ← kernel patches (GPU zap-shader; i2c FIFO force; FTS5CU56A touch driver)
├── config/                       ← r8q_bringup.config, cmdline.txt
├── initramfs/                    ← switch-root /init (+ irfs.devnodes)
├── rootfs/                       ← systemd services, networkd, GNOME/gdm wiring, gadget script, GPU + touch services
└── scripts/                      ← build-uefi / flash / deploy-esp / install-arch / host-tether / build_kernel
```

## The short version of how it boots

Download mode → `heimdall` flashes **Mu-Silicium UEFI** (with our DTB embedded)
to `BOOT` → UEFI auto-runs `\EFI\BOOT\BOOTAA64.EFI` (our kernel `Image`, EFI-stub,
with an embedded **switch-root initramfs**) → the initramfs mounts `userdata` and
`switch_root`s into **Arch Linux ARM / systemd** → NCM USB gadget comes up →
`ssh root@172.16.42.1`.

The DTB lives **inside the firmware** (Mu-Silicium exposes it as an EFI config
table via `DtPlatformDxe`), so DTB changes need a re-flash; the kernel `Image`
lives on the **ESP** and is swapped over mass-storage mode.

## How the GPU works (short version)

There is no mainline panel driver yet, so display and GPU are split: the panel
keeps scanning out of the firmware-lit `simple-framebuffer` (`card0`), while the
`msm` module is loaded post-boot with `separate_gpu_kms=1` and provides only the
Adreno 650 **render node** (`renderD128`). Compositors render on the GPU and
blit into the dumb buffer.

Two r8q-specific things make the GPU actually render (both in
[`patches/`](patches/)):
- Samsung's TrustZone rejects the generic zap shader from linux-firmware, and
  the GPU then stays locked in secure mode — every render write is silently
  dropped. You need the **Samsung-signed zap** from your own stock firmware.
- Loading that zap into the DT carveout hard-resets the SoC (Samsung's TZ
  rejects the region); the `msm.r8q_zap_dyn=1` patch loads it into dynamically
  allocated RAM instead, exactly like stock Android's `pil-tz-generic` does.

`rootfs/` ships the pieces: `r8q-gpu.service` (loads `msm` after boot — never
at coldplug, and **never unload it**), the modprobe options, and the GNOME
desktop wiring — a `mutter-device-preferred-primary` udev tag so mutter renders
on the Adreno (`renderD128`) and scans out on simpledrm, a gdm drop-in ordered
after `r8q-gpu.service`, and gdm autologin. (A tty1 autologin profile that
starts **sway** on the GPU is kept as a fallback.)

### Touchscreen

The STM **FTS5CU56A** touch controller sits on `i2c5` (`qupv3_se5`), powered by
a fixed LDO on **TLMM GPIO 92** that the firmware leaves off. No mainline driver
speaks its Samsung-TSP protocol, so `patches/fts5cu56a.c` is a small from-scratch
driver (device-ID handshake, system reset, the touchtype/scan-on command
sequence, and the 16-byte FIFO event format). Two quirks make the bus usable:
the SE5 GPI-DMA channels are TrustZone-owned and its FIFO writes hang, so
`patches/0003` adds `i2c-qcom-geni.r8q_force_fifo=1` (skip GPI, route data
through the SE-DMA path). `r8q-touch.service` loads the stack after boot.

---

## Roadmap

The device boots to an accelerated GNOME desktop you can touch and SSH into.
What's left, roughly in the order it's worth doing:

| Goal | What it needs | Notes |
|------|---------------|-------|
| **Faster boot** | Stop rendering every kernel `printk` to the slow fbcon | `loglevel=8 ignore_loglevel keep_bootcon` blits thousands of lines onto simpledrm by CPU — the single biggest boot-time sink. Lower the console loglevel (keep the ESP log for debugging) and boot should shorten dramatically. |
| **Volume-Up key** | Fix the `gpio-keys` vol-up mapping | Vol-Down works (PMIC `resin`); Vol-Up (`pm8150_gpios` gpio3) currently emits nothing. Needs an input/pinctrl check against the stock DT. Also unlocks a future GRUB/boot menu. |
| **Battery / charging** | PMIC fuel-gauge + charger drivers (`pm8150b`, `max77705`) | Battery profile data is already in the stock DT. Start with read-only capacity/voltage reporting, then charging control. |
| **Wi-Fi** | QCA6390 remoteproc (WPSS/WLAN) + `ath11k` + firmware | The headline "untethered" goal. Delicate on Samsung — remoteproc coldplug is what caused the early bootloops, so bring it up deliberately, not via udev. |
| **Bluetooth** | QCA6390 BT (`hci_qca` over UART) | Shares the QCA6390 power/remoteproc bring-up with Wi-Fi — cheapest to do right after it. |
| **USB host mode** | dwc3 role switch + VBUS (`pm8150b` regulator or powered OTG hub) | Currently peripheral-only (that's how SSH works). Host mode gets a real keyboard/mouse. Needs a role-switch path and VBUS supply. |
| **Greeter / lock screen** | Replace gdm autologin with a real login, add an on-screen keyboard | Now that touch works, a touch OSK makes a headless login viable. Pure userspace/config work; no kernel changes. |
| **Audio** | LPASS + WCD938x codec + `cs35l41` speaker amps (SoundWire) | The hardest mainline bring-up here; lowest priority for a dev device. |
