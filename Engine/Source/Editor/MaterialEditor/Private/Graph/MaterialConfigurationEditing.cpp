#include "MaterialEditingOperations.h"
#include "MaterialGraphCommands.h"

namespace Durin::MaterialEditor::MaterialEditing
{
	using namespace GraphEditInternals;

	auto SetConfiguration(DMaterial& Material, const FMaterialConfiguration& Configuration, bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(bShouldTransact);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (Transactions && !Transactions->CanTransact()) return RejectCommand("The editor transactor is busy.");
		if (const auto Valid = ValidateMaterialConfiguration(Configuration.Domain, Configuration.Properties); !Valid)
			return RejectCommand(Valid.error());
		const FMaterialConfiguration Before{Material.GetDomain(), Material.GetStaticProperties()};
		if (Before.Domain == Configuration.Domain && Before.Properties == Configuration.Properties)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		const bool bNested = Transactions && Transactions->IsActive();
		FScopedTransaction Recording("MaterialGraph", "Change Material Configuration", &Material, Transactions && !bNested);
		if (Transactions)
		{
			if (!bNested && !Recording.IsOutstanding()) return RejectCommand("Unable to start the configuration transaction.");
			Material.SetFlags(EObjectFlags::Transactional);
			if (!Material.Modify(false))
			{
				if (!bNested) Recording.Cancel();
				return RejectCommand("Unable to record the material configuration.");
			}
		}
		const auto Result = Material.SetMaterialConfiguration(Configuration);
		if (!Result)
		{
			if (!bNested) Recording.Cancel();
			return RejectCommand("The material configuration was rejected.", Result.Diagnostics);
		}
		return {.Status = Before.Domain == Material.GetDomain() && Before.Properties == Material.GetStaticProperties()
			? EMaterialGraphCommandStatus::NoChange : EMaterialGraphCommandStatus::Succeeded};
	}

	auto SetUseMaterialAttributes(DMaterial& Material, bool bEnabled, bool bShouldTransact) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(bShouldTransact);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
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
	}

}
