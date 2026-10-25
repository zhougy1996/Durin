#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"

namespace Durin
{
	inline constexpr size_t ShaderBuildDiagnosticMaxBytes = 4096;
	inline constexpr size_t ShaderCompilerMaxDiagnostics = 128;

	// One compiler error or warning, with optional source navigation information.
	struct FShaderCompilerError
	{
		std::string ErrorVirtualFilePath;
		std::string ErrorLineString;
		std::string StrippedErrorMessage;
		std::string HighlightedLine;
		std::string HighlightedLineMarker;

		FShaderCompilerError() = default;
		FShaderCompilerError(std::string Message) : StrippedErrorMessage(std::move(Message))
		{ StrippedErrorMessage.resize(std::min(StrippedErrorMessage.size(), ShaderBuildDiagnosticMaxBytes)); ExtractSourceLocation(); }
		RENDERCORE_API auto ExtractSourceLocation() -> bool;
		RENDERCORE_API auto GetErrorString() const -> std::string;
	};

	RENDERCORE_API auto FormatShaderCompilerErrors(std::span<const FShaderCompilerError> Errors) -> std::string;

	// Validation operations return their diagnostic directly; compilation owns an error array.
	using FShaderOperationResult = std::expected<void, std::string>;
	// Compiler outputs are terminal; Pending is used by nonblocking ShaderMap resolution.
	enum class EShaderBuildStatus : uint8 { Ok, Error, Canceled, Pending };

	// Non-compilation operations may complete or cancel without producing shader code.
	struct FShaderCompletion
	{
		EShaderBuildStatus Status = EShaderBuildStatus::Ok;
		std::string Error;
		FShaderCompletion() = default;
		FShaderCompletion(EShaderBuildStatus InStatus, std::string InError = {})
			: Status(InStatus), Error(InStatus == EShaderBuildStatus::Error ? std::move(InError) : std::string{})
		{ Error.resize(std::min(Error.size(), ShaderBuildDiagnosticMaxBytes)); }
		FShaderCompletion(const std::unexpected<std::string>& Failure) : FShaderCompletion(EShaderBuildStatus::Error, Failure.error()) {}
		FShaderCompletion(const FShaderOperationResult& Result)
			: FShaderCompletion(Result ? FShaderCompletion{} : FShaderCompletion(std::unexpected(Result.error()))) {}
		explicit operator bool() const { return Status == EShaderBuildStatus::Ok; }
		static auto Canceled() -> FShaderCompletion { return {EShaderBuildStatus::Canceled}; }
		static auto Pending() -> FShaderCompletion { return {EShaderBuildStatus::Pending}; }
		static auto Failure(std::string Text) -> FShaderCompletion { return {EShaderBuildStatus::Error, std::move(Text)}; }
	};

	template<class T>
	struct TShaderValueResult : FShaderCompletion
	{
		std::optional<T> Value;
		TShaderValueResult() : FShaderCompletion(EShaderBuildStatus::Error) {}
		TShaderValueResult(T Product) : Value(std::move(Product)) {}
		TShaderValueResult(FShaderCompletion Completion) : FShaderCompletion(std::move(Completion)) { require(Status != EShaderBuildStatus::Ok); }
		TShaderValueResult(const std::unexpected<std::string>& Error) : FShaderCompletion(Error) {}
		auto operator*() -> T& { return *Value; }
		auto operator*() const -> const T& { return *Value; }
		auto operator->() -> T* { return &*Value; }
		auto operator->() const -> const T* { return &*Value; }
	};
}
