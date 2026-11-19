#include "MaterialGraphDocument.h"
#include "MaterialGraphEditSession.h"
#include "DObject/Package.h"

namespace Durin::MaterialEditor
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

	namespace GraphEditInternals
	{
		auto FExpressionEdit::Commit(DObject& Owner, DTransactor* Transactions, std::string Description) -> FMaterialGraphCommandResult
		{
			const bool Nested = Transactions && Transactions->IsActive();
			FScopedTransaction Recording(Transactions, {"MaterialGraph", std::move(Description), &Owner});
			if (Transactions)
			{
				if (!Recording.IsOutstanding()) return RejectCommand("Unable to start the graph transaction.");
				FScopedPackageDirtySuppression Suppress;
				for (auto* Object : Objects)
				{
					Object->SetFlags(EObjectFlags::Transactional);
					if (!Object->Modify(false))
					{
						if (Nested) (void)Recording.End();
						else Recording.Cancel();
						return RejectCommand("Unable to record the expression.");
					}
				}
			}
			for (auto& Write : Writes) Write();
			// The transactor advances the package checkpoint when it finalizes.
			(void)Recording.End();
			if (Transactions && !Transactions->GetLastResult().IsSuccess())
			{
				for (auto& Write : Writes | std::views::reverse) Write();
				return RejectCommand(FormatTransactorResult(Transactions->GetLastResult()));
			}
			if (!Transactions) Owner.MarkPackageDirty();
			Objects.front()->PostEditChange();
			return {};
		}

		auto FExpressionEdit::CommitRename(DObject& Owner, DMaterialExpression& Expression, std::string NewName,
			DTransactor* Transactions) -> FMaterialGraphCommandResult
		{
			auto SwapName = [&Expression, Next = std::move(NewName)]() mutable {
				auto Previous = Expression.GetEditableName();
				Expression.SetEditableName(std::move(Next));
				Next = std::move(Previous);
			};
			// Validate the candidate through the asset before Modify captures the original expression.
			SwapName();
			auto Valid = ValidateGraphEdit(Owner);
			SwapName();
			if (!Valid) return Valid;
			Objects.push_back(&Expression);
			Writes.emplace_back(std::move(SwapName));
			return Commit(Owner, Transactions, "Rename Material Expression");
		}


	}

	FMaterialGraphDocument::FMaterialGraphDocument(DObject& InOwner)
		: Owner(&InOwner)
	{
		check(Cast<DMaterial>(&InOwner) || Cast<DMaterialFunction>(&InOwner));
	}

	auto FMaterialGraphDocument::SetConfiguration(const FMaterialConfiguration& Configuration, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return RejectCommand("Only materials have a domain and shading configuration.");
		if (Transactions && !Transactions->CanTransact()) return RejectCommand("The editor transactor is busy.");
		if (const auto Valid = ValidateMaterialConfiguration(Configuration.Domain, Configuration.Properties); !Valid)
			return RejectCommand(Valid.error());
		const FMaterialConfiguration Before{Material->GetDomain(), Material->GetStaticProperties()};
		if (Before.Domain == Configuration.Domain && Before.Properties == Configuration.Properties)
			return {.Status = EMaterialGraphCommandStatus::NoChange};
		FScopedTransaction Recording(Transactions, {"MaterialGraph", "Change Material Configuration"});
		if (Transactions)
		{
			const auto Recorded = Transactions->StoreUndo(Material, std::make_unique<FMaterialConfigurationChange>(*Material, Before, Configuration));
			if (!Recorded) return RejectCommand("Unable to record the configuration edit. " + FormatTransactorResult(Recorded));
		}
		const auto Result = Material->SetMaterialConfiguration(Configuration);
		if (!Result) { Recording.Cancel(); return RejectCommand("The material configuration was rejected.", Result.Diagnostics); }
		(void)Recording.End();
		if (Transactions && Transactions->GetLastResult().Code == ETransactorResultCode::Failed)
			return RejectCommand(FormatTransactorResult(Transactions->GetLastResult()));
		return {.Status = EMaterialGraphCommandStatus::Succeeded};
	}

	auto FMaterialGraphDocument::SetUseMaterialAttributes(bool bEnabled, ETransactionMode Mode) const -> FMaterialGraphCommandResult
	{
		auto Admission = ResolveEditorTransactor(Mode);
		if (!Admission) return RejectCommand(Admission.error());
		auto* Transactions = *Admission;
		if (!Owner.IsValid()) return RejectCommand("The material graph owner is no longer available.", {}, EMaterialGraphCommandStatus::StaleOwner);
		auto* Material = Cast<DMaterial>(Owner.Get());
		if (!Material) return RejectCommand("Only materials have a material output mode.");
		if (Material->GetExpressionOutputs().bUseMaterialAttributes == bEnabled) return {.Status = EMaterialGraphCommandStatus::NoChange};
		FGraphEditSession State(*Material);
		State.GetOutputs().bUseMaterialAttributes = bEnabled;
		return State.Commit("Change Material Output Mode", Transactions);
	}

}
