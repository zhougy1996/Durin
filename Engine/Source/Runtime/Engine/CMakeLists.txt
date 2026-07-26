add_durin_module(Engine)

if(DURIN_WITH_EDITOR)
	target_link_libraries(Engine PRIVATE bc7enc_rdo::bc7enc_rdo)
endif()
