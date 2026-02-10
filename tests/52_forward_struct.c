struct Node;

struct Node {
    struct Node *next;
    int val;
};

int main() {
    struct Node n;
    n.val = 123;
    n.next = 0;
    if (n.val != 123) return 1;
    return 0;
}
