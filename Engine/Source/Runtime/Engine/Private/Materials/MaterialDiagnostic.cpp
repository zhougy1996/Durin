#include "Materials/MaterialTypes.h"
#include "Materials/MaterialCompiledLayout.h"
#include "Materials/MaterialProgramTypes.h"

namespace Durin
{
	auto FormatMaterialProgramDiagnostics(std::span<const FMaterialProgramDiagnostic> Diagnostics,
		std::string_view Fallback) -> std::string
	{
		constexpr size_t MaximumBytes = 4096;
		std::string Text;
		for (const auto& Diagnostic : Diagnostics)
		{
			if (Text.size() >= MaximumBytes) break;
			const auto& Message = Diagnostic.Message;
			if (Message.empty()) continue;
			if (!Text.empty()) Text += '\n';
			Text.append(Message, 0, MaximumBytes - Text.size());
		}
		return Text.empty() ? std::string(Fallback.substr(0, MaximumBytes)) : Text;
	}

	auto GetMaterialParameterErrorText(EMaterialParameterError Error) -> std::string_view
	{
		switch (Error)
		{
		case EMaterialParameterError::None: return "";
		case EMaterialParameterError::TooManyDefinitions: return "Material declaration count exceeds its bound.";
		case EMaterialParameterError::InvalidId: return "Material declaration GUID is invalid.";
		case EMaterialParameterError::DuplicateId: return "Material declaration GUID is duplicated.";
		case EMaterialParameterError::InvalidName: return "Material declaration name must not be None.";
		case EMaterialParameterError::DuplicateName: return "Material declaration name is already occupied.";
		case EMaterialParameterError::InvalidText: return "Material declaration text is invalid or exceeds its bound.";
		case EMaterialParameterError::InvalidType: return "Material declaration type is invalid.";
		case EMaterialParameterError::InvalidDefault: return "Material declaration default must be finite.";
		case EMaterialParameterError::InvalidMetadata: return "Material declaration metadata is invalid.";
		}
		return "Unknown material parameter error.";
	}


	auto GetMaterialLayoutErrorText(EMaterialLayoutError Error) -> std::string_view
	{
		switch (Error)
		{
		case EMaterialLayoutError::None: return {};
		case EMaterialLayoutError::InvalidParameter: return "The material layout contains an invalid parameter identity.";
		case EMaterialLayoutError::DuplicateParameter: return "The material layout contains a duplicate parameter identity.";
		case EMaterialLayoutError::InvalidType: return "The material layout contains an unsupported parameter type.";
		case EMaterialLayoutError::ResourceLimit: return "The material layout exceeds the target uniform or descriptor budget.";
		case EMaterialLayoutError::InvalidField: return "The material layout fields, counts or offsets do not match its deterministic schema.";
		case EMaterialLayoutError::InvalidIdentity: return "The material layout version or identity does not match its schema.";
		case EMaterialLayoutError::InvalidReflection: return "The compiled material reflection does not match the layout and pass contract.";
		}
		return "Unknown material layout error.";
	}

}
