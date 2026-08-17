# KiChad Windows development triplet.
#
# Inherits the stock x64-windows triplet and drops the debug variant of every
# dependency.  The Windows development preset builds KiChad itself as
# RelWithDebInfo and never links the debug dependency set, so building it costs
# roughly half the dependency build time and half the installed size for no
# benefit.  Remove VCPKG_BUILD_TYPE here if you need to debug into a dependency.

include("${VCPKG_ROOT_DIR}/triplets/x64-windows.cmake")

set(VCPKG_BUILD_TYPE release)
