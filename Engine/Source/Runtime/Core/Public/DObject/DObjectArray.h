#pragma once

class FDObjectArray
{
public:

	void Add(DObject* ObjToAdd)
	{
		Objs.push_back(ObjToAdd);
	}

private:
	std::vector<DObject*> Objs;
};

extern CORE_API FDObjectArray GDObjectArray;