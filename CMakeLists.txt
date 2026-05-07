cmake_minimum_required(VERSION 3.16)
project(rba_router VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Build options ────────────────────────────────────────────────────────────
option(RBA_OPENROAD_LINK "Link against OpenROAD library instead of subprocess" OFF)
option(RBA_ENABLE_TESTS  "Build unit tests" ON)
option(RBA_ENABLE_ASAN   "Enable AddressSanitizer" OFF)

# ─── Compiler flags ───────────────────────────────────────────────────────────
add_compile_options(-O3 -Wall -Wextra -Wpedantic)
if(RBA_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# ─── Dependencies ─────────────────────────────────────────────────────────────
# nlohmann/json (header-only)
find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

# OpenROAD library linkage (optional — enables direct API calls)
if(RBA_OPENROAD_LINK)
    find_package(OpenROAD REQUIRED)
    find_package(odb REQUIRED)
    set(OPENROAD_LIBS OpenROAD::openroad odb::odb)
    add_compile_definitions(RBA_OPENROAD_LINK)
endif()

# ─── Library target ───────────────────────────────────────────────────────────
add_library(rba_lib STATIC
    src/ga_net_ordering.cpp
    src/aco_path_search.cpp
    src/pso_cost_tuner.cpp
    src/abc_via_minimizer.cpp
    src/triton_bridge.cpp
    src/rba_orchestrator.cpp
)

target_include_directories(rba_lib PUBLIC include)
target_link_libraries(rba_lib
    PUBLIC nlohmann_json::nlohmann_json
    ${OPENROAD_LIBS}
)

# ─── Executable ───────────────────────────────────────────────────────────────
add_executable(rba_router src/main.cpp)
target_link_libraries(rba_router PRIVATE rba_lib)

# ─── Tests ────────────────────────────────────────────────────────────────────
if(RBA_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Install ─────────────────────────────────────────────────────────────────
install(TARGETS rba_router RUNTIME DESTINATION bin)
install(TARGETS rba_lib ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include/rba_router)
