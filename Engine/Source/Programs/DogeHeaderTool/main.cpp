#include "DogeHeaderTool.h"

int main()
{
	std::string TestHeaderFile = "DHTTestHeader.h";

	DHT::DogeHeaderTool DHT;

	DHT.Process(TestHeaderFile);
}