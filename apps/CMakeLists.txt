# Small, single-purpose executables. Anything that needs to be reproducible by
# a reader gets an app here rather than living in a shell script.
add_executable(xc_info xc_info.cpp)
target_link_libraries(xc_info PRIVATE xc::core)
