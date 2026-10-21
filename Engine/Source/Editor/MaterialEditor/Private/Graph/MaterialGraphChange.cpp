#include "Graph/MaterialGraphChange.h"

namespace Durin::Editor
{
	auto FormatMaterialChangeError(const FMaterialChangeError& Error) -> std::string
	{
		std::string Message;
		if (!Error.Message.empty()) Message = Error.Message;
		else if (Error.PropertyCause) Message = ToString(*Error.PropertyCause);
		else switch (Error.Code)
		{
		case EMaterialChangeError::None: break;
		case EMaterialChangeError::MembershipChanged: Message = "The graph participants changed outside history."; break;
		case EMaterialChangeError::RollbackIncomplete: Message = "Rollback also failed."; break;
		case EMaterialChangeError::TargetUnavailable: Message = "The custom transaction target is unavailable."; break;
		case EMaterialChangeError::MemberUnavailable: Message = "The graph member is unavailable."; break;
		case EMaterialChangeError::PropertyRestore: Message = "The graph property could not be restored."; break;
		case EMaterialChangeError::PresentationWrite: Message = "The graph presentation rejected replay."; break;
		case EMaterialChangeError::MaterialWrite: Message = "The material write failed."; break;
		}
		if (Error.CleanupCause) Message += " " + FormatMaterialChangeError(*Error.CleanupCause);
		return Message;
	}
}
