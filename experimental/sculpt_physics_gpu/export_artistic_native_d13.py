"""Export verified D11 artistic modules from an isolated Warp 1.19 cache."""
from pathlib import Path
import argparse
import re
import shutil
import tempfile
import warp as wp
import prototype_warp_artistic_pressure as pressure
import prototype_warp_artistic_flux as flux

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if str(wp.__version__) != "1.19.0":
        raise RuntimeError("Warp 1.19.0 required")
    wp.config.cuda_output = "ptx"
    wp.config.ptx_target_arch = 75
    with tempfile.TemporaryDirectory(prefix="bse-d13-artistic-") as tmp:
        wp.config.kernel_cache_dir = tmp
        wp.init()
        if not wp.is_cuda_available():
            raise RuntimeError("CUDA required for native export")
        specs = [
            (pressure, "artistic_pressure_local", "artistic_pressure"),
            (flux, "artistic_paired_exchange", "artistic_flux"),
        ]
        for module, _, _ in specs:
            wp.set_module_options({"fast_math": False}, module=module)
            module.run_fixture("cuda:0")
        wp.synchronize()
        artifacts = []
        for _, prefix, stem in specs:
            pattern = re.compile(r"\.entry\s+(" + re.escape(prefix) + r"\w*_cuda_kernel_forward)\s*\(")
            matches = []
            for path in Path(tmp).rglob("*.ptx"):
                for symbol in pattern.findall(path.read_text(errors="ignore")):
                    matches.append((path, symbol))
            if len(matches) != 1:
                raise RuntimeError(f"Expected exactly one {prefix} PTX entry, got {len(matches)}")
            artifacts.append((stem, *matches[0]))
        args.output.mkdir(parents=True, exist_ok=True)
        for stem, path, symbol in artifacts:
            target = args.output / (stem + ".ptx")
            shutil.copyfile(path, target)
            (args.output / (stem + ".symbols")).write_text(symbol + "\n", encoding="ascii")
            print("[OK]", target, symbol)
if __name__ == "__main__":
    main()
