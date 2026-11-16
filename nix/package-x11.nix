{ lib
, stdenv
, cmake
, extra-cmake-modules
, kwin-x11
, wrapQtAppsHook
, qttools
}:

stdenv.mkDerivation rec {
  pname = "kwin-better-blur-rx";
  version = "2.5.1";

  src = ./..;

  nativeBuildInputs = [
    cmake
    extra-cmake-modules
    wrapQtAppsHook
  ];

  buildInputs = [
    kwin-x11
    qttools
  ];

  meta = with lib; {
    description = "Better Blur DX fork for KDE Plasma 6 on Wayland: blur shaped by window alpha and by shapes clients publish over D-Bus";
    license = licenses.gpl3;
    homepage = "https://github.com/Rudraksh88/better-blur-rx";
  };
}
