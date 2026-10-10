add_durin_module(TextureCompressor)

target_link_libraries(TextureCompressor PRIVATE bc7enc_rdo::bc7enc_rdo)

# Bind build identity to the managed encoder source revision, including rgbcx.
set(texture_encoder_manifest "${CMAKE_SOURCE_DIR}/Tools/DurinDevTool/durin_dev_tool/bootstrap/thirdparty/bc7enc_rdo.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${texture_encoder_manifest}")
file(READ "${texture_encoder_manifest}" texture_encoder_manifest_json)
string(JSON texture_encoder_revision GET "${texture_encoder_manifest_json}" source commit)
target_compile_definitions(TextureCompressor PRIVATE
	DURIN_TEXTURE_ENCODER_REVISION="${texture_encoder_revision}")
