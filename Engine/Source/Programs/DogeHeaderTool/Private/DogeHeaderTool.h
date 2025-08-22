#pragma once

namespace DHT
{
	class DHTParser;
	class DHTCodeGenerator;

	class DogeHeaderTool
	{
	public:
		DogeHeaderTool();

		~DogeHeaderTool();

		bool Process(const std::string& Filename);

	private:
		DHTParser* Parser_;

		DHTCodeGenerator* Generator_;
	};

}