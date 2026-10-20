# Windows PowerShell5.1 client. No launch or secret/auth reads; exit0 allows wrapper,20 blocks.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$TargetWin64,[switch]$TestProfile)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$control=$null;$lock=$null;$mutation=$false;$controlEarly=$null;$claimed=$null
# OFF is the default. Only tiny local intent files are inspected before this fast path.
# No module/compiler, target scan, hash/signature, process query or network when disabled.
try{
 $early=[IO.Path]::GetFullPath($TargetWin64).TrimEnd('\')
 $pendingEarly=Join-Path $early 'ue4ss/witchit-update-request.json'
 $controlEarly=Join-Path $early 'ue4ss/.witchit-updater'
 $inflightEarly=Join-Path $controlEarly 'requests'
 $oneShot=[IO.File]::Exists($pendingEarly)
 $enabled=$false;$settingsEarly=Join-Path $early 'ue4ss/witchit-updater-settings.json'
 if([IO.File]::Exists($settingsEarly)){
  $info=[IO.FileInfo]::new($settingsEarly)
  if($info.Length -le 1024 -and ($info.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0){$text=[IO.File]::ReadAllText($settingsEarly,[Text.UTF8Encoding]::new($false,$true));$enabled=$text -cmatch '^\s*\{\s*"auto_update_enabled"\s*:\s*true\s*\}\s*$'}
 }
 if(!$enabled -and !$oneShot){
  if([IO.File]::Exists((Join-Path $controlEarly 'launch-blocked'))){Write-Warning 'Unconfirmed transaction; launch blocked. Journal is in ue4ss/.witchit-updater.';exit 20}
  exit 0
 }
}catch{if($controlEarly -and [IO.File]::Exists((Join-Path $controlEarly 'launch-blocked'))){exit 20};Write-Warning 'Invalid local updater intent; no checks or network performed';exit 0}
Import-Module (Join-Path $PSScriptRoot 'Updater.Core.psm1') -Force
try{
 Require ($PSVersionTable.PSVersion.Major -eq 5 -and $PSVersionTable.PSEdition -ceq 'Desktop' -and [Environment]::Is64BitProcess) 'WinPS5.1 Desktop x64 required'
 $target=[IO.Path]::GetFullPath($TargetWin64).TrimEnd('\');NoReparse $target;Require ((Split-Path -Leaf $target) -ieq 'Win64') 'Win64 target required'
 Require (Test-Path -LiteralPath (Join-Path $target 'PropWitchHuntModule-Win64-Shipping.exe') -PathType Leaf) 'Game filename required; never executed here'
 if($TestProfile){Require ([IO.File]::ReadAllText((Join-Path $target 'PropWitchHuntModule-Win64-Shipping.exe')) -ceq "FAKE-FILENAME-ONLY`n") 'Real EXE refused in test profile';Require ((Get-Content -Raw -LiteralPath (Join-Path $target '.witchit-fake')) -ceq "OFFLINE FIXTURE ONLY`n") 'Mock fixture marker'}
 $profile=ReadStrict (Join-Path $PSScriptRoot $(if($TestProfile){'test-profile.json'}else{'profile.json'}))
 Require ($profile.schema -eq 1 -and $profile.approved_package_ids_sha -cmatch '^[a-f0-9]{40}$' -and $profile.activation_adapter_ready -eq $true) 'P27 package IDs/bootstrap receipt/P28 activation adapter not configured'
 Require ($profile.test_only -eq [bool]$TestProfile) 'Test trust profile separation'
 $null=AssertVerifierReady $profile.trust # Fail before any catalog download while binary/hash/key TODO.
 $control=JoinSafe $target 'ue4ss/.witchit-updater' -AllowProtected;[IO.Directory]::CreateDirectory($control)|Out-Null;$lock=[IO.File]::Open((Join-Path $control 'lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
 $journal=Join-Path $control 'journal.json';$statePath=Join-Path $control 'state.json'
 if(Test-Path -LiteralPath $journal){$j=ReadStrict $journal;if($j.status -ceq 'COMMITTED' -and (Test-Path -LiteralPath (Join-Path $control 'launch-blocked'))){Require ((MapKey (FullMap $target)) -ceq (MapKey (PSMap $j.after))) 'Committed recovery drift';[IO.File]::Delete((Join-Path $control 'launch-blocked'))};if($j.status -cin @('BACKUP_VERIFIED','APPLYING_CORE','APPLYING_FEATURES','INSTALLED_VERIFIED','RECOVERY_REQUIRED')){
  Require ($j.backup.StartsWith($control+'\backups\',[StringComparison]::OrdinalIgnoreCase) -and $j.state_path -ceq $statePath) 'Journal scope';$mutation=$true;RestoreTransaction $target $j $journal;$mutation=$false
 }}
 Require (@(Get-Process|Where-Object{$_.ProcessName -match '^(WitchIt|PropWitchHuntModule)(-|$)'}).Count -eq 0) 'Game running; no update'
 Require (Test-Path -LiteralPath $statePath -PathType Leaf) 'Bootstrap/import receipt required';$state=ReadStrict $statePath;Fields $state @('schema','packages','seen');Require ($state.schema -eq 1 -and (Has $state.packages 'core')) 'State receipt'
 $pending=JoinSafe $target 'ue4ss/witchit-update-request.json' -AllowProtected;$claimed=$null
 $requests=Join-Path $control 'requests';$inflight=@();if(Test-Path -LiteralPath $requests){NoReparse $requests;$inflight=@(Get-ChildItem -LiteralPath $requests -File -Filter '*.json')};if((Test-Path -LiteralPath $pending) -and $inflight.Count){$history=Join-Path $requests 'superseded';[IO.Directory]::CreateDirectory($history)|Out-Null;foreach($old in $inflight){[IO.File]::Move($old.FullName,(Join-Path $history ($old.BaseName+'-'+[guid]::NewGuid().ToString('N')+'.json')))};$inflight=@()};Require ($inflight.Count -le 1) 'Multiple inflight requests'
 if($inflight.Count){$claimed=$inflight[0].FullName;Require ($inflight[0].BaseName -ceq (Sha $claimed)) 'Inflight digest';$wireRequest=ReadRequest $claimed;$req=ToBundleRequest $wireRequest}
 elseif(Test-Path -LiteralPath $pending){$wireRequest=ReadRequest $pending;$req=ToBundleRequest $wireRequest;$hash=Sha $pending;$claimed=JoinSafe $control ('requests/'+$hash+'.json');Require (!(Test-Path -LiteralPath $claimed)) 'Inflight request exists; explicit retry required';[IO.Directory]::CreateDirectory((Split-Path -Parent $claimed))|Out-Null;[IO.File]::Move($pending,$claimed);Require ((Sha $claimed) -ceq $hash) 'Request claim race'}else{$req=[pscustomobject]@{request='latest';packages=[pscustomobject]@{};created_utc=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')}}
 $ids=if($req.request -ceq 'exact'){@($req.packages.PSObject.Properties.Name)}else{@('core')+@($state.packages.PSObject.Properties|Where-Object{$_.Name -cne 'core' -and $_.Value.active -and !$_.Value.private -and $_.Name -cin @($profile.packages.PSObject.Properties.Name)}|ForEach-Object{$_.Name})}
 $cache=JoinSafe $control ('staging/'+[guid]::NewGuid().ToString('N'));$available=@{};$prefixes=@{}
 foreach($id in $ids){$cfg=Prop $profile.packages $id;$repository=$cfg.repository;$base=$cfg.base_url.TrimEnd('/');$folder=JoinSafe $cache $id;[IO.Directory]::CreateDirectory($folder)|Out-Null;$cat=Join-Path $folder 'catalog.json';$sig=$cat+'.minisig'
  Download $cfg.catalog_url $cat 1048576 ([bool]$TestProfile);Download ($cfg.catalog_url+'.minisig') $sig 16384 ([bool]$TestProfile);VerifySignature $cat $sig $profile.trust;$c=ReadStrict $cat 1048576;$seen=if(Has $state.seen $repository){Prop $state.seen $repository}else{0};AssertCatalog $c $repository $seen
  $state.seen|Add-Member -NotePropertyName $repository -NotePropertyValue $c.sequence -Force;WriteJson $statePath $state;$available[$id]=@{};$prefixes[$id]=@{};if($req.request -ceq 'latest'){Require (@($c.releases).Count -le 64) 'Latest candidate budget'}
  foreach($entry in @($c.releases)){
   if($req.request -ceq 'exact' -and $entry.version -cne (Prop $req.packages $id)){continue};$url=$base+'/'+$entry.tag+'/';$mPath=Join-Path $folder ($entry.version+'.manifest');$mSig=$mPath+'.minisig'
   Download ($url+$entry.manifest) $mPath 4194304 ([bool]$TestProfile);Download ($url+$entry.signature) $mSig 16384 ([bool]$TestProfile);Require ((Sha $mPath) -ceq $entry.manifest_sha256) 'Manifest digest';VerifySignature $mPath $mSig $profile.trust;$m=ReadStrict $mPath;AssertManifest $m $entry $id $cfg;$available[$id][$entry.version]=$m;$prefixes[$id][$entry.version]=$url
  }
 }
 $plan=@{}
 if($req.request -ceq 'exact'){foreach($id in $ids){$v=Prop $req.packages $id;Require ($available[$id].ContainsKey($v)) 'Exact release missing';$plan[$id]=$available[$id][$v]}}
 else{
  foreach($cv in @($available.core.Keys|Sort-Object {[version]$_} -Descending)){$candidate=@{core=$available.core[$cv]};$ok=$true
   foreach($id in @($ids|Where-Object{$_ -cne 'core'})){$matches=@($available[$id].Keys|Where-Object{(Compatible $cv $available[$id][$_].requires.core) -and $available[$id][$_].requires.runtime_abi -ceq $profile.runtime_abi}|Sort-Object {[version]$_} -Descending);if(!$matches.Count){$ok=$false;break};$candidate[$id]=$available[$id][$matches[0]]}
   foreach($id in $candidate.Keys){if((Has $state.packages $id) -and (Ver $candidate[$id].version) -lt (Ver (Prop $state.packages $id).version)){$ok=$false}}
   if($ok){$plan=$candidate;break}
  };Require ($plan.Count -gt 0) 'No compatible latest set'
 }
 foreach($id in $plan.Keys){if($id -cne 'core'){Require ((Compatible $plan.core.version $plan[$id].requires.core) -and $plan[$id].requires.runtime_abi -ceq $profile.runtime_abi) 'Core/feature ABI'}}
 $desired=(ConvertTo-Json -InputObject $state -Depth 40|ConvertFrom-Json);$changed=@{};$stages=@{}
 foreach($id in $plan.Keys){$m=$plan[$id];$files=@{};foreach($f in @($m.files)){if($f.mode -ceq 'managed'){$files[$f.path]=$f.sha256}};$desired.packages|Add-Member -NotePropertyName $id -NotePropertyValue ([pscustomobject]@{version=$m.version;active=$true;private=$false;files=$files}) -Force
  if(!(Has $state.packages $id) -or (Prop $state.packages $id).version -cne $m.version){$changed[$id]=$m;$zip=JoinSafe $cache ($id+'.zip');Download ($prefixes[$id][$m.version]+$m.asset.name) $zip 134217728 ([bool]$TestProfile);$stage=JoinSafe $cache ($id+'/payload');ExtractVerified $zip $m $stage;$stages[$id]=$stage}
 }
 if($req.request -ceq 'exact'){foreach($p in $desired.packages.PSObject.Properties){if(!$p.Value.private -and $p.Name -cne 'core' -and !$plan.ContainsKey($p.Name)){$p.Value.active=$false}}}
 # Activation adapter is intentionally gated until P28 supplies its real checkbox storage contract.
 if((ConvertTo-Json -InputObject $desired.packages -Depth 40 -Compress) -cne (ConvertTo-Json -InputObject $state.packages -Depth 40 -Compress)){
  Require ($profile.activation_adapter -ceq 'fixture-state-only' -and [bool]$TestProfile) 'P28 native activation adapter pending; refusing to claim disabled features'
 }
 if($changed.Count -gt 0){$mutation=$true;ApplyTransaction $target $control $state $desired $changed $stages;$mutation=$false}else{WriteJson $statePath $desired}
 if($claimed){[IO.File]::Delete($claimed)};SaveStatus $control 'VERIFIED' 'Pinned set verified; no auto-connect' $true;exit 0
}catch{
 $message=$_.Exception.Message;$blocked=$mutation -or ($controlEarly -and [IO.File]::Exists((Join-Path $controlEarly 'launch-blocked')))
 if($control -and (Test-Path -LiteralPath (Join-Path $control 'journal.json'))){try{$j=ReadStrict (Join-Path $control 'journal.json');if($j.status -ceq 'RECOVERY_REQUIRED'){$blocked=$true}elseif($j.status -ceq 'ROLLED_BACK_VERIFIED'){$blocked=$false}}catch{$blocked=$true}}
 if(!$blocked -and $claimed -and (Test-Path -LiteralPath $claimed)){try{$failed=Join-Path $control 'requests/failed';[IO.Directory]::CreateDirectory($failed)|Out-Null;[IO.File]::Move($claimed,(Join-Path $failed ((Split-Path -Leaf $claimed)+'-'+[guid]::NewGuid().ToString('N')+'.json')))}catch{$blocked=$true}}
 if($control){try{SaveStatus $control $(if($blocked){'RECOVERY_REQUIRED'}else{'REFUSED_UNCHANGED'}) $message (!$blocked)}catch{$blocked=$true}}
 Write-Warning $message
 if($blocked){$recoveryDir=if($control){$control}else{$controlEarly};& (Join-Path $PSScriptRoot 'RecoveryDialog.ps1') -Journal (Join-Path $recoveryDir 'journal.json');exit 20};exit 0
}finally{if($lock){$lock.Dispose()}}
