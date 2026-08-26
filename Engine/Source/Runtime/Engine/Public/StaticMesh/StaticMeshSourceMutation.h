#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DStaticMesh;

	class IStaticMeshSourceMutationFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.StaticMeshSourceMutation";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto ChangeSourceReference(
			DStaticMesh& Mesh,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeStaticMeshSourceChangeHandler(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
}
