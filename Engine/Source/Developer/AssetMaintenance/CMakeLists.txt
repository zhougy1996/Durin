if(NOT DURIN_WITH_EDITOR)
	message(FATAL_ERROR
		"AssetMaintenance is authoring/tool infrastructure and must not enter a DurinGame composition.")
endif()

add_durin_module(AssetMaintenance)
