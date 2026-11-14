#pragma once
#include "CoreMinimal.h"
#include "Misc/ContentChanges.h"
#include "Misc/ContentPathInspection.h"
#include "Misc/ContentMountPaths.h"
#include "Misc/Paths.h"
#include "Misc/FeedbackContext.h"

namespace Durin::ContentBrowser::Transfer
{
	using ContentStorage::TransferPaths::Same;
	using ContentStorage::TransferPaths::Claim;
	using ContentStorage::TransferPaths::SafePath;
	inline auto Admitted(const auto& Inputs, std::string* Error) -> bool
	{
		for (const auto& Input : Inputs)
			if (Input.Policy->CanMutateContent && !Input.Policy->CanMutateContent())
			{ if (Error) *Error = "Transfer admission was withdrawn."; return false; }
		return true;
	}
	template<typename T> auto Publish(const std::vector<T>& Inputs, FContentChangeBatch Changes) -> void
	{
		if (Inputs.empty() || (!Changes.bFullRefresh && Changes.Changes.empty())) return;
		const auto& Host = *Inputs.front().Policy;
		if (Host.NotifyContentMutation) Host.NotifyContentMutation(std::move(Changes));
	}
	template<typename T> auto ReportDiagnostic(const std::vector<T>& Inputs, ELogLevel Level, std::string_view Message) -> void
	{
		if (Message.empty()) return;
		const auto Notify = Inputs.empty() ? FFeedbackContext::FMessageSink{} : Inputs.front().Policy->ReportDiagnostic;
		if (Notify) Notify(Level, Message);
		else FFeedbackContext::Get().Serialize(Level, Message);
	}
	template<typename T> auto Rejected(const std::vector<T>& Inputs, std::string_view Error) -> bool
	{
		ReportDiagnostic(Inputs, ELogLevel::Error, Error);
		return false;
	}

	// Capability checks inspect only the selected entry and destination. Recursive
	// collision/companion planning remains an execution-time responsibility.
	template<typename T> auto CanTransfer(const T& Input, std::string_view Destination, bool Move, std::string* Error, bool AllowCrossMountMove = false) -> bool
	{
		using namespace ContentStorage::TransferPaths;
		if (Error) Error->clear();
		auto Reject = [&](std::string_view Message) { if (Error) *Error = Message; return false; };
		const std::string TargetDirectory(Destination);
		const std::filesystem::path Source(Input.Item.PhysicalPath);
		if (!Admitted(std::span{&Input, 1}, Error) || !SafePath(Source, Move, Error) || !SafePath(TargetDirectory, true, Error)) return false;
		const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Source.generic_string());
		const auto TargetMount = ContentStorage::FContentMountPaths::ResolveMountPath(TargetDirectory);
		if (!Mount || !TargetMount || Same(Source, Mount.Mount->GetContentDir())) return Reject("The mount root cannot be transferred.");
		if (Move && !AllowCrossMountMove && Mount.Mount != TargetMount.Mount) return Reject("Moves cannot cross mount boundaries.");
		std::error_code Ec;
		if (!std::filesystem::is_directory(TargetDirectory, Ec) || Ec) return Reject("The destination is not a directory.");
		const auto Target = std::filesystem::path(TargetDirectory) / Source.filename();
		if (!SafePath(Target, true, Error)) return false;
		const auto FinalMount = ContentStorage::FContentMountPaths::ResolveMountPath(Target.generic_string());
		if (Move && !AllowCrossMountMove && (!FinalMount || Mount.Mount != FinalMount.Mount)) return Reject("Moves cannot cross nested destination mount boundaries.");
		if (Same(Source, Target) || FPaths::IsLexicalDescendantPath(Target, Source, true)) return Reject("A destination cannot be inside the source.");
		const bool Folder = Input.Item.Kind == EContentBrowserItemKind::Folder;
		if (Folder ? !std::filesystem::is_directory(Source, Ec) : !std::filesystem::is_regular_file(Source, Ec)) return Reject("The source is unavailable or changed type.");
		if (Ec) return Reject("The source could not be inspected.");
		if (std::filesystem::exists(Target, Ec) && (!Folder || !std::filesystem::is_directory(Target, Ec))) return Reject("The destination is already occupied.");
		return !Ec && Admitted(std::span{&Input, 1}, Error);
	}
	template<typename T> auto CanRename(const T& Input, std::string* Error) -> bool
	{
		if (Error) Error->clear();
		const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Input.Item.PhysicalPath);
		if (!Mount || ContentStorage::TransferPaths::Same(Input.Item.PhysicalPath, Mount.Mount->GetContentDir()))
		{ if (Error) *Error = "The mount root cannot be renamed."; return false; }
		if (!Admitted(std::span{&Input, 1}, Error) || !SafePath(Input.Item.PhysicalPath, true, Error)) return false;
		std::error_code Ec;
		const bool Folder = Input.Item.Kind == EContentBrowserItemKind::Folder;
		const bool Exists = Folder ? std::filesystem::is_directory(Input.Item.PhysicalPath, Ec) : std::filesystem::is_regular_file(Input.Item.PhysicalPath, Ec);
		if (!Exists || Ec) { if (Error) *Error = "The source is unavailable or changed type."; return false; }
		return Admitted(std::span{&Input, 1}, Error);
	}
}
