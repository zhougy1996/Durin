#include "MaterialGraph.h"
#include "MaterialAssetCreation.h"

#include "Asset/AssetCompilingManager.h"
#include "MaterialGraphOperations.h"
#include "Materials/Material.h"

namespace Durin
{
	auto PrepareNewMaterialForEditing(
		DMaterial& Material, std::string& OutError) -> bool
	{
		OutError.clear();
		const auto Reset = Material.SetMaterialExpressions({}, {});
		if (!Reset)
		{
			OutError = Reset.Diagnostics.empty()
				? std::string("Invalid empty material graph.")
				: Reset.Diagnostics.front().Message;
			return false;
		}
		Material.SetRootPosition(1280, 0);
		Material.SetMaterialGraphPresentation({});
		if (RequestMaterialRecompile(Material))
		{
			(void)FAssetCompilingManager::Get().FinishCompilationForObject(Material);
			if (Material.GetAcceptedCompiledProgram()) return true;
		}

		const std::span Diagnostics = Material.GetMaterialCompileDiagnostics();
		OutError = Diagnostics.empty()
			? "The new material did not produce a renderable program."
			: std::format("The new material did not compile: {}",
				Diagnostics.front().Message);
		return false;
	}
}
