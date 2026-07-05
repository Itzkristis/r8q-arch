# Fix: The Framebuffer Garble ("the flash") on r8q — Deep Dive

**Device:** Samsung Galaxy S20 FE 5G Snapdragon (samsung-r8q, SM8250 / Snapdragon 865)
**Stack:** mainline Linux 6.17 (postmarketOS kernel) booted via Mu-Silicium UEFI
**Date fixed:** 2026-06-23

---

## 1. What "the flash" problem actually was

Once the kernel booted, the EFI framebuffer console (`earlycon=efifb`) rendered as
fine-grained flickering **"cityscape" static** — sheared vertical bars, the boot text
smeared into garbage. You could faintly see the kernel banner running *vertically* down
the left edge, which proved the framebuffer WAS receiving the real console log — it just
wasn't being drawn correctly. The whole screen looked like corrupted flashing/static,
which blinded the earlycon and hid everything past the first few seconds of boot.

Crucially, this was **not** a data-corruption or memory-clobber problem. The bytes were
right; the *geometry* the kernel used to paint them onto the framebuffer was wrong.

---

## 2. Root cause: two owners of the framebuffer disagreeing on geometry

There are **two independent parties** that each have an opinion about the framebuffer:

1. **UEFI's GOP** (Graphics Output Protocol), provided by Mu-Silicium's `SimpleFbDxe`.
   This is what draws the "Project Silicium" logo and owns the actual display hardware
   at handoff. It has a concrete base address, width, height, **stride** (bytes per row),
   and pixel format.

2. **The kernel's `simple-framebuffer` driver**, which paints the `earlycon=efifb`
   console. It gets *its* geometry from the **device tree** node
   `framebuffer@9c000000` in `sm8250-samsung-common.dtsi`:
   ```
   reg    = <0 0x9c000000 0 0x2300000>;
   width  = 1080  (0x438)
   height = 2400  (0x960)
   stride = 4320  (0x10e0)
   format = a8r8g8b8
   ```

The garble is a **stride/format mismatch** between these two. If the kernel paints rows
at a stride even slightly different from what the hardware scanout actually uses, every
row lands shifted from the previous one → the image shears diagonally into that
"cityscape" static. Same bytes, wrong row pitch.

### Why the mismatch happened in our setup
We were **injecting the DTB ourselves** via the systemd-stub UKI's `.dtb` section.
That means:
- **UEFI** set up the GOP framebuffer using *its own* internal values.
- **The kernel** got its framebuffer geometry from *our injected DTB*.
- Nobody guaranteed those two agreed. Two separate sources of truth for one piece of
  hardware → inevitable drift → garble.

This was confirmed by ruling out every other theory:
- **Framebuffer clobber?** No — reserved-memory `memory@9c000000` (no-map) exactly
  covers the framebuffer region, so nothing overwrites it. Disproven.
- **Display driver fighting it?** No — `display-subsystem@ae00000` (`qcom,sm8250-mdss`)
  is already `status="disabled"` upstream, so DPU/DSI never probe. Disproven.
- The banner being *readable vertically* was the tell: it's the real log, mis-strided.

---

## 3. The fix: make UEFI the single owner of BOTH the framebuffer AND the FDT

The maintainer (Robotix22) pointed at the correct architecture: **stop injecting the DTB
from the loader. Let UEFI embed the FDT and install it into the EFI configuration table,
so the firmware owns both the GOP framebuffer and the device tree — from the same source.**

When one entity owns both halves, the `simple-framebuffer` node the kernel reads is the
one that ships *with the firmware that set up the GOP* — so their geometry is guaranteed
consistent. The mismatch cannot occur.

### Mechanism (EDK2 / Mu-Silicium internals)
- Mu-Silicium's `EmbeddedPkg/Drivers/DtPlatformDxe/DtPlatformDxe.inf` is a DXE driver
  whose job is exactly this: take an embedded FDT blob and **install it into the EFI
  system configuration table under `gFdtTableGuid`**.
- The Linux **EFI stub** looks for `gFdtTableGuid` in the config table at boot. If it
  finds an FDT there, it uses it — *instead of* ACPI, and instead of anything else.
- Mu-Silicium for r8q is **ACPI-based and ships no FDT by default**, so `DtPlatformDxe`
  and its blob are commented out in the device's flash-description file. We enable them.

### The two concrete changes

