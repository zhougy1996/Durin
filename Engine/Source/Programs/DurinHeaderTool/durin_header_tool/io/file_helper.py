from dataclasses import dataclass, field
from pathlib import Path
import logging
import hashlib
import os
import tempfile

@dataclass
class LightFileFingerprint:
    timestamp: float
    file_size: int

@dataclass
class FileFingerprint:
    # Timestamp and size are only a cheap guard for reusing the stored hash.
    # Once a hash is available, content identity is defined by the hash alone.
    timestamp: float = field(default=0.0, compare=False)
    file_size: int = field(default=0, compare=False)
    md5: str = ""

def calc_md5(file_path: Path, chunk_size: int = 8192) -> str:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    try:
        hash_obj = hashlib.md5()
        with open(file_path, "rb") as f:
            while chunk := f.read(chunk_size):
                hash_obj.update(chunk)
        return hash_obj.hexdigest()
    except (PermissionError, OSError) as e:
        raise IOError(f"Error reading file {file_path}: {e}")
    
def get_light_file_fingerprint(file_path: Path) -> LightFileFingerprint:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    try:
        stat = file_path.stat()
        timestamp = stat.st_mtime
        file_size = stat.st_size
        return LightFileFingerprint(timestamp=timestamp, file_size=file_size)
    except (PermissionError, OSError) as e:
        raise IOError(f"Error accessing file {file_path}: {e}")

def get_file_fingerprint_with_old_cache(file_path: Path, old_fingerprint: FileFingerprint) -> FileFingerprint:
    if not file_path.is_file():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    
    try:
        stat = file_path.stat()
        timestamp = stat.st_mtime
        file_size = stat.st_size
        
        if old_fingerprint and timestamp == old_fingerprint.timestamp and file_size == old_fingerprint.file_size:
            return old_fingerprint
        
        md5_hash = calc_md5(file_path)
        return FileFingerprint(timestamp=timestamp, file_size=file_size, md5=md5_hash)
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
