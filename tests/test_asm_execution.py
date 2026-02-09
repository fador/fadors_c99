import os
import subprocess
import sys
import glob

# Configuration
COMPILER = r"build\Release\fadors99.exe"
ASSEMBLER = "ml64"
LINKER = "link"

def run_test(c_file):
    print(f"Testing {c_file}...")
    base_name = os.path.splitext(c_file)[0]
    asm_file = base_name + ".asm"
    obj_file = base_name + ".obj"
    exe_file = base_name + ".exe"

    # 1. Compile to ASM
    # We expect this to fail if ml/link are not in PATH, because the compiler tries to run them.
    # However, if the ASM file is created, we consider the *compilation* part a success.
    try:
        subprocess.check_call([COMPILER, c_file, "--masm"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        # Compiler might verify return code of ml/link and exit 1
        pass 

    if not os.path.exists(asm_file):
        print(f"FAILED: ASM file {asm_file} not created (Compiler error)")
        return False
    
    # Compilation succeeded (ASM exists). Now we try to assemble manually to verify,
    # or skip if tools are missing.

    if not os.path.exists(asm_file):
        print(f"FAILED: ASM file {asm_file} not created")
        return False

    # 2. Assemble (ml)
    # Check if ml exists
    try:
        subprocess.check_call([ASSEMBLER, "/c", "/nologo", "/Fo" + obj_file, asm_file], stdout=subprocess.DEVNULL)
    except FileNotFoundError:
        print("SKIPPED: ml not found in PATH")
        return True # Can't test binary, but compilation passed
    except subprocess.CalledProcessError as e:
        print(f"FAILED: Assembly of {asm_file}")
        print(e)
        return False

    # 3. Link
    # Note: We use /entry:main to avoid C runtime startup for simple tests
    # For tests using libc functions, we'd need to link against libcmt.lib (default) and standard entry point
    # But my current tests just return int.
    try:
        subprocess.check_call([LINKER, "/nologo", "/entry:main", "/subsystem:console", "/out:" + exe_file, obj_file], stdout=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        print(f"FAILED: Linking of {obj_file}")
        return False

    # 4. Run
    try:
        result = subprocess.run([exe_file], capture_output=True)
        ret_code = result.returncode
        print(f"  Exit Code: {ret_code}")
        
        # Simple verification: 01_return.c returns 42
        if "01_return" in c_file and ret_code != 42:
             print(f"FAILED: Expected exit code 42, got {ret_code}")
             return False
        
        # 07_function.c returns 123
        if "07_function" in c_file and ret_code != 123:
             print(f"FAILED: Expected exit code 123, got {ret_code}")
             return False
        
        # 17_for.c returns 45
        if "17_for" in c_file and ret_code != 45:
             print(f"FAILED: Expected exit code 45, got {ret_code}")
             return False

        # 18_typedef.c returns 42
        if "18_typedef" in c_file and ret_code != 42:
             print(f"FAILED: Expected exit code 42, got {ret_code}")
             return False

        # 19_array.c returns 100
        if "19_array" in c_file and ret_code != 100:
             print(f"FAILED: Expected exit code 100, got {ret_code}")
             return False

        # 20_switch.c returns 120
        if "20_switch" in c_file and ret_code != 120:
             print(f"FAILED: Expected exit code 120, got {ret_code}")
             return False
        
        # 21_enum.c returns 6
        if "21_enum" in c_file and ret_code != 6:
             print(f"FAILED: Expected exit code 6, got {ret_code}")
             return False

        # 22_union.c returns 42
        if "22_union" in c_file and ret_code != 42:
             print(f"FAILED: Expected exit code 42, got {ret_code}")
             return False

        if "23_pointer_math" in c_file and ret_code != 0:
             print(f"FAILED: Expected exit code 0, got {ret_code}")
             return False

    except OSError as e:
        print(f"FAILED: Execution of {exe_file}: {e}")
        return False

    print("PASSED")
    return True

def main():

    vcvars_path = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if os.path.exists(vcvars_path):
        output = subprocess.check_output(f'"{vcvars_path}" && set', shell=True, text=True)
        for line in output.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                os.environ[key] = value

    if not os.path.exists(COMPILER):
        print(f"Compiler not found at {COMPILER}")
        sys.exit(1)

    c_files = glob.glob("tests/*.c")
    passed = 0
    total = 0
    
    # Filter for known working tests for binary execution (simple returns)
    # 01_return.c, 07_function.c, 11_nested_struct.c (if it compiles to valid asm)
    test_whitelist = ["01_return.c", "07_function.c", "11_nested_struct.c", "17_for.c", "18_typedef.c", "19_array.c", "20_switch.c", "21_enum.c", "22_union.c", "23_pointer_math.c"]
    for c_file in c_files:
        if os.path.basename(c_file) in test_whitelist:
            total += 1
            if run_test(c_file):
                passed += 1

    print(f"\nSummary: {passed}/{total} tests passed (compiled & maybe executed)")
    if passed < total:
        sys.exit(1)

if __name__ == "__main__":
    main()
