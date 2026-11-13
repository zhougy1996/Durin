# Water resources

The regular grid is generated from bounded surface settings by the Water module.
UV0 contains undisplaced local XY coordinates. The default opaque Lit material
uses the existing material compiler and cooked-program codec. Editor compiles its
recipe through Engine; development Game reads `Compiled/Win64/Water.bin`.

Regenerate the compiled family with the isolated WaterSurfaceFeatureProbe
`resources` mode after changing the recipe or material/compiler contracts.
The generated family contains Low/High quality and ES3_1/SM5/SM6 configurations.
The first implementation is restricted to Win64 until another host's resource
family and runtime rendering are qualified. These resources do not qualify
staged or packaged deployment.
