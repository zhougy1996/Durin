#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "Transactions/Transaction.h"
#include "DObject/PropertyDiagnostic.h"

namespace Durin
{
	enum class EActorChangeError : uint8 { None, TargetUnavailable, MemberUnavailable, MembershipChanged, PropertyRestore, TransformWrite, EmptySelection, ParentMismatch, ParentInvalid, ParentCycle, AttachmentWrite, ActorMembership, ActorNameCollision, ActorDestroy, ActorChanged, InjectedMutation, RollbackIncomplete };
	struct FActorChangeError
	{
		EActorChangeError Code = EActorChangeError::None;
		std::string TargetPath;
		std::string TargetLabel;
		std::string ExpectedParentPath, ActualParentPath;
		size_t NodeCount = 0;
		size_t MemberIndex = 0;
		std::shared_ptr<const FActorChangeError> CleanupCause;
		std::shared_ptr<const FPropertySnapshotError> PropertyCause;
	};
	struct FActorChangeResult
	{
		FActorChangeError Error;
		explicit operator bool() const { return Error.Code == EActorChangeError::None; }
	};
	DURINED_API auto FormatActorChangeError(const FActorChangeError& Error) -> std::string;
}
