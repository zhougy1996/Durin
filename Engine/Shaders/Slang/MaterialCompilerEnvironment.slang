module MaterialCompilerEnvironment;

// Dependency root for generated material programs. Keep these imports aligned
// with MaterialProgramGenerator.cpp so material identities track only the
// shared shader modules that generated fragment stages actually consume.
import Material.SurfaceMaterial;
import Material.SpecularAntialiasing;
import Lighting.DirectionalShadow;
import Lighting.ForwardLightingUniform;
import Lighting.PBRLighting;
import Lighting.SurfaceLighting;
