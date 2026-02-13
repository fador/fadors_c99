// Test global and static array initializer lists

int global_arr[5] = {10, 20, 30, 40, 50};
unsigned char global_bytes[] = {0x31, 0xED, 0x48, 0x89, 0xE7};
static int static_arr[3] = {100, 200, 300};

int main() {
    // Test global int array
    if (global_arr[0] != 10) return 1;
    if (global_arr[1] != 20) return 2;
    if (global_arr[2] != 30) return 3;
    if (global_arr[3] != 40) return 4;
    if (global_arr[4] != 50) return 5;

    // Test global unsigned char array (hex values)
    if (global_bytes[0] != 0x31) return 6;
    if (global_bytes[1] != 0xED) return 7;
    if (global_bytes[2] != 0x48) return 8;
    if (global_bytes[3] != 0x89) return 9;
    if (global_bytes[4] != 0xE7) return 10;

    // Test file-scope static array
    if (static_arr[0] != 100) return 11;
    if (static_arr[1] != 200) return 12;
    if (static_arr[2] != 300) return 13;

    // Test function-scope static array
    static int local_static[3] = {7, 8, 9};
    if (local_static[0] != 7) return 14;
    if (local_static[1] != 8) return 15;
    if (local_static[2] != 9) return 16;

    // Test function-scope static unsigned char array
    static unsigned char local_bytes[] = {0xAA, 0xBB, 0xCC};
    if (local_bytes[0] != 0xAA) return 17;
    if (local_bytes[1] != 0xBB) return 18;
    if (local_bytes[2] != 0xCC) return 19;

    return 0;
}
