# Windows PowerShell5.1 implementation. Native execution is NOT_RUN in this offline task.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$script:Utf8=[Text.UTF8Encoding]::new($false,$true)
$script:Deadline=[DateTime]::UtcNow.AddMinutes(5)
if(-not ('WitchItUpdater.ZipGuard' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'ZipGuard.cs')}
if(-not ('WitchItUpdater.JsonGuard' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'StrictJson.cs')}
function Require([bool]$Ok,[string]$Why){if(!$Ok){throw $Why}}
function Integral($Value){return ($Value -is [int] -or $Value -is [long])}
function Sha([string]$Path){return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function BytesSha([byte[]]$Bytes){$s=[Security.Cryptography.SHA256]::Create();try{return ([BitConverter]::ToString($s.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant()}finally{$s.Dispose()}}
function ReadStrict([string]$Path,[int]$Limit=4194304){$b=[IO.File]::ReadAllBytes($Path);Require ($b.Length -le $Limit) 'Metadata size';$s=$script:Utf8.GetString($b);[WitchItUpdater.JsonGuard]::Validate($s);return ($s|ConvertFrom-Json)}
function Has($Object,[string]$Key){return $null -ne $Object.PSObject.Properties[$Key]}
function Prop($Object,[string]$Key){Require (Has $Object $Key) ('Missing '+$Key);return $Object.PSObject.Properties[$Key].Value}
function Fields($Object,[string[]]$Names){Require ($Object -is [pscustomobject]) 'Object required';Require ((@($Object.PSObject.Properties.Name|Sort-Object)-join '|') -ceq (@($Names|Sort-Object)-join '|')) 'Metadata fields'}
function Ver([string]$Value){Require ($Value -cmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') 'Stable SemVer required';return [version]$Value}
function Compatible([string]$Value,[string]$Range){$v=Ver $Value;if($Range -cmatch '^>=(\d+\.\d+\.\d+) <(\d+\.\d+\.\d+)$'){return $v -ge (Ver $Matches[1]) -and $v -lt (Ver $Matches[2])};return $v -eq (Ver $Range)}
function SafeRel([string]$Rel,[switch]$AllowProtected){
 Require ($Rel.Length -gt 0 -and $Rel.Length -le 240 -and $Rel -cmatch '^[A-Za-z0-9_./-]+$' -and !$Rel.StartsWith('/')) 'Unsafe path'
 foreach($part in $Rel.Split('/')){Require ($part -notin @('','.','..') -and !$part.EndsWith('.') -and $part.Split('.')[0] -notmatch '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$') 'Unsafe Windows path';if(!$AllowProtected){Require ($part -notin @('Saved','SaveGames','WitchItRecovery','.witchit-updater','witchit-update-request.json','witchit-updater-settings.json')) 'Protected path'}}
 if(!$AllowProtected){Require ($Rel -notmatch '\.(pl|fapteleports|faphidden|fapship|fapcatalog)$') 'Protected companion'}
}
function NoReparse([string]$Path){$p=[IO.Path]::GetFullPath($Path);while($p){if(Test-Path -LiteralPath $p){Require (((Get-Item -Force -LiteralPath $p).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Reparse refused'};$up=[IO.Directory]::GetParent($p);if($null -eq $up){break};$p=$up.FullName}}
function JoinSafe([string]$Root,[string]$Rel,[switch]$AllowProtected){SafeRel $Rel -AllowProtected:$AllowProtected;$p=[IO.Path]::GetFullPath((Join-Path $Root $Rel.Replace('/','\')));Require ($p.StartsWith($Root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) 'Path escaped';NoReparse $p;return $p}
function Under([string]$Rel,$Roots){foreach($r in @($Roots)){if($Rel -ieq $r -or $Rel.StartsWith($r+'/',[StringComparison]::OrdinalIgnoreCase)){return $true}};return $false}
function WriteAtomic([string]$Path,[byte[]]$Bytes){NoReparse $Path;$dir=Split-Path -Parent $Path;[IO.Directory]::CreateDirectory($dir)|Out-Null;$tmp=Join-Path $dir ('.updater-'+[guid]::NewGuid().ToString('N')+'.tmp');$f=[IO.File]::Open($tmp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None);try{$f.Write($Bytes,0,$Bytes.Length);$f.Flush($true)}finally{$f.Dispose()};try{if(Test-Path -LiteralPath $Path){[IO.File]::Replace($tmp,$Path,[NullString]::Value)}else{[IO.File]::Move($tmp,$Path)}}finally{if(Test-Path -LiteralPath $tmp){Remove-Item -LiteralPath $tmp -Force}}}
function WriteJson([string]$Path,$Value){$text=ConvertTo-Json -InputObject $Value -Depth 40 -Compress;Require ($script:Utf8.GetByteCount($text) -le 4194304) 'Journal bound';WriteAtomic $Path ($script:Utf8.GetBytes($text))}
function ResolveVerifierTrust($Trust){
 Fields $Trust @('contract_path','public_key');Require ($Trust.contract_path -ceq 'verifier-contract.json') 'Fixed verifier contract required'
 $c=ReadStrict (Join-Path $PSScriptRoot 'verifier-contract.json') 16384
 Fields $c @('schema','product','upstream','version','platform','relative_path','sha256','sha256_status','interface','argv','valid_exit','invalid_signature_exit','other_exit','production_ready')
 Require ($c.schema -eq 1 -and $c.product -ceq 'minisign' -and $c.upstream -ceq 'jedisct1/minisign' -and $c.version -ceq '0.11' -and $c.platform -ceq 'windows-x64' -and $c.relative_path -ceq 'bin/minisign.exe') 'Verifier identity/version/path'
 Require (($c.argv -join '|') -ceq '-Vm|{file}|-x|{sig}|-P|{pubkey}' -and $c.valid_exit -eq 0 -and $c.invalid_signature_exit -eq 1) 'Verifier interface'
 Require ($c.sha256 -is [string] -and $c.sha256 -cmatch '^[a-f0-9]{64}$') 'Trusted minisign SHA256 TODO; refusing verification'
 return [pscustomobject]@{verifier=(JoinSafe $PSScriptRoot 'bin/minisign.exe');verifier_sha256=$c.sha256;public_key=$Trust.public_key}
}
function AssertVerifierReady($Trust){
 $resolved=ResolveVerifierTrust $Trust
 Require (Test-Path -LiteralPath $resolved.verifier -PathType Leaf) 'Trusted minisign unavailable; no download'
 NoReparse $resolved.verifier;Require ((Sha $resolved.verifier) -ceq $resolved.verifier_sha256) 'Trusted verifier SHA mismatch'
 Require ($resolved.public_key -is [string] -and $resolved.public_key -cmatch '^[A-Za-z0-9+/]{56}$') 'Public verification key not supplied'
 return $resolved
}
function VerifySignature([string]$Path,[string]$Signature,$Trust){
 $Trust=AssertVerifierReady $Trust
 Require ([DateTime]::UtcNow -lt $script:Deadline) 'Verification time budget'
 Require (Test-Path -LiteralPath $Trust.verifier -PathType Leaf) 'Trusted minisign unavailable';NoReparse $Trust.verifier;Require ((Sha $Trust.verifier) -ceq $Trust.verifier_sha256) 'Verifier SHA';Require ((Get-Item -LiteralPath $Signature).Length -le 16384) 'Signature bound'
 Require ($Trust.public_key -cmatch '^[A-Za-z0-9+/]{56}$') 'Public key format'
 $out=Join-Path (Split-Path -Parent $Path) ([guid]::NewGuid().ToString('N')+'.verify.out');$err=$out+'.err'
 # Only fixed verifier arguments; no shell or auth commands. Paths are already controlled.
 $args=@('-Vm',('"'+$Path+'"'),'-x',('"'+$Signature+'"'),'-P',$Trust.public_key)
 $p=Start-Process -FilePath $Trust.verifier -ArgumentList $args -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
 $handle=$p.Handle;$ticks=$p.StartTime.ToUniversalTime().Ticks
 if(!$p.WaitForExit(20000)){Require ($p.StartTime.ToUniversalTime().Ticks -eq $ticks) 'Verifier identity changed';$p.Kill();$p.WaitForExit();throw 'Verifier timeout'}
 Require ($p.ExitCode -is [int] -and $p.ExitCode -in @(0,1)) 'Verifier infrastructure failure';Require ($p.ExitCode -eq 0) 'Signature rejected'
}
function Download([string]$Url,[string]$Path,[long]$Limit,[bool]$Mock){
 Require (!(Test-Path -LiteralPath $Path)) 'Fresh download required';[IO.Directory]::CreateDirectory((Split-Path -Parent $Path))|Out-Null
 $uri=[uri]$Url;$response=$null;$file=$null;$stream=$null;$part=$Path+'.part';$watch=[Diagnostics.Stopwatch]::StartNew()
 try{
  for($redirect=0;$redirect -le 3;$redirect++){
   Require ([DateTime]::UtcNow -lt $script:Deadline) 'Total updater network budget'
   $hosts=if($redirect -eq 0){@('github.com')}else{@('github.com','release-assets.githubusercontent.com','objects.githubusercontent.com')}
   Require ((!$uri.UserInfo) -and (($Mock -and $uri.Scheme -ceq 'http' -and $uri.Host -ceq '127.0.0.1') -or (!$Mock -and $uri.Scheme -ceq 'https' -and $uri.Host -cin $hosts))) 'Transport origin'
   $rq=[Net.HttpWebRequest]::Create($uri);$rq.AllowAutoRedirect=$false;$rq.Timeout=20000;$rq.ReadWriteTimeout=20000;$response=$rq.GetResponse()
   if([int]$response.StatusCode -in @(301,302,303,307,308)){Require (!$Mock -and $redirect -lt 3) 'Redirect refused';$next=[uri]::new($uri,$response.Headers['Location']);$response.Dispose();$response=$null;$uri=$next;continue}
   Require ([int]$response.StatusCode -eq 200) 'Download status';break
  }
  Require ($null -ne $response -and [int]$response.StatusCode -eq 200 -and $response.ContentLength -ge 0 -and $response.ContentLength -le $Limit) 'Download size/status'
  $stream=$response.GetResponseStream();$file=[IO.File]::Open($part,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None);$buf=New-Object byte[] 65536;$total=[long]0
  while(($n=$stream.Read($buf,0,$buf.Length)) -gt 0){$total+=$n;Require ($total -le $Limit -and $watch.Elapsed.TotalSeconds -le 180 -and [DateTime]::UtcNow -lt $script:Deadline) 'Download budget';$file.Write($buf,0,$n)}
  Require ($total -eq $response.ContentLength) 'Truncated download';$file.Flush($true)
 }catch{throw 'Download refused (origin/status/size/time); payload unchanged'}finally{if($file){$file.Dispose()};if($stream){$stream.Dispose()};if($response){$response.Dispose()}}
 [IO.File]::Move($part,$Path)
}
function AssertCatalog($C,[string]$Repository,[long]$Seen){Fields $C @('schema','repository','sequence','releases');Require ((Integral $C.schema) -and $C.schema -eq 1 -and $C.repository -ceq $Repository -and ($C.sequence -is [int] -or $C.sequence -is [long])) 'Catalog shape';Require ($C.repository -ceq $Repository -and $C.schema -eq 1 -and $C.sequence -ge $Seen) 'Catalog sequence/identity';$versions=@{};Require ($C.releases -is [array] -and @($C.releases).Count -le 4096) 'Catalog bound';foreach($e in @($C.releases)){Fields $e @('version','tag','manifest','signature','manifest_sha256');Require ($e.version -is [string]) 'Version type';$null=Ver $e.version;Require (!$versions.ContainsKey($e.version)) 'Duplicate catalog version';$versions[$e.version]=$true;foreach($k in @('tag','manifest','signature')){SafeRel $e.$k -AllowProtected;Require (!$e.$k.Contains('/')) 'Asset basename'};Require ($e.manifest_sha256 -cmatch '^[a-f0-9]{64}$') 'Manifest SHA'}}
function AssertManifest($M,$Entry,[string]$Id,$Cfg){
 Fields $M @('schema','repository','package_id','version','source_commit','platform','updater_protocol','requires','asset','files','removals','migration_policy')
 Require ((Integral $M.schema) -and $M.schema -eq 1 -and $M.repository -ceq $Cfg.repository -and $M.package_id -ceq $Id -and $M.version -ceq $Entry.version) 'Manifest identity';Require ($M.version -is [string]) 'Version type';$null=Ver $M.version
 Require ($M.source_commit -cmatch '^[a-f0-9]{40}$' -and $M.platform -ceq 'windows-x64' -and (Integral $M.updater_protocol) -and $M.updater_protocol -eq 1 -and $M.migration_policy -ceq 'payload-only-reversible') 'Manifest compatibility'
 if($Id -ceq 'core'){Fields $M.requires @()}else{Fields $M.requires @('core','runtime_abi');$null=Compatible '1.0.0' $M.requires.core}
 Fields $M.asset @('name','bytes','sha256');SafeRel $M.asset.name -AllowProtected;Require (!$M.asset.name.Contains('/') -and $M.asset.sha256 -cmatch '^[a-f0-9]{64}$' -and $M.asset.bytes -gt 0 -and $M.asset.bytes -le 134217728) 'Asset bound'
 $seen=@{};$size=[long]0;Require ($M.files -is [array] -and @($M.files).Count -gt 0 -and @($M.files).Count -le 4096) 'Payload count'
 foreach($f in @($M.files)){Fields $f @('path','bytes','sha256','mode');Require ($f.path -is [string] -and $f.sha256 -is [string] -and $f.mode -is [string] -and ($f.bytes -is [int] -or $f.bytes -is [long])) 'File field types';SafeRel $f.path;Require ($f.path -notmatch '(^|/)(PrivateCosmetics|CreativeUnlocks)(/|$)') 'Private payload in public updater';Require ((Under $f.path $Cfg.roots) -and !$seen.ContainsKey($f.path)) 'Payload ownership/duplicate';$seen[$f.path]=$true;Require ($f.sha256 -cmatch '^[a-f0-9]{64}$' -and $f.bytes -ge 0 -and $f.mode -cin @('managed','seed')) 'File hash/mode';$size+=$f.bytes}
 Require ($size -le 536870912) 'Expanded bound';foreach($a in $seen.Keys){foreach($b in $seen.Keys){Require (!$b.StartsWith($a+'/',[StringComparison]::OrdinalIgnoreCase)) 'Payload prefix collision'}}
 Fields $M.removals @('files','packages');Require ($M.removals.files -is [array] -and $M.removals.packages -is [array] -and @($M.removals.packages).Count -eq 0) 'Recursive removals need P27 ownership contract';$removed=@{}
 foreach($r in @($M.removals.files)){SafeRel $r;Require ((Under $r $Cfg.roots) -and !$seen.ContainsKey($r) -and !$removed.ContainsKey($r)) 'Removal ownership/conflict';$removed[$r]=$true}
}
function ExtractVerified([string]$Zip,$M,[string]$Stage){
 Require ((Get-Item -LiteralPath $Zip).Length -eq $M.asset.bytes -and (Sha $Zip) -ceq $M.asset.sha256) 'ZIP hash/size';Require (!(Test-Path -LiteralPath $Stage)) 'Fresh staging'
 [WitchItUpdater.ZipGuard]::Validate($Zip);Add-Type -AssemblyName System.IO.Compression.FileSystem;$z=[IO.Compression.ZipFile]::OpenRead($Zip)
 try{
  Require ($z.Entries.Count -le 8192) 'ZIP entry budget';$wanted=@{};foreach($f in @($M.files)){$wanted['Win64/'+$f.path]=$f};$seen=@{};$files=@{}
  foreach($e in $z.Entries){$name=$e.FullName;$dir=$name.EndsWith('/');$plain=if($dir){$name.Substring(0,$name.Length-1)}else{$name};SafeRel $plain;Require ($plain.StartsWith('Win64/',[StringComparison]::Ordinal) -and !$seen.ContainsKey($plain)) 'ZIP path/duplicate';$seen[$plain]=$true
   $kind=($e.ExternalAttributes -shr 16) -band 61440;Require ($kind -in @(0,32768,16384)) 'ZIP special/link'
   if($dir){Require (@($wanted.Keys|Where-Object{$_.StartsWith($plain+'/',[StringComparison]::Ordinal)}).Count -gt 0) 'Unlisted directory';continue}
   Require ($wanted.ContainsKey($name) -and $e.Length -eq $wanted[$name].bytes) 'ZIP inventory/size';$files[$name]=$e
  }
  Require ($files.Count -eq $wanted.Count) 'Missing ZIP file'
  foreach($name in $files.Keys){$f=$wanted[$name];$dest=JoinSafe $Stage $f.path;[IO.Directory]::CreateDirectory((Split-Path -Parent $dest))|Out-Null;$input=$files[$name].Open();$output=[IO.File]::Open($dest,[IO.FileMode]::CreateNew);$buf=New-Object byte[] 65536;$total=[long]0
   try{while(($n=$input.Read($buf,0,$buf.Length)) -gt 0){$total+=$n;Require ($total -le $f.bytes) 'Expanded limit';$output.Write($buf,0,$n)};$output.Flush($true)}finally{$input.Dispose();$output.Dispose()};Require ((Sha $dest) -ceq $f.sha256) 'Extracted file SHA'
  }
 }finally{$z.Dispose()}
}
function ReadRequest([string]$Path){
 $r=ReadStrict $Path 16384;Fields $r @('request','packages','created_utc');Require ($r.request -cin @('exact','latest')) 'Request mode';Require ($r.packages -is [pscustomobject]) 'Package map'
 foreach($p in $r.packages.PSObject.Properties){Require ([WitchItUpdater.PackageIds]::IsPublic($p.Name)) 'Unknown/noncanonical/private package ID';Require ($p.Value -is [string]) 'Version type';$null=Ver $p.Value}
 if($r.request -ceq 'latest'){Require (@($r.packages.PSObject.Properties).Count -eq 0) 'latest must be empty'}else{Require (@($r.packages.PSObject.Properties.Name|Where-Object{$_ -ceq 'Core'}).Count -eq 1) 'Exact canonical Core required'}
 Require ($r.created_utc -cmatch '^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$') 'Request UTC';$t=[DateTimeOffset]::ParseExact($r.created_utc,'yyyy-MM-ddTHH:mm:ssZ',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::AssumeUniversal);$age=([DateTimeOffset]::UtcNow-$t).TotalSeconds;Require ($age -ge -300 -and $age -le 604800) 'Request age';return $r
}
function ToBundleRequest($Request){
 Fields $Request @('request','packages','created_utc');Require ($Request.request -cin @('exact','latest')) 'Request mode';Require ($Request.packages -is [pscustomobject]) 'Package map';if($Request.request -ceq 'latest'){Require (@($Request.packages.PSObject.Properties).Count -eq 0) 'latest must be empty'}else{Require (@($Request.packages.PSObject.Properties.Name|Where-Object{$_ -ceq 'Core'}).Count -eq 1) 'Exact canonical Core required'};$packages=[ordered]@{}
 foreach($p in $Request.packages.PSObject.Properties){Require ([WitchItUpdater.PackageIds]::IsPublic($p.Name)) 'Noncanonical ID at namespace boundary';$packages[[WitchItUpdater.PackageIds]::Bundle($p.Name)]=$p.Value}
 return [pscustomobject]@{request=$Request.request;packages=[pscustomobject]$packages;created_utc=$Request.created_utc}
}
function FullMap([string]$Root){
 $map=@{};function Walk([string]$P){NoReparse $P;$r=$P.Substring($Root.TrimEnd('\').Length+1).Replace('\','/');if($r -ceq 'ue4ss/.witchit-updater' -or $r -ceq 'ue4ss/witchit-update-request.json'){return};$item=Get-Item -Force -LiteralPath $P
  if($item.PSIsContainer){$map[$r]=[ordered]@{type='dir'};foreach($c in @(Get-ChildItem -Force -LiteralPath $P)){Walk $c.FullName}}else{$map[$r]=[ordered]@{type='file';bytes=[long]$item.Length;sha256=(Sha $P)}}
 };foreach($c in @(Get-ChildItem -Force -LiteralPath $Root)){Walk $c.FullName};return $map
}
function MapKey($Map){return (@(foreach($k in @($Map.Keys|Sort-Object)){$k+'|'+(ConvertTo-Json -InputObject $Map[$k] -Compress)}) -join "`n")}
function PSMap($Object){$h=@{};foreach($p in $Object.PSObject.Properties){$h[$p.Name]=$p.Value};return $h}
function RestoreTransaction([string]$Target,$J,[string]$Journal){
 $before=PSMap $J.before;$now=FullMap $Target
 foreach($k in $now.Keys){if(!$before.ContainsKey($k)){Require ($k -in @($J.added)+@($J.added_dirs)) 'New foreign data blocks exact recovery'}}
 foreach($k in @($before.Keys|Sort-Object Length)){$r=$before[$k];$dest=JoinSafe $Target $k -AllowProtected;if($r.type -ceq 'dir'){[IO.Directory]::CreateDirectory($dest)|Out-Null}else{$source=JoinSafe $J.backup $k -AllowProtected;Require ((Sha $source) -ceq $r.sha256) 'Backup corruption';if((Test-Path -LiteralPath $dest -PathType Leaf) -and (Sha $dest) -ceq $r.sha256){continue};Require ($k -in @($J.touched)) 'Changed foreign data blocks recovery';WriteAtomic $dest ([IO.File]::ReadAllBytes($source))}}
 foreach($k in @($now.Keys|Sort-Object Length -Descending)){if(!$before.ContainsKey($k)){$q=JoinSafe $Target $k -AllowProtected;if(Test-Path -LiteralPath $q -PathType Container){[IO.Directory]::Delete($q)}else{[IO.File]::Delete($q)}}}
 Require ((MapKey (FullMap $Target)) -ceq (MapKey $before)) 'Rollback equality';WriteJson $J.state_path $J.state_before;$J.status='ROLLED_BACK_VERIFIED';WriteJson $Journal $J;[IO.File]::Delete((Join-Path (Split-Path -Parent $Journal) 'launch-blocked'))
}
function SaveStatus([string]$Control,[string]$Code,[string]$Message,[bool]$Launch){WriteJson (Join-Path $Control 'last-result.json') ([ordered]@{status=$Code;message=$Message;launch_allowed=$Launch;utc=[DateTime]::UtcNow.ToString('o')})}
function ApplyTransaction([string]$Target,[string]$Control,$State,$Desired,$Plan,$Stages){
 $before=FullMap $Target;$nonce=[guid]::NewGuid().ToString('N');$backup=Join-Path $Control ('backups/'+$nonce);$touched=@{};$remove=@{};$expected=@{};foreach($k in $before.Keys){$expected[$k]=$before[$k]}
 foreach($id in $Plan.Keys){$m=$Plan[$id];$oldFiles=@{};if(Has $State.packages $id){$oldFiles=PSMap (Prop (Prop $State.packages $id) 'files')}
  foreach($f in @($m.files)){$p=$f.path;$q=JoinSafe $Target $p;Require (!$touched.ContainsKey($p)) 'Cross-package collision'
   if(Test-Path -LiteralPath $q){Require (Test-Path -LiteralPath $q -PathType Leaf) 'Payload directory collision';if($f.mode -ceq 'seed'){continue};Require ($oldFiles.ContainsKey($p) -and (Sha $q) -ceq $oldFiles[$p]) 'Unowned/changed managed file'}
   $touched[$p]=JoinSafe $Stages[$id] $p;$expected[$p]=[ordered]@{type='file';bytes=[long]$f.bytes;sha256=$f.sha256}
  }
  foreach($p in @($m.removals.files)){Require ($oldFiles.ContainsKey($p)) 'Removal lacks ownership';$q=JoinSafe $Target $p;if(Test-Path -LiteralPath $q){Require ((Test-Path -LiteralPath $q -PathType Leaf) -and (Sha $q) -ceq $oldFiles[$p]) 'Changed removal';$remove[$p]=$true;$expected.Remove($p)}}
 }
 $added=@($touched.Keys|Where-Object{!$before.ContainsKey($_)});$addedDirs=@{}
 foreach($p in $touched.Keys){$parent=$p;while($parent.Contains('/')){$parent=$parent.Substring(0,$parent.LastIndexOf('/'));if(!$before.ContainsKey($parent)){$addedDirs[$parent]=$true;$expected[$parent]=[ordered]@{type='dir'}}}}
 $size=[long]0;foreach($k in $before.Keys){$r=$before[$k];$q=JoinSafe $backup $k -AllowProtected;if($r.type -ceq 'dir'){[IO.Directory]::CreateDirectory($q)|Out-Null}else{$size+=$r.bytes;Require ($size -le 1073741824) 'Backup budget';WriteAtomic $q ([IO.File]::ReadAllBytes((JoinSafe $Target $k -AllowProtected)));Require ((Sha $q) -ceq $r.sha256) 'Backup hash'}}
 Require ((MapKey (FullMap $Target)) -ceq (MapKey $before)) 'Target drift'
 $journal=Join-Path $Control 'journal.json';$j=[ordered]@{after=$expected;status='BACKUP_VERIFIED';before=$before;state_before=$State;backup=$backup;state_path=(Join-Path $Control 'state.json');added=$added;added_dirs=@($addedDirs.Keys);touched=@($touched.Keys)+@($remove.Keys);plan=@($Plan.Keys)};WriteJson $journal $j;WriteAtomic (Join-Path $Control 'launch-blocked') ($script:Utf8.GetBytes('RECOVERY GATE'))
 try{
  foreach($id in @('core')+@($Plan.Keys|Where-Object{$_ -cne 'core'}|Sort-Object)){
   if(!$Plan.ContainsKey($id)){continue};$j.status=if($id -ceq 'core'){'APPLYING_CORE'}else{'APPLYING_FEATURES'};WriteJson $journal $j
   foreach($p in @($Plan[$id].removals.files)){if($remove.ContainsKey($p)){[IO.File]::Delete((JoinSafe $Target $p))}}
   foreach($f in @($Plan[$id].files)){if($touched.ContainsKey($f.path)){WriteAtomic (JoinSafe $Target $f.path) ([IO.File]::ReadAllBytes($touched[$f.path]))}}
  }
  Require ((MapKey (FullMap $Target)) -ceq (MapKey $expected)) 'Installed inventory';$j.status='INSTALLED_VERIFIED';WriteJson $journal $j;WriteJson $j.state_path $Desired;$j.status='COMMITTED';WriteJson $journal $j;[IO.File]::Delete((Join-Path $Control 'launch-blocked'))
 }catch{
  $original=$_.Exception.Message
  try{RestoreTransaction $Target (ReadStrict $journal) $journal}catch{$j.status='RECOVERY_REQUIRED';WriteJson $journal $j;throw 'Recovery required; launch blocked'}
  throw ('Update failed; previous state verified: '+$original)
 }
}

Export-ModuleMember -Function *
