# PCSX2's file format, so this target is under PCSX2's licence and links nothing
# from this project. LICENSE beside it says so.
add_executable(gscapture_to_gsdump convert.cpp)
set_target_properties(gscapture_to_gsdump PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
