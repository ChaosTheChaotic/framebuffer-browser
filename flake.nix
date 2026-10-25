{
  description = "Minimal QtWebEngine browser for the Linux framebuffer / KMS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs, ... }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          adblock = pkgs.callPackage ./nix/adblock.nix { };
        in
        {
          default = pkgs.callPackage ./nix/fbb.nix { fbbAdblock = adblock; };
        }
      );

      nixosModules.default = import ./nix/module.nix;

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [
              cmake
              pkg-config
              cargo
              rustc
              qt6.wrapQtAppsHook
              qt6.qtbase
              qt6.qtwebengine
              qt6.qtwayland
            ];

            QT_PLUGIN_PATH = "${pkgs.qt6.qtbase}/lib/qt-6/plugins";
            CPATH =
              with pkgs;
              lib.concatStringsSep ":" [
                "${qt6.qtbase}/include"
                "${qt6.qtbase}/include/QtWidgets"
                "${qt6.qtbase}/include/QtGui"
                "${qt6.qtbase}/include/QtCore"
                "${qt6.qtbase}/include/QtNetwork"
                "${qt6.qtwebengine}/include"
                "${qt6.qtwebengine}/include/QtWebEngineCore"
                "${qt6.qtwebengine}/include/QtWebEngineWidgets"
              ];
          };
        }
      );
    };
}
