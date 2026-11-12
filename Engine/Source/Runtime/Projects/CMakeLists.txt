add_durin_module(Projects)
target_compile_definitions(Projects PRIVATE
 DURIN_PLUGIN_BUILD_ROOT="${CMAKE_BINARY_DIR}/PluginBuild"
 DURIN_PLUGIN_ARCHITECTURE="${CMAKE_SYSTEM_PROCESSOR}"
 DURIN_PLUGIN_TOOLCHAIN="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_SIZEOF_VOID_P}")
if(WIN32)
 target_link_libraries(Projects PRIVATE bcrypt)
endif()

# Preserve ordinary-module admission in source-independent, no-plugin Game layouts.
file(READ "${DURIN_WORKSPACE_DIR}/Durin.dworkspace" _workspace)
string(JSON _project_count LENGTH "${_workspace}" Projects)
set(_ordinary_names "")
math(EXPR _project_last "${_project_count}-1")
foreach(_index RANGE ${_project_last})
 string(JSON _path GET "${_workspace}" Projects ${_index})
 file(READ "${DURIN_WORKSPACE_DIR}/${_path}" _project)
 string(JSON _count ERROR_VARIABLE _error LENGTH "${_project}" ModuleDirs)
 if(NOT _error AND _count GREATER 0)
  math(EXPR _last "${_count}-1")
  foreach(_module_index RANGE ${_last})
   string(JSON _name MEMBER "${_project}" ModuleDirs ${_module_index})
   string(APPEND _ordinary_names "\"${_name}\",")
  endforeach()
 endif()
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/OrdinaryModules.gen.h"
 "static constexpr const char* GOrdinaryModuleNames[] = {${_ordinary_names}};\n")
target_include_directories(Projects PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
