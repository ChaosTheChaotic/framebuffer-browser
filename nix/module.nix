{
  config,
  lib,
  pkgs,
  ...
}:

let
  inherit (lib)
    genAttrs
    literalExpression
    mkDefault
    mkEnableOption
    mkIf
    mkMerge
    mkOption
    types
    ;

  cfg = config.programs.framebuffer-browser;
  json = pkgs.formats.json { };
  # The launcher reads config.json from FBBROWSER_CONFIG_DIR
  finalSettings =
    cfg.settings
    // lib.optionalAttrs (cfg.extensions != [ ]) {
      extensionDirs = (cfg.settings.extensionDirs or [ ]) ++ map (e: "${e}") cfg.extensions;
    };

  configDir = pkgs.linkFarm "fbbrowser-config" [
    {
      name = "config.json";
      path = json.generate "config.json" finalSettings;
    }
  ];

  # Booleans become 1/0, which is what the launcher and shims test for
  envValue =
    v:
    if v == true then
      "1"
    else if v == false then
      "0"
    else
      toString v;
in
{
  options.programs.framebuffer-browser = {
    enable = mkEnableOption "fbbrowser, a Qt6 WebEngine browser for the Linux console";

    source = mkOption {
      type = types.nullOr types.path;
      default = null;
      description = "The source files to which framebuffer-browser should be built from";
    };

    package = mkOption {
      type = types.package;
      default = pkgs.callPackage ./fbb.nix {
        pointerSpeed = cfg.pointerSpeed;
        fbbSrc = cfg.source;
        fbbAdblock = pkgs.callPackage ./adblock.nix { };
      };
      defaultText = literalExpression "pkgs.callPackage ./fbb.nix { }";
      description = "The framebuffer-browser package to install.";
    };

    backend = mkOption {
      type = types.enum [
        "auto"
        "eglfs"
        "fb"
        "wayland"
      ];
      default = "auto";
      description = ''
        Default Qt backend, exported to login sessions as `FBBROWSER_BACKEND`.
        `--backend` on the command line still wins.
      '';
    };

    hardwareCursor = mkEnableOption ''
      the hardware cursor under eglfs. Off by default because some drivers
      (e.g. Apple Silicon) reject Qt's cursor ioctls and spam "Failed to move
      cursor on screen"; Qt then draws a software cursor instead'';

    pointerSpeed = mkOption {
      type = types.nullOr (types.numbers.between (-1) 1);
      default = null;
      example = -0.4;
      description = ''
        libinput pointer speed from -1.0 (slowest) to 1.0 (fastest); null
        leaves libinput's default. Qt has no setting for this, so a small
        LD_PRELOAD shim applies it to the browser process only. Applies to
        eglfs/linuxfb, not to `--backend wayland`.
      '';
    };

    settings = mkOption {
      type = json.type;
      default = { };
      example = literalExpression ''
        {
          url = "https://duckduckgo.com";
          searchUrl = "https://www.google.com/search?q=%s";
          width = 1920;
          height = 1080;
          adblockTtlHours = 48;
          adblockDirs = [ "~/adblock" ];
        }
      '';
      description = ''
        Contents of the browser's `config.json`, written as is, so any key
        read by `MainWindow::loadConfig` works without changes to this module.
        Unknown keys are ignored by the browser. When non-empty,
        `FBBROWSER_CONFIG_DIR` points at a read-only directory holding this
        file, so `~/.config/fbbrowser/config.json` is no longer used. When
        empty, the launcher manages that file itself as before.
      '';
    };

    extensions = mkOption {
      type = types.listOf (types.either types.package types.path);
      default = [ ];
      example = literalExpression ''
        [
          ./some-extension
          (pkgs.fetchzip {
            url = "https://example.com/somezip.zip";
            hash = "sha256-...";
            stripRoot = false;
          })
        ]
      '';
      description = ''
        Chrome manifest v3 extensions. Entries may have either a manifest.json or subdirectories with a manifest.json
        A derivation whose $out looks the same may also be used
        Extensions are loaded via loadExtension() from readonly store path
        Nothing is copied into profile
      '';
    };

    env = mkOption {
      type = types.attrsOf (
        types.nullOr (
          types.oneOf [
            types.str
            types.int
            types.float
            types.bool
            types.path
          ]
        )
      );
      default = { };
      example = literalExpression ''
        {
          FBBROWSER_SCROLL_MODE = "synth";
          FBBROWSER_SCROLL_NATURAL = false;
          FBBROWSER_DRM_DEVICE = "/dev/dri/card1";
          QTWEBENGINE_CHROMIUM_FLAGS = "--ignore-gpu-blocklist";
        }
      '';
      description = ''
        Extra session environment variables, for anything the launcher or the
        shims read (`FBBROWSER_*`, `QT_QPA_*`, `QTWEBENGINE_*`). Booleans
        become `1`/`0` and `null` drops the variable. These win over the
        dedicated options above, e.g. `FBBROWSER_BACKEND`.
      '';
    };

    users = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = [ "alice" ];
      description = ''
        Users to add to {option}`programs.framebuffer-browser.groups`.
        Deliberately opt-in: these groups grant raw access to input devices
        (`input` can read every keystroke on the machine), so enabling the
        program does not hand them out implicitly. The launcher checks real
        device access at start-up and says what is missing instead.
      '';
    };

    groups = mkOption {
      type = types.listOf types.str;
      default = [
        "tty"
        "video"
        "audio"
        "input"
        "render"
      ];
      description = ''
        Groups granted to {option}`programs.framebuffer-browser.users`.
        Upstream's README also lists `usb`; that group does not exist on
        NixOS and nothing here needs it.
      '';
    };

    keepBluetoothAudio = mkEnableOption ''
      disabling WirePlumber's Bluetooth seat monitoring. By default
      Bluetooth audio is only available while one of your sessions is the
      active logind session on the seat, so it can drop when you switch to a
      VT that is not one of your logind sessions. This keeps it connected
      regardless. Independent of programs.bcon'';
  };

  config = mkIf cfg.enable (mkMerge [
    {
      environment.systemPackages = [ cfg.package ];
      environment.sessionVariables = {
        FBBROWSER_BACKEND = cfg.backend;
      }
      // lib.optionalAttrs cfg.hardwareCursor { FBBROWSER_HWCURSOR = "1"; }
      // lib.optionalAttrs (cfg.pointerSpeed != null) {
        FBBROWSER_POINTER_SPEED = toString cfg.pointerSpeed;
      }
      // lib.optionalAttrs (finalSettings != { }) { FBBROWSER_CONFIG_DIR = "${configDir}"; }
      // lib.mapAttrs (_: envValue) (lib.filterAttrs (_: v: v != null) cfg.env);

      # eglfs needs EGL/GBM and the GPU driver from /run/opengl-driver.
      hardware.graphics.enable = mkDefault true;

      users.users = genAttrs cfg.users (_: {
        extraGroups = cfg.groups;
      });
    }

    {
      assertions = [
        {
          assertion = cfg.extensions == [ ] || lib.versionAtLeast pkgs.qt6.qtwebengine.version "6.10";
          message = "programs.framebuffer-browser.extensions requires Qt WebEngine >= 6.10.";
        }
      ];
    }

    (mkIf cfg.keepBluetoothAudio {
      services.pipewire.wireplumber.extraConfig."51-fbbrowser-bluez-seat" = {
        "wireplumber.profiles".main."monitor.bluez.seat-monitoring" = "disabled";
      };
    })
  ]);
}