**(A) Embed the DTB in the firmware.** In
`~/Mu-Silicium-main/Platforms/Samsung/r8qPkg/r8q.fdf`, uncomment the DTB block:
```
  # Device Tree
  INF EmbeddedPkg/Drivers/DtPlatformDxe/DtPlatformDxe.inf
  FILE FREEFORM = 25462CDA-221F-47DF-AC1D-259CFAA4E326 {
    SECTION RAW = r8qPkg/FdtBlob/sm8250-samsung-r8q.dtb
    SECTION UI = "Device Tree"
  }
```
Place the **upstream** `sm8250-samsung-r8q.dtb` (NOT Mu's downstream `qcom,kona-mtp` one,
NOT a hacked variant — the clean stock one, watchdog enabled) at:
```
~/Mu-Silicium-main/Platforms/Samsung/r8qPkg/FdtBlob/sm8250-samsung-r8q.dtb
```
Then rebuild the firmware (**RELEASE** — see §4) and flash to BOOT.

**(B) Remove the `.dtb` section from the UKI.** If the systemd-stub UKI *also* carries a
`.dtb`, the stub's DTB **wins over** the firmware's config-table FDT — which would defeat
the whole fix (we'd be back to a second source of truth). So strip it:
```fish
printf 'earlycon=efifb keep_bootcon loglevel=8 ignore_loglevel clk_ignore_unused pd_ignore_unused' > ~/cmdline.txt
truncate -s 266 ~/cmdline.txt
llvm-objcopy --remove-section=.dtb --update-section=.cmdline=$HOME/cmdline.txt \
  $HOME/BOOTAA64.backup.efi $HOME/BOOTAA64.EFI
llvm-objdump --headers ~/BOOTAA64.EFI | grep -E "\.linux|\.dtb|\.cmdline|\.initrd"
```
Verify the objdump shows `.linux`, `.initrd`, `.cmdline` and **NO `.dtb`**. That absence
is the point: with no DTB in the UKI, the stub falls through to UEFI's config-table FDT.

### Result
The kernel log came up **perfectly readable** — DMI "Samsung Galaxy S20 FE", BIOS 3.6,
all 8 CPUs, pstore/ramoops, full driver init — no shear, no static. The garble was gone.
UEFI owning both the GOP framebuffer and the FDT eliminated the geometry mismatch. ✓

---

## 4. Critical gotcha discovered during the fix: DEBUG vs RELEASE

Embedding `DtPlatformDxe` + the 111 KB DTB pushed **FVMAIN to 99% full** (only ~160–700
bytes free). This is not fatal — the FV still packs and produces a valid image — BUT it
interacts badly with the **DEBUG** build target:

- A **DEBUG** firmware with the embedded DTB **hangs ~10 minutes on the DXE
  driver-loading wall and may never reach Linux.** The 99%-full FV plus the slow,
  verbose framebuffer text rendering during DXE dispatch chokes it.
- A **RELEASE** firmware skips all that verbose logging, reaches the kernel quickly, and
  is the one that actually boots and shows the clean framebuffer.

**Therefore: always boot the RELEASE build.** DEBUG was only ever wanted because it
exposes USB **mass-storage mode** for deploying the UKI to the ESP — but since
DEBUG-with-embedded-DTB won't boot Linux, we use a **split workflow**:

| Job | Firmware to use | Why |
|---|---|---|
| Deploy the UKI to the ESP | **known-good backup** `~/Downloads/Mu-r8q-0.img` | reliable mass storage |
| Boot Linux + read the framebuffer | **RELEASE embedded-DTB** `~/Mu-Silicium-main/Mu-r8q-0.img` | boots clean, fixed FB |

(The UKI lives on the ESP and survives firmware reflashes, so you deploy it once with the
backup firmware, then just reflash the RELEASE firmware to iterate on DTBs.)

---

## 5. Why this was the *right* fix and not a workaround

An alternative would have been to hand-tune the kernel's `simple-framebuffer` stride to
match UEFI's GOP by trial and error. That's brittle: any firmware change to the GOP
(resolution, stride padding, format) would silently re-break it. The embedded-FDT
approach fixes the problem **structurally** — there is now exactly one source of truth
for the framebuffer geometry (the firmware), so the two halves can never disagree again,
regardless of future GOP tweaks.

---

## 6. One-paragraph summary (for the impatient)

The screen garbled because two things — UEFI's GOP framebuffer and the kernel's
`simple-framebuffer` (fed by a DTB we injected via the UKI) — disagreed on the row
**stride**, shearing the console into static. The fix was to embed the upstream DTB into
the Mu-Silicium firmware (uncomment `DtPlatformDxe` + the `SECTION RAW` DTB blob in
`r8q.fdf`), so UEFI installs the FDT into the EFI config table itself, **and** to remove
the `.dtb` section from the UKI so the systemd-stub doesn't override it. Now UEFI owns
both the framebuffer and the device tree from the same source, their geometry matches,
and the console renders cleanly. Build it as **RELEASE** (DEBUG hangs in DXE because the
FV is 99% full).