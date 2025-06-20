#pragma once

class KWindow;

class KLEE_API FKleeRenderer
{
public:
	virtual auto CreateViewport(const TSharedPtr<KWindow>& Window) -> void = 0;
};