#pragma once

#include "CoreMinimal.h"

#include "Misc/Guid.h"

namespace Durin
{
	// Identifies one derived component independently of object name or construction generation.
	struct FActorGeneratedComponentKey
	{
		FName Namespace;
		FGuid Id;

		auto IsValid() const -> bool { return !Namespace.IsNone() && Id.IsValid(); }
		auto operator==(const FActorGeneratedComponentKey&) const -> bool = default;
	};

} // namespace Durin
