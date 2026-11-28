# rv-go.ps1：在 Windows 上一键远程查看 / 控制 L540（说明见 docs/remote-view.md）
#
# 用法（在 PowerShell 里）：
#   .\rv-go.ps1                                  连 uos@192.168.137.27
#   .\rv-go.ps1 192.168.137.30                   换 IP（用户名默认 uos）
#   .\rv-go.ps1 uos@192.168.137.30 --fps 15      地址后面的参数原样传给服务端（--size、--crf、--cpu 等）
#   .\rv-go.ps1 --size 1440x960                  不写地址就用默认地址
#   .\rv-go.ps1 --revoke                         撤销登录界面的访问授权（见下面第 2 步）后退出
# 本机端口默认 8765，被占用就自动往后找；也可以先设 $env:RV_PORT。
#
# 做的事：
#   1. 比对本机和设备上 ~/remote-view 的文件（sha256），没有或不一致就打包复制过去
#   2. 看设备屏幕上现在是什么。用户会话（桌面、锁屏）直接能采集；注销后的登录界面（greeter）
#      由 dde 用户的 treeland 画，socket 在 /run/treeland（dde 0700），这时会请你输入设备的 sudo 密码，
#      用 ACL 只给登录用户开这个目录和 socket 的权限。不改用户组、不改服务配置，treeland 重启后自动失效。
#      密码只经 ssh 的标准输入交给设备上的 sudo -S，不进命令行、不落盘、不留在脚本变量里。
#   3. 建 SSH 隧道，在设备上启动 run.sh（首次会编译 rv-cap，约 1 分钟）
#   4. 服务就绪后用默认浏览器打开 http://localhost:端口
# 看着看着设备回到了登录界面（又没有权限）时，服务端会退出，这里提示输入密码，授权后自动重启，浏览器自己重连。
# 关闭窗口或按 Ctrl+C 停止。不在交互终端里运行时（输入输出被重定向，比如从别的程序里调用），
# 会自己开一个新的 PowerShell 窗口来跑。
#
# 本文件必须存成带 BOM 的 UTF-8：Windows PowerShell 5.1 把无 BOM 的文件当 GBK 读，中文会乱、甚至语法出错。

$ErrorActionPreference = 'Continue'
$Here = $PSScriptRoot
$RemoteDir = 'remote-view'
$Files = @('rv-cap.c', 'gpu-conv.c', 'gpu-conv.h', 'Makefile', 'rv-server.py', 'index.html', 'run.sh',
           'protocols/wlr-screencopy-unstable-v1.xml', 'protocols/linux-dmabuf-unstable-v1.xml')

# 和 rv-server.py 里的 EXIT_NOPERM / EXIT_NONE 对应
$EXIT_NOPERM = 3
$EXIT_NONE = 4

# 用 root 执行的脚本：经 sh -c '...' 传过去，所以里面不能有单引号、双引号和反斜杠。
# 先输出 RV-ROOT，用来区分“sudo 没通过”和“脚本本身失败”。$SUDO_USER 是 ssh 登录的用户。
$GrantScript = 'set -e; echo RV-ROOT; u=${SUDO_USER:?}; d=/run/treeland; [ -d $d ] || exit 4; n=0; ' +
               'for s in $d/wayland-[0-9]; do [ -S $s ] || continue; setfacl -m u:${u}:rw $s; n=$((n+1)); done; ' +
               '[ $n -gt 0 ] || exit 5; setfacl -m u:${u}:x $d; echo RV-OK'
$RevokeScript = 'echo RV-ROOT; u=${SUDO_USER:?}; for f in /run/treeland /run/treeland/wayland-[0-9]; do ' +
                '[ -e $f ] && setfacl -x u:${u} $f; done; echo RV-OK'

