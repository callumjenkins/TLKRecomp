import sys,re
# produce (pc, master_after_charge) sequences for one run: cyc lines (interp) + aot trace lines
def load(cycf, trf):
    ev=[]
    for l in open(cycf):
        m=re.search(r'pc=\$(\w+) .*master_delta=(\d+) master=(\d+) start=(\d+)',l)
        ev.append((int(m.group(4)), 'I', m.group(1), int(m.group(2))))
    for l in open(trf):
        f=l.split()
        if len(f)>2 and f[2]=='aot':
            mc=int(f[5].split('=')[1]); ev.append((int(f[1]),'A',f[3][4:],mc))
    ev.sort(key=lambda e:e[0])
    return ev
L=load(sys.argv[1],sys.argv[2]); A=load(sys.argv[3],sys.argv[4])
lo,hi=int(sys.argv[5]),int(sys.argv[6])
L=[e for e in L if lo<=e[0]<=hi]; A=[e for e in A if lo<=e[0]<=hi]
for i in range(max(len(L),len(A))):
    a=L[i] if i<len(L) else None; b=A[i] if i<len(A) else None
    flag='' if (a and b and a[2]==b[2] and a[0]==b[0]) else '  <<<'
    print(a,b,flag)
# smarter: merge-join by start; tolerate entries present on one side only if both sides re-align on the next start
print("=== join ===")
i=j=0; out=0
while i<len(L) and j<len(A) and out<1:
    a,b=L[i],A[j]
    if a[0]==b[0]:
        i+=1; j+=1; continue
    if a[0]<b[0] and i+1<len(L) and L[i+1][0]==b[0]: i+=1; continue
    if b[0]<a[0] and j+1<len(A) and A[j+1][0]==a[0]: j+=1; continue
    print("DIVERGE at", a, b)
    for k in range(max(0,i-6), min(len(L),i+4)): print("  L", L[k])
    for k in range(max(0,j-6), min(len(A),j+4)): print("  A", A[k])
    out+=1
