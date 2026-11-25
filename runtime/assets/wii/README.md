# Wii first-run bootstrap payload

This directory contains the small `shared2/wc24` first-run tree for the Wii NAND
the port uses when none is imported. The build copies it next to the program as
`wii_bootstrap/` (`runtime/cmake/PublicProducts.cmake`), and the runtime
(`runtime/include/nand_path.h`) copies whichever of these files are missing into
the NAND it uses (its managed NAND in the user data folder, or a configured NAND
root). It never overwrites a file that already exists.

The layout and required files follow Dolphin's direct-disc Wii bootstrap:
`Source/Core/Core/WiiRoot.cpp` and `Data/Sys/Wii/shared2/wc24`.
