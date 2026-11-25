#include "Widgets/MaterialEditingSession.h"
#include "Graph/MaterialGraphCanvas.h"
#include "MaterialGraph.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Transactions/Transactor.h"
#include "Misc/MountPaths.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto CopyExpressions(DMaterial& Destination, const DMaterial& Source) -> FMaterialProgramValidationResult
		{
			std::vector<DMaterialExpression*> Expressions;
			for (const auto& Expression : Source.GetExpressionCollection().Expressions)
				Expressions.push_back(Expression.Get());
			return Destination.SetMaterialExpressions(Expressions, Source.CaptureRootInputs());
		}
	}

	FMaterialEditingSession::FMaterialEditingSession() = default;

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
		SourceRevision = Source->GetGraphChanges().GetEditRevision();
		FPackagePath Path;
		const auto Mount = FMountPaths::FindMountForVirtualPath(Source->GetPackage()->GetPackagePath());
		if (!Mount)
		{
			Error = (Mount ? std::string{} : ToString(Mount.error()));
			return false;
		}
		if (const auto PathValidation = FPackagePath::TryCreateWithDiagnostic(std::format("{}__MaterialEditorPreview/{}",
			Mount->Mount->VirtualRoot, FGuid::NewGuid().ToString()), Path); !PathValidation) { Error = ToString(PathValidation.error()); return false; }
		WorkingPackage = NewObject<DPackage>(DPackage::StaticClass(), nullptr,
			NAME_None, EObjectFlags::Transient);
		WorkingPackage->InitializeAssetPackage(Path);
		Working = NewObject<DMaterial>(DMaterial::StaticClass(), WorkingPackage.Get(),
			InSource.GetFName(), EObjectFlags::Transient);
		Working->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		if (const auto Configuration = Working->SetMaterialConfiguration({InSource.GetDomain(), InSource.GetStaticProperties()}); !Configuration)
		{
			Error = FormatMaterialProgramDiagnostics(Configuration.Diagnostics);
			return false;
		}
		if (!CopyExpressions(*Working, InSource))
		{
			Error = "The source material's authored state is invalid.";
			return false;
		}
		const auto [X, Y] = InSource.GetRootPosition();
		Working->SetRootPosition(X, Y);
		Working->CompileEdits();
		Working->SetEditCompileMode(Mode);
		Graph = DMaterialGraph::Create(*Working);
		PublishedWorkingRevision = Working->GetGraphChanges().GetEditRevision();
		WorkingPackage->ClearDirty();

		return true;
	}

	auto FMaterialEditingSession::HasUnappliedChanges() const -> bool
	{
		if (!Working || !WorkingPackage) return false;
		if (WorkingPackage->IsDirty()) return true;
		if (!Source) return false;
		if (Source->GetGraphChanges().GetEditRevision() != SourceRevision)
			return Working->GetGraphChanges().GetEditRevision() != PublishedWorkingRevision;
		return Source->GetRootPosition() != Working->GetRootPosition()
			|| Source->GetMaterialGraphPresentation() != Working->GetMaterialGraphPresentation()
			|| HasSemanticChanges();
	}

	auto FMaterialEditingSession::HasSemanticChanges() const -> bool
	{
		return Source->GetDomain() != Working->GetDomain()
			|| Source->GetStaticProperties() != Working->GetStaticProperties()
			|| CaptureMaterialOutputBindings(*Source) != CaptureMaterialOutputBindings(*Working)
			|| !std::ranges::equal(Source->GetExpressionCollection().Expressions, Working->GetExpressionCollection().Expressions,
				[](const auto& Original, const auto& Draft) {
					return Original && Draft && HaveSameMaterialExpressionFields(*Original, *Draft);
				});
	}

	auto FMaterialEditingSession::CheckSource(std::string& Error) const -> bool
	{
		if (bPublishing)
		{
			Error = "Material publication is in progress.";
			return false;
		}
		if (!Source || !Working || !Source->GetPackage())
		{
			Error = "The source material is no longer available.";
			return false;
		}
		if (Source->GetGraphChanges().GetEditRevision() != SourceRevision)
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
		if (!HasSemanticChanges())
		{
			bApplyPending = false;
			return ApplyCurrent(Error);
		}
		bApplyPending = true;
		RequestedWorkingRevision = Working->GetGraphChanges().GetEditRevision();
		if (!Working->GetMaterialCompileStatus().IsCurrent()) Working->CompileEdits();
		Tick(Error);
		return Error.empty();
	}

	auto FMaterialEditingSession::Tick(std::string& Error) -> void
	{
		if (!bApplyPending || !Working) return;
		if (Working->GetGraphChanges().GetEditRevision() != RequestedWorkingRevision)
		{
			bApplyPending = false;
			Error = "The preview changed while Apply was compiling. Apply the latest edits again.";
			return;
		}
		if (!Working->IsCompilationFinished()) return;
		bApplyPending = false;
		ApplyCurrent(Error);
	}

	auto FMaterialEditingSession::FinishAndApply(std::string& Error) -> bool
	{
		bApplyPending = false;
		if (!CheckSource(Error)) return false;
		if (!HasUnappliedChanges()) return true;
		if (!HasSemanticChanges()) return ApplyCurrent(Error);
		const auto Status = Working->GetMaterialCompileStatus();
		if (Status.HasUnsubmittedEdits() || (!Status.IsCurrent() && Working->IsCompilationFinished()))
			Working->CompileEdits();
		FAssetCompilingManager::Get().FinishCompilationForObject(*Working);
		return ApplyCurrent(Error);
	}

	auto FMaterialEditingSession::ApplyCurrent(std::string& Error) -> bool
	{
		if (!CheckSource(Error)) return false;
		if (!HasUnappliedChanges()) return true;
		const bool bSemantic = HasSemanticChanges();
		const FMaterialConfiguration Configuration{Working->GetDomain(), Working->GetStaticProperties()};
		if (bSemantic)
		{
			if (!Working->GetMaterialCompileStatus().IsCurrent())
			{
				Error = "Apply requires a successful compilation of the current preview. Compile the latest edits and retry.";
				return false;
			}
			if (const auto Validation = ValidateMaterialConfiguration(Configuration.Domain, Configuration.Properties); !Validation)
			{
				Error = Validation.error();
				return false;
			}
		}
		const auto [X, Y] = Working->GetRootPosition();
		const auto WorkingRevision = Working->GetGraphChanges().GetEditRevision();
		struct FPublishScope
		{
			bool& Flag;
			explicit FPublishScope(bool& InFlag) : Flag(InFlag) { Flag = true; }
			~FPublishScope() { Flag = false; }
		} Publishing(bPublishing);
		{
			FScopedMaterialGraphChange GraphBatch(*Source);
			if (bSemantic)
			{
				const auto PreviousMode = Source->GetEditCompileMode();
				Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
				const auto Result = CopyExpressions(*Source, *Working);
				if (!Result)
				{
					Source->SetEditCompileMode(PreviousMode);
					Error = "The material changes could not be applied to the source.";
					return false;
				}
				require(Source->SetMaterialConfiguration(Configuration));
				if (Source->GetMaterialCompileStatus().HasUnsubmittedEdits()) Source->CompileEdits();
				Source->SetEditCompileMode(PreviousMode);
			}
			else
			{
				Source->SetMaterialGraphPresentation(Working->GetMaterialGraphPresentation());
			}
			Source->SetRootPosition(X, Y);
			if (Source->GetGraphChanges().GetEditRevision() != SourceRevision)
				Source->MarkPackageDirty();
			SourceRevision = Source->GetGraphChanges().GetEditRevision();
		}
		// Publication listeners can edit the draft while the source is being updated.
		if (Working->GetGraphChanges().GetEditRevision() == WorkingRevision)
		{
			PublishedWorkingRevision = WorkingRevision;
			WorkingPackage->ClearDirty();
		}
		return true;
	}
}
