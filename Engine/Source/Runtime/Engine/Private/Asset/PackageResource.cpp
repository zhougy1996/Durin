#include "Asset/PackageResource.h"

#include "Misc/FileHelper.h"

namespace Durin::Asset
{
	namespace Private
	{
		struct FPackageResourceRequestState
		{
			mutable std::mutex Mutex;
			std::condition_variable Ready;
			std::atomic_bool bCancelled = false;
			bool bTerminal = false;
			FPackageResourceReadResult Result;
			std::function<void()> OnCancel;

			auto Complete(FPackageResourceReadResult InResult) -> void
			{
				{
					std::lock_guard Lock(Mutex);
					if (bTerminal) return;
					Result = std::move(InResult);
					bTerminal = true;
				}
				Ready.notify_all();
			}
		};
	}

	namespace
	{
		auto Result(EPackageResourceReadStatus Status, std::string Message = {})
			-> FPackageResourceReadResult
		{
			return {.Status = Status, .Message = std::move(Message)};
		}

		class FLoosePackageResource final : public FPackageResource
		{
		public:
			FLoosePackageResource(std::filesystem::path InSegmentPath, uint64 Extent)
				: FPackageResource(Extent), SegmentPath(std::move(InSegmentPath)) {}

		private:
			auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool& bCancelled)
				-> FPackageResourceReadResult override
			{
				if (bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, "Package range request was cancelled.");
				std::error_code Error;
				const uint64 BeforeSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error)
					return Result(Error == std::errc::no_such_file_or_directory
						? EPackageResourceReadStatus::MissingSegment
						: EPackageResourceReadStatus::IoError,
						"Package bulk segment cannot be opened.");
				if (BeforeSize != GetSegmentExtent())
					return Result(EPackageResourceReadStatus::TruncatedSegment,
						"Package bulk segment extent changed after registration.");

