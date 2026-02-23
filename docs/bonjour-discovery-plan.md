# Plan: Bonjour/mDNS Auto-Discovery

## Context

Currently, the receiver IP must be typed manually via `zinkos set ip X.X.X.X`. This works but is friction for new users. Adding mDNS discovery means receivers announce themselves on the network and a macOS menu bar app shows them in a list — select one and you're streaming. Zero typing.

## Two Independent Components

### Part 1: Receiver — Avahi advertisement (simple)

The Linux receiver advertises itself as `_zinkos._udp` via Avahi. Just an XML file.

**New file:** `receiver/c/zinkos.service.avahi`
```xml
<service-group>
  <name replace-wildcards="yes">Zinkos on %h</name>
  <service>
    <type>_zinkos._udp</type>
    <port>4010</port>
    <txt-record>version=1</txt-record>
    <txt-record>format=s16le-48000-2ch</txt-record>
  </service>
</service-group>
```

**Modify:** `receiver/c/Makefile`
- Add `install-avahi` target: copies XML to `/etc/avahi/services/zinkos.service`
- Add `uninstall-avahi` target
- Hook into existing `install-service` / `uninstall-service`

**Modify:** `receiver/c/install.sh`
- Soft warning if `avahi-daemon` not installed (receiver still works without it)

**Modify:** `receiver/c/zinkos-rx.service`
- Add `After=avahi-daemon.service` and `Wants=avahi-daemon.service` (weak dep)

### Part 2: macOS Swift menu bar app

A lightweight menu bar app that discovers receivers and writes to the same plist the driver reads.

**New directory:** `sender/app/`

```
sender/app/
  Package.swift
  Sources/ZinkosMenu/
    ZinkosMenuApp.swift     # @main, MenuBarExtra, no dock icon
    ReceiverBrowser.swift   # NWBrowser for _zinkos._udp discovery
    ReceiverInfo.swift      # Model: name, host, ip, port
    MenuView.swift          # SwiftUI dropdown: receiver list + status
    PlistConfig.swift       # UserDefaults(suiteName: "com.zinkos.driver")
    AudioDaemon.swift       # Reload coreaudiod via AppleScript admin prompt
```

**Key technical choices:**
- **MenuBarExtra** (macOS 13+) — native SwiftUI menu bar item, no dock icon
- **NWBrowser** (Network framework) — discovers `_zinkos._udp` services
- **Resolution** — resolve hostname to IP only when user selects (not all upfront)
- **Plist write** — `UserDefaults(suiteName: "com.zinkos.driver")` — same as `defaults write`, driver sees it
- **coreaudiod reload** — AppleScript `with administrator privileges` triggers macOS password dialog
- **Build** — `swift build -c release`, no Xcode project needed

**Menu dropdown structure:**
```
 Connected to: 192.168.1.87:4010
 ─────────────────────────────
 Receivers
   ✓ Zinkos on living-room-pi
     Zinkos on bedroom-laptop
 ─────────────────────────────
 Set IP Manually...
 ─────────────────────────────
 Quit Zinkos Menu
```

Selecting a receiver → resolves IP → writes plist → prompts for password → restarts coreaudiod → streaming.

## Implementation Order

1. Receiver Avahi files (quick — just XML + Makefile changes)
2. Swift app scaffold (Package.swift, app entry, model)
3. ReceiverBrowser (NWBrowser discovery)
4. PlistConfig + AudioDaemon (config write + reload)
5. MenuView (wire it all together)
6. Polish: .gitignore, README, login item

## Files Modified

- `receiver/c/Makefile` — add avahi targets
- `receiver/c/install.sh` — avahi check
- `receiver/c/zinkos-rx.service` — avahi ordering

## Files Created

- `receiver/c/zinkos.service.avahi` — mDNS service definition
- `sender/app/Package.swift`
- `sender/app/Sources/ZinkosMenu/*.swift` (6 files)

## Verification

1. On Linux: install avahi service, run `avahi-browse -r _zinkos._udp` from another machine — should show receiver
2. On Mac: `cd sender/app && swift build && swift run` — menu bar icon appears, discovered receivers show in dropdown
3. Select a receiver — plist updates, coreaudiod restarts, audio streams
4. CLI still works independently (`zinkos set ip` / `zinkos reload`)
