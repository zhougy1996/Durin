#include "Actors/ActorChange.h"

namespace Durin
{
	auto FormatActorChangeError(const FActorChangeError& Error) -> std::string
	{
		auto Message = [&]() -> std::string {
		switch (Error.Code)
		{
		case EActorChangeError::None: return {};
		case EActorChangeError::ActorChanged: return "The actor changed after planning.";
		case EActorChangeError::InjectedMutation: return "Injected mutation failure.";
		case EActorChangeError::RollbackIncomplete: return "Rollback also failed.";
		case EActorChangeError::ActorMembership: return "The actor no longer belongs to the recorded level.";
		case EActorChangeError::ActorNameCollision: return "The requested actor name is occupied.";
		case EActorChangeError::ActorDestroy: return "The level rejected actor removal.";
		case EActorChangeError::EmptySelection: return "The transaction has no participants.";
		case EActorChangeError::ParentMismatch: return "The actor parent changed outside history.";
		case EActorChangeError::ParentInvalid: return "The desired actor parent is invalid.";
		case EActorChangeError::ParentCycle: return "The actor attachment would introduce a cycle.";
		case EActorChangeError::AttachmentWrite: return "The actor attachment rejected replay.";
		case EActorChangeError::TransformWrite: return "The transform target rejected replay.";
		case EActorChangeError::MemberUnavailable:
			return "The graph member is unavailable.";
		case EActorChangeError::MembershipChanged: return "The graph participants changed outside history.";
		case EActorChangeError::PropertyRestore:
			return Error.PropertyCause ? ToString(*Error.PropertyCause) : "The graph property could not be restored.";
		case EActorChangeError::TargetUnavailable: return "The custom transaction target is unavailable.";
		}
		return "The custom transaction failed.";
		}();
		if (Error.CleanupCause) Message += " " + FormatActorChangeError(*Error.CleanupCause);
		return Message;
	}

}
