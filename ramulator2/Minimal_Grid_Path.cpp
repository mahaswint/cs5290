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
#define vvi vector<vector<int>>
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
    int n;
    cin>>n;
    vvi v(n+1,vi(n+1));
    vector<vector<array<int,2>>>dp(n+1,vector<array<int,2>>(n+1,{-1,-1}));
    for(int i=0;i<n;i++){
        string s;
        cin>>s;
        if(n==1){
            cout<<s<<endl;return;
        }
        for(int j=0;j<n;j++){
            v[i+1][j+1]=s[j]-'A';
            // v[i+1][j+1]++;
        }

    }
    //check same length one's at one time only keep the valid ones in bfs
    dp[1][2]={1,1};
    dp[2][1]={1,1};
    for(int k=2;k<2*n;k++){
        int s=max({0ll,k-n});
        int mini=100;
        for(int i=s;i<=n;i++){
            if(k-i<0)break;
            if(dp[i][k-i][0]==-1){
                continue;
            }
            mini=min(mini,v[i][k-i]);
        }
        for(int i=s;i<=n;i++){
            if(k-i<0)break;
            if(dp[i][k-i][0]==-1 || (v[i][k-i]!=mini)){
                continue;
            }
            if(i+1<=n){
                dp[i+1][k-i]={i,k-i};
            }
            if(k-i+1<=n){
                dp[i][k-i+1]={i,k-i};
            }
        }
    }
    int i=n;int j=n;
    vector<char>ans(2*n);
    int k=2*n-1;
    while(k){
        auto nxt=dp[i][j];
        ans[k]=v[i][j]+'A';
        k--;
        i=nxt[0];j=nxt[1];
    }
    for(int i=1;i<2*n;i++){
        cout<<ans[i];
    }
    


    
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