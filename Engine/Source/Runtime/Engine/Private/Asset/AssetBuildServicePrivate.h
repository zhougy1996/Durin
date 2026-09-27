#pragma once
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin::AssetBuildPrivate
{
	ENGINE_API auto CreateSession(std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver)
		-> std::expected<std::shared_ptr<DerivedData::FBuildSession>, DerivedData::FBuildError>;
	// Release only after execution returns. The service retains abandoned handles
	// until shutdown so concurrent caller destruction cannot escape its drain.
	ENGINE_API auto ReleaseSession(const std::shared_ptr<DerivedData::FBuildSession>& Session) -> void;
	struct FSessionScope
	{
		std::shared_ptr<DerivedData::FBuildSession> Session;
		~FSessionScope() { ReleaseSession(Session); }
	};
}
#endif
