int main() {
    int a = 0;
    int b = 0;
    
    // Postfix increment
    b = a++; 
    if (b != 0) return 1;
    if (a != 1) return 2;

    // Prefix increment
    b = ++a;
    if (b != 2) return 3;
    if (a != 2) return 4;

    // Postfix decrement
    b = a--;
    if (b != 2) return 5;
    if (a != 1) return 6;

    // Prefix decrement
    b = --a;
    if (b != 0) return 7;
    if (a != 0) return 8;

    // Pointers
    int arr[3];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    
    int *p = arr;
    if (*p != 10) return 9;
    
    p++;
    if (*p != 20) return 10;
    
    p--;
    if (*p != 10) return 11;
    return 42;
}
