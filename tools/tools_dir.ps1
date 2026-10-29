# Where the external tools live (Ghidra, a JDK, Real-ESRGAN, stable-diffusion.cpp, llvm-mingw):
# the folder in the L2HD_TOOLS environment variable, or tools\external next to this script.
# The scripts look for them in these sub-folders:
#   ghidra_*\support\analyzeHeadless.bat, jdk*\ (for Ghidra)
#   realesrgan\realesrgan-ncnn-vulkan.exe
#   sdcpp\bin\sd-cli.exe, sdcpp\models\...
#   llvm-mingw-*\bin\i686-w64-mingw32-clang.exe (only if the compiler is not on PATH)
if ($env:L2HD_TOOLS) { $env:L2HD_TOOLS } else { Join-Path $PSScriptRoot "external" }
