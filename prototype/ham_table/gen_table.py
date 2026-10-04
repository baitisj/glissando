"""Builds the code lengths in app/src/text_messaging/HamText.cpp (LENGTHS).

97 symbols: printable ASCII 0x20-0x7E, ESC (a raw byte follows) and EMPTY
(the rest of the block is padding). Character counts come from
chat_corpus.txt, 94 typical chat lines, blended 70/30 with the prose of the
repository's docs as of October 2026, so ordinary English is not punished
for missing from the small sample. EMPTY is forced to be the rarest symbol,
so the complemented canonical code gives it the all-zero codeword.

    python3 gen_table.py      prints the table
"""
import re, heapq, collections, os, subprocess
HERE=os.path.dirname(os.path.abspath(__file__))
ROOT=os.path.dirname(os.path.dirname(HERE))
msgs=[l.rstrip('\n') for l in open(os.path.join(HERE,'chat_corpus.txt')) if l.strip()]
# The docs as they stood when the table was built, so it can be rebuilt
# exactly whatever the docs say now.
DOCS_COMMIT='493f13a'
def at_commit(path):
    return subprocess.run(['git','-C',ROOT,'show',f'{DOCS_COMMIT}:{path}'],capture_output=True,text=True,check=True).stdout
names=subprocess.run(['git','-C',ROOT,'ls-tree','--name-only',DOCS_COMMIT,'docs/'],capture_output=True,text=True,check=True).stdout.split()
prose=''
for f in sorted(n for n in names if n.endswith('.md'))+['README.md']:
    t=at_commit(f)
    t=re.sub(r'```.*?```','',t,flags=re.S); t=re.sub(r'\(http[^)]*\)|\([^)]*\.(svg|md|png)[^)]*\)','',t)
    t=re.sub(r'[#*`|>_\[\]]','',t); t=re.sub(r'\s+',' ',t)
    prose+=t
prose=''.join(c for c in prose if 32<=ord(c)<127)
NSYM=97; ESC=95; EOM=96  # EOM is HamText's EMPTY
def counts(texts, weight):
    c=collections.Counter(); n=0
    for t in texts:
        for ch in t: c[ord(ch)-32]+=1; n+=1
    return {k:v*weight/n for k,v in c.items()}
def lengths_for(train_msgs, maxlen=15):
    f=[1e-4]*NSYM
    for src,w in ((counts(train_msgs,0.7),1),(counts([prose],0.3),1)):
        for k,v in src.items(): f[k]+=v
    f[ESC]=2e-4
    f[EOM]=min(f)/2
    while True:
        h=[(f[i],i,(i,)) for i in range(NSYM)]; heapq.heapify(h); L=[0]*NSYM; k=NSYM
        while len(h)>1:
            a=heapq.heappop(h); b=heapq.heappop(h)
            for s in a[2]+b[2]: L[s]+=1
            heapq.heappush(h,(a[0]+b[0],k,a[2]+b[2])); k+=1
        if max(L)<=maxlen: break
        f=[x**0.9 for x in f]; f[EOM]=min(f[:EOM])/2
    assert abs(sum(2**-l for l in L)-1)<1e-12
    assert L[EOM]==max(L)
    return L
if __name__=='__main__':
    L=lengths_for(msgs)
    print('max',max(L),'EOM',L[EOM],'ESC',L[ESC])
    for i in range(0,NSYM,16): print(', '.join(str(x) for x in L[i:i+16])+',')
    print(''.join(f"{chr(32+i)}:{L[i]} " for i in range(95) if L[i]<=6))
