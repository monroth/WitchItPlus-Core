"""Bounded updater protocol shared by publishing and the OFFLINE fake-tree model.
The Windows client is updater.ps1; this module never installs to a real game.
"""
from pathlib import Path, PurePosixPath
import hashlib,json,re,stat,subprocess,tempfile,urllib.request,urllib.parse,zipfile

MAX_JSON=4*1024*1024
CANONICAL_PUBLIC_IDS=('Core','BaseWorldPlus','FillAPotPlus','MatchRulesPlusPlus')
CANONICAL_TO_BUNDLE={'Core':'core','BaseWorldPlus':'editor-tweaks','FillAPotPlus':'fillapot-plus','MatchRulesPlusPlus':'matchrules-plus'}
class Refusal(ValueError): pass

def digest(data):return hashlib.sha256(data).hexdigest()
def canonical(value):return (json.dumps(value,sort_keys=True,ensure_ascii=True,separators=(',',':'))+'\n').encode()
def need(ok,message):
 if not ok:raise Refusal(message)
def strict(data,limit=MAX_JSON):
 need(type(data)is bytes and len(data)<=limit,'metadata size')
 def pairs(rows):
  result={};seen=set()
  for key,val in rows:
   need(key.casefold()not in seen,'duplicate JSON key');seen.add(key.casefold());result[key]=val
  return result
 try:
  value=json.loads(data.decode('utf-8'),object_pairs_hook=pairs,parse_constant=lambda _:(_ for _ in()).throw(Refusal('non-finite JSON')))
 except (UnicodeError,json.JSONDecodeError,RecursionError)as e:raise Refusal('invalid JSON')from e
 def walk(x,n=0):
  need(n<=20,'JSON depth')
  if isinstance(x,dict):
   for v in x.values():walk(v,n+1)
  elif isinstance(x,list):
   for v in x:walk(v,n+1)
 walk(value);return value

def fields(x,required,optional=()):
 need(type(x)is dict and set(required)<=set(x)and set(x)<=set(required)|set(optional),'metadata fields')
def sha(s):need(type(s)is str and re.fullmatch('[a-f0-9]{64}',s)is not None,'SHA256')
def version(s):
 need(type(s)is str and len(s)<=32 and re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)',s)is not None,'stable SemVer required')
 values=tuple(map(int,s.split('.')));need(all(v<=2147483647 for v in values),'version component budget');return values
def compatible(v,r):
 version(v);need(type(r)is str,'range type')
 if re.fullmatch(r'>=\d+\.\d+\.\d+ <\d+\.\d+\.\d+',r):
  lo,hi=r.split();return version(lo[2:])<=version(v)<version(hi[1:])
 return version(v)==version(r)
def relpath(s,protected=True):
 need(type(s)is str and 0<len(s)<=240 and re.fullmatch(r'[A-Za-z0-9_./-]+',s)is not None,'unsafe path')
 p=PurePosixPath(s);need(not p.is_absolute()and '/'.join(p.parts)==s,'noncanonical path')
 for part in s.split('/'):
  need(part not in ('','.','..')and not part.endswith(('.',' '))and part.split('.')[0].upper()not in {'CON','PRN','AUX','NUL',*[f'COM{i}'for i in range(1,10)],*[f'LPT{i}'for i in range(1,10)]},'Windows path')
  if protected:need(part.casefold()not in ('saved','savegames','witchitrecovery','.witchit-updater','witchit-update-request.json','witchit-updater-settings.json'),'protected path')
 if protected:need(not s.lower().endswith(('.pl','.fapteleports','.faphidden','.fapship','.fapcatalog')),'protected companion')
 return s

def under(path,roots):return any(path.casefold()==r.casefold()or path.casefold().startswith(r.casefold()+'/')for r in roots)
def no_links(p):
 for q in [p,*p.parents]:need(not q.is_symlink(),'symlink/reparse')
 if p.exists()and hasattr(p.stat(),'st_file_attributes'):need(not p.stat().st_file_attributes&1024,'reparse')

