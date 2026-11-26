#!/usr/bin/env python3
import argparse
import hashlib
from pathlib import Path
import struct
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "module"
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
MODVERSION = struct.Struct("<Q56s")
MODULE_NAMES = {"gzvm.ko": "gzvm", "gzvm-duchamp.ko": "gzvm_duchamp"}
# Both the factory driver and the validated build use this struct module size.
TARGET_MODULE_SIZE = 0x440


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def elf_sections(data):
    elf = ELF_HEADER.unpack_from(data)
    identification, file_type, machine = elf[:3]
    if identification[:6] != b"\x7fELF\x02\x01" or (file_type, machine) != (1, 183):
        raise ValueError("Expected an arm64 ELF relocatable module")

    section_offset, entry_size, entry_count, names_index = elf[6], *elf[11:14]
    if entry_size != SECTION_HEADER.size or not 0 < names_index < entry_count:
        raise ValueError("Invalid section table")

    sections = [
        SECTION_HEADER.unpack_from(data, section_offset + i * entry_size)
        for i in range(entry_count)
    ]

    def contents(section):
        offset, size = section[4:6]
        if offset + size > len(data):
            raise ValueError("Section outside module")
        return data[offset:offset + size]

    names = contents(sections[names_index])
    return {
        names[section[0]:].split(b"\0", 1)[0].decode("ascii"): contents(section)
        for section in sections
        if section[1] not in (0, 8)
    }


def module_metadata(data, name, release, symbol_crcs):
    sections = elf_sections(data)
    module_size = len(sections.get(".gnu.linkonce.this_module", b""))
    if module_size != TARGET_MODULE_SIZE:
        raise ValueError(
            f"{name} has struct module size {module_size:#x}, expected {TARGET_MODULE_SIZE:#x}"
        )
    metadata = dict(
        item.decode("ascii").split("=", 1)
        for item in sections[".modinfo"].split(b"\0")
        if b"=" in item
    )
    if metadata.get("name") != name or metadata["vermagic"].split()[0] != release:
        raise ValueError(f"Unexpected module name or vermagic for {name}")
    if "modversions" not in metadata["vermagic"].split():
        raise ValueError(f"{name} must be built with module versioning")

    versions = list(MODVERSION.iter_unpack(sections["__versions"]))
    if not versions:
        raise ValueError(f"No symbol CRCs for {name}")
    for crc, encoded in versions:
        symbol = encoded.split(b"\0", 1)[0].decode("ascii")
        if symbol_crcs.get(symbol) != crc:
            raise ValueError(f"{name} references a missing or incompatible symbol {symbol}")

    note = sections[".note.gnu.build-id"]
    if len(note) != 36 or struct.unpack_from("<III", note) != (4, 20, 3) or note[12:16] != b"GNU\0":
        raise ValueError(f"Unexpected build ID note for {name}")
    print(f"{name} uses {len(versions)} matching symbol CRCs")
    return sha256(data), sha256(note)


def main():
    parser = argparse.ArgumentParser(description="Build the duchamp KernelSU module ZIP")
    parser.add_argument("module_directory", type=Path)
    parser.add_argument("symvers", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    target = dict(line.split("=", 1) for line in (MODULE / "target.conf").read_text().splitlines())
    symvers = args.symvers.read_bytes()
    if sha256(symvers) != target["TARGET_SYMVERS_SHA"]:
        parser.error("Module.symvers does not match the tested kernel")
    symbol_crcs = {
        fields[1]: int(fields[0], 16)
        for line in symvers.decode("ascii").splitlines()
        if (fields := line.split())
    }

    contents = {}
    manifest = []
    for prefix, (filename, name) in zip(("DRIVER", "ENABLER"), MODULE_NAMES.items()):
        data = (args.module_directory / filename).read_bytes()
        file_digest, note_digest = module_metadata(
            data, name, target["TARGET_RELEASE"], symbol_crcs
        )
        manifest.extend([f"{prefix}_FILE_SHA={file_digest}", f"{prefix}_NOTE_SHA={note_digest}"])
        contents[f"lib/{filename}"] = data
    for filename in ("module.prop", "target.conf", "compat.sh", "customize.sh", "boot-completed.sh"):
        contents[filename] = (MODULE / filename).read_bytes().replace(b"\r\n", b"\n")
    contents["build.conf"] = ("\n".join(manifest) + "\n").encode("ascii")
    contents["skip_mount"] = b""
    contents["LICENSE"] = (ROOT / "LICENSE").read_bytes()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, "w", compression=ZIP_DEFLATED) as package:
        for name, data in sorted(contents.items()):
            entry = ZipInfo(name)
            entry.create_system = 3
            entry.external_attr = (0o100755 if name == "boot-completed.sh" else 0o100644) << 16
            package.writestr(entry, data, compress_type=ZIP_DEFLATED)
    print(args.output)


if __name__ == "__main__":
    main()
