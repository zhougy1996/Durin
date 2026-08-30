#include "MaterialAssetCreation.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/Material.h"

namespace Durin
{
	auto PrepareNewMaterialForEditing(
		DMaterial& Material, std::string& OutError) -> bool
	{
		OutError.clear();
		if (RequestMaterialRecompile(Material))
		{
			(void)FAssetCompilingManager::Get().FinishCompilationForObject(Material);
			if (Material.GetAcceptedCompiledProgram()) return true;
		}

		const std::span Diagnostics = Material.GetMaterialCompileDiagnostics();
		OutError = Diagnostics.empty()
			? "The new material did not produce a renderable program."
			: std::format("The new material did not compile: {}",
				Diagnostics.front().Source.Message);
		return false;
	}
}
