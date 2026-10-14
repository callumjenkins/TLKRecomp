import sys
import re
ok=lambda f: len(f)>2 and (f[2]=="frame" or re.fullmatch("[0-9A-F]{4}",f[2]))
L=[f for f in (l.split() for l in open(sys.argv[1])) if ok(f)]; A=[f for f in (l.split() for l in open(sys.argv[2])) if ok(f)]
print(len(L),len(A))
def key(x):
    if len(x)>2 and x[2].startswith('21') and '2140'<=x[2]<='217F': return x[:3]+[x[-1].split('/')[0]]
    return x[:3]
for i,(x,y) in enumerate(zip(L,A)):
    if key(x)!=key(y):
        print("first diff line",i)
        print("WINDOW", L[i-1][0], L[i][0], L[i-1][1], L[i][1])
        for j in range(max(0,i-4),i+6): print(' '.join(L[j])[:150]); print(' '.join(A[j])[:150]); print()
        break
else: print("no diff in common prefix")
