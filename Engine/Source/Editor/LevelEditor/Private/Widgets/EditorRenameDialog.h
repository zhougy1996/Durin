#pragma once

namespace Durin
{
	// Reports whether a rename dialog is idle, accepted, or cancelled.
	enum class EEditorRenameDialogResult : uint8
	{
		None,
		Renamed,
		Cancelled,
	};

	// Owns modal rename input and validates the proposed asset name.
	class FEditorRenameDialog
	{
	public:
		using FCommit = std::function<std::string(std::string_view)>;

		auto Open(std::string_view InitialName) -> void;
		auto Cancel() -> void;
		auto Draw(const char* PopupTitle, std::string_view CurrentName, const FCommit& Commit) -> EEditorRenameDialogResult;

	private:
		std::array<char, 128> NameBuffer{};
		std::string ValidationError;
		bool bRequestOpen = false;
		bool bActive = false;
	};
} // namespace Durin
