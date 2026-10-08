#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin::Editor
{
	// Feature-owned settings content; callbacks run on the editor game thread.
	struct FEditorSettingsPage
	{
		std::string Id;
		std::string Label;
		std::string Keywords;
		std::string Description;
		bool bProject = false;
		std::function<void(std::string&)> Draw;
		std::function<bool()> Reset;
		std::function<void()> OnOpen;
	};

	// Owns a page registration for exactly the lifetime of its feature UI.
	class DURINED_API FEditorSettingsPageRegistration
	{
	public:
		FEditorSettingsPageRegistration() = default;
		explicit FEditorSettingsPageRegistration(FEditorSettingsPage Page);
		~FEditorSettingsPageRegistration();
		FEditorSettingsPageRegistration(const FEditorSettingsPageRegistration&) = delete;
		auto operator=(const FEditorSettingsPageRegistration&) -> FEditorSettingsPageRegistration& = delete;
		FEditorSettingsPageRegistration(FEditorSettingsPageRegistration&& Other) noexcept;
		auto operator=(FEditorSettingsPageRegistration&& Other) noexcept -> FEditorSettingsPageRegistration&;
		auto IsValid() const -> bool { return Handle != 0; }
	private:
		uint64 Handle = 0;
	};

	// Registration and open requests are game-thread-only and independent of MainFrame.
	class DURINED_API FEditorSettingsRegistry
	{
	public:
		static auto Get() -> FEditorSettingsRegistry&;
		auto GetPages() const -> std::vector<FEditorSettingsPage>;
		auto RequestOpen(std::string PageId = {}) -> void;
		auto TakeOpenRequest() -> std::optional<std::string>;
	private:
		friend class FEditorSettingsPageRegistration;
		auto Register(FEditorSettingsPage Page) -> uint64;
		auto Unregister(uint64 Handle) -> void;
		struct FEntry { uint64 Handle; FEditorSettingsPage Page; };
		std::vector<FEntry> Pages;
		uint64 NextHandle = 1;
		std::optional<std::string> OpenRequest;
	};
}
