# OpenShao

<p align="center">
  <img src="docs/images/openshao.png" width="240" alt="OpenShao logo">
</p>

<p align="center">
  <a href="https://github.com/marcoorquera/OpenShao/releases/latest"><img src="https://img.shields.io/badge/Download-Latest-2563eb?style=for-the-badge&amp;logo=github&amp;logoColor=white" alt="Download the latest release"></a>
  <a href="https://github.com/marcoorquera/OpenShao/releases/latest"><img src="https://img.shields.io/github/v/release/marcoorquera/OpenShao?style=for-the-badge&amp;logo=github&amp;label=Latest%20release&amp;cacheSeconds=300" alt="Latest stable release"></a>
  <a href="https://github.com/marcoorquera/OpenShao/releases"><img src="https://img.shields.io/github/downloads/marcoorquera/OpenShao/total?style=for-the-badge&amp;logo=github&amp;label=Release%20downloads" alt="Total GitHub release downloads"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-GPL--2.0--or--later-blue?style=for-the-badge" alt="GPL-2.0-or-later license"></a>
</p>

OpenShao is a native PC port of **Mortal Kombat: Shaolin Monks**, made by statically recompiling the original Xbox game.
This fork builds on the upstream [OpenShao](https://github.com/OAleex/OpenShao) project and adds deterministic UDP frame-lockstep **LAN / Peer-to-Peer Ko-op and Versus Netplay**.

> [!IMPORTANT]
> OpenShao does not include the game. You need your own **USA/Europe Xbox copy**.

## Contents

- [Requirements](#requirements)
- [Install](#install)
- [Update](#update)
- [Features](#features)
- [Known issues](#known-issues)
- [FAQ](#faq)
- [Building](#building)
- [Credits](#credits)

## Requirements

- Windows 10 1903 or later, or Linux with glibc 2.38 or later
- x64 CPU with SSE4.2 and POPCNT
- Direct3D 12 GPU, or Vulkan 1.2 with dynamic rendering and timeline semaphores
- Controller, or keyboard and mouse
- On Linux, the system `libcurl` for mod and update downloads

## Install

### Windows Release Installation

1. Go to the [Releases](https://github.com/marcoorquera/OpenShao/releases) page and download the latest `OpenShao-*-Windows-x64.zip` package.
2. Right-click the `.zip` archive and extract it to a folder on your PC (for example, `C:\Games\OpenShao`).
3. Run `OpenShao.exe` once to open the launcher.
4. Select your USA/Europe Xbox game copy (an ISO, XISO, ZIP, extracted folder or `default.xbe`).
5. Click **Play** to start single-player or local Ko-op, or close the launcher to start a 2-PC LAN Netplay session below.

Keep the game image or folder where it is. OpenShao reads it in place.

### 2-PC LAN / Peer-to-Peer Netplay Quick Start (Windows)

1. Install the same Windows release `.zip` and configure the game in the launcher on **both PCs**.
2. On **PC 1 (Host / Player 1)**, open PowerShell in the extracted `OpenShao` folder, check your local IP with `ipconfig`, and run:
   ```powershell
   .\OpenShao.exe --no-launcher --netplay-host 27886
   ```
   *(If Windows Defender Firewall prompts for access, allow UDP port `27886` on Private and Public networks.)*
3. On **PC 2 (Client / Player 2)**, open PowerShell in the extracted `OpenShao` folder and connect using PC 1's IP address:
   ```powershell
   .\OpenShao.exe --no-launcher --netplay-join 192.168.1.50:27886
   ```
4. Both PCs will automatically handshake, synchronize clocks and inputs, and enter **Ko-op** or **Versus** together.

### Linux (including SteamOS / Steam Deck)

1. Download or build the `OpenShao-*-Linux-x64.tar.gz` package and extract it.
2. Run `./OpenShao`, select your game copy, and click **Play**.
3. On Steam Deck, run the native Linux build directly in Desktop Mode or add `OpenShao` as a Non-Steam Game to launch it from Gaming Mode.

## Update

The launcher checks for a new version when it opens. Select **Update** and it installs the new version and restarts.
Saves, settings and mods are kept.

To update manually, extract the new release to a new folder.
The first start of 0.3.0 moves the data and saves of earlier releases to the new folders.

## Features

- **Launcher** with game selection, mods, folders and automatic updates.
- **Konfigure Recompilation**, an in-game settings menu for display, graphics, controls, audio and language.
- **Pause menu** with Continue, Options, Konfigure Recompilation, Exit Game and Exit to Desktop. Exit Game returns to the main menu.
- **Widescreen** at 16:9, 16:10, 21:9, 32:9 or the window's own shape, with the original 4:3 as default.
- **Render scale** from 25% to 400%, anisotropic filtering and V-Sync.
- **Remappable controls** for controllers, keyboard and mouse, with mouse camera support.
- **Local Ko-op** with keyboard and mouse plus one controller, or two controllers.
- **LAN / Peer-to-Peer Ko-op and Versus (Netplay)** over UDP frame lockstep between two systems (including Windows and Linux / Steam Deck).
- **Separate text and voice languages**, including Brazilian Portuguese through a language pack.
- **Mods** from the [OpenShao-Mods catalog](https://github.com/OAleex/OpenShao-Mods), installed and updated in the launcher.

## Known issues

- Most graphical issues have been fixed, but some can still appear. Any problem is worth reporting: [open an issue](https://github.com/OAleex/OpenShao/issues) with a screenshot.
- Mortal Kombat II and The Suffering demo do not run.
- A scene can pause briefly the first time it is shown while its shaders compile.

## FAQ

### The launcher says my game is invalid

Use the original USA/Europe Xbox release. Renamed or converted images from other releases are not supported.
The check covers the executable, archives and required movies.

### Where are my saves, settings and logs?

Open **Files** in the launcher. Each folder has an **Open** button, and **Move** places it somewhere else.

| System | Data | Saves |
| --- | --- | --- |
| Windows | `%LOCALAPPDATA%\OpenShao` | `Saved Games\OpenShao` |
| Linux | `~/.local/share/OpenShao` | `~/.local/share/OpenShao/saves` |

### The game does not start after I changed the display settings

Open the launcher and select **Files > Video > Restore**.

### What are the keyboard and mouse controls?

| Input | Key or button |
| --- | --- |
| Move | W A S D |
| D-pad | Arrow keys |
| A / B | Space / Esc |
| X / Y | J or left click / K or middle click |
| Black / White | C / Q |
| Triggers | Shift / E |
| Start / Back | Enter / Backspace |
| Camera | Mouse |
| FPS display | F9 |
| Release or capture the mouse | F10 |

Change them in **Konfigure Recompilation > Controls**. Each action has a primary and an alternate key.

### How do controllers and Ko-op work?

Xbox, PlayStation and other SDL-compatible controllers work. The game shows Xbox buttons.
In single player, the keyboard, mouse and first controller all control player 1.
In Ko-op, the keyboard and mouse are player 1 and a controller is player 2. With two controllers, each controller is one player.

For a controller without a mapping, put an SDL `gamecontrollerdb.txt` in the data folder.

### How does LAN Ko-op (Netplay) work?

Netplay synchronizes both players' inputs on each frame over UDP port `27886`.

1. On the host system (Player 1), open the launcher and select **Host LAN**.
2. Adjust the frame delay if needed (`1f` to `2f` for local networks).
3. Select **Play** to start waiting for the peer connection.
4. On the client system (Player 2), open the launcher and select **Join LAN**.
5. Enter the host IP address and port, then select **Play**.
6. Once both systems connect, enter **Ko-op** or **Versus** from the main menu.

To test with two windows on the same computer, connect the client to `127.0.0.1:27886`. The second instance automatically uses an isolated `netplay-peer` data folder when the primary data folder is in use.

Lockstep netplay can desynchronize during gameplay. LAN streaming avoids this.

### How does LAN streaming work?

The host runs the only copy of the game in local Ko-op. It sends video and audio to Player 2 over UDP port `27886`. Player 2 sends a gamepad or keyboard state back. Player 2 does not need the game. LAN streaming is available on Windows.

1. On the host system, run `.\OpenShao.exe --no-launcher --stream-host 27886`.
2. On the Player 2 system, run `.\OpenShao.exe --no-launcher --stream-join <host IP>:27886`.
3. Wait for Player 2 to connect before you select **Ko-op**.

Player 2 joins the host as one more gamepad. It becomes Player 2 behind the host's first gamepad. Player 2 can use the default OpenShao keyboard keys in the viewer window. Press F11 or Alt+Enter in the viewer window to switch to full screen.

### How do I install mods or the Portuguese language pack?

Open **Mods** in the launcher and select **Install**. A downloaded `.tar.xz` installs through **Install from file**.
Then choose the language in **Konfigure Recompilation > Language**. Mod authors can read the [mod guide](docs/MODS.md).

### How can I improve performance?

Lower **Render scale** in **Konfigure Recompilation > Graphics**. Press F9 to see the frame rate. The game is limited to 60 FPS.

### Why does OpenShao bring the open window forward instead of starting?

Only one copy can run at a time with the same data folder.

### Are there command-line options?

| Option | Effect |
| --- | --- |
| `--configure` | Open only the launcher settings |
| `--no-launcher` | Start the game with the saved settings |
| `--setup` | Choose the game again with the system dialogs |
| `--disc <image>`, `--game <folder>` | Play another copy of the game for this session |
| `--data-dir <path>` | Use another data folder for this session |
| `--save-dir <path>` | Use another saves folder for this session |
| `--portable-saves` | Keep saves in `hdd` next to the executable |
| `--paths` | Print the folders in use |
| `--netplay-host [port]` | Host a UDP lockstep netplay session as Player 1 (default port `27886`) |
| `--netplay-join <host[:port]>` | Join a UDP lockstep netplay session as Player 2 |
| `--netplay-delay <1..8>` | Set the netplay frame input delay |
| `--stream-host [port]` | Host a LAN streaming session (default port `27886`) |
| `--stream-join <host[:port]>` | Join a LAN streaming session as Player 2 |

### How do I report a problem?

[Open an issue](https://github.com/OAleex/OpenShao/issues) with the steps that cause it and the latest log from **Files > Logs > Open**.
Include a screenshot for graphical problems. Do not attach game files.

## Building

You need the original `default.xbe`, Python 3.11, Git and a C++ toolchain: Visual Studio 2022 with LLVM 21 on Windows, or Clang 18 on Linux.

```sh
git clone https://github.com/marcoorquera/OpenShao.git
cd OpenShao
git clone https://github.com/TGP482/XboxXBERecomp.git third_party/XboxXBERecomp
git -C third_party/XboxXBERecomp checkout 40e9c35451305f09ca9d86f677e4b9c2c37ff26b
python tools/build_recomp.py --xbe path/to/default.xbe --sdk third_party/XboxXBERecomp --test
```

On Windows, add `--llvm "C:/Program Files/LLVM"`. See [Building](docs/BUILDING.md) for packages, options and the Linux setup.

## AI usage

Development uses AI assistance for executable analysis, code, debugging, tests, build scripts and documentation.
The maintainer reviews every change. Validation includes automated tests and gameplay.

## Credits

Forked from and based on [OpenShao](https://github.com/OAleex/OpenShao) by [OAleex](https://github.com/OAleex).
Built with [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp). See [tools and dependencies](UPSTREAM.md), [third-party notices](licenses/THIRD_PARTY.txt) and the [Brazilian Portuguese credits](docs/TRANSLATION_CREDITS.md).

The code is licensed under [GPL-2.0-or-later](LICENSE). The license does not cover the original game or its assets.
OpenShao is not affiliated with or endorsed by the owners of Mortal Kombat or Xbox.
