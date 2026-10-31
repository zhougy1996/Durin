#pragma once

#include "CoreMinimal.h"
#include "DObject/OverriddenPropertySet.h"
#include "DObject/OverriddenPropertyRestore.h"

namespace Durin
{
	class FPropertyVisitorPath;

	// Stateless facade over each object's single immutable state. Live edits retain owner-thread rules.
	class FOverridableManager final
	{
	public:
		static COREDOBJECT_API auto GetOverriddenProperties(const DObject& Object) -> std::shared_ptr<const FOverriddenPropertySet>;
		static COREDOBJECT_API auto GetOverriddenPropertyEntries(const DObject& Object) -> std::vector<FAuthoredOverrideEntry>;
		static auto HasOverriddenProperties(const DObject& Object) -> bool { return GetOverriddenProperties(Object) != nullptr; }
		static COREDOBJECT_API auto GetOverriddenPropertyOperation(DObject& Object, const FAuthoredOverridePath& Path)
			-> std::expected<FOverriddenPropertyQuery, FAuthoredOverrideDiagnostic>;
		static COREDOBJECT_API auto GetOverriddenPropertyOperation(DObject& Object, const FPropertyVisitorPath& Path)
			-> std::expected<FOverriddenPropertyQuery, FAuthoredOverrideDiagnostic>;
		static COREDOBJECT_API auto OverrideProperty(DObject& Object, const FAuthoredOverridePath& Path,
			EAuthoredOverrideProvenance Provenance = EAuthoredOverrideProvenance::Forced,
			FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;
		static COREDOBJECT_API auto RestoreOverriddenPropertyOperation(DObject& Object,
			std::span<const FAuthoredOverrideEntry> Entries, FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr,
			FAuthoredOverrideValidationWork* Work = nullptr) -> bool;
		// Consumes sealed archive evidence. No caller-supplied paths or trust flags enter this route.
		static COREDOBJECT_API auto RestoreOverriddenPropertyOperation(
			std::unique_ptr<FOverriddenPropertyRestoreContext> Context,
			FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;
		static COREDOBJECT_API auto ClearOverriddenProperty(DObject& Object, const FAuthoredOverridePath& Path) -> bool;
		static COREDOBJECT_API auto ClearOverriddenPropertySubtree(DObject& Object, const FAuthoredOverridePath& Path) -> uint64;
		static COREDOBJECT_API auto ResetOverriddenProperties(DObject& Object) -> void;
		static COREDOBJECT_API auto CopyOverriddenProperties(DObject& Object, const DObject& Source,
			FAuthoredOverrideDiagnostic* OutDiagnostic = nullptr) -> bool;
	private:
		static auto Publish(DObject& Object, std::vector<FAuthoredOverrideEntry> Entries) -> void;
	};
}
