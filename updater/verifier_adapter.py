"""Pinned minisign0.11 win64 contract. No download, key creation or secret reads."""
from pathlib import Path
import subprocess,tempfile,re
from protocol import need,strict,digest,no_links,Refusal,fields
class VerifierContract:
 def __init__(self,root,contract=None):
  self.root=Path(root);self.contract=strict((self.root/'verifier-contract.json').read_bytes())if contract is None else contract
  c=self.contract;fields(c,['schema','product','upstream','version','platform','relative_path','sha256','sha256_status','interface','argv','valid_exit','invalid_signature_exit','other_exit','production_ready']);need(c['schema']==1 and c['argv']==['-Vm','{file}','-x','{sig}','-P','{pubkey}']and c['valid_exit']==0 and c['invalid_signature_exit']==1,'verifier interface contract');need(c['product']=='minisign'and c['version']=='0.11'and c['platform']=='windows-x64'and c['relative_path']=='bin/minisign.exe','verifier identity/path contract')
 def verify_files(self,file,signature,pubkey,runner=None):
  """Returns0/1 only for observed0/1; all infrastructure faults fail closed."""
  c=self.contract;pin=c['sha256'];need(type(pin)is str and re.fullmatch('[a-f0-9]{64}',pin)is not None,'trusted binary SHA256 not supplied')
  exe=self.root/'bin/minisign.exe';no_links(exe);need(exe.is_file()and digest(exe.read_bytes())==pin,'trusted binary missing/hash mismatch')
  need(type(pubkey)is str and re.fullmatch('[A-Za-z0-9+/]{56}',pubkey)is not None,'public verification key not supplied')
  file=Path(file);signature=Path(signature);no_links(file);no_links(signature);need(file.is_file()and signature.is_file()and 0<signature.stat().st_size<=16384,'signature input')
  argv=[str(exe),'-Vm',str(file),'-x',str(signature),'-P',pubkey]
  try:r=(runner or subprocess.run)(argv,capture_output=True,timeout=20)
  except (OSError,subprocess.TimeoutExpired)as e:raise Refusal('verifier unavailable/timeout')from e
  need(type(r.returncode)is int and r.returncode in (0,1),'unexpected verifier exit; not a valid signature result')
  return r.returncode
 def verify(self,data,signature,pubkey,runner=None):
  with tempfile.TemporaryDirectory(prefix='witchit-minisign-contract-')as tmp:
   p=Path(tmp);(p/'data').write_bytes(data);(p/'signature').write_bytes(signature)
   need(self.verify_files(p/'data',p/'signature',pubkey,runner)==0,'signature rejected')
