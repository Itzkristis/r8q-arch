[ -f ~/.bashrc ] && . ~/.bashrc
# Auto-start sway on the tty1 autologin VT (guarded so ssh sessions are unaffected).
# r8q-gpu.service loads msm after multi-user, so wait briefly for the render node;
# if it never appears, stay at a shell (an instantly-dying exec sway crash-loops getty).
if [ -z "$WAYLAND_DISPLAY" ] && [ "$(tty)" = "/dev/tty1" ]; then
    for i in $(seq 1 30); do [ -e /dev/dri/renderD128 ] && break; sleep 1; done
    if [ -e /dev/dri/renderD128 ]; then
        export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
        mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
        # Render on Adreno 650 (turnip), scan out on simpledrm card0.
        # card1 = msm KMS stub (no connectors) — must not be picked as output.
        export WLR_DRM_DEVICES=/dev/dri/card0
        export WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128
        export WLR_RENDERER=${R8Q_WLR_RENDERER:-vulkan}
        export XKB_DEFAULT_LAYOUT=us
        exec sway -d > /tmp/sway.log 2>&1
    fi
fi
