# Framebuffer Browser

QtWebEngine-based web browser for Linux framebuffer. Forked and adapted from https://github.com/femelo/Framebuffer-browser.

## Features

1. A simple web browser that draws on framebuffer directly, does not require X server.
2. Low memory footprint, works well on devices with >= 128MB RAM.
3. Webkit-based, supports modern HTML5 features, 100/100 on ACID3 test.
4. No OpenGL implementation required, works on embedded processors that have no GPU or GPU driver is not available. OpenGL support is also available.
5. Packaged as a rootfs including all dependencies, could be used as a standalone OS or inside a chroot container on existing Linux distributions.
6. Have basic hotkeys for navigation, mouse and keyboard support as well as custom devices which where supported by evdev and QT (touchscreens,touchpads,joysticks etc...)
7. Full screen video player support (youtube etc...)

## Install

```shell
git clone https://github.com/femelo/framebuffer-browser.git
cd framebuffer-browser
mkdir build && cd build && cmake .. && make && cd ..
./start-fbbrowser.sh
```

## Configure

All device input and other settings are described in the script **start-fbbrowser.sh**, also note that your local account must be in the following groups:

```shell
tty, audio, video, usb, input
```

- `linuxfb` - uses linux framebuffer device (unaccelerated vesa mode)
- `eglfs` - uses linux kernel mode setting drm drivers (accelerated, your device must be supported by the kernel)

## NixOS Installation & Flakes

This repository includes a Nix Flake providing a standalone package, a development shell, and a complete NixOS module.

### Using as a NixOS Module

1. Add `framebuffer-browser` as an input in your system's `flake.nix`:

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
2. Enable and configure the module in your `configuration.nix`:

```nix
programs.framebuffer-browser = {
  enable = true;
  users = [ "yourusername" ];     # Automatically grants required input, video, and tty permissions
  backend = "auto";               # Choose between "auto", "eglfs", "fb", or "wayland"
  hardwareCursor = false;         # Set to true if your KMS driver supports hardware cursors
  pointerSpeed = 0.0;             # Optional libinput pointer speed adjustment (-1.0 to 1.0, though note this does not actually use libinput)
};
```

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
