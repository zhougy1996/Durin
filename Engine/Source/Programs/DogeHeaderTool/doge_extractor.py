class BaseExtractor:
    def match(self, cursor) -> bool:
        pass

    def extract(self, cursor, ctx):
        pass


class ClassExtractor(BaseExtractor):
    def match(self, cursor) -> bool:
        return cursor.kind == CursorKind.CLASS_DECL and cursor.is_definition()

    def extract(self, cursor, ctx):