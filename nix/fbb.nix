{
  lib,
  stdenv,
  cmake,
  qt6,
  coreutils,
  util-linux,
  defaultBackend ? "auto",
  pointerSpeed ? null,
  fbbSrc ? null,
}:

assert lib.assertOneOf "defaultBackend" defaultBackend [
  "auto"
  "eglfs"
  "fb"
  "wayland"
];

stdenv.mkDerivation {
  pname = "framebuffer-browser";
  version = "0-unstable";

  src = if fbbSrc != null then fbbSrc else ../.;

  nativeBuildInputs = [
    cmake
    qt6.wrapQtAppsHook
  ];

  buildInputs = with qt6; [
    qtbase
    qtwebengine
    qtwayland
  ];

  postPatch = ''
    substituteInPlace CMakeLists.txt \
      --replace-fail 'set(CMAKE_PREFIX_PATH /opt/qt6.8/6.8.0/gcc_64)' ""
  '';

  postBuild = ''
    $CC -O3 -Wall -o fbbrowser-vt-reset ${./vtreset.c}
    $CC -O3 -Wall -shared -fPIC -o libfbbrowser-pointer.so ${./pointerspeed.c}
  '';

  installPhase = ''
    runHook preInstall

    install -Dm755 fbbrowser $out/libexec/fbbrowser/fbbrowser
    install -Dm755 fbbrowser-vt-reset $out/bin/fbbrowser-vt-reset
    install -Dm755 libfbbrowser-pointer.so $out/lib/fbbrowser/libfbbrowser-pointer.so

    install -Dm755 ${./launcher.sh} $out/bin/fbbrowser
    substituteInPlace $out/bin/fbbrowser \
      --subst-var-by path ${
        lib.makeBinPath [
          coreutils
          util-linux
        ]
      } \
      --subst-var-by unwrapped $out/libexec/fbbrowser/fbbrowser \
      --subst-var-by vtReset $out/bin/fbbrowser-vt-reset \
      --subst-var-by pointerShim $out/lib/fbbrowser/libfbbrowser-pointer.so \
      --subst-var-by defaultBackend ${defaultBackend} \
      --subst-var-by pointerSpeedDefault "${if pointerSpeed != null then toString pointerSpeed else ""}"

    runHook postInstall
  '';

  # The launcher is a shell script; only the real Qt binary gets wrapped.
  dontWrapQtApps = true;
  postFixup = ''
    wrapQtApp $out/libexec/fbbrowser/fbbrowser
  '';

  meta = {
    description = "Minimal QtWebEngine (Qt6) web browser for the Linux framebuffer / KMS, no X11 or Wayland needed";
    homepage = "https://github.com/femelo/framebuffer-browser";
    license = lib.licenses.lgpl3Only;
    mainProgram = "fbbrowser";
    platforms = lib.intersectLists lib.platforms.linux qt6.qtwebengine.meta.platforms;
  };
}
