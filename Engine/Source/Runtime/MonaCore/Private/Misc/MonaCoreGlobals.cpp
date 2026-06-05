#include "MonaCoreGlobals.h"

#include "MonaUIBackend.h"

namespace Durin::Mona
{
	IMonaUIBackend* GMonaUIBackend = nullptr;
}

namespace Durin::MonaUI
{
	auto DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool
	{
		return Mona::GMonaUIBackend != nullptr
			&& Mona::GMonaUIBackend->DrawTexture(Texture, Size);
	}
}
