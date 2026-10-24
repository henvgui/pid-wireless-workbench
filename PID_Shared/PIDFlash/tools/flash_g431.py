"""Compatibility entry point. Use flash_stm32.py for the current F103 default."""
import sys
from flash_stm32 import Link, flash_kb, main, target_profile, validate_image

if __name__ == "__main__":
    if not any(arg == "--part" or arg.startswith("--part=") for arg in sys.argv[1:]):
        sys.argv.extend(["--part", "STM32G431CBT6"])
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        sys.exit(1)
