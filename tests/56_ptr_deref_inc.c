// Test: dereference combined with post/pre increment/decrement on pointers
int main() {
    int arr[5];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    arr[3] = 40;
    arr[4] = 50;

    // --- *p++ : read value, then advance pointer ---
    int *p = arr;
    int val;

    val = *p++;
    if (val != 10) return 1;    // should read arr[0]
    if (*p  != 20) return 2;    // p now points to arr[1]

    val = *p++;
    if (val != 20) return 3;    // should read arr[1]
    if (*p  != 30) return 4;    // p now points to arr[2]

    // --- *++p : advance pointer, then read ---
    // p is at arr[2], ++p moves to arr[3]
    val = *++p;
    if (val != 40) return 5;
    if (*p  != 40) return 6;    // p should be at arr[3]

    // --- *p-- : read value, then move pointer back ---
    val = *p--;
    if (val != 40) return 7;    // should read arr[3]
    if (*p  != 30) return 8;    // p now at arr[2]

    // --- *--p : move back, then read ---
    val = *--p;
    if (val != 20) return 9;    // moved to arr[1], then read
    if (*p  != 20) return 10;

    return 0;
}
