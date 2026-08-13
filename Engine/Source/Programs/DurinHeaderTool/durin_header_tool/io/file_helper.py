from dataclasses import dataclass, field
from pathlib import Path
import logging
import hashlib
import os
import tempfile

@dataclass(eq=False)
class FileFingerprint:
    # Timestamp and size are only a cheap guard for reusing the stored hash.
    # Once a hash is available, content identity is defined by the hash alone.
    timestamp: float = field(default=0.0, compare=False)
    file_size: int = field(default=0, compare=False)
    sha256: str = ""
    legacy_md5: str = field(default="", compare=False, repr=False)

    def __eq__(self, other: object) -> bool:
        return (
            isinstance(other, FileFingerprint)
            and bool(self.sha256)
            and bool(other.sha256)
            and self.sha256 == other.sha256
        )

    def to_json(self) -> dict[str, object]:
        if len(self.sha256) != 64 or any(character not in "0123456789abcdef" for character in self.sha256):
            raise ValueError("File fingerprint SHA256 must be a lowercase SHA-256 digest.")
        return {
            "Timestamp": self.timestamp,
            "FileSize": self.file_size,
            "SHA256": self.sha256,
        }

    @classmethod
    def from_json(cls, data: object, *, allow_legacy_md5: bool = False) -> "FileFingerprint":
        if not isinstance(data, dict):
            raise ValueError("File fingerprint must be a JSON object.")
        if set(data) == {"Timestamp", "FileSize", "SHA256"}:
            timestamp = data["Timestamp"]
            file_size = data["FileSize"]
            digest = data["SHA256"]
            if not isinstance(timestamp, (int, float)) or isinstance(timestamp, bool):
                raise ValueError("File fingerprint Timestamp must be numeric.")
            if not isinstance(file_size, int) or isinstance(file_size, bool) or file_size < 0:
                raise ValueError("File fingerprint FileSize must be a non-negative integer.")
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                raise ValueError("File fingerprint SHA256 must be a lowercase SHA-256 digest.")
            return cls(timestamp=float(timestamp), file_size=file_size, sha256=digest)
        if allow_legacy_md5 and set(data) == {"Timestamp", "FileSize", "MD5"}:
            timestamp = data["Timestamp"]
            file_size = data["FileSize"]
            digest = data["MD5"]
            if not isinstance(timestamp, (int, float)) or isinstance(timestamp, bool):
                raise ValueError("Legacy file fingerprint Timestamp must be numeric.")
            if not isinstance(file_size, int) or isinstance(file_size, bool) or file_size < 0:
                raise ValueError("Legacy file fingerprint FileSize must be a non-negative integer.")
            if not isinstance(digest, str):
                raise ValueError("Legacy file fingerprint MD5 must be a string.")
            return cls(timestamp=float(timestamp), file_size=file_size, legacy_md5=digest)
        raise ValueError("File fingerprint has an invalid JSON object shape.")


def calc_sha256(file_path: Path, chunk_size: int = 8192) -> str:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    try:
        hash_obj = hashlib.sha256()
        with open(file_path, "rb") as f:
            while chunk := f.read(chunk_size):
                hash_obj.update(chunk)
        return hash_obj.hexdigest()
    except (PermissionError, OSError) as e:
        raise IOError(f"Error reading file {file_path}: {e}")
    
def get_file_fingerprint_with_old_cache(file_path: Path, old_fingerprint: FileFingerprint) -> FileFingerprint:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    
    try:
        stat = file_path.stat()
        timestamp = stat.st_mtime
        file_size = stat.st_size
        
        if (
            old_fingerprint
            and old_fingerprint.sha256
            and timestamp == old_fingerprint.timestamp
            and file_size == old_fingerprint.file_size
        ):
            return old_fingerprint
        
        return FileFingerprint(timestamp=timestamp, file_size=file_size, sha256=calc_sha256(file_path))
    except (PermissionError, OSError) as e:
        raise IOError(f"Error accessing file {file_path}: {e}")

def generate_file(file_path: Path, content: str, compare: bool = True) -> None:
    file_path.parent.mkdir(parents=True, exist_ok=True)
    # Only write file if content has changed
    if compare and file_path.exists():
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                existing_content = f.read()
                if existing_content == content:
                    logging.debug(f"No changes detected for {file_path}. Skipping write.")
                    return
        except UnicodeError:
            # Generated files are UTF-8 text. Invalid existing bytes are a
            # damaged cache entry and should be replaced, not block recovery.
            pass
    # Write beside the destination so os.replace can commit the complete file
    # atomically. A killed DHT process must never leave a truncated generated file.
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=file_path.parent,
            prefix=f".{file_path.name}.",
            suffix=".tmp",
            delete=False,
        ) as temp_file:
            temp_path = Path(temp_file.name)
            temp_file.write(content)
            temp_file.flush()
            os.fsync(temp_file.fileno())
        os.replace(temp_path, file_path)
        temp_path = None
    finally:
        if temp_path is not None:
            temp_path.unlink(missing_ok=True)
