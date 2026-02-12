// Test: writing through incremented pointers and char-level operations
int main() {
    // --- Test 1: *p++ = value (write through pointer, then advance) ---
    int arr[5];
    arr[0] = 0;
    arr[1] = 0;
    arr[2] = 0;
    arr[3] = 0;
    arr[4] = 0;

    int *p = arr;

    *p++ = 10;   // write 10 to arr[0], p -> arr[1]
    *p++ = 20;   // write 20 to arr[1], p -> arr[2]
    *p++ = 30;   // write 30 to arr[2], p -> arr[3]

    if (arr[0] != 10) return 1;
    if (arr[1] != 20) return 2;
    if (arr[2] != 30) return 3;
    if (arr[3] != 0)  return 4;  // should be untouched

    // --- Test 2: *++p = value (advance pointer, then write) ---
    // p is at arr[3] now
    *p = 40;     // write to arr[3]
    *++p = 50;   // advance to arr[4], then write 50

    if (arr[3] != 40) return 5;
    if (arr[4] != 50) return 6;

    // --- Test 3: combine read and write with increments ---
    int src[3];
    src[0] = 111;
    src[1] = 222;
    src[2] = 333;

    int dst[3];
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;

    int *rp = src;
    int *wp = dst;

    // *wp++ = *rp++: read from src, write to dst, advance both
    *wp++ = *rp++;
    *wp++ = *rp++;
    *wp++ = *rp++;

    if (dst[0] != 111) return 7;
    if (dst[1] != 222) return 8;
    if (dst[2] != 333) return 9;

    // --- Test 4: *--p = value (move back, then write) ---
    // wp is past dst[2], so --wp -> dst[2]
    *--wp = 999;
    if (dst[2] != 999) return 10;

    *--wp = 888;
    if (dst[1] != 888) return 11;

    // --- Test 5: multiple increments in sequence ---
    int data[4];
    data[0] = 0;
    data[1] = 0;
    data[2] = 0;
    data[3] = 0;

    int *d = data;
    int i = 0;

    // Each iteration: write i*10 via *d++, increment i with i++
    *d++ = i * 10; i++;
    *d++ = i * 10; i++;
    *d++ = i * 10; i++;
    *d++ = i * 10; i++;

    if (data[0] != 0)  return 12;
    if (data[1] != 10) return 13;
    if (data[2] != 20) return 14;
    if (data[3] != 30) return 15;
    if (i != 4)        return 16;

    return 0;
}