function Say($msg, $color = 'Gray') { Write-Host $msg -ForegroundColor $color }
function Step($msg) { Write-Host ''; Write-Host $msg -ForegroundColor Cyan }
function Fail($msg) { Write-Host $msg -ForegroundColor Red }
function SayIndented($text, $color = 'DarkGray') {
	foreach ($l in ("$text" -split "`n")) { if ($l.Trim()) { Say "    $l" $color } }
}

# ---- 参数 ----
$rest = @($args)
$spawned = $false
if ($rest.Count -gt 0 -and $rest[0] -eq '--spawned') {
	$spawned = $true
	$rest = @($rest | Select-Object -Skip 1)
}
$Target = 'uos@192.168.137.27'
if ($rest.Count -gt 0 -and -not "$($rest[0])".StartsWith('-')) {
	$Target = "$($rest[0])"
	if ($Target -notmatch '@') { $Target = "uos@$Target" }
	$rest = @($rest | Select-Object -Skip 1)
}
$User = $Target.Split('@')[0]
$Revoke = $rest -contains '--revoke'
$ServerArgs = @($rest | Where-Object { "$_" -ne '--revoke' } | ForEach-Object { "$_" })

# ---- 不是交互终端就换个新窗口跑：ssh -t 需要真终端，输出和密码输入也要让人看得见 ----
if (-not $spawned -and ([Console]::IsInputRedirected -or [Console]::IsOutputRedirected)) {
	$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '--spawned') +
	           @($args | ForEach-Object { "`"$_`"" })
	Start-Process powershell.exe -ArgumentList $argList
	# 输出被重定向时按控制台代码页（GBK）编码，Git Bash 之类按 UTF-8 读会乱码
	[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
	Write-Output '当前不是交互终端，已在新的 PowerShell 窗口里启动远程查看，请看那个窗口。'
	exit 0
}

# 在设备上用 sudo 以 root 执行 $Script。密码从 SecureString 里取出来只写进 ssh 的标准输入，
# 由设备上的 sudo -S 读走；用完立刻清掉明文。返回退出码、是否进了 root、是否成功和 stderr。
function Invoke-Sudo([Security.SecureString]$Password, [string]$Script) {
	$si = New-Object Diagnostics.ProcessStartInfo $ssh
	$si.Arguments = "-o ConnectTimeout=8 $Target `"sudo -S -p '' -- sh -c '$Script'`""
	$si.UseShellExecute = $false
	$si.RedirectStandardInput = $true
	$si.RedirectStandardOutput = $true
	$si.RedirectStandardError = $true
	$si.StandardOutputEncoding = [Text.Encoding]::UTF8
	$si.StandardErrorEncoding = [Text.Encoding]::UTF8
	$p = [Diagnostics.Process]::Start($si)
	$outTask = $p.StandardOutput.ReadToEndAsync()
	$errTask = $p.StandardError.ReadToEndAsync()
	$bytes = $null
	$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
	try {
		$bytes = [Text.Encoding]::UTF8.GetBytes([Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) + "`n")
		$p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
		$p.StandardInput.Close()
	} catch {
		# ssh 已经退出（连不上）时写管道会失败，结果看退出码
	} finally {
		[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
		if ($bytes) { [Array]::Clear($bytes, 0, $bytes.Length) }
	}
	$p.WaitForExit()
	$out = $outTask.Result
	@{
		Code = $p.ExitCode
		Root = $out -match 'RV-ROOT'
		Ok   = $out -match 'RV-OK'
		Err  = ($errTask.Result -replace "`r", '').Trim()
	}
}

# 问密码并用 sudo 执行，密码不对最多再问两次。$What 是给人看的动作名。
function Request-Sudo([string]$Script, [string]$What) {
	for ($try = 1; $try -le 3; $try++) {
		$pw = Read-Host -AsSecureString "  输入 $Target 的 sudo 密码（直接回车放弃）"
		if ($pw.Length -eq 0) {
			Say '  已放弃。' Yellow
			return $false
		}
		$r = Invoke-Sudo $pw $Script
		$pw.Dispose()
		if ($r.Ok) { return $true }
		if (-not $r.Root) {
			if ($r.Code -eq 255) {
				Fail "  SSH 连接失败："
				SayIndented $r.Err
				return $false
			}
			Fail "  sudo 没通过（密码不对，或者 $User 没有 sudo 权限）："
			SayIndented $r.Err
			continue
		}
		switch ($r.Code) {
			4 { Fail "  ${What}失败：设备上没有 /run/treeland，登录界面的 treeland 没在运行？" }
			5 { Fail "  ${What}失败：/run/treeland 下没有 Wayland socket。" }
			default { Fail "  ${What}失败（退出码 $($r.Code)）：" }
		}
		SayIndented $r.Err
		return $false
	}
	Fail '  连续 3 次没通过，不再尝试。'
	return $false
}

function Grant-Greeter {
	Say '  屏幕现在是登录界面（greeter）：它由 dde 用户的 treeland 画，socket 在 /run/treeland 下，只有 dde 能连。' Yellow
	Say "  要用 sudo 给 $User 加 ACL：/run/treeland 可进入、里面的 Wayland socket 可读写。" Yellow
	Say '  不改用户组、不改服务配置；treeland 重启（比如重启机器）后自动失效，也可以用 --revoke 撤销。' Yellow
	Say '  密码只经 SSH 交给设备上的 sudo，不保存、不进命令行。' DarkGray
	if (Request-Sudo $GrantScript '授权') {
		Say '  已授权。' Green
		return $true
	}
	return $false
}

# 在设备上问 rv-server.py 现在能采集哪个 treeland；输出缩进显示，返回它的退出码
function Invoke-Probe {
	& $ssh @SshOpts $Target "python3 ~/$RemoteDir/rv-server.py --probe" | ForEach-Object { Say "  $_" }
	return $LASTEXITCODE
}

$exitCode = 1
$prevEnc = [Console]::OutputEncoding
# 设备端日志是 UTF-8，控制台默认 GBK 会乱码
[Console]::OutputEncoding = New-Object Text.UTF8Encoding $false
try {
	$Host.UI.RawUI.WindowTitle = "L540 远程查看 - $Target"

	$ssh = Get-Command ssh -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
	if (-not $ssh) {
		Fail '找不到 ssh。到“设置 → 系统 → 可选功能”里添加“OpenSSH 客户端”后再试。'
		exit 1
	}
	$ssh = $ssh.Source
	$SshOpts = @('-o', 'ConnectTimeout=8', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3')

	foreach ($f in $Files) {
		if (-not (Test-Path -LiteralPath (Join-Path $Here $f))) {
			Fail "本机缺文件 $f：rv-go.ps1 要和 remote-view 目录里的其他文件放在一起。"
			exit 1
		}
	}

	$base = 8765
	if ($env:RV_PORT) { $base = [int]$env:RV_PORT }
	$RemotePort = $base
	$LocalPort = $null
	foreach ($p in $base..($base + 20)) {
		try {
			$l = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), $p
			$l.Start(); $l.Stop()
			$LocalPort = $p
			break
		} catch { }
	}
	if (-not $LocalPort) {
		Fail "本机端口 $base 到 $($base + 20) 都被占用了，设 `$env:RV_PORT 换一段再试。"
		exit 1
	}
	$Url = "http://localhost:$LocalPort"

	Say '==== L540 远程查看 ====' Green
	if ($Revoke) {
		Say "设备：$Target"
	} else {
		Say "设备：$Target    本机地址：$Url"
		if ($LocalPort -ne $base) { Say "（本机 $base 端口被占用，改用 $LocalPort）" Yellow }
		if ($ServerArgs.Count) { Say "服务端参数：$($ServerArgs -join ' ')" }
	}

	# ---- 1. 检查 / 复制 ----
	Step "[1/4] 检查设备上的 ~/$RemoteDir ..."
	$t0 = Get-Date
	$remoteOut = & $ssh @SshOpts $Target "cd ~/$RemoteDir 2>/dev/null && sha256sum $($Files -join ' ') 2>/dev/null; exit 0"
	if ($LASTEXITCODE -ne 0) {
		Fail "连不上 $Target（ssh 退出码 $LASTEXITCODE，原因见上面 ssh 的报错）。"
		Say '请确认：设备开机了、IP 没变、这台电脑能 ssh 登录它（最好配好密钥免密码）。'
		exit 1
	}
	Say ("  SSH 连接正常（{0:N1} 秒）" -f ((Get-Date) - $t0).TotalSeconds)

	if ($Revoke) {
		Step '撤销登录界面的访问授权 ...'
		if (Request-Sudo $RevokeScript '撤销') {
			Say "  已撤销，$User 又连不上登录界面的 treeland 了。" Green
			$exitCode = 0
		}
		exit $exitCode
	}

	$remote = @{}
	foreach ($line in @($remoteOut)) {
		if ("$line" -match '^([0-9a-f]{64})\s+\*?(\S+)$') { $remote[$Matches[2]] = $Matches[1] }
	}
	$sha = [Security.Cryptography.SHA256]::Create()
	$missing = @(); $changed = @()
	foreach ($f in $Files) {
		$s = [IO.File]::OpenRead((Join-Path $Here $f))
		try { $h = -join ($sha.ComputeHash($s) | ForEach-Object { $_.ToString('x2') }) } finally { $s.Dispose() }
		if (-not $remote.ContainsKey($f)) { $missing += $f } elseif ($remote[$f] -ne $h) { $changed += $f }
	}

	if ($missing.Count -eq 0 -and $changed.Count -eq 0) {
		Say '  设备上已是最新版本，不用复制。' Green
	} else {
		if ($remote.Count -eq 0) {
			Say "  设备上还没有 ~/$RemoteDir，复制过去 ..." Yellow
		} else {
			Say "  和本机不一致：$((@($missing) + @($changed)) -join ', ')" Yellow
			Say '  复制过去 ...' Yellow
		}
		$t0 = Get-Date
		# PowerShell 5.1 的管道会把二进制数据当文本改坏，这里自己把 tar 的输出接到 ssh 的输入
		$tarExe = Join-Path $env:SystemRoot 'System32\tar.exe'
		if (-not (Test-Path $tarExe)) { $tarExe = 'tar' }
		$ti = New-Object Diagnostics.ProcessStartInfo $tarExe
		$ti.Arguments = '-czf - ' + ($Files -join ' ')
		$ti.WorkingDirectory = $Here
		$ti.UseShellExecute = $false
		$ti.RedirectStandardOutput = $true
		$si = New-Object Diagnostics.ProcessStartInfo $ssh
		# -m：解出的文件用当前时间，保证 make 会重新编译
		$si.Arguments = "-o ConnectTimeout=8 $Target `"mkdir -p ~/$RemoteDir && tar -xmzf - -C ~/$RemoteDir && chmod +x ~/$RemoteDir/run.sh`""
		$si.UseShellExecute = $false
		$si.RedirectStandardInput = $true
		$tp = [Diagnostics.Process]::Start($ti)
		$sp = [Diagnostics.Process]::Start($si)
		$counter = New-Object IO.MemoryStream
		$tp.StandardOutput.BaseStream.CopyTo($counter)
		$bytes = $counter.Length
		$counter.Position = 0
		$counter.CopyTo($sp.StandardInput.BaseStream)
		$sp.StandardInput.Close()
		$tp.WaitForExit(); $sp.WaitForExit()
		if ($tp.ExitCode -ne 0 -or $sp.ExitCode -ne 0) {
			Fail "复制失败（tar 退出码 $($tp.ExitCode)，ssh 退出码 $($sp.ExitCode)）。"
			exit 1
		}
		Say ("  已复制 {0} 个文件（压缩后 {1:N0} KB，{2:N1} 秒）。" -f $Files.Count, ($bytes / 1KB), ((Get-Date) - $t0).TotalSeconds) Green
	}

	# ---- 2. 屏幕上是什么、要不要授权 ----
	Step '[2/4] 看设备屏幕上现在是什么 ...'
	$code = Invoke-Probe
	if ($code -eq $EXIT_NOPERM) {
		if (-not (Grant-Greeter)) { exit 1 }
		$code = Invoke-Probe
	}
	if ($code -ne 0) {
		if ($code -eq $EXIT_NOPERM) {
			Fail '  授权后还是连不上登录界面的 treeland，见上面的说明。'
		} elseif ($code -eq $EXIT_NONE) {
			Fail '  设备上没有能连上的 treeland（正在注销 / 重启？），等一会再试。'
		} else {
			Fail "  检查失败（退出码 $code），见上面的输出。"
		}
		exit 1
	}

	# ---- 3/4. 启动服务端，就绪后开浏览器 ----
	Step '[3/4] 在设备上启动服务端（需要时会先编译 rv-cap，首次约 1 分钟）...'
	Step '[4/4] 服务就绪后会自动打开浏览器。'
	Say '---- 下面是设备端的输出；关闭本窗口或按 Ctrl+C 停止 ----' DarkGray

	# 隧道一建好本机端口就能连上，但服务端没起来时请求会失败，所以要等 HTTP 真的有响应
	$probe = [PowerShell]::Create()
	[void]$probe.AddScript({
		param($url)
		for ($i = 0; $i -lt 180; $i++) {
			try {
				$q = [Net.WebRequest]::Create("$url/")
				$q.Proxy = $null
				$q.Timeout = 2000
				$q.GetResponse().Close()
				[Console]::WriteLine("`r`n>>> 服务已就绪，已用默认浏览器打开 $url （没弹出来就手动打开，要用 Chrome / Edge）`r`n")
				[void][Diagnostics.Process]::Start($url)
				return
			} catch {
				[Threading.Thread]::Sleep(1000)
			}
		}
		[Console]::WriteLine("`r`n>>> 等了 3 分钟服务还没就绪，看上面设备端的输出`r`n")
	}).AddArgument($Url)
	$probeHandle = $probe.BeginInvoke()

	$remoteCmd = "sh ~/$RemoteDir/run.sh --port $RemotePort"
	if ($ServerArgs.Count) { $remoteCmd += ' ' + ($ServerArgs -join ' ') }
	while ($true) {
		# -tt：强制分配终端。有终端时断线服务端能收到 SIGHUP 退出，没终端只能靠 stdin 关闭来判断
		& $ssh @SshOpts -tt -o ExitOnForwardFailure=yes -L "${LocalPort}:127.0.0.1:$RemotePort" $Target $remoteCmd
		$exitCode = $LASTEXITCODE
		if ($exitCode -ne $EXIT_NOPERM) { break }
		# 用户会话没了、屏幕回到登录界面，而且 treeland 重启过（授权跟着没了）
		Write-Host ''
		Say '设备回到了登录界面，要重新授权才能接着看（浏览器页面不用关，授权后会自己重连）。' Yellow
		if (-not (Grant-Greeter)) { break }
		Say '---- 重启服务端 ----' DarkGray
	}

	if (-not $probeHandle.IsCompleted) { $probe.Stop() }
	$probe.Dispose()

	Write-Host ''
	if ($exitCode -eq 0) {
		Say '服务端已停止。' Green
	} elseif ($exitCode -eq $EXIT_NOPERM) {
		Fail '没有授权，服务端已停止。'
	} elseif ($exitCode -eq 255) {
		Fail '服务端异常退出：SSH 连接断开或端口转发失败（见上面 ssh 的报错）。'
	} else {
		Fail "服务端异常退出（退出码 $exitCode），原因见上面设备端的输出。"
	}
} finally {
	[Console]::OutputEncoding = $prevEnc
	if ($spawned) { Read-Host '按回车关闭窗口' | Out-Null }
}
exit $exitCode
