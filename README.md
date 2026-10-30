# Framebuffer Browser

A small QtWebEngine (Chromium) web browser for the Linux console. It draws straight to the display through KMS/DRM (`eglfs`), so it needs no X server and no Wayland compositor. Forked and adapted from https://github.com/femelo/Framebuffer-browser.

## Features

- Runs on a bare virtual terminal (VT) using Qt's `eglfs` (KMS/DRM) backend. A `wayland` backend is available for testing inside a compositor.
- Tabs, an address bar that tells URLs from searches, and keyboard navigation.
- Built-in ad and tracker blocking (uBlock Origin / EasyList filter lists, handled by a Rust engine), including special handling for YouTube.
- Chrome extensions (Manifest V3), installable from the Chrome Web Store, from `.crx`/`.zip` files or from unpacked folders.
- Mouse, keyboard and touchpad input through libinput or plain evdev, with pointer-speed scaling and two-finger scrolling for touchpads.
- Full-screen video (YouTube etc.).
- A NixOS module and flake that handle permissions, configuration and extensions declaratively.

## Requirements

- A GPU/driver with KMS and EGL (needed for `eglfs`). Qt6 WebEngine needs OpenGL; the `linuxfb` backend has none, so web content will most likely **not** render with `--backend fb`. It exists as a last resort.
- Qt 6.10 or newer (Qt WebEngine). The extension manager API only exists from 6.10.
- Your user must be able to open the devices. On most distributions that means being in the `video` and `input` groups (the NixOS module can do this for you, see below). The launcher checks and tells you what is missing.
- Run it from a real text VT (Ctrl+Alt+F3 etc.), not from inside a terminal emulator, SSH session or graphical session. `eglfs` and `linuxfb` take over the display, which someone else already owns in those cases. The launcher warns about this.

## Install

### With Nix

The repository is a flake.

```shell
nix run github:ChaosTheChaotic/framebuffer-browser            # try it
nix run github:ChaosTheChaotic/framebuffer-browser -- --help
nix develop                                                   # development shell
```

