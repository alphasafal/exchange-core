# Small, single-purpose executables. Anything that needs to be reproducible by
# a reader gets an app here rather than living in a shell script.
add_executable(xc_info xc_info.cpp)
target_link_libraries(xc_info PRIVATE xc::core)

add_executable(replay replay.cpp)
target_link_libraries(replay PRIVATE xc::core)

add_executable(exchange_sim exchange_sim.cpp)
target_link_libraries(exchange_sim PRIVATE xc::core)

add_executable(xc_client xc_client.cpp)
target_link_libraries(xc_client PRIVATE xc::core)
