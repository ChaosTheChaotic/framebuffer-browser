# Turn Chrome Web Store packages (.crx) into unpacked extension directories
# that fbbrowser can load (QtWebEngine only loads unpacked directories).
{
  lib,
  runCommand,
  fetchurl,
  unzip,
  qt6,
}:
rec {
  # https://wiki.qt.io/QtWebEngine/ChromiumVersions
  chromiumBaseFor =
    qtWebEngineVersion:
    if lib.versionAtLeast qtWebEngineVersion "6.11" then "140.0.0.0" else "134.0.0.0";

  unpackCrx =
    {
      src,
      name ? "extension",
    }:
    runCommand "fbb-ext-${name}" { nativeBuildInputs = [ unzip ]; } ''
      bash ${./unpack-crx.sh} ${src} $out
    '';

  # Downloads an extension from the Chrome Web Store by id (the 32 letter a-p
  # string at the end of its store URL)
  # The store always serves the newest version, so hash stops matching when the extension updates
  fetchCrx =
    {
      id,
      hash,
      name ? id,
      prodversion ? null, # derived from this systems Qt WebEngine
    }:
    let
      pv = if prodversion == null then chromiumBaseFor qt6.qtwebengine.version else prodversion;
    in
    unpackCrx {
      inherit name;
      src = fetchurl {
        name = "${name}.crx";
        url =
          "https://clients2.google.com/service/update2/crx?response=redirect"
          + "&prodversion=${pv}&acceptformat=crx2,crx3"
          + "&x=id%3D${id}%26installsource%3Dondemand%26uc";
        inherit hash;
      };
    };
}
