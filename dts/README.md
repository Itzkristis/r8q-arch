# Device tree — the display fix

The r8q device tree is **upstream** since Linux 6.18 (`sm8250-samsung-r8q.dts` +
`sm8250-samsung-common.dtsi`). The only change needed to get a **persistent live
framebuffer on the panel** is in `sm8250-samsung-common.dtsi` — two additions:

### 1. `&dispcc { protected-clocks = <0 … 57>; }`
Keep the display clock controller **enabled** (so it probes and `fw_devlink` is
satisfied — disabling the node hangs boot), but mark every dispcc clock as
*protected* so the qcom clock driver registers the controller yet **never touches
those clocks**. This leaves the byte/pixel/MDP clocks that Samsung's UEFI left
driving the live DSI scanout exactly as firmware set them.

### 2. `framebuffer@9c000000 { power-domains = <&dispcc MDSS_GDSC>; }`
Make `simpledrm` an active consumer of the **MDSS GDSC** power domain so genpd
never powers it off (`pd_ignore_unused` does *not* cover it). Without this the
panel freezes ~13 s in when Linux gates `mdss_gdsc`. **No `clocks` in this node** —
those are handled by `protected-clocks`; adding clocks here hangs boot.

## How to use
These are the complete source files. Drop them into a mainline kernel tree at
`arch/arm64/boot/dts/qcom/`, build the DTB, and embed it into Mu-Silicium
(`scripts/build-uefi.sh`). Boot with `simpledrm` enabled and the cmdline in
[`../config/cmdline.txt`](../config/cmdline.txt) (`clk_ignore_unused
pd_ignore_unused arm-smmu.disable_bypass=0`).

Good upstream-patch candidate for samsung-r8q (the `protected-clocks` + fb
power-domain pattern is the sanctioned "no panel driver yet" approach; cf.
`sm8250-sony-xperia-edo.dtsi`, which uses the clock-hold variant).
