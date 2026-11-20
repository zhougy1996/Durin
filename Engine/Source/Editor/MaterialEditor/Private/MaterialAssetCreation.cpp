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
		FMaterialGraphPresentation Presentation;
		TStrongObjectPtr<MaterialEditor::DMaterialGraph> Graph(MaterialEditor::DMaterialGraph::Create(Material));
		if (!Graph->CalculateLayout({}, Presentation, &OutError)) return false;
		if (Material.SetMaterialGraphPresentation(std::move(Presentation)) == EMaterialGraphPresentationResult::Rejected)
		{
			OutError = "The new material graph presentation could not be initialized.";
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
				Diagnostics.front().Message);
		return false;
	}
}
