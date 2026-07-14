# Installation

This gets you a booting **Arch Linux ARM** on the r8q, reachable from your PC
over the USB cable (SSH + internet). Read [`PREREQUISITES.md`](PREREQUISITES.md)
first — **this wipes `userdata`.**

Throughout: `$KSRC` = your mainline kernel source, `$OUT` = its build dir
(`O=`), `$MUSIL` = your Mu-Silicium checkout. `KV` is the kernel release (e.g.
`7.1.2`).

---

## 1. Build the kernel (Image + DTB)

Drop the two device-tree files from [`dts/`](dts/) into
`$KSRC/arch/arm64/boot/dts/qcom/` (they carry the display fix).
`build_kernel.sh` also applies the kernel patches from [`patches/`](patches/)
(required for GPU acceleration later — harmless otherwise). Then:

```bash
# arm64 defconfig + our fragment, LLVM=1, bring-up drivers built-in
KSRC=... OUT=... ./scripts/build_kernel.sh
```

The kernel is built with:
- an **embedded switch-root initramfs** from [`initramfs/`](initramfs/)
  (`CONFIG_INITRAMFS_SOURCE` = the dir with `init` + `irfs.devnodes`; you also
  need a static aarch64 busybox in `bin/busybox`),
- **`CONFIG_CMDLINE_FORCE`** set to the string in
  [`config/cmdline.txt`](config/cmdline.txt) (the phone has no keyboard, so the
  cmdline is baked in). Keep `simpledrm` **enabled**.

Outputs: `$OUT/arch/arm64/boot/Image` and
`$OUT/arch/arm64/boot/dts/qcom/sm8250-samsung-r8q.dtb`.

## 2. Embed the DTB and build UEFI

The DTB lives **inside the firmware** (Mu-Silicium exposes it to the kernel via
`DtPlatformDxe`). Enable the "Device Tree" FREEFORM block in
`$MUSIL/Platforms/Samsung/r8qPkg/r8q.fdf`, then:

```bash
MUSIL=$MUSIL DTB=$OUT/arch/arm64/boot/dts/qcom/sm8250-samsung-r8q.dtb ./scripts/build-uefi.sh
# -> $MUSIL/Mu-r8q-0.img
```

## 3. Flash UEFI to BOOT

Put the phone in **download mode** (power off; VolUp+VolDown; plug USB):

```bash
./scripts/flash.sh $MUSIL/Mu-r8q-0.img       # heimdall flash --BOOT ...; it reboots
```

The phone now boots Mu-Silicium UEFI on every power-on.

## 4. Prepare the ESP (once)

Boot the phone into Mu-Silicium **mass-storage** mode. The ESP is the phone's
`cache` partition — reformat it vfat once (UFS logical block is 4096):

```bash
ESP=$(lsblk -o NAME,PARTLABEL -rn | awk '$2=="cache"{print "/dev/"$1}')
sudo mkfs.vfat -F 32 -S 4096 -n R8QESP "$ESP"
```

## 5. Deploy the kernel Image to the ESP

Still in mass-storage mode:

```bash
./scripts/deploy-esp.sh $OUT/arch/arm64/boot/Image   # -> ESP:/EFI/BOOT/BOOTAA64.EFI
```

## 6. Install Arch onto userdata

Still in mass-storage mode (this **formats userdata**):

```bash
KV=7.1.2 OUT=$OUT KSRC=$KSRC ./scripts/install-arch.sh
```

It extracts Arch Linux ARM, **moves the kernel module tree aside** (cold-plugging
the full tree hard-resets the SoC — see the note in the script), lays down our
[`rootfs/`](rootfs/) overlay, enables `sshd` + `systemd-networkd` +
`r8q-usb-gadget`, sets root autologin on `tty1`, root password `root`, and
disables the pacman sandbox (our kernel has no Landlock).

## 7. Boot

Exit mass storage and let the phone boot. You should see the panel show the
switch-root message, then systemd, then a root shell (autologin). On the PC an
NCM network device appears:

```bash
DEV=<the new cdc_ncm netdev>
sudo ip addr add 172.16.42.14/24 dev "$DEV"; sudo ip link set "$DEV" up
ssh root@172.16.42.1            # password: root
```

## 8. USB tethering (internet + pacman)

Share the PC's internet to the phone (host NAT is runtime — re-run after a PC
reboot):

```bash
./scripts/host-tether.sh        # enables ip_forward + MASQUERADE for 172.16.42.0/24
```

The phone already has `Gateway=172.16.42.14` + DNS baked into
`rootfs/etc/systemd/network/20-usb0.network`, so once the host NAT is up:

```bash
ssh root@172.16.42.1 'ping -c2 archlinux.org'
# first pacman use on a fresh rootfs:
ssh root@172.16.42.1 'pacman-key --init && pacman-key --populate archlinuxarm && pacman -Sy'
```

## 9. GPU acceleration (Adreno 650) + sway

Prereq: the kernel was built **with the [`patches/`](patches/) applied**
(`build_kernel.sh` does this) and its modules are installed on the rootfs —
at minimum `msm.ko` and its dependencies under `/lib/modules/$KV/`.

**a) Userspace + generic firmware** (on the phone, over SSH):

```bash
pacman -S mesa vulkan-freedreno linux-firmware-qcom sway foot seatd
systemctl enable --now seatd
```

That provides `/lib/firmware/qcom/a650_sqe.fw` and `a650_gmu.bin`.

**b) The zap shader — from YOUR device's stock firmware.** Samsung's TrustZone
only authenticates a **Samsung-signed** zap; the generic
`qcom/sm8250/a650_zap.mbn` from linux-firmware is rejected (`-22`) and the GPU
then silently drops every render write. Get the stock firmware for your model
(e.g. the AP tarball from samfw/frija), pull `a650_zap.mdt` + `a650_zap.b00/.b01/.b02`
out of the `vendor` image (`/vendor/firmware/`), and install them as:

```
/lib/firmware/qcom/sm8250/a650_zap.mbn    <- the stock a650_zap.mdt, renamed
/lib/firmware/qcom/sm8250/a650_zap.b00
/lib/firmware/qcom/sm8250/a650_zap.b01
/lib/firmware/qcom/sm8250/a650_zap.b02
```

**c) Module options + post-boot load.** The [`rootfs/`](rootfs/) overlay ships
these (already in place if you re-ran the overlay):

- `etc/modprobe.d/r8q-gpu.conf` — `blacklist msm` **plus**
  `options msm separate_gpu_kms=1 r8q_zap_dyn=1 r8q_zap_secvid=0`.
  `r8q_zap_dyn=1` is **required**: it loads the zap into dynamically allocated
  RAM; pointing it at the DT carveout makes Samsung's TZ **hard-reset the SoC**.
- `etc/systemd/system/r8q-gpu.service` — loads `msm` after `multi-user.target`
  (never let udev coldplug it). `systemctl enable r8q-gpu.service`.
- `root/.bash_profile` — tty1 autologin waits for `renderD128`, then starts
  **sway** with the vulkan (turnip) renderer: render node `renderD128`,
  scanout on simpledrm `card0`.

**d) Verify** (after a reboot):

```bash
ssh root@172.16.42.1 'ls /dev/dri; dmesg | grep -i zap'
# want: renderD128 present, "r8q: zap region dma_alloc'd at ..." and NO "zap auth failed"
```

Sway should be on the panel. Rules of the road: **never `rmmod msm`** (GMU/IOMMU
teardown wedges the kernel — load once per boot), and never write the SECVID
registers from the kernel (the hypervisor traps them; that is what
`r8q_zap_secvid=0` keeps disabled).
