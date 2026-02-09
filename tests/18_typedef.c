typedef int MyInt;
typedef MyInt *MyIntPtr;

int main() {
    MyInt x = 42;
    MyIntPtr p = &x;
    return *p;
}
