// Test initializer lists for arrays

int main() {
    // Array initializer list
    int arr[5] = {10, 20, 30, 40, 50};
    
    if (arr[0] != 10) return 1;
    if (arr[1] != 20) return 2;
    if (arr[2] != 30) return 3;
    if (arr[3] != 40) return 4;
    if (arr[4] != 50) return 5;
    
    // Partial initializer - remaining should be zero
    int arr2[4] = {1, 2};
    if (arr2[0] != 1) return 6;
    if (arr2[1] != 2) return 7;
    if (arr2[2] != 0) return 8;
    if (arr2[3] != 0) return 9;
    
    // Sum of initialized array
    int sum = 0;
    for (int i = 0; i < 5; i++) {
        sum += arr[i];
    }
    if (sum != 150) return 10;
    
    return 0;
}
