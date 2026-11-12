#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeTransfer.h"
#include "Operations/ContentBrowserPaths.h"
#include "Misc/Paths.h"
#include "Misc/StringHelper.h"

namespace Durin::ContentBrowser::Private::TransferPaths
{
	inline auto Same(const std::filesystem::path& A, const std::filesystem::path& B) -> bool
	{
		std::filesystem::path Relative;
		return FPaths::TryMakeLexicalRelativePath(A, B, Relative) && Relative.empty();
	}
	inline auto Claim(const std::filesystem::path& Path) -> std::string
	{
		auto Key = std::filesystem::absolute(Path).lexically_normal().generic_string();
#ifdef _WIN32
		Key = StringUtils::FoldAscii(Key);
#endif
		return "NativeFilesystem:" + Key;
	}
	inline auto SafePath(const std::filesystem::path& Path, bool Writable, std::string* Error) -> bool
	{
		const auto Mount = FContentBrowserPaths::ResolveMountPath(Path.generic_string());
		if (!Mount || (Writable && !Mount.Mount->bContentWritable))
		{ if (Error) *Error = "Transfer requires an admitted content mount."; return false; }
		std::filesystem::path Relative;
		if (!FPaths::TryMakeLexicalRelativePath(Path, Mount.Mount->GetContentDir(), Relative)) return false;
		auto Current = Mount.Mount->GetContentDir();
		auto Inspect = [&](const std::filesystem::path& Candidate) {
			std::error_code Ec;
			const auto Status = std::filesystem::symlink_status(Candidate, Ec);
			if (Ec == std::errc::no_such_file_or_directory) return true;
			if (Ec || std::filesystem::is_symlink(Status)) return false;
#ifdef _WIN32
			if (std::filesystem::exists(Status))
			{
				const DWORD Flags = GetFileAttributesW(Candidate.c_str());
				if (Flags == INVALID_FILE_ATTRIBUTES || (Flags & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
			}
#endif
			return true;
		};
		if (!Inspect(Current)) { if (Error) *Error = "Transfer cannot traverse a reparse point."; return false; }
		for (const auto& Component : Relative)
		{
			Current /= Component;
			if (!Inspect(Current)) { if (Error) *Error = "Transfer cannot traverse a reparse point or unreadable path."; return false; }
		}
		return true;
	}
	inline auto Admitted(const std::vector<FNativeTransferInput>& Inputs, std::string* Error) -> bool
	{
		for (const auto& Input : Inputs)
			if (Input.Policy->CanMutateContent && !Input.Policy->CanMutateContent())
			{ if (Error) *Error = "Transfer admission was withdrawn."; return false; }
		return true;
	}
	inline auto KeyAt(FContentBrowserItemContributionKey Key, std::string_view VirtualPath, bool Directory = false) -> FContentBrowserItemContributionKey
	{
		Key.Item = FContentBrowserItemDataKey(Directory ? EContentBrowserItemFlags::Type_Folder : EContentBrowserItemFlags::Type_File, VirtualPath);
		return Key;
	}
	inline auto Publish(const std::vector<FNativeTransferInput>& Inputs, FContentChangeBatch Changes) -> void
	{
		if (Inputs.empty() || (!Changes.bFullRefresh && Changes.Changes.empty())) return;
		const auto& Host = *Inputs.front().Policy;
		if (Host.NotifyScopedContentMutation) Host.NotifyScopedContentMutation(std::move(Changes));
		else if (Host.NotifyMountedContentMutation) Host.NotifyMountedContentMutation();
	}
	inline auto Rejected(std::vector<FNativeTransferInput> Inputs, std::string Error) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		std::vector<FContentBrowserItemContributionKey> Keys;
		for (const auto& Input : Inputs) Keys.push_back(Input.Identity);
		return std::make_unique<FContentBrowserItemDataTransferContext>(std::move(Keys), std::vector<FContentBrowserTransferDestination>{},
			[Error = std::move(Error)](std::string* Out) { if (Out) *Out = Error; return false; }, FContentBrowserItemDataTransferContext::FExecute{});
	}
}
