#pragma once

#include <spawn.h>
#include <unistd.h>

namespace Durin::Testing::ApplicationHost
{
	class FScopedFileDescriptor
	{
	public:
		FScopedFileDescriptor() = default;
		explicit FScopedFileDescriptor(int File) : File(File) {}
		~FScopedFileDescriptor() { Reset(); }

		FScopedFileDescriptor(const FScopedFileDescriptor&) = delete;
		auto operator=(const FScopedFileDescriptor&) -> FScopedFileDescriptor& = delete;

		FScopedFileDescriptor(FScopedFileDescriptor&& Other) noexcept
			: File(Other.Release())
		{
		}

		auto operator=(FScopedFileDescriptor&& Other) noexcept
			-> FScopedFileDescriptor&
		{
			if (this != &Other) Reset(Other.Release());
			return *this;
		}

		auto Get() const -> int { return File; }
		auto IsValid() const -> bool { return File >= 0; }

		auto Release() -> int
		{
			const int Released = File;
			File = -1;
			return Released;
		}

		auto Reset(int NewFile = -1) -> void
		{
			if (File >= 0) close(File);
			File = NewFile;
		}

	private:
		int File = -1;
	};

	class FScopedSpawnFileActions
	{
	public:
		FScopedSpawnFileActions()
			: InitializationResult(posix_spawn_file_actions_init(&Actions))
		{
		}

		~FScopedSpawnFileActions()
		{
			if (InitializationResult == 0) posix_spawn_file_actions_destroy(&Actions);
		}

		FScopedSpawnFileActions(const FScopedSpawnFileActions&) = delete;
		auto operator=(const FScopedSpawnFileActions&)
			-> FScopedSpawnFileActions& = delete;

		auto GetInitializationResult() const -> int { return InitializationResult; }
		auto Get() -> posix_spawn_file_actions_t* { return &Actions; }

	private:
		posix_spawn_file_actions_t Actions{};
		int InitializationResult = 0;
	};
}
