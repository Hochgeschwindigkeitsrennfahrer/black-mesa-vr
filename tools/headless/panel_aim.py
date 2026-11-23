# VR panel click aiming for headless runs: replica of vr_settings_panel.cpp BuildLayout + angles for "fake right 0 0 1.70 <pitch> <yaw> 0".
# Usage: python3 -I panel_aim.py <vr_settings_panel.cpp> <devrows 0/1> dump | "<Row label>@<tab index>" [x offset in column, 800 = -/+ value, 880 = pill, 780 = choice, 512 = button]
import re, sys, math
src=open(sys.argv[1]).read()
a=src.index('static std::vector<Row> rows = {'); b=src.index('return rows;',a)
rows=[(m.group(1),m.group(2)) for m in re.finditer(r'\{ "((?:[^"\\]|\\.)*)", RowKind::(\w+)', src[a:b])]
kHeader,kRow,kSec,kTab,kGap=96,64,48,72,16; kTop=kHeader+kTab+24; kTexW=2048
def layout(dev):
    n=len(rows); hidden=[False]*n; inDev=False
    for i,(l,k) in enumerate(rows):
        if k in('Tab','Close','Section'): inDev=False
        elif k=='DevSection': inDev=True
        hidden[i]=inDev and not dev
    tabs=[i for i,(l,k) in enumerate(rows) if k=='Tab']
    tabW=(kTexW-80-(len(tabs)-1)*kGap)//len(tabs)
    pos={}; bottom=kTop
    H=lambda k: kSec if k in('Section','DevSection') else kRow
    for t,ti in enumerate(tabs):
        x0=40+t*(tabW+kGap); pos[rows[ti][0]]=(t,-1,x0+tabW//2,kHeader+kTab//2)
        first=ti+1; last=first
        while last<n and rows[last][1] not in('Tab','Close'): last+=1
        total=sum(0 if hidden[i] else H(rows[i][1]) for i in range(first,last))
        split=last;best=total;acc=0
        for i in range(first,last):
            if hidden[i]: continue
            if i>first and rows[i][1] in('Section','DevSection'):
                worst=max(acc,total-acc)
                if worst<best: best=worst;split=i
            acc+=H(rows[i][1])
        y=[kTop,kTop]
        for i in range(first,last):
            if hidden[i]: continue
            c=0 if i<split else 1
            key=rows[i][0]+'@%d'%t
            n2=2
            while key in pos:   # same label twice in a tab: "Preset@2", "Preset@2#2"
                key=rows[i][0]+'@%d#%d'%(t,n2); n2+=1
            pos[key]=(t,c,c*1024,y[c]+H(rows[i][1])//2)
            y[c]+=H(rows[i][1])
        bottom=max(bottom,y[0],y[1])
    texH=bottom+16+kRow+16
    return pos,texH
def aim(dev,name,xoff=800):
    pos,texH=layout(dev)
    k=name if name in pos else [p for p in pos if p.startswith(name+'@')][0]
    t,c,x,y=pos[k]
    px = x if c<0 else x+xoff
    Hh=40*texH/kTexW; D=max(44,Hh/2/math.tan(math.radians(22)))
    xh=(px/kTexW-0.5)*40; yh=(0.5-y/texH)*Hh-3
    # fake right controller's ray points ~10.5 deg below its commanded pitch (runs panel4tabs-1, panelcal-1)
    return (-math.degrees(math.atan2(yh,D))-10.5, -math.degrees(math.atan2(xh,D)), px, y, texH)
if __name__=='__main__':
    dev=sys.argv[2]=='1'
    if sys.argv[3]=='dump':
        pos,texH=layout(dev); print('texH',texH)
        for t in range(4):
            for c in (0,1):
                hs=sum(1 for k,v in pos.items() if v[0]==t and v[1]==c)
                print('tab',t,'col',c,'rows',hs)
    else:
        print('%.3f %.3f  # px %d py %d texH %d'%aim(dev,sys.argv[3],int(sys.argv[4]) if len(sys.argv)>4 else 800))
