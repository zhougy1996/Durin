#include "Toolkits/AssetEditorSession.h"

#include "Asset/PackageReload.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Asset.h"
#include "DObject/Object.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transaction.h"
#include "Transactions/Transactor.h"

#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin
{
	auto FAssetEditorSession::BindEditor(DEditorEngine* Editor) -> void { BoundEditor = Editor; bEditorBound = true; }
	auto FAssetEditorSession::GetEditor() const -> DEditorEngine*
	{ return bEditorBound ? BoundEditor.Get() : GetTransactionEditor(); }
	auto FAssetEditorSession::Activate(DObject* Object) -> bool
	{
		if (!IsValid(Object)) return false;
		std::vector<DObject*> AuthoredObjects{Object};
		for (size_t Index = 0; Index < AuthoredObjects.size(); ++Index)
		{
			AuthoredObjects[Index]->SetFlags(EObjectFlags::Transactional);
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(AuthoredObjects[Index], EObjectQueryScope::LiveOnly))
				AuthoredObjects.push_back(Child);
		}
		if (GetEditor() && GetEditor()->GetTransactor())
		{
			if (DPackage* Package = Object->GetPackage())
			{
				DTransactor& Transactions = *GetEditor()->GetTransactor();
				if (!Transactions.GetPackageRevisionState(*Package))
				{
					if (Package->IsDirty()) Transactions.InvalidateSavedState(*Package);
					else Transactions.EstablishSavedState(*Package);
				}
			}
		}
		return true;
	}

	auto FAssetEditorSession::IsDirty(const DObject* Object) const -> bool
	{
		return Object && Object->GetPackage() && Object->GetPackage()->IsDirty();
	}

	auto FAssetEditorSession::CanSave(const DObject* Object) const -> bool
	{
		return Object && Object->GetPackage();
	}

	auto FAssetEditorSession::Save(DObject* Object,
		const std::function<bool()>& BeforeSave,
		const std::function<void(std::string)>& ReportError) -> bool
	{
		if (!CanSave(Object) || (BeforeSave && !BeforeSave())) return false;
		FAssetWriteResult Result;
		if (SavePackage(Object->GetPackage(), {}, &Result))
		{
			if (GetEditor() && GetEditor()->GetTransactor()) GetEditor()->GetTransactor()->MarkSaved(*Object->GetPackage());
			return true;
		}
		if (ReportError) ReportError((Result.Error ? Result.Error->ToString() : std::string{}));
		return false;
	}

	auto FAssetEditorSession::Discard(DObject* Object,
		const std::function<void()>& BeforeDiscard,
		const std::function<void(std::string)>& ReportError) -> bool
	{
		if (!CanSave(Object)) return false;
		if (BeforeDiscard) BeforeDiscard();
		DPackage* PreviousPackage = Object->GetPackage();
		FPackagePath PackagePath;
		if (!FPackagePath::TryCreate(PreviousPackage->GetPackagePath(), PackagePath)) return false;
		FPackageReloadRequest Request{.Packages = {PreviousPackage}};
		auto* Editor = GetEditor();
		auto* Editors = Editor ? Editor->GetEditorSubsystem<DAssetEditorSubsystem>() : nullptr;
		if (Editors)
		{
			if (!Editors->AddPackageReloadParticipants(Request))
			{
				if (ReportError) ReportError("Discard requires idle editor and transaction state.");
				return false;
			}
		}
		if (!Editors && GetEditor() && GetEditor()->GetTransactor())
		{
			auto Participant = CreateTransactorReloadParticipant(
				*GetEditor()->GetTransactor(), Request.Packages);
			if (!Participant)
			{
				if (ReportError) ReportError("Discard requires an idle transaction history.");
				return false;
			}
			Request.Participants.push_back(std::move(Participant));
		}
		auto Operation = ReloadPackages(Request);
		FPackageReloadResult Result = Operation.GetResult();
		if (Result.Status == EPackageReloadStatus::Failed
			|| Result.Status == EPackageReloadStatus::Cancelled)
		{
			if (ReportError && !Result.Diagnostics.empty())
				ReportError(FormatPackageReloadDiagnostic(Result.Diagnostics.front()));
			return false;
		}
		DPackage* ReplacementPackage = FindPackage(PackagePath.GetView());
		if (!ReplacementPackage || ReplacementPackage == PreviousPackage)
		{
			if (ReportError) ReportError("Discard did not publish a replacement package.");
			return false;
		}
		Result = Operation.Wait();
		if (!Result.Succeeded())
		{
			if (ReportError) ReportError(Result.Diagnostics.empty()
				? "Discard is waiting for old resource retirement."
				: FormatPackageReloadDiagnostic(Result.Diagnostics.front()));
			return false;
		}
		// The reload participant publishes history and the replacement save baseline together.
		return true;
	}

	auto FAssetEditorSession::CanUndo() const -> bool
	{
		return GetEditor() && GetEditor()->GetTransactor() && GetEditor()->GetTransactor()->CanUndo();
	}

	auto FAssetEditorSession::CanRedo() const -> bool
	{
		return GetEditor() && GetEditor()->GetTransactor() && GetEditor()->GetTransactor()->CanRedo();
	}

	auto FAssetEditorSession::GetUndoDescription() const -> std::string
	{
		return CanUndo()
			? GetEditor()->GetTransactor()->GetUndoContext().Description : std::string{};
	}

	auto FAssetEditorSession::GetRedoDescription() const -> std::string
	{
		return CanRedo()
			? GetEditor()->GetTransactor()->GetRedoContext().Description : std::string{};
	}

	auto FAssetEditorSession::Undo() -> bool
	{
		return CanUndo() && GetEditor()->GetTransactor()->Undo();
	}

	auto FAssetEditorSession::Redo() -> bool
	{
		return CanRedo() && GetEditor()->GetTransactor()->Redo();
	}

}
