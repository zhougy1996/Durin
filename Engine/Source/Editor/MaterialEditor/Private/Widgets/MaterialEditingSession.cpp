#include "Widgets/MaterialEditingSession.h"
#include "Graph/MaterialGraphCanvas.h"
#include "MaterialGraph.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/MaterialAttributeDefinitionMap.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/Class.h"
#include "Transactions/Transactor.h"
#include "Misc/MountPaths.h"

namespace Durin::MaterialEditor
{
	auto FMaterialEditingSession::FAuthoredState::Matches(const DMaterial& Material, bool bIncludeLayout) const -> bool
	{
		const auto& Current = Material.GetExpressionCollection().Expressions;
		if (Domain != Material.GetDomain() || Properties != Material.GetStaticProperties()
			|| RootBindings.UseMaterialAttributes != Material.UsesMaterialAttributes() || Expressions.size() != Current.size()) return false;
		if (bIncludeLayout && (RootPosition != Material.GetRootPosition()
			|| Presentation != Material.GetMaterialGraphPresentation())) return false;
		const auto& Packed = *Material.GetExpressionInputForProperty(EMaterialProperty::MP_MaterialAttributes);
		if (RootBindings.AttributeSource != Packed.GetExpressionId() || RootBindings.AttributeOutputIndex != Packed.OutputIndex)
			return false;
		if (!std::ranges::equal(RootBindings.Properties, FMaterialAttributeDefinitionMap::GetDefinitions(),
			[&](const auto& Saved, const auto& Field) {
				const auto Live = Material.GetNumericInputForProperty(Field.Property);
				return Saved.Source == Live->Connection.GetExpressionId() && Saved.OutputIndex == Live->Connection.OutputIndex
					&& Saved.UseConstant == Live->UseConstant && std::ranges::equal(Saved.Constant, Live->Constant);
			})) return false;
		// Compare detached snapshot fields against references in this particular copy of the asset.
		std::unordered_map<FGuid, DMaterialExpression*> Nodes;
		for (const auto& Node : Current) Nodes.emplace(Node->Id, Node.Get());
		struct FResetConnections
		{
			const decltype(Connections)& Fields;
			~FResetConnections() { for (const auto& [Input, Id] : Fields) Input->Expression.Reset(); }
		} Reset{Connections};
		for (const auto& [Input, Id] : Connections)
		{
			const auto Found = Nodes.find(Id);
			if (Found == Nodes.end()) return false;
			Input->Expression = Found->second;
		}
		for (size_t Index = 0; Index < Expressions.size(); ++Index)
		{
			const auto* Before = Expressions[Index].Get();
			const auto* After = Current[Index].Get();
			if (!Before || !After || Before->GetClass() != After->GetClass()) return false;
			bool bIdentical = true;
			Before->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (Property->NamePrivate == FName("EditorLayout")) return;
				for (uint32 Element = 0; bIdentical && Element < Property->GetArrayDim(); ++Element)
					bIdentical = ArePropertyValuesIdentical(Property, Before, Element, After, Element);
			});
			if (!bIdentical) return false;
		}
		return true;
	}

	auto FMaterialEditingSession::Capture(const DMaterial& Material, FAuthoredState& OutState) -> std::expected<void, FObjectGraphError>
	{
		FAuthoredState Candidate;
		Candidate.Properties = Material.GetStaticProperties();
		Candidate.Domain = Material.GetDomain();
		Candidate.RootPosition = Material.GetRootPosition();
		Candidate.RootBindings = CaptureMaterialOutputBindings(Material);
		Candidate.Presentation = Material.GetMaterialGraphPresentation();
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
		{
			const auto Duplicated = DuplicateObject(Expression.Get(), nullptr, NAME_None);
			if (!Duplicated) return std::unexpected(Duplicated.error());
			auto* Copy = Duplicated.value();
			VisitStoredMaterialInputs(*Copy, [&](FMaterialExpressionInput& Input) {
				if (!Input.Expression) return;
				Candidate.Connections.emplace_back(&Input, Input.GetExpressionId());
				Input.Expression.Reset();
			});
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
		SourceRevision = Source->GetPackage()->GetEditRevision();
		if (const auto Captured = Capture(InSource, Applied); !Captured)
		{
			Error = ToString(Captured.error());
			return false;
		}
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
		if (const auto Configuration = Working->SetMaterialConfiguration({Applied.Domain, Applied.Properties}); !Configuration)
		{
			Error = FormatMaterialProgramDiagnostics(Configuration.Diagnostics);
			return false;
		}
		if (!CopyExpressions(*Working, InSource))
		{
			Error = "The source material's authored state is invalid.";
			return false;
		}
		Working->SetRootPosition(Applied.RootPosition.first, Applied.RootPosition.second);
		Working->CompileEdits();
		Working->SetEditCompileMode(Mode);
		Graph = DMaterialGraph::Create(*Working);
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
		if (Applied.Matches(*Working, false))
		{
			bApplyPending = false;
			return ApplyCurrent(Error);
		}
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
		if (!Working->IsCompilationFinished()) return;
		bApplyPending = false;
		ApplyCurrent(Error);
	}

	auto FMaterialEditingSession::FinishAndApply(std::string& Error) -> bool
	{
		bApplyPending = false;
		if (!CheckSource(Error)) return false;
		if (!HasUnappliedChanges()) return true;
		if (Applied.Matches(*Working, false)) return ApplyCurrent(Error);
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
		if (Applied.Matches(*Working, false))
		{
			Applied.RootPosition = Working->GetRootPosition();
			Applied.Presentation = Working->GetMaterialGraphPresentation();
			FScopedMaterialGraphChange GraphBatch(*Source);
			Source->SetRootPosition(Applied.RootPosition.first, Applied.RootPosition.second);
			Source->SetMaterialGraphPresentation(Applied.Presentation);
			if (Transactor.IsValid()) Transactor->InvalidateSavedState(*Source->GetPackage());
			SourceRevision = Source->GetPackage()->GetEditRevision();
			ObservedWorkingRevision = 0;
			return true;
		}
		if (!Working->GetMaterialCompileStatus().IsCurrent())
		{
			Error = "Apply requires a successful compilation of the current preview. Compile the latest edits and retry.";
			return false;
		}
		FAuthoredState Candidate;
		if (const auto Captured = Capture(*Working, Candidate); !Captured)
		{
			Error = ToString(Captured.error());
			return false;
		}
		if (const auto Validation = ValidateMaterialConfiguration(Candidate.Domain, Candidate.Properties); !Validation)
		{
			Error = Validation.error();
			return false;
		}
		const auto PreviousMode = Source->GetEditCompileMode();
		Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		FScopedMaterialGraphChange GraphBatch(*Source);
		const auto Result = CopyExpressions(*Source, *Working);
		if (!Result)
		{
			Source->SetEditCompileMode(PreviousMode);
			Error = "The material changes could not be applied to the source.";
			return false;
		}
		// The complete candidate configuration passed admission before publication.
		(void)Source->SetMaterialConfiguration({Candidate.Domain, Candidate.Properties});
		Source->SetRootPosition(Candidate.RootPosition.first, Candidate.RootPosition.second);
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
