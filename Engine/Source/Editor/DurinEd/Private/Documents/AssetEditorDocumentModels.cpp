#include "Documents/AssetEditorDocumentModels.h"

#include "Asset/PackageReload.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Asset.h"
#include "DObject/Object.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "Editor/EditorEngine.h"
#include "Transactions/Transaction.h"
#include "Transactions/Transactor.h"

#include "Documents/EditorDocumentManager.h"

namespace Durin::Editor
{
	auto FEditableAssetDocumentModel::Activate(
		const FEditorDocument& Document, DObject* Object) -> bool
	{
		DocumentHost.RequestFocus(Document.Id);
		if (!Object) return false;
		std::vector<DObject*> AuthoredObjects{Object};
		for (size_t Index = 0; Index < AuthoredObjects.size(); ++Index)
		{
			AuthoredObjects[Index]->SetFlags(EObjectFlags::Transactional);
			for (DObject* Child : GDObjectArray.GetObjectsWithOuter(AuthoredObjects[Index], EObjectQueryScope::LiveOnly))
				AuthoredObjects.push_back(Child);
		}
		if (GEditor)
		{
			if (DPackage* Package = Object->GetPackage())
			{
				DTransactor& Transactions = *GEditor->GetTransactor();
				if (!Transactions.GetPackageRevisionState(*Package))
				{
					if (Package->IsDirty()) Transactions.InvalidateSavedState(*Package);
					else Transactions.EstablishSavedState(*Package);
				}
			}
		}
		ActiveResourceId = Document.ResourceId;
		return true;
	}

	auto FEditableAssetDocumentModel::Close(std::string_view ResourceId) -> void
	{
		if (ActiveResourceId == ResourceId) ActiveResourceId.clear();
	}

	auto FEditableAssetDocumentModel::IsDirty(const DObject* Object) const -> bool
	{
		return Object && Object->GetPackage() && Object->GetPackage()->IsDirty();
	}

	auto FEditableAssetDocumentModel::CanSave(const DObject* Object) const -> bool
	{
		return Object && Object->GetPackage();
	}

	auto FEditableAssetDocumentModel::Save(DObject* Object,
		const std::function<bool()>& BeforeSave,
		const std::function<void(std::string)>& ReportError) -> bool
	{
		if (!CanSave(Object) || (BeforeSave && !BeforeSave())) return false;
		const FAssetWriteResult Result = SavePackage(Object->GetPackage());
		if (Result)
		{
			if (GEditor) GEditor->GetTransactor()->MarkSaved(*Object->GetPackage());
			return true;
		}
		if (ReportError) ReportError(Result.Message);
		return false;
	}

	auto FEditableAssetDocumentModel::Discard(DObject* Object,
		const std::function<void()>& BeforeDiscard,
		const std::function<void(DPackage*, DPackage*)>& AfterReload,
		const std::function<void(std::string)>& ReportError,
		FEditorDocumentManager* DocumentManager) -> bool
	{
		if (!CanSave(Object)) return false;
		if (BeforeDiscard) BeforeDiscard();
		DPackage* PreviousPackage = Object->GetPackage();
		FPackagePath PackagePath;
		if (!FPackagePath::TryCreate(PreviousPackage->GetPackagePath(), PackagePath)) return false;
		FPackageReloadRequest Request{.Packages = {PreviousPackage}};
		if (DocumentManager) Request.Participants.push_back(DocumentManager->CreateDocumentReloadParticipant());
		if (GEditor)
		{
			auto Participant = CreateTransactorReloadParticipant(
				*GEditor->GetTransactor(), Request.Packages);
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
		if (AfterReload) AfterReload(PreviousPackage, ReplacementPackage);
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

	auto FEditableAssetDocumentModel::CanUndo() const -> bool
	{
		return GEditor && GEditor->GetTransactor()->CanUndo();
	}

	auto FEditableAssetDocumentModel::CanRedo() const -> bool
	{
		return GEditor && GEditor->GetTransactor()->CanRedo();
	}

	auto FEditableAssetDocumentModel::GetUndoDescription() const -> std::string
	{
		return CanUndo()
			? GEditor->GetTransactor()->GetUndoContext().Description : std::string{};
	}

	auto FEditableAssetDocumentModel::GetRedoDescription() const -> std::string
	{
		return CanRedo()
			? GEditor->GetTransactor()->GetRedoContext().Description : std::string{};
	}

	auto FEditableAssetDocumentModel::Undo() -> bool
	{
		return CanUndo() && GEditor->GetTransactor()->Undo();
	}

	auto FEditableAssetDocumentModel::Redo() -> bool
	{
		return CanRedo() && GEditor->GetTransactor()->Redo();
	}

	auto FReadOnlyAssetDocumentModel::Activate(
		const FEditorDocument& Document, const DObject* Object) -> bool
	{
		DocumentHost.RequestFocus(Document.Id);
		if (!Object) return false;
		ActiveResourceId = Document.ResourceId;
		return true;
	}

	auto FReadOnlyAssetDocumentModel::Close(std::string_view ResourceId) -> void
	{
		if (ActiveResourceId == ResourceId) ActiveResourceId.clear();
	}
}