`packages.default` builds the browser together with the launcher script, the input shims and `fbbrowser-vt-reset`. Supported systems are `x86_64-linux` and `aarch64-linux`. See [NixOS module](#nixos-module) for a system-wide setup.

### Without Nix

Build with CMake against Qt 6.10+ (Widgets, WebEngineWidgets). The ad blocker is a Rust static library in `adblock/` (`fbb_adblock.h` plus `libfbb_adblock.a`), so you also need a Rust toolchain; check `CMakeLists.txt` for how the library is located. Under Nix it is built separately and passed in.

```shell
git clone https://github.com/ChaosTheChaotic/framebuffer-browser.git
cd framebuffer-browser
mkdir build && cd build && cmake .. && make
```

The Nix package wraps the binary in `launcher.sh` (backend detection, permission checks, KMS configuration, input shims). Without Nix you have to set `QT_QPA_PLATFORM` and friends yourself, and the extension commands below are not available.

## Running

```shell
fbbrowser                      # opens the start-up URL from config.json
fbbrowser https://example.com  # URL on the command line wins over config.json
fbbrowser ./page.html          # relative files work too
fbbrowser --dry-run            # show what would be started, don't start it
```

Only one URL is accepted, and Chromium flags cannot be passed as arguments. Use `QTWEBENGINE_CHROMIUM_FLAGS` for those.

### Options

| Option | Meaning |
| --- | --- |
| `-b`, `--backend <auto\|eglfs\|fb\|wayland>` | Qt platform backend. Default `auto`, or `$FBBROWSER_BACKEND`. |
| `-d`, `--debug` | Qt platform logging (also enables the shims' debug output). |
| `-n`, `--dry-run` | Print the chosen backend, DRM node, config directory, extension directory, environment and command, then exit. |
| `-h`, `--help` | Full help. |
| `--install-extension <src>` | Install a Chrome extension. See [Extensions](#extensions). |
| `--list-extensions` | List installed extensions. |
| `--remove-extension <name>` | Remove an installed extension. |
| `--chromium-version` | Print the Chromium version of the Qt WebEngine this build runs on. |

### Backends

- `auto` picks `eglfs` if a KMS-capable DRM node you can access exists (preferring one with a connected display), otherwise falls back to `linuxfb`. If `QT_QPA_PLATFORM` is already set, it is used as-is.
- `eglfs` is the accelerated backend. The launcher writes a small Qt KMS JSON naming the DRM node (to `$XDG_RUNTIME_DIR`, or the config directory if that is unset). Provide your own with `QT_QPA_EGLFS_KMS_CONFIG` to stop that.
- `fb` (also accepted as `linuxfb`) uses `/dev/fbN`. Expect it **not** to render web content.
- `wayland` runs as an ordinary window, for testing inside a compositor.

### If a crash leaves the VT dead

A crash can leave a text VT blank with no keyboard. From another VT or over SSH:

```shell
sudo fbbrowser-vt-reset /dev/ttyN
```

The launcher also runs this automatically on exit when stdin is a terminal.

## Using the browser

### Keyboard

| Keys | Action |
| --- | --- |
| Ctrl+Q | Quit |
| Ctrl+T | New tab (opens the start-up URL) |
| Ctrl+W | Close tab (closing the last tab quits) |
| Ctrl+L | Focus and select the address bar |
| Ctrl+R | Reload |
| Ctrl+Left / Ctrl+Right | Back / forward |
| Esc | Leave full screen |

Tabs can also be switched and closed with the mouse. Tab titles are cut to 20 characters. Pages that request full screen (videos) hide the tab bar and address bar until they leave full screen.

### The address bar

What you type is classified like this:

1. A leading `?` always searches: `? rust lifetimes`.
2. `./file`, `../file`, `~/file` and existing absolute paths like `/etc/hostname` open as local files.
3. Input with a scheme is a URL if it has `//` (for example `https://…`, `file:///…`), or is one of `about:`, `data:`, `blob:`, `view-source:`. Plain `localhost:3000` is not mistaken for a scheme.
4. `host`, `host:port`, `host/path?query#fragment`, IPv4 and IPv6 addresses become URLs when they look like hosts:
   - names ending in a **real TLD** get `https://` (the IANA list, see below);
   - names with an explicit port, IP addresses, `localhost` and the private suffixes `.local`, `.lan`, `.internal`, `.test`, `.invalid`, `.example` get `http://`;
   - a bare word is a search unless it is `localhost` or has a port or a trailing `/`.
5. Anything else is a web search.

Searches use `searchUrl` (default DuckDuckGo). The list of valid TLDs is cached in `~/.cache/framebuffer-browser/tlds.txt` and refreshed in the background when older than 7 days (checked every 6 hours). Until a list has loaded, a short built-in list plus any two-letter name is accepted. Set `"tldUpdate": false` to never download it.

### Behaviour worth knowing

- JavaScript cannot open new windows: `window.open` and `target="_blank"` links do nothing.
- JavaScript may access the clipboard. Local files may read other local files and remote URLs.
- The browser identifies itself as desktop Chrome 140 on Linux x86_64 (also on aarch64). This string is hard-coded in `mainwindow.cc` and does not follow the engine version.
- Cookies, local storage and cache are persistent and live in Qt's default WebEngine profile location (`~/.local/share/framebuffer-browser` and `~/.cache/framebuffer-browser`).
- There is no history, bookmarks or download manager UI, and no toolbar buttons for extensions.

## Configuration

`config.json` is read from the directory in `$FBBROWSER_CONFIG_DIR`, default `${XDG_CONFIG_HOME:-~/.config}/fbbrowser`. The launcher creates it with defaults on first run if it does not exist. The browser is started with that directory as its working directory, so relative paths in the config are relative to it. Wrong types and bad values are ignored with a warning on stderr. Unknown keys are ignored.

```json
{
  "url": "https://duckduckgo.com",
  "searchUrl": "https://www.google.com/search?q=%s",
  "width": 0,
  "height": 0,
  "proxyHost": "",
  "proxyPort": 0,
  "adblock": true,
  "adblockDownload": true,
  "adblockDirs": [],
  "extensionDirs": []
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `url` | none | Start-up URL, also what a new tab opens. Goes through the address-bar rules, so `"example.com"` works. A URL given on the command line wins. |
| `searchUrl` | DuckDuckGo | Search template; `%s` is replaced by the percent-encoded query. Must be http(s) and contain `%s`. |
| `tldUpdate` | `true` | Refresh the TLD list in the background. |
| `width`, `height` | `0` | Window size in pixels. Set both. `0` means full screen. |
| `proxyHost`, `proxyPort` | empty, `0` | HTTP proxy for the whole application. Empty host means no proxy. |
| `extensionDirs` | `[]` | Extra folders of unpacked extensions. See [Extensions](#extensions). `~` is expanded. |
| `adblock*` | | See [Ad blocking](#ad-blocking). |

## Ad blocking

Network requests from every site are checked against the filter lists before they are made. This works on all sites.

Cosmetic filtering (hiding page elements) and scriptlets (`##+js` rules) are **only** applied on the hosts in `adblockScriptletHosts`, which by default are the YouTube hosts. On other sites you get network blocking only.

| Key | Default | Meaning |
| --- | --- | --- |
| `adblock` | `true` | Master switch. |
| `adblockDownload` | `true` | Download and refresh the lists. `false` uses only cached lists and `adblockDirs`. |
| `adblockLists` | built-in set | **Replaces** the built-in lists; array of http(s) URLs. |
| `adblockTtlHours` | `96` | Refresh period for lists that have no `! Expires:` header. Minimum 1. |
| `adblockDirs` | `[]` | Folders whose `*.txt` files (ABP/uBO syntax, not hosts files) are added to the rules. Never modified. Read at start-up and after every list refresh. |
| `adblockResources` | Brave's `resources.json` | Scriptlet bodies, downloaded and cached like a list. `""` disables scriptlets. |
| `adblockScriptletHosts` | `www.youtube.com`, `m.youtube.com`, `music.youtube.com`, `www.youtube-nocookie.com` | Hosts that get scriptlet and cosmetic injection. If a YouTube host is listed, the built-in YouTube handling is installed too. `[]` turns all of this off. |

The built-in lists are uBlock filters (ads, privacy, badware, quick-fixes, unbreak), EasyList and EasyPrivacy from uBlock Origin's asset CDN. Only Brave's scriptlet set is supported, so uBO-specific scriptlets in the lists are dropped. The YouTube handling does not depend on the resources file.

Lists are cached under `${XDG_CACHE_HOME:-~/.cache}/framebuffer-browser/adblock` (`lists/` and `meta.json`) and revalidated with ETag / Last-Modified. Failed downloads are retried later. Rebuilding the engine happens in the background and does not block browsing.

At start-up the first page load is held for up to half a second if its host gets scriptlets, so the scripts are in place before the page's own JavaScript. A page that loaded before they arrived is reloaded once, if that happens within the first 8 seconds.

## Extensions

Chrome extensions are supported through Qt WebEngine's extension manager.

- **Qt WebEngine 6.10 or newer** and **Manifest V3 only**. Manifest V2 extensions are refused.
- Qt implements only part of Chrome's extension APIs, and this browser has no toolbar buttons or popup UI. Extensions that work purely in the background or in content scripts (content blockers, dark-mode, userscript-style tools) are the realistic fit; anything that needs its popup will not be usable.
- Extensions are loaded unpacked at every start and enabled automatically. They are not copied into the browser profile.
- The start-up page waits up to 2 seconds for them. Tabs opened before late extensions arrive are reloaded once (only within the first 8 seconds).
- Extensions are not updated automatically. Install again to update.

### Installing from the Chrome Web Store, files and folders

```shell
# Chrome Web Store page URL
fbbrowser --install-extension 'https://chromewebstore.google.com/detail/some-extension/abcdefghijklmnopabcdefghijklmnop'

# or just the 32 letter extension id from that URL
fbbrowser --install-extension abcdefghijklmnopabcdefghijklmnop

# a .crx or .zip, local or by URL
fbbrowser --install-extension ~/Downloads/extension.crx
fbbrowser --install-extension https://example.com/extension.zip

# an unpacked extension directory (has manifest.json)
fbbrowser --install-extension ~/src/my-extension

fbbrowser --list-extensions
fbbrowser --remove-extension abcdefghijklmnopabcdefghijklmnop
```

Restart the browser afterwards. A Web Store id is downloaded from Google's update service as a `.crx`; the CRX header (version 2 or 3) is skipped and the zip inside is unpacked into the extension folder. Installing again replaces the old copy.

Installed extensions live in `${XDG_DATA_HOME:-~/.local/share}/framebuffer-browser/extensions`. Override the folder with `FBBROWSER_EXTENSIONS_DIR`. The download claims the Chromium version the browser really runs on (`fbbrowser --chromium-version`, e.g. `134.0.6998.208` for Qt 6.10 and `140.0.7339.264` for Qt 6.11). The Web Store uses it to hand out the newest release of the extension that supports that engine. Set `FBBROWSER_CHROME_VERSION` to claim something else.

One caveat: an unpacked extension gets an id derived from its folder, not its Web Store id, unless its manifest contains a `key`. Extensions that depend on a fixed id (some OAuth flows, `externally_connectable`) may therefore not work.

### Extra folders

`extensionDirs` in `config.json` (or `settings.extensionDirs` on NixOS) lists additional folders. Each entry is either an extension itself (it has `manifest.json`) or a folder with extension subfolders. Only one level is scanned.

### On NixOS

The extension folder above works there too. For a declarative setup use `programs.framebuffer-browser.extensions`, see below.

## Input devices

Qt uses libinput for automatic device discovery. To pin a device, set `FBBROWSER_KEYBOARD_DEV`, `FBBROWSER_MOUSE_DEV` or `FBBROWSER_TOUCHSCREEN_DEV` to an evdev node (`event3` or a full path). Setting any of them switches Qt to plain evdev for all input.

### Pointer speed

`FBBROWSER_POINTER_SPEED` ranges from `-1.0` to `1.0`; `0` leaves it unchanged. It is implemented by an `LD_PRELOAD` shim that scales relative mouse motion and absolute touchpad deltas, and not by libinput. Motion is multiplied by `1 + speed` (`-0.5` halves it, `1.0` doubles it). It applies to `eglfs` and `linuxfb` only, and only to the browser process.

### Touchpad scrolling

A second shim handles touchpad scrolling. `FBBROWSER_SCROLL_MODE`:

- `auto` (default): ignore one-finger scrolling, keep two-finger scroll events the driver provides, and synthesise scrolling from two-finger motion if the driver never sends any.
- `synth`: always synthesise from two-finger motion.
- `off`: do not touch scroll events.

`FBBROWSER_SCROLL_THRESHOLD` is the pixels of two-finger motion per wheel notch (default `20`). `FBBROWSER_SCROLL_NATURAL=0` switches from Mac-style to traditional wheel direction. Troubleshooting aids: `FBBROWSER_SCROLL_DEBUG` (non-empty, verbose log), `FBBROWSER_SCROLL_STRIP=0`, `FBBROWSER_SCROLL_CLAIM=0`, `FBBROWSER_SCROLL_PRIMARY_FD=<fd>`.

Both shims turn themselves off inside Chromium's helper processes.

## Environment variables

| Variable | Meaning |
| --- | --- |
| `FBBROWSER_BACKEND` | Default for `--backend`. |
| `FBBROWSER_CONFIG_DIR` | Directory holding `config.json`. |
| `FBBROWSER_EXTENSIONS_DIR` | Folder for installed extensions. |
| `FBBROWSER_CHROME_VERSION` | Chrome version claimed for Web Store downloads. Default: the engine's own, see `--chromium-version`. |
| `FBBROWSER_DRM_DEVICE` | DRM node for `eglfs`, e.g. `/dev/dri/card2`. Default: first node with display outputs. |
| `FBBROWSER_FB_DEVICE` | Framebuffer node for `linuxfb`. |
| `FBBROWSER_KEYBOARD_DEV`, `FBBROWSER_MOUSE_DEV`, `FBBROWSER_TOUCHSCREEN_DEV` | Pin input devices (evdev). |
| `FBBROWSER_HWCURSOR` | `1` uses the hardware cursor under `eglfs`. Off by default because some drivers (e.g. Apple Silicon/Asahi) reject Qt's cursor calls and spam `Failed to move cursor on screen`; Qt then draws a software cursor. |
| `FBBROWSER_POINTER_SPEED` | See [Pointer speed](#pointer-speed). |
| `FBBROWSER_SCROLL_*` | See [Touchpad scrolling](#touchpad-scrolling). |
| `QT_QPA_EGLFS_KMS_CONFIG` | Your own Qt KMS JSON. The launcher then does not write one. |
| `QT_QPA_PLATFORM` | With backend `auto`, used as-is and detection is skipped. |
| `QTWEBENGINE_CHROMIUM_FLAGS` | Chromium command-line flags. |

Audio (PipeWire/Bluetooth) needs `XDG_RUNTIME_DIR`, so log in through a proper session; the launcher warns if it is unset.

## NixOS module

Add the flake as an input and import the module:

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    framebuffer-browser.url = "github:ChaosTheChaotic/framebuffer-browser";
  };

  outputs = { self, nixpkgs, framebuffer-browser, ... }@inputs: {
    nixosConfigurations.your-hostname = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        framebuffer-browser.nixosModules.default
      ];
    };
  };
}
```

Then in `configuration.nix`:

```nix
programs.framebuffer-browser = {
  enable = true;
  users = [ "yourusername" ];   # added to the groups below
  backend = "auto";             # "auto", "eglfs", "fb" or "wayland"
  hardwareCursor = false;
  pointerSpeed = 0.0;           # -1.0 .. 1.0, see "Pointer speed"

  settings = {                  # becomes config.json
    url = "https://duckduckgo.com";
    searchUrl = "https://www.google.com/search?q=%s";
    adblockTtlHours = 48;
  };

  extensions = [
    ./some-extension                      # unpacked directory
    ./other-extension.crx                 # packed file, unpacked at build time
    {                                     # Chrome Web Store
      id = "abcdefghijklmnopabcdefghijklmnop";
      hash = "sha256-...";
    }
  ];

  env = {                       # extra session environment variables
    FBBROWSER_SCROLL_MODE = "synth";
    FBBROWSER_SCROLL_NATURAL = false;
  };
};
```

Session variables only apply to new logins, so log out and in again after changing them.

### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `enable` | `false` | Install the browser and set the session variables. Also turns on `hardware.graphics` (as a default). |
| `package` | built from this repo | The package to install. |
| `source` | `null` | Build from a different source tree. |
| `backend` | `"auto"` | Exported as `FBBROWSER_BACKEND`. `--backend` still wins. |
| `hardwareCursor` | `false` | Sets `FBBROWSER_HWCURSOR=1`. |
| `pointerSpeed` | `null` | Sets `FBBROWSER_POINTER_SPEED`. `null` leaves the default. |
| `settings` | `{ }` | Contents of `config.json`, written as is. Any key from [Configuration](#configuration) works. |
| `extensions` | `[ ]` | Extensions to load. Each entry may be an unpacked directory, a `.crx`/`.zip` file, a derivation or flake input that is any of those, or `{ id; hash; name?; prodversion?; }` for the Chrome Web Store. Requires Qt WebEngine 6.10+. |
| `env` | `{ }` | Extra session variables. Booleans become `1`/`0`, `null` removes a variable. These win over the dedicated options. |
| `users` | `[ ]` | Users added to `groups`. Opt-in on purpose: `input` can read every keystroke on the machine. |
| `groups` | `tty video audio input render` | Groups granted to `users`. |
| `keepBluetoothAudio` | `false` | Disables WirePlumber's Bluetooth seat monitoring so Bluetooth audio stays connected when you switch to a VT that is not one of your logind sessions. |

### Things to know about NixOS

- When `settings` or `extensions` is non-empty, `FBBROWSER_CONFIG_DIR` points at a **read-only** store directory, so `~/.config/fbbrowser/config.json` is no longer used. When both are empty, the launcher manages that file as usual.
- A `.crx`, `.zip` or directory is recognised while building, not by its file name, so anything can be passed in, including a flake input (see below). No hash is needed for those.
- Web Store entries (`{ id; hash; }`) fetch the *newest* version, so the hash stops matching when the extension updates and the build fails with the new hash. To get the first hash, set `hash = "";`, rebuild and copy it from the error. For a stable build, download the `.crx` once, keep it in your configuration repository and list the file instead.
- Web Store entries claim the Chromium base version of your Qt WebEngine: `134.0.0.0` for Qt 6.10 and `140.0.0.0` for 6.11 and later. This is derived from `pkgs.qt6.qtwebengine.version`, rounding *down*, which is always safe. Override it per entry with `prodversion`.
- Extensions added with `fbbrowser --install-extension` also work on NixOS; they go to your user's data directory, not the store.

### Extensions from flake inputs (no hash in your configuration)

A flake input is pinned by `flake.lock`, so the `.crx` needs no `hash` in your own configuration. Declare it with `flake = false`:

```nix
inputs.ublock-crx = {
  flake = false;
  type = "file";
  url = "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=134.0.0.0&acceptformat=crx2,crx3&x=id%3Dabcdefghijklmnopabcdefghijklmnop%26installsource%3Dondemand%26uc";
};
```

The URL in an input is fixed, so it cannot follow your Qt version; `134.0.0.0` is the floor that is safe on Qt 6.10 and newer. Then use it (pass `inputs` to your modules, e.g. with `specialArgs`):

```nix
programs.framebuffer-browser.extensions = [ inputs.ublock-crx ];
```

`nix flake update ublock-crx` moves it to the newest release. A `.crx` hosted in a git repository or tarball (`github:you/extensions` with `flake = false`) or simply committed next to your configuration (`./foo.crx`) works the same way and never changes unless you change it.

## Files and directories

| Path | Contents |
| --- | --- |
| `${XDG_CONFIG_HOME:-~/.config}/fbbrowser/config.json` | Configuration (or `$FBBROWSER_CONFIG_DIR`). |
| `~/.cache/framebuffer-browser/adblock/` | Cached filter lists and `meta.json`. |
| `~/.cache/framebuffer-browser/tlds.txt` | Cached IANA TLD list. |
| `${XDG_DATA_HOME:-~/.local/share}/framebuffer-browser/extensions/` | Installed extensions. |
| `$XDG_RUNTIME_DIR/fbbrowser-kms.json` | KMS configuration generated by the launcher for `eglfs`. |

## Troubleshooting

- **Black screen or immediate exit under `eglfs`**: read the launcher's warnings. They say whether the DRM device is missing, not accessible (add yourself to `video`), or belongs to a running graphical session.
- **Keyboard or mouse do nothing**: you cannot read `/dev/input/event*`; add yourself to `input`. If you pinned devices, check the node.
- **VT stays dead after a crash**: `fbbrowser-vt-reset`, see above.
- **Pointer too fast or slow**: `FBBROWSER_POINTER_SPEED`.
- **Touchpad does not scroll**: try `FBBROWSER_SCROLL_MODE=synth` and `FBBROWSER_SCROLL_DEBUG=1`.
- **An extension does not load**: the browser prints `Failed to load extension <path>: <error>` on stderr. Check that it is Manifest V3 and that Qt WebEngine is 6.10+.
- **Pages load before the ad blocker is ready**: the first launch has no cached lists yet. Give it a moment and reload.

## TODO

- cmake option for statically linking
- easy cross platform build options
- autobuild and package for some popular distributions

## More information on Linux Framebuffer

- https://github.com/caramelli/higfxback/blob/master/Linux-Framebuffer.md
- https://github.com/caramelli/fbmark

## License

This project is licensed under the LGPLv3 and commercial license.

Commercial license can be purchased by individual users or organizations.

Please note that you don't need to buy commercial license if your use of this project is compitable with LGPLv3.
