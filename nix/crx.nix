# Turn Chrome Web Store packages (.crx) into unpacked extension directories
# that fbbrowser can load (QtWebEngine only loads unpacked directories).
{
  lib,
  runCommand,
  fetchurl,
  unzip,
}:
rec {
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
  # Take the new hash from the error, or pin a copy you host yourself with unpackCrx.
  fetchCrx =
    {
      id,
      hash,
      name ? id,
      prodversion ? "140.0.0.0", # Chromium version to claim; the store refuses very old ones
    }:
    unpackCrx {
      inherit name;
      src = fetchurl {
        name = "${name}.crx";
        url =
          "https://clients2.google.com/service/update2/crx?response=redirect"
          + "&prodversion=${prodversion}&acceptformat=crx2,crx3"
          + "&x=id%3D${id}%26installsource%3Dondemand%26uc";
        inherit hash;
      };
    };
}