				std::ifstream Stream(SegmentPath, std::ios::binary);
				if (!Stream.is_open())
					return Result(EPackageResourceReadStatus::IoError,
						"Package bulk segment range cannot be opened.");
				Stream.seekg(static_cast<std::streamoff>(Offset), std::ios::beg);
				if (!Stream)
					return Result(EPackageResourceReadStatus::IoError,
						"Package bulk segment range seek failed.");
				std::vector<std::byte> Bytes(static_cast<size_t>(Size));
				if (Size != 0)
					Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Size));
				if (Stream.gcount() != static_cast<std::streamsize>(Size))
					return Result(EPackageResourceReadStatus::TruncatedSegment,
						"Package bulk segment range is truncated.");
				if (bCancelled.load(std::memory_order_acquire))
					return Result(EPackageResourceReadStatus::Cancelled, "Package range request was cancelled.");
				const uint64 AfterSize = std::filesystem::file_size(SegmentPath, Error);
				if (Error || AfterSize != BeforeSize)
					return Result(EPackageResourceReadStatus::TruncatedSegment,
						"Package bulk segment changed during a range read.");
				return {.Status = EPackageResourceReadStatus::Success,
					.Buffer = FSharedByteBuffer::Take(std::move(Bytes))};
			}

			std::filesystem::path SegmentPath;
		};

		auto CompleteRetired() -> FPackageResourceRequest
		{
			return FPackageResourceRequest::Completed(Result(
				EPackageResourceReadStatus::Retired, "Package resource is retired."));
		}
	}

	auto FPackageResourceRequest::IsReady() const -> bool
	{
		if (!State) return true;
		std::lock_guard Lock(State->Mutex);
		return State->bTerminal;
	}

	auto FPackageResourceRequest::Cancel() -> void
	{
		if (!State) return;
		State->bCancelled.store(true, std::memory_order_release);
		std::function<void()> OnCancel;
		{
			std::lock_guard Lock(State->Mutex);
			OnCancel = State->OnCancel;
		}
		if (OnCancel) OnCancel();
	}

	auto FPackageResourceRequest::Wait() const -> FPackageResourceReadResult
	{
		if (!State) return Result(EPackageResourceReadStatus::Retired, "Package request is invalid.");
		std::unique_lock Lock(State->Mutex);
		State->Ready.wait(Lock, [&] { return State->bTerminal; });
		return State->Result;
	}

	auto FPackageResourceRequest::Completed(FPackageResourceReadResult InResult)
		-> FPackageResourceRequest
	{
		auto State = std::make_shared<Private::FPackageResourceRequestState>();
		State->Complete(std::move(InResult));
		return FPackageResourceRequest(std::move(State));
	}

	auto FPackageResourceRequest::Transform(
		FPackageResourceRequest Input,
		std::function<FPackageResourceReadResult(FPackageResourceReadResult)> Function)
		-> FPackageResourceRequest
	{
		auto State = std::make_shared<Private::FPackageResourceRequestState>();
		State->OnCancel = [Input]() mutable { Input.Cancel(); };
		std::thread([State, Input = std::move(Input), Function = std::move(Function)]() mutable {
			State->Complete(Function(Input.Wait()));
		}).detach();
		return FPackageResourceRequest(std::move(State));
	}

	FPackageResource::~FPackageResource()
	{
		Retire();
	}

	auto FPackageResource::ReadRangeAsync(uint64 Offset, uint64 Size)
		-> FPackageResourceRequest
	{
		if (Offset > SegmentExtent || Size > SegmentExtent - Offset
			|| Size > static_cast<uint64>(std::numeric_limits<size_t>::max()))
			return FPackageResourceRequest::Completed(Result(
				EPackageResourceReadStatus::InvalidRange, "Package resource range is invalid."));

		auto State = std::make_shared<Private::FPackageResourceRequestState>();
		{
			std::lock_guard Lock(Mutex);
			if (bRetired) return CompleteRetired();
			++ActiveRequests;
			++ReadStats.RequestCount;
			ReadStats.RequestedBytes = Size
				> std::numeric_limits<uint64>::max() - ReadStats.RequestedBytes
				? std::numeric_limits<uint64>::max()
				: ReadStats.RequestedBytes + Size;
			Requests.push_back(State);
		}
		auto Self = shared_from_this();
		std::thread([Self = std::move(Self), State, Offset, Size] {
			FPackageResourceReadResult ReadResult;
			if (State->bCancelled.load(std::memory_order_acquire))
				ReadResult = Result(EPackageResourceReadStatus::Cancelled,
					"Package range request was cancelled.");
			else
				ReadResult = Self->ReadRangeImpl(Offset, Size, State->bCancelled);
			State->Complete(std::move(ReadResult));
			{
				std::lock_guard Lock(Self->Mutex);
				--Self->ActiveRequests;
			}
			Self->Quiescent.notify_all();
		}).detach();
		return FPackageResourceRequest(std::move(State));
	}

	auto FPackageResource::ReadRange(uint64 Offset, uint64 Size)
		-> FPackageResourceReadResult
	{
		return ReadRangeAsync(Offset, Size).Wait();
	}

	auto FPackageResource::Retire() -> void
	{
		std::unique_lock Lock(Mutex);
		if (!bRetired)
		{
			bRetired = true;
			for (auto& Weak : Requests)
				if (auto State = Weak.lock())
					State->bCancelled.store(true, std::memory_order_release);
		}
		Quiescent.wait(Lock, [&] { return ActiveRequests == 0; });
		Requests.clear();
	}

	auto FPackageResource::IsRetired() const -> bool
	{
		std::lock_guard Lock(Mutex);
		return bRetired;
	}

	auto FPackageResource::GetReadStats() const -> FPackageResourceReadStats
	{
		std::lock_guard Lock(Mutex);
		return ReadStats;
	}

	FPackageResourceManager::~FPackageResourceManager()
	{
		Shutdown();
	}

	auto FPackageResourceManager::RegisterLoosePackage(
		std::string LogicalPackageId,
		const std::filesystem::path& PackagePath,
		const FPackageBulkSegmentSummary& Summary,
		std::span<const FPackageBulkDataEntry> Entries,
		FPackageResourceHandle& OutHandle,
		std::string* OutError) -> bool
	{
		if (LogicalPackageId.empty() || Summary.Extent == 0
			|| !ValidatePackageBulkDataMetadata(Summary, Entries, OutError)) return false;
		std::filesystem::path SegmentPath = PackagePath;
		SegmentPath.replace_extension(".dbulk");
		std::filesystem::path BackupPath = SegmentPath;
		BackupPath += ".durin-backup";
		std::error_code Error;
		const auto HasValidGeneration = [&](const std::filesystem::path& Path) {
			Error.clear();
			const uint64 Extent = std::filesystem::file_size(Path, Error);
			FXxHash128 Digest;
			return !Error && Extent == Summary.Extent
				&& FFileHelper::HashFileXx128(Path, Digest, Error) && Digest == Summary.Digest;
		};
		if (!HasValidGeneration(SegmentPath))
		{
			if (!HasValidGeneration(BackupPath))
			{
				if (OutError) *OutError =
					"Loose package bulk segment and backup do not match the package generation.";
				return false;
			}
			std::vector<std::byte> BackupBytes;
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::LoadFileToArray(BackupBytes, BackupPath)
				|| !FFileHelper::SaveArrayToFileAtomically(
					BackupBytes, SegmentPath, &PublicationError))
			{
				if (OutError) *OutError = "Loose package bulk backup recovery failed.";
				return false;
			}
		}
		std::filesystem::remove(BackupPath, Error);
		std::ifstream PaddingStream(SegmentPath, std::ios::binary);
		uint64 Cursor = 0;
		for (const FPackageBulkDataEntry& Entry : Entries)
		{
			if (Entry.Placement != EPackageBulkDataPlacement::External) continue;
			const uint64 PaddingSize = Entry.SegmentOffset - Cursor;
			if (PaddingSize != 0)
			{
				std::array<std::byte, EditorBulkDataExternalAlignment - 1> Padding{};
				PaddingStream.seekg(static_cast<std::streamoff>(Cursor), std::ios::beg);
				PaddingStream.read(reinterpret_cast<char*>(Padding.data()),
					static_cast<std::streamsize>(PaddingSize));
				if (PaddingStream.gcount() != static_cast<std::streamsize>(PaddingSize)
					|| std::ranges::any_of(std::span(Padding).first(
						static_cast<size_t>(PaddingSize)), [](std::byte Byte) { return Byte != std::byte{0}; }))
				{
					if (OutError) *OutError = "Loose package bulk segment has nonzero alignment padding.";
					return false;
				}
			}
			Cursor = Entry.SegmentOffset + Entry.StoredSize;
		}

		auto Resource = std::make_shared<FLoosePackageResource>(SegmentPath, Summary.Extent);
		FPackageResourceHandle Previous;
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown)
			{
				if (OutError) *OutError = "Package resource manager is shut down.";
				return false;
			}
			auto& Slot = Resources[std::move(LogicalPackageId)];
			Previous = std::move(Slot);
			Slot = Resource;
		}
		if (Previous) Previous->Retire();
		OutHandle = std::move(Resource);
		if (OutError) OutError->clear();
		return true;
	}

	auto FPackageResourceManager::RetirePackage(std::string_view LogicalPackageId) -> void
	{
		FPackageResourceHandle Resource;
		{
			std::lock_guard Lock(Mutex);
			const auto It = Resources.find(std::string(LogicalPackageId));
			if (It == Resources.end()) return;
			Resource = std::move(It->second);
			Resources.erase(It);
		}
		Resource->Retire();
	}

	auto FPackageResourceManager::FindPackage(std::string_view LogicalPackageId) const
		-> FPackageResourceHandle
	{
		std::lock_guard Lock(Mutex);
		const auto It = Resources.find(std::string(LogicalPackageId));
		return It == Resources.end() ? FPackageResourceHandle{} : It->second;
	}

	auto FPackageResourceManager::GetRegisteredPackageCount() const -> uint64
	{
		std::lock_guard Lock(Mutex);
		return static_cast<uint64>(Resources.size());
	}

	auto FPackageResourceManager::RetireAllPackages() -> void
	{
		std::unordered_map<std::string, FPackageResourceHandle> Retiring;
		{
			std::lock_guard Lock(Mutex);
			Retiring = std::move(Resources);
		}
		for (auto& [Name, Resource] : Retiring) Resource->Retire();
	}

	auto FPackageResourceManager::Shutdown() -> void
	{
		std::unordered_map<std::string, FPackageResourceHandle> Retiring;
		{
			std::lock_guard Lock(Mutex);
			if (bShutdown) return;
			bShutdown = true;
			Retiring = std::move(Resources);
		}
		for (auto& [Name, Resource] : Retiring) Resource->Retire();
	}

	auto GetPackageResourceManager() -> FPackageResourceManager&
	{
		static FPackageResourceManager Manager;
		return Manager;
	}
}
