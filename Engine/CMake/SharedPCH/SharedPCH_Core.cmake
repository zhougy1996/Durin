include_guard(GLOBAL)

# Create a shared PCH target
set(DUMMY_SRC "${CMAKE_CURRENT_LIST_DIR}/DummyForPCH.cpp")
add_library(SharedPCH_Core STATIC ${DUMMY_SRC})
target_link_libraries(SharedPCH_Core PUBLIC
    glm::glm
)
set(CORE_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Source/Runtime/Core/Public")
target_compile_definitions(SharedPCH_Core PUBLIC
    $<$<CONFIG:Debug>:DURIN_BUILD_DEBUG=1>
    $<$<CONFIG:Release>:DURIN_BUILD_RELEASE=1>
)
set_property(TARGET SharedPCH_Core PROPERTY INCLUDE_DIRECTORIES "${CORE_INC_DIR}")
target_precompile_headers(SharedPCH_Core PUBLIC
    "$<$<COMPILE_LANGUAGE:CXX>:${CORE_INC_DIR}/CoreMinimal.h>"
)
