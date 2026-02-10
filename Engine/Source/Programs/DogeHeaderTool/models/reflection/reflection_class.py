import os
import utils.json_utils

# ReflectionClassInfo represents the extracted information about a class from the C++ code.
class ReflectionClassInfo:
    def __init__(self, name: str, parent: str = None):
        self.name = name
        self.parent = parent