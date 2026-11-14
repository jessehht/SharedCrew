# SharedCrew

Free shared cockpit for X-Plane 12. Two pilots on two PCs fly one aircraft together:
one is the host, the other joins. Switches, knobs, radios and autopilot buttons stay in sync
both ways, and the controls can be handed over in flight.

Status: early development (0.3). Tested on one PC with the test peer; not yet tested between two homes.

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
3. Host: click **Host a session**, then **Copy** the session code (like `K7P-4QX`) and send it to the
   other pilot (Discord, WhatsApp...).
4. Other pilot: **Paste** the code and click **Join**.
5. **Take controls** hands the aircraft over.

No VPN or router settings needed. The SharedCrew server hands out the short codes and tells both
PCs where the other one is, so they can connect directly. If a direct connection is not possible
(for example with Starlink or some fibre providers), the traffic goes through the server instead.

If the server can't be reached, SharedCrew falls back to a long code without a server: the router
port is opened with UPnP when the router allows it, and otherwise the guest gets a **reply code**
for the host to paste (**Let guest in**). Entering an IP address still works too.

## Server

The matchmaking and relay server is in `server/` (Python 3, standard library only, one UDP port).
The plugin uses `sharedcrew.mijngms.com:51010`. To use another server, add `server=host:port`
to `SharedCrew/settings.txt`, or `server=off` to never use one.

```
sudo useradd --system --no-create-home --shell /usr/sbin/nologin sharedcrew
sudo install -D -m 644 server/sharedcrew_server.py /opt/sharedcrew/sharedcrew_server.py
sudo install -m 644 server/sharedcrew.service /etc/systemd/system/
sudo systemctl enable --now sharedcrew
sudo ufw allow 51010/udp
python3 server/test_server.py 127.0.0.1 51010
```

## Aircraft

- **Standard X-Plane aircraft** (Cessna 172 and others): `profiles/default.txt`. The pilot flying
  sends position and controls; **Take controls** hands the aircraft over.
- **Aircraft with a SmartCopilot config** (all ToLiss Airbuses and many others): SharedCrew reads the
  `smartcopilot.cfg` in the aircraft folder (it is never copied). The host's PC computes the aircraft and
  both pilots fly at the same time, like a real Airbus with two sidesticks. SharedCrew provides
  `scp/api/ismaster`, so the aircraft switches on its own shared-cockpit logic.
  - ToLiss: each pilot uses their own MCDU (captain left, first officer right), so both can have a
    different page open while the FMS data (route, performance, V-speeds) is shared.
    Use the 3D MCDU in the cockpit, not the 2D pop-up.
  - MCDU key presses and other short-lived values are checked every frame and delivered in order.
- Both pilots can always use every switch and button, whoever is flying.

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
master some/dataref        # from the pilot flying, only when it changes
back  some/dataref         # from the other pilot to the pilot flying, every frame
override some/dataref      # set to 1 on the other pilot's PC while connected
command some/command       # mirror button presses
fixedroles                 # host always computes the aircraft, both pilots fly together
import smartcopilot        # also use the aircraft's smartcopilot.cfg
```

Use DataRefTool to find an aircraft's datarefs and commands.

## Testing on one PC

`SharedCrew-TestPeer.exe` (built to `build/dist/TestPeer`) pretends to be the second pilot.
Start it, join `127.0.0.1` in X-Plane, then use its keys: T take controls (it flies turns),
G give back, L/B toggle lights, A press autopilot HDG.

`SharedCrew-TestPeer.exe --nat-check` checks your router: home and internet address, UPnP, and a sample session code.
`Test via server.bat` gets a short code from the server; `Test via relay.bat` only answers through the relay.

## License

GPL-3.0. SharedCrew is free and stays free: anyone who shares a changed version must share the source too.
