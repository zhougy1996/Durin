#pragma once

#include "DObject/Package.h"
#include "Editor/EditorTransaction.h"

namespace Durin
{
	// Centralizes the transaction checkpoint handoff at level-document lifecycle boundaries.
	struct FLevelDocumentRevisionState
	{
		static auto CompleteSave(
			FEditorTransactionManager* Transactions,
			DPackage& Package,
			bool bSucceeded
		) -> void
		{
			if (bSucceeded && Transactions) Transactions->MarkSaved(Package);
		}

		static auto Activate(FEditorTransactionManager* Transactions, DPackage* Package) -> void
		{
			if (!Transactions) return;
			Transactions->Clear();
			if (!Package) return;
			if (Package->IsDirty())
				Transactions->InvalidateSavedState(*Package);
			else
				Transactions->EstablishSavedState(*Package);
		}

		static auto Discard(FEditorTransactionManager* Transactions, DPackage& Package) -> void
		{
			if (Transactions) Transactions->ForgetPackage(Package);
			Package.ClearDirty();
		}
	};
}