def request(data,public_ids,now=None):
 import datetime
 x=strict(data,16384);fields(x,['request','packages','created_utc']);need(x['request']in ('exact','latest'),'request mode')
 need(type(x['packages'])is dict and set(x['packages'])<=set(CANONICAL_PUBLIC_IDS)and set(x['packages'])<=set(public_ids),'unknown/noncanonical/private package ID')
 if x['request']=='latest':need(x['packages']=={},'latest packages must be empty')
 else:
  need('Core'in x['packages'],'exact canonical Core required')
  for v in x['packages'].values():version(v)
 need(type(x['created_utc'])is str and re.fullmatch(r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ',x['created_utc'])is not None,'UTC timestamp')
 try:t=datetime.datetime.fromisoformat(x['created_utc'].replace('Z','+00:00'))
 except ValueError as e:raise Refusal('UTC timestamp')from e
 if now is not None:need(-300<=(now-t).total_seconds()<=7*86400,'request timestamp')
 return x

def bundle_request(validated_request):
 """Explicit namespace crosswalk after validation; never accepts case aliases.
 Preserve the wire request separately; do not lower/casefold caller IDs.
 """
 ids=validated_request['packages'];need(type(ids)is dict and set(ids)<=set(CANONICAL_PUBLIC_IDS),'noncanonical request ID')
 if validated_request['request']=='exact':need('Core'in ids,'canonical Core required')
 else:need(validated_request['request']=='latest'and not ids,'invalid latest')
 return {**validated_request,'packages':{CANONICAL_TO_BUNDLE[k]:v for k,v in ids.items()}}

class Minisign:
 """Real external verifier; no key generation, no credential/auth reads."""
 def __init__(self,executable,executable_sha256,public_key):
  self.exe=Path(executable);self.hash=executable_sha256;self.key=public_key
 def verify(self,data,signature):
  need(self.exe.is_file(),'trusted minisign missing');sha(self.hash);need(digest(self.exe.read_bytes())==self.hash,'verifier hash')
  need(type(self.key)is str and re.fullmatch(r'[A-Za-z0-9+/]{56}',self.key)is not None and len(signature)<=16384,'signature/key')
  with tempfile.TemporaryDirectory(prefix='witchit-verify-')as t:
   p=Path(t);(p/'metadata').write_bytes(data);(p/'signature').write_bytes(signature)
   try:r=subprocess.run([str(self.exe),'-Vm',str(p/'metadata'),'-x',str(p/'signature'),'-P',self.key],capture_output=True,timeout=20)
   except (OSError,subprocess.TimeoutExpired)as e:raise Refusal('signature verifier failure')from e
   need(r.returncode==0,'signature verification failed')

class LoopbackTransport:
 """Test-only transport; production Windows client never selects this implicitly."""
 def __init__(self,base):
  u=urllib.parse.urlsplit(base);need(u.scheme=='http'and u.hostname=='127.0.0.1'and u.port is not None and not u.username and not u.password,'loopback-only mock')
  self.base=base.rstrip('/')
 def get(self,path,limit=MAX_JSON):
  relpath(path,False)
  class NoRedirect(urllib.request.HTTPRedirectHandler):
   def redirect_request(self,*_):raise Refusal('mock redirect refused')
  try:
   with urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect()).open(self.base+'/'+path,timeout=5)as r:
    data=r.read(limit+1);need(len(data)<=limit,'download size');return data
  except (OSError,urllib.error.URLError)as e:raise Refusal('download unavailable')from e

def catalog(data,signature,verify,repo,seen):
 verify.verify(data,signature);c=strict(data,1048576);fields(c,['schema','repository','sequence','releases'])
 need(type(c['schema'])is int and c['schema']==1 and c['repository']==repo and type(c['sequence'])is int and c['sequence']>=seen,'catalog identity/sequence')
 need(type(c['releases'])is list and len(c['releases'])<=4096,'catalog releases');found={}
 for e in c['releases']:
  fields(e,['version','tag','manifest','signature','manifest_sha256']);version(e['version']);need(e['version']not in found,'duplicate version');sha(e['manifest_sha256'])
  for k in ['tag','manifest','signature']:relpath(e[k],False);need('/'not in e[k],'asset basename')
  found[e['version']]=e
 return c,found

def manifest(data,signature,verify,entry,package,repository,roots):
 need(digest(data)==entry['manifest_sha256'],'manifest digest');verify.verify(data,signature);m=strict(data)
 fields(m,['schema','repository','package_id','version','source_commit','platform','updater_protocol','requires','asset','files','removals','migration_policy'])
 need(type(m['schema'])is int and m['schema']==1 and m['repository']==repository and m['package_id']==package and m['version']==entry['version'],'manifest identity')
 version(m['version']);need(type(m['source_commit'])is str and re.fullmatch('[a-f0-9]{40}',m['source_commit']),'source commit')
 need(m['platform']=='windows-x64'and type(m['updater_protocol'])is int and m['updater_protocol']==1 and m['migration_policy']=='payload-only-reversible','compatibility/protocol/migration')
 fields(m['requires'],[]if package=='core'else['core','runtime_abi']);need(package!='core'or m['requires']=={},'core dependencies')
 if package!='core':compatible('1.0.0',m['requires']['core']);need(type(m['requires']['runtime_abi'])is str,'ABI')
 fields(m['asset'],['name','bytes','sha256']);relpath(m['asset']['name'],False);need('/'not in m['asset']['name'],'asset basename');sha(m['asset']['sha256']);need(type(m['asset']['bytes'])is int and 0<m['asset']['bytes']<=128*1024*1024,'ZIP size')
 need(type(m['files'])is list and 0<len(m['files'])<=4096,'file count');seen=set();size=0
 for f in m['files']:
  fields(f,['path','bytes','sha256','mode']);p=relpath(f['path']);need(not any(x.casefold()in ('privatecosmetics','creativeunlocks')for x in p.split('/')),'private payload in public updater');need(under(p,roots),'package ownership');need(p.casefold()not in seen,'duplicate payload path');seen.add(p.casefold());sha(f['sha256']);need(type(f['bytes'])is int and f['bytes']>=0,'file bytes');size+=f['bytes'];need(f['mode']in ('managed','seed'),'file mode')
 for a in seen:
  need(not any(b.startswith(a+'/')for b in seen),'file/directory collision')
 need(size<=512*1024*1024,'expanded size');fields(m['removals'],['files','packages']);need(m['removals']['packages']==[],'recursive package removal requires P27 inventory contract')
 removed=set()
 for p in m['removals']['files']:
  relpath(p);need(under(p,roots)and p.casefold()not in seen and p.casefold()not in removed,'removal ownership/conflict');removed.add(p.casefold())
 return m

def unpack(data,m,destination):
 import io
 need(len(data)==m['asset']['bytes']and digest(data)==m['asset']['sha256'],'ZIP hash/size');need(not destination.exists(),'fresh staging required')
 wanted={'Win64/'+f['path']:f for f in m['files']};found=set();out={}
 try:
  with zipfile.ZipFile(io.BytesIO(data))as z:
   need(len(z.infolist())<=8192,'ZIP entry budget')
   for e in z.infolist():
    n=e.filename;directory=n.endswith('/');p=n[:-1]if directory else n;relpath(p);need(p.startswith('Win64/'),'ZIP prefix');fold=p.casefold();need(fold not in found,'duplicate ZIP entry');found.add(fold)
    mode=e.external_attr>>16;need(not e.extra,'ZIP extra fields unsupported');need(not e.flag_bits&1 and stat.S_IFMT(mode)in (0,stat.S_IFREG,stat.S_IFDIR),'ZIP link/special/encrypted');need(e.compress_type in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED),'ZIP compression')
    if directory:need(any(x.startswith(p+'/')for x in wanted),'unlisted directory');continue
    need(n in wanted,'unlisted ZIP file');f=wanted[n];need(e.file_size==f['bytes'],'expanded file size');b=z.read(e);need(len(b)==f['bytes']and digest(b)==f['sha256'],'payload hash');out[n]=b
 except (zipfile.BadZipFile,RuntimeError,NotImplementedError)as e:raise Refusal('invalid ZIP')from e
 need(set(out)==set(wanted),'missing ZIP file')
 for n,b in out.items():
  q=destination/PurePosixPath(n).relative_to('Win64');no_links(q);q.parent.mkdir(parents=True,exist_ok=True);q.write_bytes(b)
 return destination

def resolve(available,installed,req,abi):
 selected=({'core'}|{k for k,v in installed.items()if v['active']and not v.get('private')})if req['request']=='latest'else set(req['packages'])
 need('core'in available and selected<=set(available),'selected package unavailable')
 def ok(plan):
  core=plan['core']['version']
  for k,m in plan.items():
   if k!='core'and(not compatible(core,m['requires']['core'])or m['requires']['runtime_abi']!=abi):return False
  return True
 if req['request']=='exact':
  try:plan={k:available[k][v]for k,v in req['packages'].items()}
  except KeyError as e:raise Refusal('exact version unavailable')from e
  need(ok(plan),'exact incompatible set');return plan
 import itertools,math
 need(math.prod(len(available[k])for k in selected)<=4096,'resolution budget')
 for core in sorted(available['core'],key=version,reverse=True):
  names=sorted(selected-{'core'});choices=[sorted(available[k],key=version,reverse=True)for k in names]
  for versions in itertools.product(*choices):
   plan={'core':available['core'][core],**{k:available[k][v]for k,v in zip(names,versions)}}
   if ok(plan)and all(k not in installed or version(m['version'])>=version(installed[k]['version'])for k,m in plan.items()):return plan
 raise Refusal('no compatible latest set')
