#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"

namespace Durin
{
	// Provides a stable string identity for one editor toolkit kind.
	class FAssetEditorToolkitId
	{
	public:
		FAssetEditorToolkitId() = default;
		explicit FAssetEditorToolkitId(std::string Value)
			: Value(std::move(Value))
		{
		}

		auto GetValue() const -> std::string_view { return Value; }
		auto IsValid() const -> bool { return !Value.empty(); }
		auto operator==(const FAssetEditorToolkitId&) const -> bool = default;

	private:
		std::string Value;
	};

	// Reports whether a close request completed, needs confirmation, was rejected, or was cancelled.
	enum class EAssetEditorCloseResult : uint8
	{
		Rejected,
		Closed,
		PendingConfirmation,
		Cancelled,
	};

	// Selects how the host resolves a pending asset editor close.
	enum class EAssetEditorCloseResponse : uint8
	{
		Save,
		Discard,
		Cancel,
	};

}
