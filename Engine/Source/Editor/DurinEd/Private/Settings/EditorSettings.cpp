#include "Settings/EditorSettings.h"

namespace Durin::Editor
{
	auto FEditorSettingsRegistry::Get() -> FEditorSettingsRegistry&
	{
		static FEditorSettingsRegistry Registry;
		return Registry;
	}

	auto FEditorSettingsRegistry::Register(FEditorSettingsPage Page) -> uint64
	{
		if (Page.Id.empty() || Page.Label.empty() || !Page.Draw
			|| std::ranges::any_of(Pages, [&](const FEntry& Entry) { return Entry.Page.Id == Page.Id; }))
			return 0;
		const uint64 Handle = NextHandle++;
		Pages.push_back({Handle, std::move(Page)});
		return Handle;
	}

	auto FEditorSettingsRegistry::Unregister(uint64 Handle) -> void
	{
		std::erase_if(Pages, [Handle](const FEntry& Entry) { return Entry.Handle == Handle; });
	}

	auto FEditorSettingsRegistry::GetPages() const -> std::vector<FEditorSettingsPage>
	{
		std::vector<FEditorSettingsPage> Result;
		for (const auto& Entry : Pages) Result.push_back(Entry.Page);
		return Result;
	}

	auto FEditorSettingsRegistry::RequestOpen(std::string PageId) -> void
	{
		OpenRequest = std::move(PageId);
	}

	auto FEditorSettingsRegistry::TakeOpenRequest() -> std::optional<std::string>
	{
		return std::exchange(OpenRequest, std::nullopt);
	}

	FEditorSettingsPageRegistration::FEditorSettingsPageRegistration(FEditorSettingsPage Page)
		: Handle(FEditorSettingsRegistry::Get().Register(std::move(Page))) {}
	FEditorSettingsPageRegistration::~FEditorSettingsPageRegistration()
	{
		FEditorSettingsRegistry::Get().Unregister(Handle);
	}
	FEditorSettingsPageRegistration::FEditorSettingsPageRegistration(FEditorSettingsPageRegistration&& Other) noexcept
		: Handle(std::exchange(Other.Handle, 0)) {}
	auto FEditorSettingsPageRegistration::operator=(FEditorSettingsPageRegistration&& Other) noexcept
		-> FEditorSettingsPageRegistration&
	{
		if (this != &Other)
		{
			FEditorSettingsRegistry::Get().Unregister(Handle);
			Handle = std::exchange(Other.Handle, 0);
		}
		return *this;
	}
}
