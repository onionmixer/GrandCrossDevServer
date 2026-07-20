import socket,struct,sys,os
HOST="127.0.0.1";P=int(sys.argv[1]);SVC=[P];PMAP,MOUNTP,NFSP=100000,100005,100003
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.settimeout(5);_x=[1]
def call(prog,vers,proc,args=b""):
    _x[0]+=1;xid=_x[0]
    m=struct.pack(">IIIIII",xid,0,2,prog,vers,proc)+struct.pack(">II",0,0)*2+args
    s.sendto(m,(HOST,P if prog==PMAP else SVC[0]));d,_=s.recvfrom(65536)
    o=12;vf,vl=struct.unpack(">II",d[o:o+8]);o+=8+vl;o+=4
    return d[o:]
def sstr(v):
    b=v.encode();p=(4-len(b)%4)%4
    return struct.pack(">I",len(b))+b+b"\0"*p
def u32(b,o):return struct.unpack(">I",b[o:o+4])[0],o+4
r=call(PMAP,2,3,struct.pack(">IIII",NFSP,2,17,0));p,_=u32(r,0)
if p:SVC[0]=p
mode=sys.argv[2]
if mode=="get":
    r=call(MOUNTP,1,1,sstr(os.environ["GN_EXPORT"]));st,o=u32(r,0);root=r[o:o+32]
    r=call(NFSP,2,4,root+sstr("sub"));st,o=u32(r,0);subfh=r[o:o+32]
    r=call(NFSP,2,4,subfh+sstr("deep.txt"));st,o=u32(r,0);filefh=r[o:o+32]
    open("/tmp/fh.bin","wb").write(filefh)
    print("  핸들 획득: sub/deep.txt")
else:
    filefh=open("/tmp/fh.bin","rb").read()
    r=call(NFSP,2,1,filefh)                     # GETATTR (재시작 후 옛 핸들)
    st,o=u32(r,0)
    print("  재시작 후 GETATTR status=%d %s"%(st,"OK(영속화 동작)" if st==0 else "STALE(실패)"))
    if st==0:
        r=call(NFSP,2,6,filefh+struct.pack(">III",0,100,0))   # READ
        st,o=u32(r,0)
        if st==0:
            o+=68; ln,o=u32(r,o)
            print("  READ 내용: %r"%r[o:o+ln])
