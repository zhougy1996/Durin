// Surface pass resources are owned by the pass adapter.
import Material.SpecularAntialiasing;
import Lighting.DirectionalShadow;
import Lighting.ViewLightingUniform;
import Lighting.SurfaceLighting;
#define DURIN_DECLARE_BINDINGS_Surface 1
#define DURIN_MATERIAL_BINDING(Role, Name, Type, Reflected, Runtime, Set, Slot, Stride, Stages, Entries) [[vk::binding(Slot, Set)]] Type Name;
#include "MaterialBindingSchema.inl"
#undef DURIN_MATERIAL_BINDING
#undef DURIN_DECLARE_BINDINGS_Surface
