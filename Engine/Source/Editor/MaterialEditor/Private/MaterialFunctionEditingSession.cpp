#include "MaterialFunctionEditingSession.h"
#include "Materials/MaterialExpression.h"

#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Misc/MountPaths.h"
#include "Transactions/Transactor.h"

namespace Durin::MaterialEditor
{

	auto FMaterialFunctionEditingSession::RefreshSourceRevision() -> void
	{
		if (const auto* Original = Source.Get(); Original && Original->GetPackage())
			SourceRevision = {Original->GetFunctionRevision(), Original->GetPackage()->GetEditRevision()};
	}

	auto FMaterialFunctionEditingSession::Initialize(DMaterialFunction& Original,
		DTransactor* Transactions, std::string& Error) -> bool
	{
		if (!Original.GetPackage()) { Error = "A function document requires a source package."; return false; }
		Source = &Original;
		Transactor = Transactions;
		RefreshSourceRevision();
		const auto Mount = FMountPaths::FindMountForVirtualPath(Original.GetPackage()->GetPackagePath());
		if (!Mount) { Error = ToString(Mount.error()); return false; }
		FPackagePath Path;
		if (const auto Result = FPackagePath::TryCreateWithDiagnostic(std::format("{}__FunctionEditorPreview/{}",
			Mount->Mount->VirtualRoot, FGuid::NewGuid().ToString()), Path); !Result)
		{ Error = ToString(Result.error()); return false; }
		WorkingPackage = NewObject<DPackage>(DPackage::StaticClass(), nullptr, NAME_None, EObjectFlags::Transient);
		WorkingPackage->InitializeAssetPackage(Path);
		const auto Copy = DuplicateObject(&Original, WorkingPackage.Get(), Original.GetFName());
		if (!Copy) { Error = ToString(Copy.error()); return false; }
		Working = Cast<DMaterialFunction>(Copy.value());
		Working->ClearFlags(EObjectFlags::Standalone | EObjectFlags::Public);
		Working->SetFlags(EObjectFlags::Transient | EObjectFlags::Transactional);
		WorkingPackage->ClearDirty();
		if (Transactor.IsValid()) Transactor.Get()->EstablishSavedState(*WorkingPackage);
		return true;
	}

	auto FMaterialFunctionEditingSession::IsDirty() const -> bool
	{ return WorkingPackage && WorkingPackage->IsDirty(); }

	auto FMaterialFunctionEditingSession::Publish(std::string& Error, bool bForce) -> bool
	{
		if (bPublishing) { Error = "Function publication is in progress."; return false; }
		if (!IsDirty() && !bForce) return true;
		const TStrongObjectPtr<DMaterialFunction> Original(Source.Get());
		if (!Original || !Original->GetPackage() || !Working)
		{ Error = "The source function is no longer available."; return false; }
		if (SourceRevision != std::pair{Original->GetFunctionRevision(), Original->GetPackage()->GetEditRevision()})
		{ Error = "The source function changed outside this document. Discard or reopen before applying."; return false; }
		std::vector<DMaterialFunctionInterface*> Roots;
		for (const auto& Dependency : Working->GetFunctionDependencies()) Roots.push_back(Dependency.Get());
		std::vector<FMaterialFunctionOwnerStamp> Closure;
		const auto Dependencies = ValidateMaterialFunctionDependencies(Roots, Closure);
		if (!Dependencies || std::ranges::contains(Roots, Original.Get())
			|| std::ranges::any_of(Closure, [&](const auto& Entry) { return Entry.AssetPath == Original->GetObjectPath(); }))
		{ Error = "Apply requires valid, non-recursive function dependencies."; return false; }
		const auto Presentation = Working->GetFunctionPresentation();
		if (Presentation.SchemaVersion != CurrentMaterialFunctionPresentationSchemaVersion)
		{ Error = "The function presentation schema is unsupported."; return false; }
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Working->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		const auto WorkingRevision = WorkingPackage->GetEditRevision();
		struct FPublishScope { bool& Flag; explicit FPublishScope(bool& Value) : Flag(Value) { Flag = true; } ~FPublishScope() { Flag = false; } } Scope(bPublishing);
		if (!Original->SetFunctionExpressions(Expressions))
		{ Error = "The function changes could not be applied to the source."; return false; }
		(void)Original->SetFunctionPresentation(Presentation);
		if (Transactor.IsValid()) Transactor.Get()->InvalidateSavedState(*Original->GetPackage());
		// Notification listeners may edit the draft; those newer edits remain unpublished.
		if (WorkingPackage->GetEditRevision() == WorkingRevision)
		{
			WorkingPackage->ClearDirty();
			if (Transactor.IsValid()) Transactor.Get()->MarkSaved(*WorkingPackage);
		}
		RefreshSourceRevision();
		return true;
	}
}
