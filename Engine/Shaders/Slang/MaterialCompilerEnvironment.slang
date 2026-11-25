module MaterialCompilerEnvironment;

// Dependency root for generated material programs. Keep these imports aligned
// with MaterialProgramGenerator.cpp so material identities track only the
// shared shader modules that generated vertex and fragment stages actually consume.
import Material.SurfaceMaterial;
import Material.MeshUniforms;
import Material.GBufferDecode;
import Material.SingleLayerWaterLighting;
import SceneData;
import ProceduralSky;
import Material.SpecularAntialiasing;
import Lighting.DirectionalShadow;
import Lighting.ViewLightingUniform;
import Lighting.PBRLighting;
import Lighting.SurfaceLighting;

import VertexFactory.LocalVertexFactory;
import VertexFactory.SplineMeshVertexFactory;

#include "VertexFactory/MaterialVertexEvaluation.slang"
#include "VertexFactory/LocalMaterialEntry.slang"
#include "VertexFactory/SplineMaterialEntry.slang"
#include "Material/SurfacePasses.slang"
#include "Material/SceneTextures.slang"
#include "Material/PostProcessPass.slang"

#include "Material/SurfacePassResources.slang"
