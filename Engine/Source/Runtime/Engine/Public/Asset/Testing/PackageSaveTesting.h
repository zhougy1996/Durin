#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

namespace Durin::Testing
{
	enum class EPackageSaveFaultPoint : uint8
	{
		CreateDirectories, PublishCompanion, StagePackage, PublishPackage, PublishRegistry
	};
	using FPackageSaveFault = std::function<bool(EPackageSaveFaultPoint, size_t)>;
	// Captured by async operations at admission; callbacks must own what they need
	// through completion. No thread-local scope survives an async suspension.
	ENGINE_API auto CapturePackageSaveFault() -> FPackageSaveFault;
	class FScopedPackageSaveFault final
	{
	public:
		ENGINE_API explicit FScopedPackageSaveFault(FPackageSaveFault Fault);
		ENGINE_API ~FScopedPackageSaveFault();
		FScopedPackageSaveFault(const FScopedPackageSaveFault&) = delete;
		auto operator=(const FScopedPackageSaveFault&) -> FScopedPackageSaveFault& = delete;
	private:
		FPackageSaveFault Fault;
		const FPackageSaveFault* Previous;
	};
}
