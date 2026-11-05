#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"

namespace Durin::Editor
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

	// Identifies an open editor document within the current toolkit session.
	struct FDocumentId
	{
		uint64 Value = 0;

		auto IsValid() const -> bool { return Value != 0; }
		auto operator==(const FDocumentId&) const -> bool = default;
	};

	// Stores host-visible state for one open editor document tab.
	struct FEditorDocument
	{
		FDocumentId Id;
		FAssetEditorToolkitId ToolkitId;
		std::string DocumentKey;
		std::string ResourceId;
		std::string Label;
		bool bDirty = false;
		bool bClosable = true;
	};

	// Describes a request to open or activate a toolkit document.
	struct FDocumentRequest
	{
		FAssetEditorToolkitId ToolkitId;
		std::string DocumentKey;
		std::string ResourceId;
		std::string Label;
		bool bClosable = true;
	};

	// Reports whether a toolkit rejected, completed, or deferred an open request.
	enum class EDocumentOpenResult : uint8
	{
		Rejected,
		Opened,
		Deferred,
	};

	// Reports whether a close request completed, needs confirmation, was rejected, or was cancelled.
	enum class EDocumentCloseResult : uint8
	{
		Rejected,
		Closed,
		PendingConfirmation,
		Cancelled,
	};

	// Selects how the host resolves a pending dirty-document close.
	enum class EDocumentCloseResponse : uint8
	{
		Save,
		Discard,
		Cancel,
	};

	// Selects whether an asset editor shares one document or opens per resource.
	enum class EDocumentPolicy : uint8
	{
		Singleton,
		PerResource,
	};

	// Suggests where a toolkit should dock when first hosted.
	enum class EToolkitHostDockPreference : uint8
	{
		None,
		Center,
	};

	// Defines host-facing metadata and singleton-document policy for a toolkit.
	struct FAssetEditorToolkitDescriptor
	{
		FAssetEditorToolkitId ToolkitId;
		std::string DisplayName;
		std::string RootKey;
		bool bShowInWindowMenu = true;
		bool bOpenByDefault = false;
		EToolkitHostDockPreference DefaultHostDockPreference = EToolkitHostDockPreference::Center;
		std::string SingletonDocumentKey;
		std::string SingletonDocumentLabel;
		bool bSingletonDocumentClosable = true;

		auto HasSingletonDocument() const -> bool
		{
			return !SingletonDocumentKey.empty() && !SingletonDocumentLabel.empty();
		}
	};

	// Maps an asset class to the toolkit and document policy that edits it.
	struct FAssetEditorRegistration
	{
		std::string AssetClassName;
		FAssetEditorToolkitId ToolkitId;
		EDocumentPolicy DocumentPolicy = EDocumentPolicy::PerResource;
		std::string SingletonDocumentKey;
		std::string SingletonLabel;
		bool bClosable = true;
	};
}
