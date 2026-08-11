add_durin_module(Launch)

if(WIN32)
	target_link_libraries(Launch PRIVATE Dbghelp)
endif()
