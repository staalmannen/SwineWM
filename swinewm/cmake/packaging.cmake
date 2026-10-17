# Installation layout and the MSI installer (CPack + WiX Toolset).
#
# Everything goes into one directory (C:\Program Files\SwineWM by default):
#
#   swinewm.exe, swinemsg.exe, swinemenu.exe
#   config                    default sway config
#   waybar\config.jsonc       default bar config
#   waybar\style.css          default bar style
#   mako\config               default notification config
#   wallpapers\*.png
#   LICENSE.txt
#
# swinewm looks for its config files next to the executable after the
# user's own directories, so these act as the defaults. See doc/packaging.md.

install(TARGETS swinewm swinemsg swinemenu RUNTIME DESTINATION .)
install(FILES config.in DESTINATION . RENAME config)
install(DIRECTORY resources/waybar resources/mako DESTINATION .)
install(FILES resources/wallpapers/swinewm-default.png DESTINATION wallpapers)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE DESTINATION .
  RENAME LICENSE.txt)

set(CPACK_PACKAGE_NAME "SwineWM")
set(CPACK_PACKAGE_VENDOR "SwineWM")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Sway-like tiling window manager, bar and notifications for Windows")
# X.Y.Z.N: the MSI compares X.Y.Z, and a build with the same X.Y.Z
# replaces an installed one (CPack allows same-version upgrades).
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
# File names carry the readable version: SwineWM-0.1.0-win64.msi on a
# release, SwineWM-0.1.0-5-g1234abc-win64.msi in between.
set(CPACK_PACKAGE_FILE_NAME "SwineWM-${SWINEWM_VERSION}-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SwineWM")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/staalmannen/SwineWM")
# Start menu shortcut: SwineWM -> swinewm.exe
set(CPACK_PACKAGE_EXECUTABLES "swinewm" "SwineWM")
# The WiX generator wants the licence as .txt or .rtf.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE
  ${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt)

# The upgrade code identifies "SwineWM" across all versions, so that
# installing a newer MSI replaces the older one. Never change it.
set(CPACK_WIX_UPGRADE_GUID "F927A982-0D2D-4684-8A0D-5AB7C8D0E98D")
set(CPACK_WIX_PRODUCT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/swinewm.ico)
set(CPACK_WIX_INSTALL_SCOPE "perMachine")
# WiX v4 and later are installed as a .NET tool ("wix"); needs CMake 3.29.
# Pass -D CPACK_WIX_VERSION=3 to cpack to use the old WiX 3 instead.
if(NOT DEFINED CPACK_WIX_VERSION)
  set(CPACK_WIX_VERSION 4)
endif()
# Adds the install directory to PATH so swinemsg works in any terminal.
set(CPACK_WIX_PATCH_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix-patch.xml)
# Add/Remove Programs entries
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT ${CPACK_PACKAGE_HOMEPAGE_URL})
set(CPACK_WIX_PROPERTY_ARPHELPLINK "${CPACK_PACKAGE_HOMEPAGE_URL}/issues")

# "cpack" alone builds the MSI and a portable zip.
set(CPACK_GENERATOR "WIX;ZIP")
include(CPack)
