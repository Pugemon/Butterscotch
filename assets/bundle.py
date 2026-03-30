import os
import re
import struct

INPUT_DIR = "t3x"
OUTPUT_FILE = "undertale.t3b"

ALIGN = 1024  # 1 KB

# регулярка для твоего формата
PATTERN = re.compile(r"s\d+x\d+_page_(\d+)\.t3x$")

def align_up(x, align):
    """Выравнивание вверх до ближайшего multiple of align"""
    return (x + align - 1) // align * align

def build_archive(input_dir, output_file):
    files = os.listdir(input_dir)

    # словарь: id -> путь к файлу
    id_to_path = {}
    for f in files:
        match = PATTERN.match(f)
        if not match:
            continue
        file_id = int(match.group(1))
        id_to_path[file_id] = os.path.join(input_dir, f)

    if not id_to_path:
        raise RuntimeError("No matching files found")

    max_id = max(id_to_path.keys())
    count = max_id + 1

    print(f"IDs detected: 0..{max_id}, total slots: {count}")

    offsets = [0] * (count + 1)

    with open(output_file, "wb") as out:

        # HEADER + OFFSETS
        header_size = 8
        offsets_size = (count + 1) * 4
        data_offset = align_up(header_size + offsets_size, ALIGN)

        # резервируем пространство под header + offsets + padding
        out.seek(data_offset)
        current_offset = 0

        # пишем DATA с выравниванием 1KB
        for i in range(count):
            aligned_offset = align_up(current_offset, ALIGN)
            padding = aligned_offset - current_offset
            if padding > 0:
                out.write(b"\x00" * padding)
                current_offset = aligned_offset

            offsets[i] = current_offset

            path = id_to_path.get(i)
            if path:
                with open(path, "rb") as f:
                    data = f.read()
                    out.write(data)
                    current_offset += len(data)
            # если файла нет — size = 0, оставляем слот пустым

        offsets[count] = current_offset

        # запись HEADER + OFFSETS в начало файла
        out.seek(0)
        # HEADER: uint32 count, uint32 data_offset
        out.write(struct.pack("<II", count, data_offset))
        for off in offsets:
            out.write(struct.pack("<I", off))

    print(f"Archive written: {output_file}")
    print(f"DATA size: {current_offset} bytes")

if __name__ == "__main__":
    build_archive(INPUT_DIR, OUTPUT_FILE)