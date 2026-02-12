// Test: array[index++] = *ptr++ and similar compound expressions
int main() {
    // Source data
    int src[6];
    src[0] = 100;
    src[1] = 200;
    src[2] = 300;
    src[3] = 400;
    src[4] = 500;
    src[5] = 600;

    // Destination buffer
    int dst[6];
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 0;
    dst[4] = 0;
    dst[5] = 0;

    int *p = src;
    int out_pos = 0;

    // --- Test 1: output[out_pos++] = *p++ ---
    // The canonical pattern: copy src[0..2] to dst[0..2]
    dst[out_pos++] = *p++;
    dst[out_pos++] = *p++;
    dst[out_pos++] = *p++;

    if (out_pos != 3)   return 1;
    if (dst[0] != 100)  return 2;
    if (dst[1] != 200)  return 3;
    if (dst[2] != 300)  return 4;
    // p should now point to src[3]
    if (*p != 400)       return 5;

    // --- Test 2: output[out_pos++] = *p++ in a while loop ---
    // Copy remaining 3 elements
    int count = 3;
    while (count > 0) {
        dst[out_pos++] = *p++;
        count = count - 1;
    }

    if (out_pos != 6)   return 6;
    if (dst[3] != 400)  return 7;
    if (dst[4] != 500)  return 8;
    if (dst[5] != 600)  return 9;

    // --- Test 3: verify index and pointer are both advanced correctly ---
    // out_pos should be 6, p should point past src[5]
    // Reset and test with pre-increment on index
    int dst2[4];
    dst2[0] = 0;
    dst2[1] = 0;
    dst2[2] = 0;
    dst2[3] = 0;

    int *q = src;
    int idx = 0;

    // Store at idx=0, then idx becomes 1; read src[0], q advances
    dst2[idx++] = *q++;
    if (idx != 1)       return 10;
    if (dst2[0] != 100) return 11;

    // Store at idx=1, then idx becomes 2; read src[1], q advances
    dst2[idx++] = *q++;
    if (idx != 2)       return 12;
    if (dst2[1] != 200) return 13;

    return 0;
}
