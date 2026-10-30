#include "Rendering/SceneInterface.h"

#include "DynamicRHI.h"
#include "RHICapabilities.h"

namespace Durin
{
	auto FSceneInterface::GetDefaultFeatureLevel() -> ERHIFeatureLevel
	{
		const auto* Caps = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		return Caps ? Caps->FeatureLevel : ERHIFeatureLevel::SM5;
	}
	FSceneInterface::FSceneInterface() : FSceneInterface(GetDefaultFeatureLevel()) {}
	FSceneInterface::FSceneInterface(ERHIFeatureLevel InFeatureLevel) : FeatureLevel(InFeatureLevel)
	{
		requiref(InFeatureLevel <= GetDefaultFeatureLevel(), "Scene feature level exceeds the active RHI capability.");
	}
}
