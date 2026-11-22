#include "MaterialEditingOperations.h"
#include "MaterialGraphEditSession.h"
#include "DObject/Package.h"

namespace Durin::MaterialEditor::MaterialEditing
{
	using namespace GraphEditInternals;

	namespace
	{

		class FMaterialConfigurationChange final : public FMaterialCommandChange
		{
		public:
			FMaterialConfigurationChange(DMaterial& Target, FMaterialConfiguration InBefore, FMaterialConfiguration InAfter)
				: Material(&Target), Before(InBefore), After(InAfter), Packages{Target.GetPackage()} {}
			auto GetDescription() const -> std::string_view override { return "Change Material Configuration"; }
			auto GetOwningModule() const -> std::string_view override { return "MaterialEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto GetModifiedObjects() const -> std::vector<DObject*> override { return {Material.Get()}; }
			auto Replay(ETransactionOperation Operation) -> FMaterialChangeResult override
			{
				if (!Material.IsValid()) return {{.Code = EMaterialChangeError::TargetUnavailable}};
				const auto Result = Material.Get()->SetMaterialConfiguration(Operation == ETransactionOperation::Undo ? Before : After);
				if (!Result) return {{.Code = EMaterialChangeError::MaterialWrite, .Message = FormatMaterialProgramDiagnostics(Result.Diagnostics)}};
				return {};
			}
		private:
			TWeakObjectPtr<DMaterial> Material;
			FMaterialConfiguration Before, After;
			std::array<DPackage*, 1> Packages;
		};

	}

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
		FScopedTransaction Recording("MaterialGraph", "Change Material Configuration", nullptr, Transactions != nullptr);
		if (Transactions)
		{
			std::string Error;
			const auto Recorded = Transactions->StoreUndo(std::make_unique<FMaterialConfigurationChange>(Material, Before, Configuration), &Error);
			if (!Recorded) return RejectCommand("Unable to record the configuration edit. " + Error);
		}
		const auto Result = Material.SetMaterialConfiguration(Configuration);
		if (!Result) { Recording.Cancel(); return RejectCommand("The material configuration was rejected.", Result.Diagnostics); }
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto SetUseMaterialAttributes(DMaterial& Material, bool bEnabled, ETransactionMode Mode) -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (Material.GetExpressionOutputs().bUseMaterialAttributes == bEnabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		FGraphEditSession State(Material);
		State.GetOutputs().bUseMaterialAttributes = bEnabled;
		return State.Commit("Change Material Output Mode", Transactions);
	}

}
