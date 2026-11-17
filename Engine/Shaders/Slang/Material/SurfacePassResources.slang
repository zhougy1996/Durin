// Surface pass resources are owned by the pass adapter.
import Material.SpecularAntialiasing;
import Lighting.DirectionalShadow;
import Lighting.ViewLightingUniform;
import Lighting.SurfaceLighting;
#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage) Type Name;
#include "SurfaceParameters.inl"
#undef DURIN_MATERIAL_PARAMETER
