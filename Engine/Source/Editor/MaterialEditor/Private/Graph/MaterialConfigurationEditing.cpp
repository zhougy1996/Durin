#include "MaterialEditingOperations.h"
#include "MaterialGraphEditSession.h"

namespace Durin::MaterialEditor::MaterialEditing
{
	using namespace GraphEditInternals;

	auto SetConfiguration(DMaterial& Material, const FMaterialConfiguration& Configuration, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (Transactions && !Transactions->CanTransact()) return RejectCommand("The editor transactor is busy.");
		if (const auto Valid = ValidateMaterialConfiguration(Configuration.Domain, Configuration.Properties); !Valid)
			return RejectCommand(Valid.error());
		const FMaterialConfiguration Before{Material.GetDomain(), Material.GetStaticProperties()};
		if (Before.Domain == Configuration.Domain && Before.Properties == Configuration.Properties)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		const bool bNested = Transactions && Transactions->IsActive();
		FScopedTransaction Recording("MaterialGraph", "Change Material Configuration", &Material, Transactions != nullptr);
		if (Transactions)
		{
			if (!Recording.IsOutstanding()) return RejectCommand("Unable to start the configuration transaction.");
			FScopedPackageDirtySuppression Suppress;
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

	auto SetUseMaterialAttributes(DMaterial& Material, bool bEnabled, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (Material.UsesMaterialAttributes() == bEnabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		if (!Material.CanEditMaterialAttributesMode()) return RejectCommand("Change the material attributes mode in the Surface domain.");
		FGraphEditSession State(Material);
		State.SetRootMode(bEnabled);
		return State.Commit("Change Material Output Mode", Transactions);
	}

}
