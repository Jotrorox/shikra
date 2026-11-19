#!/usr/bin/env python3
# mathcheck.py -- exact-arithmetic check of rt_math.c against the host libm (DESIGN.md "M6 Linux
# as built"). Not part of the build or of make test (slow: pure Python rationals, ~300 bits).
#   ./math_unit -dump | python3 tools/mathcheck.py
# reads "fn x y mine libm" lines (hex floats; y unused for one-argument functions) and prints,
# per function, the largest true error in ulps of Shikra's result and of libm's.
import sys, math
from fractions import Fraction as F
P=1500; ONE=1<<P
def atan_inv(n,h=False):
    s,t,k,sg=0,ONE//n,1,1
    while t:
        s+=sg*(t//k); t//=n*n; k+=2; sg=sg if h else -sg
    return s
PI=F(4*(4*atan_inv(5)-atan_inv(239)),ONE); LN2=F(2*atan_inv(3,True),ONE)
EPS=F(1,1<<300)
def rnd(x): return F(int(x*(1<<330)),1<<330)
def sin_s(x):
    s=F(0); t=x; k=1
    while abs(t)>EPS: s+=t; t=rnd(-t*x*x/((k+1)*(k+2))); k+=2
    return s
def cos_s(x):
    s=F(0); t=F(1); k=0
    while abs(t)>EPS: s+=t; t=rnd(-t*x*x/((k+1)*(k+2))); k+=2
    return s
def red(x):
    n=round(x/(PI/2)); return n, rnd(x-n*PI/2)
def sin(x): n,r=red(x); return [sin_s(r),cos_s(r),-sin_s(r),-cos_s(r)][n%4]
def cos(x): n,r=red(x); return [cos_s(r),-sin_s(r),-cos_s(r),sin_s(r)][n%4]
def tan(x): return sin(x)/cos(x)
def exp(x):
    k=round(x/LN2); r=rnd(x-k*LN2); s=F(0); t=F(1); i=1
    while abs(t)>EPS: s+=t; t=rnd(t*r/i); i+=1
    return s*F(2)**k
def log(x):
    x=F(x); k=0
    while x>2: x/=2; k+=1
    while x<1: x*=2; k-=1
    s=(x-1)/(x+1); r=F(0); t=s; i=1
    while abs(t)>EPS: r+=t/i; t=rnd(t*s*s); i+=2
    return 2*r+k*LN2
def atan(x):
    x=F(x); 
    if x<0: return -atan(-x)
    if x>1: return PI/2-atan(1/x)
    if x>F(1,2): return PI/4-atan((1-x)/(1+x))
    r=F(0); t=x; i=1
    while abs(t)>EPS: r+=t/i; t=rnd(-t*x*x); i+=2
    return r
def sqrtF(x):
    x=F(x); n=int(x*(1<<700)); r=math.isqrt(n); return F(r,1<<350)
def asin(x): x=F(x); return atan(x/sqrtF(1-x*x)) if abs(x)<1 else (PI/2 if x>0 else -PI/2)
def acos(x):
    x=F(x)
    if x==0: return PI/2
    a=atan(sqrtF(1-x*x)/abs(x)); return a if x>0 else PI-a
def atan2(y,x):
    y=F(y);x=F(x)
    if x>0: return atan(y/x)
    return atan(y/x)+(PI if y>=0 else -PI)
def pow_(x,y): return exp(F(y)*log(x))

fns={'sin':sin,'cos':cos,'tan':tan,'exp':exp,'log':log,'asin':asin,'acos':acos,'atan2':atan2,'pow':pow_}
import collections
worst=collections.defaultdict(lambda:[0,0,0,None])
for line in sys.stdin:
    p=line.split()
    if len(p)!=5 or p[0] not in fns: continue
    a=float.fromhex(p[1]); b=float.fromhex(p[2]); mine=float.fromhex(p[3]); lib=float.fromhex(p[4])
    if p[0] in ('atan2','pow'): t=fns[p[0]](F(a),F(b))
    else: t=fns[p[0]](F(a))
    if t==0: continue
    u=math.ulp(float(t)) if float(t)!=0 else 2**-1074
    em=abs(float((F(mine)-t)/F(u))); el=abs(float((F(lib)-t)/F(u)))
    w=worst[p[0]]; w[2]+=1
    if em>w[0]: w[0]=em; w[3]=(a,b)
    if el>w[1]: w[1]=el
for k,w in worst.items(): print("%-6s n=%d  mine max %.3f ulp  libm max %.3f ulp  (mine worst at %r)"%(k,w[2],w[0],w[1],w[3]))
