#include "CacheStorageOwnership.h"
#include "Misc/Paths.h"
#if defined(_WIN32)
#include "Windows/WindowsPlatform.h"
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Durin::DerivedData::Private
{
	namespace
	{
		std::mutex OwnershipMutex;
		std::shared_ptr<FCacheStorageLease> ProcessLease;
		auto Failure(std::string Message) -> std::unexpected<FCacheError>
		{ return std::unexpected(FCacheError{ECacheError::StorageFailure, std::move(Message)}); }
	}
	auto OpenCacheStorageLease(const std::filesystem::path& Root, bool Exclusive)
		-> std::expected<std::shared_ptr<FCacheStorageLease>, FCacheError>
	{
		std::error_code Error;
		const auto Absolute = std::filesystem::absolute(Root, Error);
		if (Error || Root.empty() || Absolute == Absolute.root_path()) return Failure("Invalid cache root for ownership.");
		if (!Exclusive) std::filesystem::create_directories(Absolute, Error);
		if (Error) return Failure("Cannot create cache ownership directory: " + Error.message());
		const auto Canonical = std::filesystem::canonical(Absolute, Error);
		if (Error || Canonical == Canonical.root_path() || !std::filesystem::is_directory(Canonical, Error)) return Failure("Cache root must be an existing directory.");
		const auto LockPath = Canonical / CacheLockName;
		const auto Status = std::filesystem::symlink_status(LockPath, Error);
		if (!Error && std::filesystem::exists(Status) && !std::filesystem::is_regular_file(Status))
			return Failure("Cache ownership path must be a regular file, not a link.");
		if (Error && Error != std::errc::no_such_file_or_directory) return Failure("Cannot inspect cache ownership file.");
		auto Lease = std::make_shared<FCacheStorageLease>(); Lease->Root = Canonical;
#if defined(_WIN32)
		HANDLE File = CreateFileW(LockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (File == INVALID_HANDLE_VALUE) return Failure("Cannot open cache ownership file.");
		Lease->Handle = reinterpret_cast<intptr_t>(File);
		if (GetFileAttributesW(LockPath.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return Failure("Cache ownership file is a reparse point.");
		OVERLAPPED Position{};
		if (!LockFileEx(File, LOCKFILE_FAIL_IMMEDIATELY | (Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &Position))
			return Failure("Cache root is in use; offline maintenance requires all cache users to stop.");
#else
		const int File = open(LockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (File < 0) return Failure("Cannot open cache ownership file: " + std::generic_category().message(errno));
		Lease->Handle = File;
		if (flock(File, (Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
			return Failure("Cache root is in use; offline maintenance requires all cache users to stop.");
#endif
		return Lease;
	}
	FCacheStorageLease::~FCacheStorageLease()
	{
		if (Handle == -1) return;
#if defined(_WIN32)
		CloseHandle(reinterpret_cast<HANDLE>(Handle));
#else
		close(int(Handle));
#endif
	}
	auto AcquireCacheStorageOwnership() -> std::expected<std::shared_ptr<FCacheStorageLease>, FCacheError>
	{
		const auto Configured = std::filesystem::path(FPaths::DerivedDataCacheDir());
		std::error_code Error;
		const auto Root = std::filesystem::weakly_canonical(Configured, Error);
		if (Error) return Failure("Cannot resolve configured cache root.");
		std::lock_guard Lock(OwnershipMutex);
		if (ProcessLease && ProcessLease->Root == Root && std::filesystem::exists(Root / CacheLockName, Error) && !Error) return ProcessLease;
		auto Lease = OpenCacheStorageLease(Root, false);
		if (Lease) ProcessLease = *Lease;
		return Lease;
	}
	auto ReleaseCacheStorageOwnership() -> void
	{
		std::lock_guard Lock(OwnershipMutex); ProcessLease.reset();
	}
}
