#include "Shader/ShaderDiagnostics.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	auto FShaderCompilerOutput::GetRetainedSize() const -> uint64
	{
		uint64 Bytes = sizeof(*this) + CompiledShaders.capacity() * sizeof(FShaderCode);
		Bytes += Errors.capacity() * sizeof(FShaderCompilerError);
		for (const auto& Error : Errors) Bytes += Error.ErrorVirtualFilePath.capacity() + Error.ErrorLineString.capacity()
			+ Error.StrippedErrorMessage.capacity() + Error.HighlightedLine.capacity() + Error.HighlightedLineMarker.capacity();
		for (const auto& Shader : CompiledShaders)
		{
			Bytes += Shader.Code.GetRetainedCapacityBytes() + Shader.PlatformData.GetRetainedCapacityBytes()
				+ Shader.Reflection.ResourceBindings.capacity() * sizeof(FShaderResourceBinding)
				+ Shader.Reflection.PushConstantRanges.capacity() * sizeof(FPushConstantRange)
				+ Shader.SourceEntryPoint.capacity() + Shader.BinaryEntryPoint.capacity() + Shader.DebugName.capacity();
			for (const auto& Binding : Shader.Reflection.ResourceBindings) Bytes += Binding.Name.capacity();
		}
		return Bytes;
	}

	auto FShaderCompilerError::ExtractSourceLocation() -> bool
	{
		const auto Close = StrippedErrorMessage.find("):");
		if (Close == std::string::npos) return false;
		const auto Open = StrippedErrorMessage.rfind('(', Close);
		if (Open == std::string::npos || Open == 0 || Open + 1 == Close) return false;
		const auto Line = std::string_view(StrippedErrorMessage).substr(Open + 1, Close - Open - 1);
		if (!std::ranges::all_of(Line, [](char Value) { return (Value >= '0' && Value <= '9') || Value == ',' || Value == ' '; })) return false;
		ErrorVirtualFilePath = StrippedErrorMessage.substr(0, Open); ErrorLineString = Line;
		StrippedErrorMessage.erase(0, Close + 2);
		if (StrippedErrorMessage.starts_with(' ')) StrippedErrorMessage.erase(0, 1);
		return true;
	}

	auto FShaderCompilerOutput::AddDiagnostics(std::string_view Messages) -> void
	{
		while (!Messages.empty() && Errors.size() < ShaderCompilerMaxDiagnostics) {
			const auto End = Messages.find('\n');
			auto Line = Messages.substr(0, End);
			if (Line.ends_with('\r')) Line.remove_suffix(1);
			if (!Line.empty()) Errors.emplace_back(std::string(Line));
			if (End == std::string_view::npos) break;
			Messages.remove_prefix(End + 1);
		}
	}

	auto FShaderCompilerError::GetErrorString() const -> std::string
	{
		if (ErrorVirtualFilePath.empty()) return StrippedErrorMessage;
		return ErrorVirtualFilePath + (ErrorLineString.empty() ? "" : "(" + ErrorLineString + ")") + ": " + StrippedErrorMessage;
	}

	auto FormatShaderCompilerErrors(std::span<const FShaderCompilerError> Errors) -> std::string
	{
		std::string Text;
		for (const auto& Error : Errors) {
			if (!Text.empty()) Text += '\n';
			Text += Error.GetErrorString();
			if (Text.size() >= ShaderBuildDiagnosticMaxBytes) { Text.resize(ShaderBuildDiagnosticMaxBytes); break; }
		}
		return Text;
	}

}
