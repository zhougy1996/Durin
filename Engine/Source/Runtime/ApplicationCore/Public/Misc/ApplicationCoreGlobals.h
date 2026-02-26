#pragma once

namespace Doge
{
	class FGenericApplication;

	extern APPLICATIONCORE_API TSharedPtr<FGenericApplication> GApp;

	APPLICATIONCORE_API auto ApplicationInit() -> void;
}