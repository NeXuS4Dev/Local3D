# Third party dependency policy.
#
# Local3D vendors a very small, deliberately chosen set of dependencies.  Each
# one has a documented justification in docs/architecture/dependencies.md:
#
#   doctest        - header only unit test framework (tests only).
#   stb_image      - single file image decoding for texture import.
#   Vulkan headers - Khronos C headers so the Vulkan backend type checks even
#                    on machines without the Vulkan SDK installed.
#   Dear ImGui     - editor UI.  Backends (SDL3/Vulkan) are only compiled when
#                    those systems are available.
#
# Everything else (math, ECS, containers, serialization, render graph, physics
# reference backend, audio mixer) is implemented in-tree so the engine keeps a
# small, auditable dependency surface.
include_guard(GLOBAL)

set(L3D_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/../third_party")

# --- doctest ---------------------------------------------------------------
if(L3D_BUILD_TESTS AND NOT TARGET Local3D::doctest)
  add_library(Local3D_doctest INTERFACE)
  add_library(Local3D::doctest ALIAS Local3D_doctest)
  target_include_directories(Local3D_doctest INTERFACE "${L3D_THIRD_PARTY_DIR}/doctest")
endif()

# --- stb -------------------------------------------------------------------
if(NOT TARGET Local3D::stb)
  add_library(Local3D_stb INTERFACE)
  add_library(Local3D::stb ALIAS Local3D_stb)
  target_include_directories(Local3D_stb INTERFACE "${L3D_THIRD_PARTY_DIR}/stb")
endif()

# --- Vulkan headers --------------------------------------------------------
set(L3D_VULKAN_HEADERS_DIR "${L3D_THIRD_PARTY_DIR}/vulkan/include")
if(NOT TARGET Local3D::VulkanHeaders)
  add_library(Local3D_VulkanHeaders INTERFACE)
  add_library(Local3D::VulkanHeaders ALIAS Local3D_VulkanHeaders)
  target_include_directories(Local3D_VulkanHeaders INTERFACE "${L3D_VULKAN_HEADERS_DIR}")
endif()

# Prefer a real loader if the machine has one (Windows SDK / distro package);
# fall back to fetching the entry points ourselves at runtime.
find_library(L3D_VULKAN_LOADER NAMES vulkan vulkan-1)
if(L3D_VULKAN_LOADER)
  message(STATUS "Local3D: found Vulkan loader ${L3D_VULKAN_LOADER}")
endif()

if(L3D_RHI_VULKAN AND NOT EXISTS "${L3D_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h")
  message(WARNING "L3D_RHI_VULKAN=ON but Vulkan headers are missing; disabling Vulkan backend")
  set(L3D_RHI_VULKAN OFF)
endif()

# --- Dear ImGui ------------------------------------------------------------
if(L3D_BUILD_EDITOR AND NOT TARGET Local3D::imgui)
  set(L3D_IMGUI_DIR "${L3D_THIRD_PARTY_DIR}/imgui")
  add_library(Local3D_imgui STATIC ${L3D_IMGUI_DIR}/imgui.cpp ${L3D_IMGUI_DIR}/imgui_draw.cpp
                                   ${L3D_IMGUI_DIR}/imgui_tables.cpp
                                   ${L3D_IMGUI_DIR}/imgui_widgets.cpp)
  add_library(Local3D::imgui ALIAS Local3D_imgui)
  target_include_directories(Local3D_imgui PUBLIC "${L3D_IMGUI_DIR}")
  # Dear ImGui is vendored upstream code: build it without our warning policy.
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    target_compile_options(Local3D_imgui PRIVATE -w)
  endif()
  set_target_properties(Local3D_imgui PROPERTIES FOLDER "Local3D/third_party" POSITION_INDEPENDENT_CODE ON)
endif()

# --- SDL3 (optional window/input backend) ---------------------------------
set(L3D_HAVE_SDL3 OFF)
if(L3D_PLATFORM_BACKEND STREQUAL "Auto" OR L3D_PLATFORM_BACKEND STREQUAL "SDL3")
  find_package(SDL3 QUIET)
  if(SDL3_FOUND)
    set(L3D_HAVE_SDL3 ON)
  elseif(L3D_PLATFORM_BACKEND STREQUAL "SDL3")
    message(FATAL_ERROR "L3D_PLATFORM_BACKEND=SDL3 but SDL3 was not found. "
                        "Install SDL3 or set L3D_PLATFORM_BACKEND=Headless.")
  endif()
endif()

if(L3D_HAVE_SDL3)
  message(STATUS "Local3D: SDL3 window/input backend ENABLED")
  add_compile_definitions(L3D_PLATFORM_SDL3=1)
else()
  message(STATUS "Local3D: SDL3 not found - using the headless platform backend "
                 "(windowing/input still fully functional via injected events)")
endif()
