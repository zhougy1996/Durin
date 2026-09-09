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
		const auto Reset = Material.SetMaterialDefinitionsAndProgram(
			{}, MakeDefaultMaterialProgram());
		if (!Reset)
		{
			OutError = Reset.Diagnostics.empty()
				? std::string(GetMaterialParameterErrorText(Reset.Error))
				: Reset.Diagnostics.front().Message;
			return false;
		}
		FMaterialGraphPresentation Presentation;
		const auto Layout =
			Editor::Material::FMaterialGraphOperations::CalculateLayout(
				Material, {}, Presentation);
		if (!Layout || !Material.SetMaterialGraphPresentation(
				std::move(Presentation)))
		{
			OutError = Layout.Message.empty()
				? "The new material graph presentation could not be initialized."
				: Layout.Message;
			return false;
		}
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
