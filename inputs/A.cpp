#include <iostream>
using namespace std;


int main() {
    int x = 5; 

    if (x > 10) { //false condition never evaluated
        cout << "Greater than 10";
    }

    cout << "Program running";

    return x;

    cout << "This will never execute";
}
