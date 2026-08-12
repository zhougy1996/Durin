add_durin_module(Engine)

if(DURIN_WITH_EDITOR)
	# Transitional TextureCube self-build only; Texture2D build code lives in EngineAssetBuild.
	target_link_libraries(Engine PRIVATE bc7enc_rdo::bc7enc_rdo)
endif()
