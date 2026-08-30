#pragma once

#include "DObject/Package.h"
#include "Editor/Transactor.h"

namespace Durin::Editor::Level
{
	// Centralizes the transaction checkpoint handoff at level-document lifecycle boundaries.
	struct FLevelDocumentRevisionState
	{
		static auto CompleteSave(
			::Durin::DTransactor* Transactions,
			DPackage& Package,
			bool bSucceeded
		) -> void
		{
			if (bSucceeded && Transactions) Transactions->MarkSaved(Package);
		}

		static auto Activate(::Durin::DTransactor* Transactions, DPackage* Package) -> void
		{
			if (!Transactions) return;
			(void)Transactions->Reset();
			if (!Package) return;
			if (Package->IsDirty())
				Transactions->InvalidateSavedState(*Package);
			else
				Transactions->EstablishSavedState(*Package);
		}

		static auto Discard(::Durin::DTransactor* Transactions, DPackage& Package) -> void
		{
			if (Transactions) Transactions->ForgetPackage(Package);
			Package.ClearDirty();
		}
	};
}
