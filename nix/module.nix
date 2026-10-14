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
      };

      # eglfs needs EGL/GBM and the GPU driver from /run/opengl-driver.
      hardware.graphics.enable = mkDefault true;

      users.users = genAttrs cfg.users (_: {
        extraGroups = cfg.groups;
      });
    }

    (mkIf cfg.keepBluetoothAudio {
      services.pipewire.wireplumber.extraConfig."51-fbbrowser-bluez-seat" = {
        "wireplumber.profiles".main."monitor.bluez.seat-monitoring" = "disabled";
      };
    })
  ]);
}
