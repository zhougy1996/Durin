#include "MaterialEditingOperations.h"
#include "MaterialGraphCommands.h"

namespace Durin::MaterialEditor::MaterialEditing
{
	using namespace GraphEditInternals;

	auto SetConfiguration(DMaterial& Material, const FMaterialConfiguration& Configuration, bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		return WithGraphRecording(bShouldTransact, [&](DTransactor* Transactions) -> FMaterialGraphCommandResult {
			if (const auto Valid = ValidateMaterialConfiguration(Configuration.Domain, Configuration.Properties); !Valid)
				return RejectCommand(Valid.error());
			const FMaterialConfiguration Before{Material.GetDomain(), Material.GetStaticProperties()};
			if (Before.Domain == Configuration.Domain && Before.Properties == Configuration.Properties)
				return {.Status = EMaterialGraphCommandStatus::NoChange};
			const std::array<DObject*, 1> Objects{&Material};
			return RecordGraphObjects(Material, Transactions, "Change Material Configuration", Objects, /*bBatchGraphChanges=*/false, [&]() -> FMaterialGraphCommandResult {
				const auto Result = Material.SetMaterialConfiguration(Configuration);
				if (!Result) return RejectCommand("The material configuration was rejected.", Result.Diagnostics);
				return {.Status = Before.Domain == Material.GetDomain() && Before.Properties == Material.GetStaticProperties()
					? EMaterialGraphCommandStatus::NoChange : EMaterialGraphCommandStatus::Succeeded};
			});
		});
	}

	auto SetUseMaterialAttributes(DMaterial& Material, bool bEnabled, bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		return WithGraphRecording(bShouldTransact, [&](DTransactor* Transactions) -> FMaterialGraphCommandResult {
			if (Material.UsesMaterialAttributes() == bEnabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
			if (!Material.CanEditMaterialAttributesMode()) return RejectCommand("Change the material attributes mode in the Surface domain.");
			auto Root = Material.CaptureRootInputs();
			Root.bUseMaterialAttributes = bEnabled;
			if (auto Valid = ValidateProposedGraph(Material, Material.GetExpressionCollection(), {}, &Root, nullptr); !Valid)
				return Valid;
			const std::array<DObject*, 1> Objects{&Material};
			return RecordGraphEdit(Material, Transactions, "Change Material Output Mode", Objects, [&] {
				require(FMaterialExpressionEditing::SetRootInputs(Material, std::move(Root)));
			});
		});
	}

}
