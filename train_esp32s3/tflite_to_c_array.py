#!/usr/bin/env python3
"""
Convert a .tflite file to a C source file for embedding in ESP32-S3 firmware.
Usage:
  python3 tflite_to_c_array.py input.tflite [output.c]
Default output: model_data.c in current directory.
"""

import os
import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: tflite_to_c_array.py <input.tflite> [output.c]", file=sys.stderr)
        sys.exit(1)
    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else "model_data.c"

    with open(input_path, "rb") as f:
        data = f.read()

    with open(output_path, "w") as f:
        f.write("/* Auto-generated from %s - do not edit */\n\n" % os.path.basename(input_path))
        f.write("#include <stdint.h>\n\n")
        f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        f.write("const unsigned char g_magic_wand_model_data[] = {\n")
        for i in range(0, len(data), 12):
            chunk = data[i:i+12]
            hex_str = ", ".join("0x%02x" % b for b in chunk)
            f.write("  " + hex_str + ",\n")
        f.write("};\n\n")
        f.write("const unsigned int g_magic_wand_model_data_len = sizeof(g_magic_wand_model_data);\n\n")
        f.write("#ifdef __cplusplus\n}\n#endif\n")

    print("Wrote %s (%d bytes)" % (output_path, len(data)))

if __name__ == "__main__":
    main()
