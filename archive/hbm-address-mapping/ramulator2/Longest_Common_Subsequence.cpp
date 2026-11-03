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
    int n,m;
    cin>>n>>m;
    vector<vector<int>> dp(n+1,vector<int>(m+1,0));
    vector<int>a,b;
    for(int i=0;i<n+m;i++){
        int k;cin>>k;
        if(i<n){
            a.pb(k);
        }
        else b.pb(k);
    }
    for(int i=1;i<=n;i++){
        for(int j=1;j<=m;j++){
            if(a[i-1]==b[j-1]){
                dp[i][j]=max({dp[i-1][j-1]+1,dp[i][j-1],dp[i-1][j]});
                // cerr<<i<<" "<<j<<" "<<dp[i][j]<<endl;
            }
            else dp[i][j]=max({dp[i][j-1],dp[i-1][j]});
        }
    }
    int k=dp[n][m];
    cout<<k<<endl;
    int kk=k;
    int i=n;int j=m;
    vector<int>ans(k+1);
    while(k){
    
        if(dp[i-1][j]==dp[i][j]){
            i--;
        }
        else if(dp[i][j-1]==dp[i][j])j--;
        else{
            ans[k]=a[i-1];
            i--;j--;k--;
        }
    }
    for(int i=1;i<=kk;i++)cout<<ans[i]<<" ";
    cout<<endl;
    
}
signed main() {
    fast;
    int t=1;
    // cin>>t;
    while (t--) {
        solve();
    }
    return 0;
}