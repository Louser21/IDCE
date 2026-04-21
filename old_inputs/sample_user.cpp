#include <iostream>
using namespace std;

int f() {
    return 5;
    cout << "dead";
}

int main() {
    int x = 10;
    if (x > 0) {
        cout << "alive\n";
    }
    return 0;
    x++;
}
