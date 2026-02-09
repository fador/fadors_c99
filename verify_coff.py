import os
import subprocess
import sys

def run_cmd(cmd, env=None):
    print(f"[CMD] {cmd}")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True, env=env)
    return result

def main():
    tests = [
        ("tests/01_return.c", 42),
        ("tests/02_arithmetic.c", 7),
        ("tests/03_variables.c", 30),
        ("tests/04_if.c", 100),
        ("tests/06_while.c", 10),
        ("tests/07_function.c", 123),
        ("tests/12_string.c", 72),
        ("tests/14_params.c", 10),
        ("tests/15_nested_calls.c", 10),
        ("tests/19_array.c", 100),
        ("tests/20_switch.c", 120),
        ("tests/21_enum.c", 6),
        ("tests/22_union.c", 42),
        ("tests/23_pointer_math.c", 0)
    ]
    
    fadors = "build\\Release\\fadors99.exe"
    
    
    # We need vcvars64 for the linker
    vcvars_path = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    if os.path.exists(vcvars_path):
        output = subprocess.check_output(f'"{vcvars_path}" && set', shell=True, text=True)
        for line in output.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                os.environ[key] = value

    env = os.environ.copy()
    # Set FADORS_LINKER
    env["FADORS_LINKER"] = 'link'
    
    results = []
    
    for test_path, expected_code in tests:
        name = os.path.basename(test_path)
        print(f"--- Running {name} ---")
        
        # Compile with --obj
        res = run_cmd(f"{fadors} {test_path} --obj", env=env)
        if res.returncode != 0:
            print(f"Compilation failed for {name}:\n{res.stdout}\n{res.stderr}")
            results.append((name, "FAIL (Comp)"))
            continue
            
        # Execute the resulting EXE
        exe_path = test_path.replace(".c", ".exe")
        if not os.path.exists(exe_path):
            print(f"EXE not found for {name}")
            results.append((name, "FAIL (EXE missing)"))
            continue
            
        res = run_cmd(os.path.normpath(exe_path))
        # Windows return codes are usually 0-255 for success/small errors, 
        # but can be large for crashes. We need to handle this.
        actual_code = res.returncode & 0xFFFFFFFF
        if actual_code > 0x7FFFFFFF:
             actual_code -= 0x100000000
             
        if actual_code == expected_code:
            print(f"SUCCESS: {name} returned {actual_code}")
            results.append((name, "PASS"))
        else:
            print(f"FAILURE: {name} returned {actual_code}, expected {expected_code}")
            if res.stdout:
                print(f"STDOUT: {res.stdout}")
            if res.stderr:
                print(f"STDERR: {res.stderr}")
            results.append((name, f"FAIL (Got {actual_code})"))
            
    print("\n" + "="*20)
    print("FINAL TEST RESULTS (COFF BACKEND)")
    print("="*20)
    all_pass = True
    for name, status in results:
        print(f"{name:25} : {status}")
        if status != "PASS":
            all_pass = False
            
    if all_pass:
        print("\nALL COFF TESTS PASSED!")
        sys.exit(0)
    else:
        print("\nSOME TESTS FAILED.")
        sys.exit(1)

if __name__ == "__main__":
    main()
