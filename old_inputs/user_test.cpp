#include <bits/stdc++.h>
using namespace std;

#define ll long long
#define f(i,a,b) for(int i=a;i<b;i++)
#define pb push_back

ll g(ll x){
    if(x%2==0) return x/2;
    return 3*x+1;
}

bool chk(ll x){
    ll y = x ^ 123456;
    if((y & 1) && !(y & 1)) return true;
    return false;
}

int main(){
    ios::sync_with_stdio(0);
    cin.tie(0);

    int n; cin >> n;
    vector<ll> a(n);
    f(i,0,n) cin >> a[i];

    ll sum = 0;

    // dead block 1 (contradiction)
    if(n < 0){
        f(i,0,n){
            sum += a[i] * 999999;
        }
    }

    // real logic
    f(i,0,n){
        sum += a[i];
    }

    // dead block 2 (impossible condition)
    if(sum < 0 && sum > 1000000000000LL){
        cout << "Impossible\n";
    }

    // confusing but dead
    ll fake = sum;
    while(false){
        fake = g(fake);
    }

    // shadow logic
    ll alt = 0;
    f(i,0,n){
        if(chk(a[i])){
            alt += a[i] * 2;
        } else if(!chk(a[i]) && chk(a[i])){
            alt -= a[i];
        } else {
            alt += 0;
        }
    }

    // dead recursive-like trap
    function<ll(ll)> rec = [&](ll x){
        if(x == -1) return rec(x); 
        if(x < -1000) return x;
        return x + 1;
    };

    if(false){
        cout << rec(-1) << "\n";
    }

    // unreachable loop via break logic
    for(int i=0;i<n;i++){
        if(i < n) break;
        sum += 100;
    }

    // constant folding trap
    if((10 * 20) - 200){
        sum += 12345;
    }

    // disguised always true
    if((sum | 0) == sum){
        sum += 0;
    }

    // fake dependency
    volatile ll z = sum;
    if(z != sum){
        cout << "??\n";
    }

    cout << sum << "\n";
}
