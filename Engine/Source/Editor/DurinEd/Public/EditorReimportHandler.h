#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	class DObject;

	// Describes which mutation requests one handler can service for a loaded object.
	struct FReimportCapabilities
	{
		bool bCanReimport = false;
		bool bCanReimportFromFile = false;
		std::string Diagnostic;
	};

	// Type-level actions for menus. Per-asset source availability remains unknown.
	struct FReimportActions
	{
		bool bSupportsReimport = false;
		bool bSupportsReimportFromFile = false;
	};

	// Type-owned source slot description; dialogs are presented only during a command.
	struct FReimportSourceFileDialog
	{
		std::string Title;
		std::string FilterName;
		std::string Pattern;
	};

	// Identifies one terminal reimport outcome independently from persistence.
	enum class EReimportStatus : uint8
	{
		Unsupported,
		MissingSource,
		SourceOrBuildFailure,
		Succeeded,
		PersistenceFailure,
	};

	struct FReimportResult
	{
		EReimportStatus Status = EReimportStatus::Unsupported;
		std::string Message;

		auto Succeeded() const -> bool
		{
			return Status == EReimportStatus::Succeeded;
		}
		explicit operator bool() const { return Succeeded(); }
	};

	// Accepted requests invoke their completion exactly once on the game thread.
	using FReimportCompletion = std::function<void(FReimportResult)>;

	// Self-registering editor extension point for mutating an object from source files.
	class FReimportHandler
	{
	public:
		DURINED_API FReimportHandler();
		FReimportHandler(const FReimportHandler&) = delete;
		FReimportHandler(FReimportHandler&&) = delete;
		DURINED_API virtual ~FReimportHandler();

		auto operator=(const FReimportHandler&) -> FReimportHandler& = delete;
		auto operator=(FReimportHandler&&) -> FReimportHandler& = delete;

		virtual auto GetPriority() const -> int32 { return 0; }
		// Metadata-only candidate actions. Never load assets or inspect source files here.
		// Source availability is unknown until command execution validates the object.
		virtual auto QueryReimportActions(std::string_view AssetClassName) const
			-> FReimportActions { return {}; }
		virtual auto GetSourceFileDialogs(const DObject& Object) const
			-> std::vector<FReimportSourceFileDialog> { return {}; }
		virtual auto GetReimportCapabilities(
			const DObject& Object) const -> FReimportCapabilities = 0;
		virtual auto Reimport(
			DObject& Object,
			FReimportCompletion Completion) const -> void = 0;
		virtual auto ReimportFromFiles(
			DObject& Object,
			std::span<const std::string> Filenames,
			FReimportCompletion Completion) const -> void = 0;
	};

	struct FReimportOptions
	{
		bool bSave = true;
	};

	// Selects registered handlers and coordinates terminal notification and persistence.
	class FReimportManager
	{
	public:
		DURINED_API static auto RegisterHandler(FReimportHandler& Handler) -> void;
		DURINED_API static auto UnregisterHandler(FReimportHandler& Handler) -> void;

		// For UI queries: reads class metadata only; results are candidates, not validation.
		DURINED_API static auto QueryReimportActions(std::string_view AssetClassName)
			-> FReimportActions;
		// Command-only: validates the loaded object and delegates source layout to its handler.
		DURINED_API static auto GetSourceFileDialogs(const DObject& Object,
			std::string& OutError) -> std::vector<FReimportSourceFileDialog>;
		DURINED_API static auto GetCapabilities(const DObject& Object)
			-> FReimportCapabilities;
		DURINED_API static auto Reimport(
			DObject& Object,
			const FReimportOptions& Options,
			FReimportCompletion Completion) -> void;
		DURINED_API static auto ReimportFromFiles(
			DObject& Object,
			std::span<const std::string> Filenames,
			const FReimportOptions& Options,
			FReimportCompletion Completion) -> void;
	};
}
