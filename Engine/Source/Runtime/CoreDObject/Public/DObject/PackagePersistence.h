#pragma once
#include "DObject/PackageCapture.h"
#include "Misc/PackageWriter.h"

namespace Durin
{
	enum class EPackageSaveMode : uint8 { Delta, Complete };
	struct FPackageSaveOptions
	{
		// Explicit physical destination; an empty path uses the configured resolver.
		std::filesystem::path Destination;
		EPackageSaveMode Mode = EPackageSaveMode::Delta;
		FPackageCaptureOptions Capture;
	};
	struct FSavePackageContext
	{
		FPackageSaveOptions Options;
		std::shared_ptr<IPackageWriter> Writer = GetFilePackageWriter();
		auto Capture(DPackage* Package, ObjectPackage::FLinkerTables& OutLinker,
			std::string* OutError = nullptr, uint32 FormatVersion = ObjectPackage::DastV10FormatVersion) const
			-> FPackageSaveResult
		{
			return CapturePackageLinker(Package, Options.Mode == EPackageSaveMode::Complete
				? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, Options.Capture, OutLinker, OutError, FormatVersion);
		}
		// Detached callers supply file order and recovery paths; ownership transfers.
		auto BeginWrite(std::vector<FPackageWriteFile> Files) const
			-> std::unique_ptr<IPackageWriteOperation>
		{ return (Writer ? Writer : GetFilePackageWriter())->Begin(std::move(Files)); }
	};
	COREDOBJECT_API auto ToPackageSaveResult(FPackageWriteResult Result) -> FPackageSaveResult;
	using FPackageDestinationResolver = std::function<std::filesystem::path(const DPackage&)>;
	COREDOBJECT_API auto SetPackageDestinationResolver(FPackageDestinationResolver Resolver) -> void;

	// Package persistence only: no asset admission, catalog publication or recovery policy.
	// Own and destroy on GameThread before object-system/scheduler shutdown.
	class COREDOBJECT_API FPackageSaveOperation
	{
	public:
		~FPackageSaveOperation();
		static auto Begin(DPackage* Package, const FPackageSaveOptions& Options,
			FPackageSaveResult& Admission, bool bAsync = true) -> std::unique_ptr<FPackageSaveOperation>;
		static auto Begin(DPackage* Package, const FSavePackageContext& Context,
			FPackageSaveResult& Admission, bool bAsync = true) -> std::unique_ptr<FPackageSaveOperation>;
		auto IsStagingReady() const -> bool;
		auto IsCompleted() const -> bool;
		auto Complete() -> FPackageSaveResult;
		// Waits only for I/O; never waits on queued GameThread work.
		auto WaitAndComplete() -> FPackageSaveResult;
		auto Cancel() -> FPackageSaveResult;
		// Transaction coordinator seam: retain rollback state until FinalizeCommit.
		auto CommitStaged() -> FPackageSaveResult;
		auto RollbackCommit() -> FPackageSaveResult;
		auto FinalizeCommit() -> FPackageSaveResult;
	private:
		FPackageSaveOperation();
		struct FState;
		std::unique_ptr<FState> State;
	};
}
