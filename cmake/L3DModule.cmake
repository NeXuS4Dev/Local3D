# l3d_add_module(NAME <name> ...)
#
# Creates a static library named Local3D_<Name> with the alias Local3D::<Name>,
# installs the standard include layout (include/local3d/<name>/...) and applies
# the shared compiler policy.  Every engine subsystem is declared through this
# function so adding a module is a three line CMakeLists.txt.
include_guard(GLOBAL)

function(l3d_add_module)
  set(options C_MODULE)
  set(oneValueArgs NAME NAMESPACE)
  set(multiValueArgs SOURCES HEADERS DEPS PRIVATE_DEPS DEFINES)
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_NAME)
    message(FATAL_ERROR "l3d_add_module: NAME is required")
  endif()

  string(TOLOWER "${ARG_NAME}" l3d_name_lower)
  set(target "Local3D_${ARG_NAME}")
  set(namespace "Local3D::${ARG_NAMESPACE}")
  if(NOT ARG_NAMESPACE)
    set(namespace "Local3D::${ARG_NAME}")
  endif()

  if(ARG_C_MODULE)
    add_library(${target} STATIC ${ARG_SOURCES} ${ARG_HEADERS})
    set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)
  else()
    add_library(${target} STATIC ${ARG_SOURCES} ${ARG_HEADERS})
  endif()

  add_library(${namespace} ALIAS ${target})

  target_include_directories(
    ${target}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

  target_link_libraries(${target} PUBLIC L3D::CompilerWarnings)

  if(ARG_DEPS)
    target_link_libraries(${target} PUBLIC ${ARG_DEPS})
  endif()
  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()
  if(ARG_DEFINES)
    target_compile_definitions(${target} PUBLIC ${ARG_DEFINES})
  endif()

  if(L3D_WARNINGS_AS_ERRORS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
      target_compile_options(${target} PRIVATE /WX)
    else()
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  set_target_properties(
    ${target}
    PROPERTIES FOLDER "Local3D/engine"
               CXX_STANDARD 20
               CXX_STANDARD_REQUIRED ON
               CXX_EXTENSIONS OFF
               EXPORT_NAME "${ARG_NAME}")
endfunction()

# l3d_add_test(NAME <name> SOURCES ... DEPS ...)
function(l3d_add_test)
  set(oneValueArgs NAME)
  set(multiValueArgs SOURCES DEPS)
  cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(target "Local3D_Test_${ARG_NAME}")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE Local3D::doctest ${ARG_DEPS})
  set_target_properties(${target} PROPERTIES FOLDER "Local3D/tests" CXX_STANDARD 20)
  add_test(NAME "${ARG_NAME}" COMMAND ${target} --no-colors=true --force-colors=false)
  set_tests_properties("${ARG_NAME}" PROPERTIES TIMEOUT 300)
endfunction()
