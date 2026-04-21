#include <bits/stdc++.h>
using namespace std;

int main(){
    ios::sync_with_stdio(0);
    cin.tie(0);

    int n; cin >> n;
    vector<int> a(n);
    for(int i=0;i<n;i++) cin >> a[i];

    int sum = 0;

    for(int i=0;i<n;i++){
        sum += a[i];
    }

    // simple dead code (always false)
    if(n < 0){
        cout << "never\n";
    }

    // constant false
    if(5 > 10){
        sum += 1000;
    }

    // unused variable
    int x = sum * 2;
    x = x - sum;

    // always true but useless
    if(sum == sum){
        sum += 0;
    }

    // loop that never runs
    for(int i=0;i<0;i++){
        sum += i;
    }

    cout << sum << "\n";
}