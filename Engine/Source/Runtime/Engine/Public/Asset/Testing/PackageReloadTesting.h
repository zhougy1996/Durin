#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

namespace Durin::Testing
{
	enum class EPackageReloadFaultPoint : uint8
	{
		PreflightBudget,
		QuiesceSelected,
		ReadMain,
		ReadBulk,
		CreateSkeleton,
		ResolveDependency,
		ApplyValues,
		RestoreLedger,
		PreparePostLoad,
		PrepareReferences,
		PrepareNativeParticipant,
		PrepareHistory,
		PrepareRuntimeProduct,
		ReserveRenderPublish,
		RevalidateDisk,
		RevalidateReferencers,
		BeforeCommit,
	};

	using FPackageReloadFault = std::function<bool(EPackageReloadFaultPoint, uint64, uint64)>;

	// GameThread-only. The synchronous Reload invocation observes the active seam;
	// nested scopes restore the previous seam, including exception unwinding.
	class FScopedPackageReloadFault final
	{
	public:
		ENGINE_API explicit FScopedPackageReloadFault(FPackageReloadFault Fault);
		ENGINE_API ~FScopedPackageReloadFault();
		FScopedPackageReloadFault(const FScopedPackageReloadFault&) = delete;
		auto operator=(const FScopedPackageReloadFault&) -> FScopedPackageReloadFault& = delete;
	private:
		FPackageReloadFault Fault;
		const FPackageReloadFault* Previous;
	};
}
