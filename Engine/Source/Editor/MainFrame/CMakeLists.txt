add_durin_module(MainFrame)

target_include_directories(MainFrame PRIVATE
	${DURIN_PROJECT_SOURCE_DIR}/Editor/LevelEditor/Public
	${DURIN_PROJECT_SOURCE_DIR}/Editor/MaterialEditor/Public
	${DURIN_PROJECT_SOURCE_DIR}/Editor/TextureEditor/Public
)
