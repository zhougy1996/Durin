#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "Transactions/Transaction.h"
#include "DObject/PropertyDiagnostic.h"

namespace Durin::Editor
{
	enum class EActorChangeError : uint8 { None, TargetUnavailable, MemberUnavailable, MembershipChanged, PropertyRestore, TransformWrite, EmptySelection, ParentMismatch, ParentInvalid, ParentCycle, AttachmentWrite, ResourceUnavailable, ActorMembership, ActorNameCollision, ActorSpawn, ActorDestroy, WorldEnding, ActorType, ActorUnsupported, ActorChanged, ActorRename, InjectedMutation, RollbackIncomplete };
	enum class EActorMutationPhase : uint8 { None, TemporaryRename, Remove, Create, FinalRename, Update };
	enum class EActorChangeConstraint : uint8 { None, Class, ComponentGraph, Parent, Children, BeginningPlay, EndingPlay };
	struct FActorChangeError
	{
		EActorChangeError Code = EActorChangeError::None;
		std::string TargetPath;
		std::string TargetLabel;
		std::string ExpectedParentPath, ActualParentPath;
		size_t NodeCount = 0;
		size_t MemberIndex = 0;
		EActorMutationPhase MutationPhase = EActorMutationPhase::None;
		EActorChangeConstraint ActorConstraint = EActorChangeConstraint::None;
		std::shared_ptr<const FActorChangeError> CleanupCause;
		std::shared_ptr<const FPropertySnapshotError> PropertyCause;
	};
	struct FActorChangeResult
	{
		FActorChangeError Error;
		explicit operator bool() const { return Error.Code == EActorChangeError::None; }
	};
	DURINED_API auto FormatActorChangeError(const FActorChangeError& Error) -> std::string;


	// Adapts domain diagnostics at the history boundary.
	class FActorCommandChange : public FCommandChange
	{
	public:
		virtual auto Replay(ETransactionOperation Operation) -> FActorChangeResult = 0;
		auto Apply() -> FCommandChangeResult override { return Convert(Replay(ETransactionOperation::Redo)); }
		auto Revert() -> FCommandChangeResult override { return Convert(Replay(ETransactionOperation::Undo)); }
	private:
		static auto Convert(const FActorChangeResult& Result) -> FCommandChangeResult
		{
			if (Result) return {};
			bool bRecovery = false;
			for (auto* Error = &Result.Error; Error; Error = Error->CleanupCause.get())
				bRecovery |= Error->Code == EActorChangeError::RollbackIncomplete;
			return std::unexpected(FCommandChangeError{FormatActorChangeError(Result.Error), bRecovery});
		}
	};
}
