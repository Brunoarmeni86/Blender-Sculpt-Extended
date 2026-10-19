"""Standalone regression for the actual Multires UV duplicate-vertex guards.

Run with Python 3; a C++ compiler is required. This extracts and compiles the
guards from multires_bake.cc without requiring a Blender build. It does not
replace bake image or integration tests.
"""

import itertools
import pathlib
import re
import shutil
import subprocess
import tempfile


SOURCE = pathlib.Path(__file__).resolve().parents[2] / "source/blender/render/intern/multires_bake.cc"
FUNCTIONS = (
    "rasterize_triangle_center_reference",
    "rasterize_triangle_coverage",
    "rasterize_triangle",
)


def extract_guard(source, name):
    start = source.index("static void " + name + "(")
    # These functions test three UV vertices for duplicates before sorting.
    match = re.search(r"if\s*\((\([^{}]+?)\)\s*\{\s*return;", source[start:])
    if match is None:
        raise AssertionError(f"Duplicate-vertex guard missing: {name}")
    return match.group(1)


def main():
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if compiler is None:
        raise RuntimeError("A C++ compiler is required")
    source = SOURCE.read_text()
    guards = [extract_guard(source, name) for name in FUNCTIONS]
    cpp = "#include <cassert>\nstruct UV { float x, y; };\n"
    for index, guard in enumerate(guards):
        cpp += f"bool skip_{index}(UV low, UV mid, UV high) {{\n"
        cpp += "UV p_low=low, p_mid=mid, p_high=high;\n"
        cpp += f"return {guard};\n}}\n"
    cpp += "int main() {\n"
    valid = ((0, 0), (0, 1), (1, 0))
    # Every ordering must survive, including the former false-positive case.
    checks = 0
    for index in range(len(guards)):
        for points in itertools.permutations(valid):
            args = ",".join(f"UV{{{x}.0f,{y}.0f}}" for x, y in points)
            cpp += f"assert(!skip_{index}({args}));\n"
            checks += 1
        for duplicate_pair in ((0, 1), (0, 2), (1, 2)):
            points = list(valid)
            points[duplicate_pair[1]] = points[duplicate_pair[0]]
            args = ",".join(f"UV{{{x}.0f,{y}.0f}}" for x, y in points)
            cpp += f"assert(skip_{index}({args}));\n"
            checks += 1
    cpp += "}\n"
    with tempfile.TemporaryDirectory() as directory:
        directory = pathlib.Path(directory)
        cpp_path = directory / "guard_test.cc"
        executable = directory / "guard_test"
        cpp_path.write_text(cpp)
        subprocess.run([compiler, "-std=c++17", str(cpp_path), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print(f"PASS: {checks} checks across {len(guards)} actual rasterization guards")


if __name__ == "__main__":
    main()
