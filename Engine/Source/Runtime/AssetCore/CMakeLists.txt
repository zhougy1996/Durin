add_durin_module(AssetCore)

# AssetCore translation units intentionally reuse compact anonymous-namespace
# helper names and friend seams. Keep the module source-isolated until those
# internal identities are made unity-safe.
set_target_properties(AssetCore PROPERTIES UNITY_BUILD OFF)
