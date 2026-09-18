#include "Widgets/MaterialEditingSession.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/Class.h"
#include "Editor/Transactor.h"
#include "Misc/MountPaths.h"

namespace Durin::Editor::Material
{
	auto FMaterialEditingSession::FAuthoredState::Matches(const DMaterial& Material) const -> bool
	{
		const auto& Current = Material.GetExpressionCollection().Expressions;
		if (Outputs != Material.GetExpressionOutputs() || Properties != Material.GetStaticProperties()
			|| Presentation != Material.GetMaterialGraphPresentation() || Expressions.size() != Current.size()) return false;
		for (size_t Index = 0; Index < Expressions.size(); ++Index)
		{
			const auto* Before = Expressions[Index].Get();
			const auto* After = Current[Index].Get();
			if (!Before || !After || Before->GetClass() != After->GetClass()) return false;
			bool bIdentical = true;
			Before->GetClass()->ForEachProperty([&](FProperty* Property) {
				for (uint32 Element = 0; bIdentical && Element < Property->GetArrayDim(); ++Element)
					bIdentical = ComparePropertyValues(Property, Before, Element, After, Element)
						== EPropertyIdentityResult::Identical;
			});
			if (!bIdentical) return false;
		}
		return true;
	}

	auto FMaterialEditingSession::Capture(const DMaterial& Material, FAuthoredState& OutState) -> FObjectGraphResult
	{
		FAuthoredState Candidate;
		Candidate.Outputs = Material.GetExpressionOutputs();
		Candidate.Properties = Material.GetStaticProperties();
		Candidate.Presentation = Material.GetMaterialGraphPresentation();
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
		{
			const auto Duplicated = DuplicateObject(Expression.Get(), nullptr, NAME_None);
			if (!Duplicated) return {.Error = Duplicated.Error};
			auto* Copy = Duplicated.Object;
			Candidate.Expressions.emplace_back(Copy);
		}
		OutState = std::move(Candidate);
		return {};
	}

	namespace
	{
		auto CopyExpressions(DMaterial& Destination, const DMaterial& Source) -> FMaterialProgramValidationResult
		{
			std::vector<DMaterialExpression*> Expressions;
			for (const auto& Expression : Source.GetExpressionCollection().Expressions)
				Expressions.push_back(Expression.Get());
			return Destination.SetMaterialExpressions(Expressions, Source.GetExpressionOutputs());
		}
	}

