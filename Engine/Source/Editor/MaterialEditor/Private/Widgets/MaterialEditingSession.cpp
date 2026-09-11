#include "Widgets/MaterialEditingSession.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/Transactor.h"
#include "Misc/MountPaths.h"

namespace Durin::Editor::Material
{
	auto FMaterialEditingSession::Capture(const DMaterial& Material) -> FAuthoredState
	{
		const auto Definitions = Material.GetParameterDefinitions();
		const auto Calls = Material.GetMaterialFunctionCalls();
		return {*Material.GetMaterialProgram(), {Definitions.begin(), Definitions.end()},
			Material.GetStaticProperties(), Material.GetMaterialGraphPresentation(), {Calls.begin(), Calls.end()}};
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
		Applied = Capture(InSource);
		FPackagePath Path;
		const auto Mount = FMountPaths::FindMountForVirtualPath(Source->GetPackage()->GetPackagePath());
		if (!Mount)
		{
			Error = Mount.Message;
			return false;
		}
		if (!FPackagePath::TryCreate(std::format("{}__MaterialEditorPreview/{}",
			Mount.Mount->VirtualRoot, FGuid::NewGuid().ToString()), Path, &Error)) return false;
		WorkingPackage = NewObject<DPackage>(DPackage::StaticClass(), nullptr,
			NAME_None, EObjectFlags::Transient);
		WorkingPackage->InitializeAssetPackage(Path);
		WorkingPackage->SetStandaloneResidency(false);
		Working = NewObject<DMaterial>(DMaterial::StaticClass(), WorkingPackage.Get(),
			InSource.GetFName(), EObjectFlags::Transient);
		Working->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		if (!Working->SetMaterialDefinitionsAndProgram(Applied.Definitions, Applied.Program, Applied.FunctionCalls)
			|| !Working->SetStaticProperties(Applied.Properties))
		{
			Error = "The source material's authored state is invalid.";
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
			bHasChanges = Capture(*Working) != Applied;
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
			&& Capture(*Source) != Applied)
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
		const FAuthoredState Candidate = Capture(*Working);
		if (!ValidateMaterialStaticProperties(Candidate.Properties, Error)) return false;
		const auto PreviousMode = Source->GetEditCompileMode();
		Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		const auto Result = Source->SetMaterialDefinitionsAndProgram(
			Candidate.Definitions, Candidate.Program, Candidate.FunctionCalls);
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
		Applied = Candidate;
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
