#pragma once

#include "DObject/Package.h"
#include "Editor/Transaction.h"

namespace Durin::Editor::Level
{
	// Centralizes the transaction checkpoint handoff at level-document lifecycle boundaries.
	struct FLevelDocumentRevisionState
	{
		static auto CompleteSave(
			::Durin::Editor::FTransactionManager* Transactions,
			DPackage& Package,
			bool bSucceeded
		) -> void
		{
			if (bSucceeded && Transactions) Transactions->MarkSaved(Package);
		}

		static auto Activate(::Durin::Editor::FTransactionManager* Transactions, DPackage* Package) -> void
		{
			if (!Transactions) return;
			Transactions->Clear();
			if (!Package) return;
			if (Package->IsDirty())
				Transactions->InvalidateSavedState(*Package);
			else
				Transactions->EstablishSavedState(*Package);
		}

		static auto Discard(::Durin::Editor::FTransactionManager* Transactions, DPackage& Package) -> void
		{
			if (Transactions) Transactions->ForgetPackage(Package);
			Package.ClearDirty();
		}
	};
}
