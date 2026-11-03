#include <bits/stdc++.h>
#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>
using namespace std;
using namespace __gnu_pbds; 
#define ordered_set tree<int, null_type,less<int>, rb_tree_tag,tree_order_statistics_node_update> 
#define float double
#define yes cout<<"YES"<<endl
#define no cout<<"NO"<<endl
#define endl '\n'
#define p(x) cout<<x<<endl
#define c(x) cout<<x<<" "
#define int long long
#define pb push_back
#define scanarr int a[n];for(int i=0;i<n;i++)cin>>a[i]
#define scannarr(a) int a[(int)(a)];for(int i=0;i<(int)(a);i++)cin>>a[i]
#define fr(i, a, b) for (int i = (a); i < (int)(b); ++i)
#define frr(i, a, b) for (int i = (a); i >= (int)(b); --i)
#define vi vector<int>
#define frr(i, a, b) for (int i = (a); i >= (int)(b); --i)
#define fast                          \
    ios_base::sync_with_stdio(false); \
    cin.tie(NULL)
#define sz(a) (int)(a.size())
#define all(a) a.begin(), a.end()
#define mod 1000000007

int gcd(int a, int b) {
    return b == 0 ? a : gcd(b, a % b);
}

void solve() {
    int l,r;
    cin>>l>>r;
    // map<int,int>m;
    function<int(int)> sol = [&] (int n)->int{
        if(n==0)return 0;
        // if(m[n]!=0)return m[n];
        if(n==2) return 3;
        if(n==1) return 1;
        int h=0;
        int hh=1;
        while(hh*2<=n){
            h++;hh*=2;
        }
        if(n==(2*hh-1))return h+1;
        int kk=n-hh+2;
        int a=(sol(kk)+h+1);
        int b=(2*hh-1-n+h+1);
        int ans=min(a,b);
        // m[n]=ans;
        // cerr<<h<<" "<<hh<<" "<<n<<" "<<ans<<endl;
        return ans;
    };
    int ans=0;

    for(int i=l;i<=r;i++){
        // if(m[i])ans+=m[i];
        ans+=sol(i);
        cout<<sol(i)<<"\n";
    }
    cout<<ans<<endl;
}
signed main() {
    fast;
    int t=1;
    cin>>t;
    while (t--) {
        solve();
    }
    return 0;
}