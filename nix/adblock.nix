{ rustPlatform }:
rustPlatform.buildRustPackage {
  pname = "fbb-adblock";
  version = "0.1.0";
  src = ../adblock;
  cargoLock.lockFile = ../adblock/Cargo.lock;
  doCheck = false;
  installPhase = ''
    runHook preInstall
    install -Dm644 "$(find target -name libfbb_adblock.a | head -n1)" $out/lib/libfbb_adblock.a
    install -Dm644 include/fbb_adblock.h $out/include/fbb_adblock.h
    runHook postInstall
  '';
}
