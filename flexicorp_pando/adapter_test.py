import ctypes, json, sys, threading, time
lib = ctypes.CDLL(sys.argv[1]); corpus = sys.argv[2]
V=ctypes.c_void_p; S=ctypes.c_char_p
lib.flexicorp_pando_api_version.restype=ctypes.c_int
lib.flexicorp_pando_build_string.restype=S; lib.flexicorp_pando_build_json.restype=S
lib.flexicorp_pando_open.restype=V; lib.flexicorp_pando_open.argtypes=[S,S,ctypes.c_int]
lib.flexicorp_pando_request.restype=V; lib.flexicorp_pando_request.argtypes=[V,S,S,S,S,ctypes.POINTER(ctypes.c_int)]
lib.flexicorp_pando_query.restype=V; lib.flexicorp_pando_query.argtypes=[V,S,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,S]
lib.flexicorp_pando_busy.restype=ctypes.c_size_t; lib.flexicorp_pando_busy.argtypes=[V]
lib.flexicorp_pando_idle_seconds.restype=ctypes.c_double; lib.flexicorp_pando_idle_seconds.argtypes=[V]
lib.flexicorp_pando_free.argtypes=[V]; lib.flexicorp_pando_close.argtypes=[V]
lib.flexicorp_pando_last_error.restype=S; lib.flexicorp_pando_last_error.argtypes=[V]
fails=[]
def check(c,m):
    if not c: fails.append(m); print("FAIL",m)
def req(h,m,p,q=None,b=None):
    st=ctypes.c_int(0)
    r=lib.flexicorp_pando_request(h,m.encode(),p.encode(),q.encode() if q else None,json.dumps(b).encode() if b is not None else None,ctypes.byref(st))
    t=ctypes.string_at(r).decode(); lib.flexicorp_pando_free(r); return st.value,json.loads(t)
check(lib.flexicorp_pando_api_version()==3,"api 3")
bj=json.loads(lib.flexicorp_pando_build_json()); print("build", bj["build_string"])
h=lib.flexicorp_pando_open(None,corpus.encode(),0); check(h,"open")
st,hh=req(h,"GET","/health"); check(st==200 and hh.get("embedded_in")=="flexicorp_pando","health")
st,r=req(h,"POST","/query",b={"query":'[upos="NOUN"]',"limit":3,"total":"async"}); check(st==200,"query")
job=r["result"]["job_id"]
for _ in range(300):
    st,s=req(h,"get","/status?job="+job)          # lowercase method, query in the path
    if s["result"]["finished"]: break
    time.sleep(0.02)
check(s["result"]["finished"],"status finished"); print("total",s["result"]["total"])
st,r=req(h,"POST","/run",b={"cql":'[upos="NOUN"]; count by lemma;',"group_limit":3}); check(st==200 and r["ok"],"run")
st,r=req(h,"GET","/values/upos/",q="limit=2"); check(st==200,"values trailing slash")
st,r=req(h,"POST","/query",b={"query":'[upos="NOUN"'}); check(st==400,"bad cql 400")
st,r=req(h,"POST","/query",b={"query":'[] [] []',"total":True,"timeout_ms":1}); check(st in (200,408),"timeout"); print("timeout probe",st)
p=lib.flexicorp_pando_query(h,b'[lemma="house"]',0,3,0,5,None); legacy=json.loads(ctypes.string_at(p)); lib.flexicorp_pando_free(p)
check(legacy.get("success") is True,"legacy query envelope")
p=lib.flexicorp_pando_query(h,b'[upos="NOUN"]; count by upos;',0,3,0,5,None); legacy=json.loads(ctypes.string_at(p)); lib.flexicorp_pando_free(p)
check(legacy.get("success") is True,"legacy program")
# concurrency: 8 threads x mixed requests, compare to baseline
reqs=[("POST","/query",None,{"query":'a:[upos="VERB"] > b:[upos="NOUN"]',"limit":5,"total":True}),
      ("POST","/query",None,{"query":'head:[upos="VERB"] sub:dep_subtree(head)',"limit":5}),
      ("POST","/query",None,{"query":'[upos="DET"] [upos="ADJ"]? [upos="NOUN"]',"limit":5,"total":True}),
      ("POST","/run",None,{"cql":'[upos="ADJ"]; count by lemma;',"group_limit":5}),
      ("GET","/context","pos=100&left=3&right=3",None)]
import re
mask=lambda s: re.sub(r'"elapsed_ms": ?[0-9.e+-]+','',json.dumps(s,sort_keys=True))
base=[mask(req(h,*x)[1]) for x in reqs]
bad=[0]
def worker(t):
    for k in range(10):
        for i,x in enumerate(reqs):
            j=(i+t+k)%len(reqs); st,r=req(h,*reqs[j])
            if mask(r)!=base[j]: bad[0]+=1
ts=[threading.Thread(target=worker,args=(t,)) for t in range(8)]
t0=time.time(); [t.start() for t in ts]; time.sleep(0.2); busy=lib.flexicorp_pando_busy(h); [t.join() for t in ts]
print(f"concurrency: {8*10*len(reqs)} requests in {time.time()-t0:.1f}s, mismatches {bad[0]}, busy during {busy}")
check(bad[0]==0,"concurrency mismatches"); check(busy>=1,"busy during requests")
check(lib.flexicorp_pando_busy(h)==0,"busy after"); check(lib.flexicorp_pando_idle_seconds(h)>=0,"idle")
lib.flexicorp_pando_close(h)
check(lib.flexicorp_pando_open(None,b"/nonexistent",0) is None and lib.flexicorp_pando_last_error(None),"open failure")
print("adapter_test:", "FAILED" if fails else "all passed"); sys.exit(1 if fails else 0)