	FMaterialEditingSession::~FMaterialEditingSession()
	{
		if (Working) FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Working);
		if (WorkingPackage && Transactor.IsValid()) Transactor->ForgetPackage(*WorkingPackage);
		if (Working) MarkAsGarbage(Working.Get());
		if (WorkingPackage) MarkAsGarbage(WorkingPackage.Get());
	}

	auto FMaterialEditingSession::Initialize(DMaterial& InSource,
		EMaterialEditCompileMode Mode, std::string& Error, DTransactor* InTransactor) -> bool
	{
		if (Working || !InSource.GetPackage())
		{
			Error = "A material working copy requires a source package.";
			return false;
		}
		Source = &InSource;
		Transactor = InTransactor;
		SourceRevision = Source->GetPackage()->GetEditRevision();
		if (const auto Captured = Capture(InSource, Applied); !Captured)
		{
			Error = FormatObjectGraphError(Captured.Error);
			return false;
		}
		FPackagePath Path;
		const auto Mount = FMountPaths::FindMountForVirtualPath(Source->GetPackage()->GetPackagePath());
		if (!Mount)
		{
			Error = Mount.Message;
			return false;
		}
		if (const auto PathValidation = FPackagePath::TryCreate(std::format("{}__MaterialEditorPreview/{}",
			Mount.Mount->VirtualRoot, FGuid::NewGuid().ToString()), Path); !PathValidation) { Error = FormatObjectError(PathValidation.Error); return false; }
		WorkingPackage = NewObject<DPackage>(DPackage::StaticClass(), nullptr,
			NAME_None, EObjectFlags::Transient);
		WorkingPackage->InitializeAssetPackage(Path);
		WorkingPackage->SetStandaloneResidency(false);
		Working = NewObject<DMaterial>(DMaterial::StaticClass(), WorkingPackage.Get(),
			InSource.GetFName(), EObjectFlags::Transient);
		Working->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		if (!CopyExpressions(*Working, InSource))
		{
			Error = "The source material's authored state is invalid.";
			return false;
		}
		if (const auto Properties = Working->SetStaticProperties(Applied.Properties); !Properties)
		{
			Error = FormatMaterialError(Properties.Error);
			return false;
		}
		Working->SetMaterialGraphPresentation(Applied.Presentation);
		Working->CompileEdits();
		Working->SetEditCompileMode(Mode);
		WorkingPackage->ClearDirty();
		if (Transactor.IsValid()) Transactor->EstablishSavedState(*WorkingPackage);
		return true;
	}

	auto FMaterialEditingSession::HasUnappliedChanges() const -> bool
	{
		if (!Working || !WorkingPackage) return false;
		const uint64 Revision = WorkingPackage->GetEditRevision();
		if (ObservedWorkingRevision != Revision)
		{
			ObservedWorkingRevision = Revision;
			bHasChanges = !Applied.Matches(*Working);
		}
		return bHasChanges;
	}

	auto FMaterialEditingSession::CheckSource(std::string& Error) const -> bool
	{
		if (!Source || !Working || !Source->GetPackage())
		{
			Error = "The source material is no longer available.";
			return false;
		}
		if (Source->GetPackage()->GetEditRevision() != SourceRevision
			&& !Applied.Matches(*Source))
		{
			Error = "The source material changed outside this document. Discard or reopen before applying.";
			return false;
		}
		return true;
	}

	auto FMaterialEditingSession::RequestApply(std::string& Error) -> bool
	{
		if (!CheckSource(Error)) return false;
		if (!HasUnappliedChanges()) return true;
		bApplyPending = true;
		RequestedWorkingRevision = WorkingPackage->GetEditRevision();
		if (!Working->GetMaterialCompileStatus().IsCurrent()) Working->CompileEdits();
		Tick(Error);
		return Error.empty();
	}

	auto FMaterialEditingSession::Tick(std::string& Error) -> void
	{
		if (!bApplyPending || !Working) return;
		if (WorkingPackage->GetEditRevision() != RequestedWorkingRevision)
		{
			bApplyPending = false;
			Error = "The preview changed while Apply was compiling. Apply the latest edits again.";
			return;
		}
		const auto& Status = Working->GetMaterialCompileStatus();
		if (Status.State == EMaterialCompileState::Pending
			|| Status.State == EMaterialCompileState::Running
			|| Status.State == EMaterialCompileState::Deferred) return;
		bApplyPending = false;
		ApplyCurrent(Error);
	}

	auto FMaterialEditingSession::FinishAndApply(std::string& Error) -> bool
	{
		bApplyPending = false;
		if (!CheckSource(Error)) return false;
		if (!HasUnappliedChanges()) return true;
		if (!Working->GetMaterialCompileStatus().IsCurrent())
		{
			Working->CompileEdits();
			FAssetCompilingManager::Get().FinishCompilationForObject(*Working);
		}
		return ApplyCurrent(Error);
	}

	auto FMaterialEditingSession::ApplyCurrent(std::string& Error) -> bool
	{
		if (!CheckSource(Error)) return false;
		if (!HasUnappliedChanges()) return true;
		if (!Working->GetMaterialCompileStatus().IsCurrent())
		{
			Error = "Apply requires a successful compilation of the current preview. Compile the latest edits and retry.";
			return false;
		}
		FAuthoredState Candidate;
		if (const auto Captured = Capture(*Working, Candidate); !Captured)
		{
			Error = FormatObjectGraphError(Captured.Error);
			return false;
		}
		if (const auto Validation = ValidateMaterialStaticProperties(Candidate.Properties); !Validation)
		{
			Error = FormatMaterialError(Validation.Error);
			return false;
		}
		const auto PreviousMode = Source->GetEditCompileMode();
		Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		const auto Result = CopyExpressions(*Source, *Working);
		if (!Result)
		{
			Source->SetEditCompileMode(PreviousMode);
			Error = "The material changes could not be applied to the source.";
			return false;
		}
		Source->SetStaticProperties(Candidate.Properties);
		Source->SetMaterialGraphPresentation(Candidate.Presentation);
		if (Source->GetMaterialCompileStatus().HasUnsubmittedEdits()) Source->CompileEdits();
		Source->SetEditCompileMode(PreviousMode);
		if (Transactor.IsValid()) Transactor->InvalidateSavedState(*Source->GetPackage());
		Applied = std::move(Candidate);
		SourceRevision = Source->GetPackage()->GetEditRevision();
		ObservedWorkingRevision = 0;
		return true;
	}

	auto FMaterialEditingSession::MarkSaved() -> void
	{
		if (WorkingPackage) WorkingPackage->ClearDirty();
		if (WorkingPackage && Transactor.IsValid()) Transactor->MarkSaved(*WorkingPackage);
	}
}
