# SharedCrew

Free shared cockpit for X-Plane 12. Two pilots on two PCs fly one aircraft together:
one is the host, the other joins. Switches, knobs, radios and autopilot buttons stay in sync
both ways, and the controls can be handed over in flight.

Status: early development (0.1). Builds on Windows. Not yet tested in X-Plane.

## How it works

- The **pilot flying** sends position, attitude, yoke, throttle and fuel 30 times per second.
  The other PC follows smoothly.
- **Switches and knobs** are synced both ways. The last change wins.
- **Buttons** (commands) pressed on one PC are pressed on the other too.
- What to sync is defined per aircraft in text files in `profiles/`.
  `default.txt` covers standard X-Plane datarefs. Add a file with a `match` line for each aircraft.

## Using it

1. Both pilots load the same aircraft at the same airport.
2. Open *Plugins > SharedCrew > Show / hide window*.
3. Host: click **Host a session**. The host needs UDP port 51000 forwarded in the router,
   or both pilots use the same VPN (ZeroTier, Radmin VPN).
4. Other pilot: enter the host's address and click **Join host**.
5. **Take controls** hands the aircraft over.

## Building (Windows)

Needs Visual Studio 2022 Build Tools (C++) and CMake.

```
powershell -ExecutionPolicy Bypass -File tools\get-sdk.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\install.ps1
```

The plugin ends up in `build/dist/SharedCrew`.

## Writing a profile

```
title My Aircraft
match My_Aircraft          # part of the .acf file name
nodefault                  # optional: don't include default.txt
sync  some/dataref         # two-way
sync  some/array[2]        # one element of an array
stream some/dataref        # from the pilot flying, every frame
command some/command       # mirror button presses
```

Use DataRefTool to find an aircraft's datarefs and commands.

## Testing on one PC

`SharedCrew-TestPeer.exe` (built to `build/dist/TestPeer`) pretends to be the second pilot.
Start it, join `127.0.0.1` in X-Plane, then use its keys: T take controls (it flies turns),
G give back, L/B toggle lights, A press autopilot HDG.

## License

GPL-3.0. SharedCrew is free and stays free: anyone who shares a changed version must share the source too.
