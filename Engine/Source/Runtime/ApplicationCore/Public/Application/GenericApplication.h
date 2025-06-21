#pragma once

class APPLICATION_CORE_API FGenericApplication
{
public:
	virtual ~FGenericApplication() = default;

	virtual auto Tick() -> void;

	virtual auto ProcessDeferredEvents() -> void;
};
