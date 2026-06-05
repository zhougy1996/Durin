#include "MonaCoreGlobals.h"

#include "MonaUIInterface.h"

namespace Durin::Mona
{
	IMonaUIInterface* GMonaUI = nullptr;
}

namespace Durin::MonaUI
{
	auto DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool
	{
		return Mona::GMonaUI != nullptr
			&& Mona::GMonaUI->DrawTexture(Texture, Size);
	}
}
