#pragma once

namespace Durin::Testing
{
	template <typename T>
	struct TFactoryImportResult
	{
		bool bSucceeded = false;
		std::string Message;
		T* Asset = nullptr;

		explicit operator bool() const { return bSucceeded; }
	};
}
