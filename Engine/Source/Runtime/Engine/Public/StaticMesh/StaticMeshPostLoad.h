#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	class IStaticMeshPostLoadFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.StaticMeshPostLoad";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto PostLoadUncooked(
			DStaticMesh& Mesh,
			FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
			std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeStaticMeshPostLoadFeature(
		DStaticMesh& Mesh,
		FStaticMeshDerivedDataDiagnostic& OutDiagnostic,
		std::string& OutError) -> bool;
}
