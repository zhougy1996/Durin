#pragma once

#include "CoreMinimal.h"
#include "Transactions/Transaction.h"
#include "Transactions/TransactionRecord.h"
#include "Materials/MaterialDiagnostic.h"

namespace Durin::Editor
{
	enum class EMaterialChangeError : uint8 { None, TargetUnavailable, MaterialWrite, PresentationWrite, MemberUnavailable, MembershipChanged, PropertyRestore, RollbackIncomplete };
	struct FMaterialChangeError
	{
		EMaterialChangeError Code = EMaterialChangeError::None;
		FGuid ParameterId;
		std::string TargetPath;
		std::string TargetLabel;
		std::string ExpectedParentPath, ActualParentPath;
		size_t NodeCount = 0;
		size_t MemberIndex = 0;
		std::shared_ptr<const FMaterialChangeError> CleanupCause;
		std::shared_ptr<const FTransactionSnapshotError> MemberCause;
		std::shared_ptr<const FPropertySnapshotError> PropertyCause;
		std::shared_ptr<const FMaterialError> MaterialCause;
	};
	struct FMaterialChangeResult
	{
		FMaterialChangeError Error;
		explicit operator bool() const { return Error.Code == EMaterialChangeError::None; }
	};
	auto FormatMaterialChangeError(const FMaterialChangeError& Error) -> std::string;


	// Adapts domain diagnostics at the history boundary.
	class FMaterialCommandChange : public FCommandChange
	{
	public:
		virtual auto Replay(ETransactionOperation Operation) -> FMaterialChangeResult = 0;
		auto Apply() -> FCommandChangeResult override { return Convert(Replay(ETransactionOperation::Redo)); }
		auto Revert() -> FCommandChangeResult override { return Convert(Replay(ETransactionOperation::Undo)); }
	private:
		static auto Convert(const FMaterialChangeResult& Result) -> FCommandChangeResult
		{
			if (Result) return {};
			bool bRecovery = false;
			for (auto* Error = &Result.Error; Error; Error = Error->CleanupCause.get())
				bRecovery |= Error->Code == EMaterialChangeError::RollbackIncomplete;
			return std::unexpected(FCommandChangeError{FormatMaterialChangeError(Result.Error), bRecovery});
		}
	};
}
