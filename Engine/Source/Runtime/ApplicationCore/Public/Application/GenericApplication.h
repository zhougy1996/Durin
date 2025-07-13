#pragma once

class APPLICATIONCORE_API FGenericApplication
{
public:
	virtual ~FGenericApplication() = default;

	virtual auto Tick() -> void;

	virtual auto ProcessDeferredEvents() -> void;
};
