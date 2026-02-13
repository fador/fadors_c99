// Test: 2D array support
int main() {
    char grid[3][4];
    grid[0][0] = 10;
    grid[0][3] = 20;
    grid[1][0] = 30;
    grid[2][3] = 40;

    if (grid[0][0] != 10) return 1;
    if (grid[0][3] != 20) return 2;
    if (grid[1][0] != 30) return 3;
    if (grid[2][3] != 40) return 4;

    // int 2D array
    int mat[2][3];
    mat[0][0] = 100;
    mat[0][2] = 200;
    mat[1][1] = 300;

    if (mat[0][0] != 100) return 5;
    if (mat[0][2] != 200) return 6;
    if (mat[1][1] != 300) return 7;

    return 0;
}
