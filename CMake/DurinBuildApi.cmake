# Shared Durin build API surface: project/module/PCH/output/third-party helpers.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/Project/ProjectOutputs.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Project/SharedPCH.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Project/ProjectTargets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Project/ProjectSetup.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/ThirdParty/ThirdPartyUtils.cmake")
