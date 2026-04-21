#include <bits/stdc++.h>
using namespace std;

int main(){
    int n; cin >> n;
    vector<int> a(n);
    for(int i=0;i<n;i++) cin >> a[i];

    int s = 0;

    for(int i=0;i<n;i++){
        s += a[i];
    }

    if(false){
        cout << s*100 << "\n";
    }

    int x = 10;
    x = x - 10;

    if(x){
        s += 500;
    }

    cout << s << "\n";
}