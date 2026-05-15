#!/usr/bin/env python3
# Rewrite the MoltenVK ICD JSON's library_path to a .app-bundle-relative
# path so the loader inside the bundle finds Contents/Frameworks/libMoltenVK
# without depending on system Vulkan installs. Called by the macOS CI:
#
#   rewrite_icd.py <src-icd-json> <dst-icd-json>
#
# Source: $VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json
# Dest:   WhiteoutFlakes.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
#
# The dest's library_path is relative to the JSON's parent dir; from
# Resources/vulkan/icd.d/ the bundle's Frameworks/ is three levels up.

import json
import sys

if len(sys.argv) != 3:
    print(f"usage: {sys.argv[0]} <src-icd> <dst-icd>", file=sys.stderr)
    sys.exit(2)

src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
    j = json.load(f)
j["ICD"]["library_path"] = "../../../Frameworks/libMoltenVK.dylib"
with open(dst, "w") as f:
    json.dump(j, f, indent=2)
print(f"Wrote {dst} -> {j['ICD']['library_path']}")
