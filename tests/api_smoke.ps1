<#
    api_smoke.ps1 —— 聊天网站端到端接口测试

    用法（在 test_web 目录下任意位置执行）：
        powershell -ExecutionPolicy Bypass -File tests\api_smoke.ps1

    脚本会自己：
      1. 用私有端口 8099 编译出 build\verify\srv.exe，并在 build\verify\ 下运行
         （CHAT_DB_PATH 是相对路径，因此数据库落在该私有目录，不污染项目根目录）
      2. 启动服务器，跑完全部接口断言
      3. 重启服务器验证数据持久化（账号/身份码/好友/消息保留，游客会话被清理）
      4. 关闭服务器并清理临时文件

    实现说明：所有 HTTP 调用都用 curl.exe 完成（-w 取状态码、-o 落盘取响应体），
    避免 PowerShell Invoke-WebRequest 在非 2xx 时读取响应体的各种坑。

    退出码 0 表示全部通过。
#>

$ErrorActionPreference = 'Stop'

$Root     = Split-Path -Parent $PSScriptRoot
$Port     = 8099
$Base     = "http://127.0.0.1:$Port"
$Verify   = Join-Path $Root 'build\verify'
$Jars     = Join-Path $Verify 'jars'
$DbGlob   = Join-Path $Verify 'chat.db*'
$ExePath  = Join-Path $Verify 'srv.exe'
$BodyFile = Join-Path $Verify '_resp.tmp'

$script:Pass = 0
$script:Fail = 0
$script:Failed = @()

function Check {
    param([string]$Name, [bool]$Cond, [string]$Detail = '')
    if ($Cond) {
        $script:Pass++
        Write-Host ("  [PASS] {0}" -f $Name) -ForegroundColor Green
    } else {
        $script:Fail++
        $script:Failed += $Name
        Write-Host ("  [FAIL] {0} {1}" -f $Name, $Detail) -ForegroundColor Red
    }
}

function Section { param([string]$T) Write-Host "`n== $T ==" -ForegroundColor Cyan }

# 发一个请求。返回 { status; body; json }，非 2xx 也照常返回响应体
function Api {
    param(
        [string]$Method,
        [string]$Path,
        [hashtable]$Form,
        [string]$Jar
    )

    $cargs = @('-s', '--max-time', '25', '-o', $BodyFile, '-w', '%{http_code}',
               '-X', $Method, "$Base$Path")
    if ($Jar) { $cargs += @('-b', $Jar, '-c', $Jar) }

    if ($null -ne $Form) {
        $pairs = @()
        foreach ($k in $Form.Keys) {
            $pairs += '{0}={1}' -f [Uri]::EscapeDataString($k), [Uri]::EscapeDataString([string]$Form[$k])
        }
        $cargs += @('-d', ($pairs -join '&'))
    }

    $raw = & curl.exe @cargs
    $status = 0
    if ($raw -match '^\d+$') { $status = [int]$raw }

    $text = ''
    if (Test-Path $BodyFile) {
        $text = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($BodyFile))
        Remove-Item $BodyFile -Force -ErrorAction SilentlyContinue
    }

    $j = $null
    try { $j = $text | ConvertFrom-Json } catch { }

    return [pscustomobject]@{ status = $status; body = $text; json = $j }
}

function IsJson { param([string]$Text) try { $null = $Text | ConvertFrom-Json; return $true } catch { return $false } }

# 发送一个「已经百分号编码好」的请求体（用 --data-binary，curl 不会再解码也不会再编码）。
# 用于构造无法用普通字符串表达的字节，例如非法 UTF-8 的 %FF。
function ApiRaw {
    param(
        [string]$Path,
        [string]$RawBody,
        [string]$Jar
    )
    $cargs = @('-s', '--max-time', '25', '-o', $BodyFile, '-w', '%{http_code}',
               '-X', 'POST', "$Base$Path")
    if ($Jar) { $cargs += @('-b', $Jar, '-c', $Jar) }
    $cargs += @('--data-binary', $RawBody)

    $raw = & curl.exe @cargs
    $status = 0
    if ($raw -match '^\d+$') { $status = [int]$raw }

    $text = ''
    if (Test-Path $BodyFile) {
        $text = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($BodyFile))
        Remove-Item $BodyFile -Force -ErrorAction SilentlyContinue
    }

    $j = $null
    try { $j = $text | ConvertFrom-Json } catch { }

    return [pscustomobject]@{ status = $status; body = $text; json = $j }
}

function New-Jar {
    param([string]$Name)
    $p = Join-Path $Jars "$Name.txt"
    Remove-Item $p -Force -ErrorAction SilentlyContinue
    return $p
}

function Start-Server {
    if (Test-Path $ExePath) { Remove-Item $ExePath -Force }
    Push-Location $Root
    try {
        $sources = (Get-ChildItem 'src\*.c').FullName
        $cargs = @('-O2', '-std=c11', '-Iinclude', '-Ithird_party', "-DCHAT_PORT=$Port",
                   '-DCHAT_POLL_WAIT_MS=300',
                   '-o', $ExePath) + $sources + @((Join-Path $Root 'build\sqlite3.o'), '-lws2_32')
        # 编译器的警告会写到 stderr；在 $ErrorActionPreference='Stop' 下，
        # 2>&1 会把它们当成终止错误，导致测试还没开始就中断。这里临时放宽，
        # 只在 $LASTEXITCODE 非 0（真正编译失败）时判定失败。
        $ErrorActionPreference = 'Continue'
        try {
            $out = & gcc @cargs 2>&1
        } finally {
            $ErrorActionPreference = 'Stop'
        }
        if ($LASTEXITCODE -ne 0) { Write-Host $out; throw 'gcc 编译失败' }
    } finally { Pop-Location }

    # CHAT_DB_PATH 是相对路径 "chat.db"，在私有目录里启动即可隔离数据库
    $proc = Start-Process -FilePath $ExePath -WorkingDirectory $Verify -PassThru -WindowStyle Hidden
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        if ($proc.HasExited) { throw '服务器进程提前退出' }
        $c = New-Object System.Net.Sockets.TcpClient
        try {
            $c.Connect('127.0.0.1', $Port); $c.Close(); return $proc
        } catch { } finally { $c.Dispose() }
    }
    throw '服务器未在 15 秒内就绪'
}

function Stop-Server {
    param($Proc)
    if ($Proc -and -not $Proc.HasExited) {
        Stop-Process -Id $Proc.Id -Force
        Start-Sleep -Milliseconds 400
    }
}

function Get-Count {
    param($Node)
    if ($null -eq $Node) { return 0 }
    return @($Node).Count
}

# ============================================================
Write-Host "编译并启动服务器 (端口 $Port) ..." -ForegroundColor Yellow
New-Item -ItemType Directory -Force -Path $Verify, $Jars | Out-Null
Remove-Item $DbGlob -Force -ErrorAction SilentlyContinue

# 服务器启动时会把工作目录切到可执行文件所在目录，
# 因此 public/ 必须和 srv.exe 放在一起（这里复制一份，测试结束再删）
$StaticCopy = Join-Path $Verify 'public'
Remove-Item $StaticCopy -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $Root 'public') $StaticCopy -Recurse -Force

$server = Start-Server
Write-Host '服务器已就绪' -ForegroundColor Yellow

try {
    # --------------------------------------------------------
    Section '1. 游客模式'

    $guest  = New-Jar 'guest'
    $guest2 = New-Jar 'guest2'

    $r = Api POST '/api/guest' $null $guest
    Check 'POST /api/guest -> 200' ($r.status -eq 200) "实际 $($r.status)"
    Check '游客 id 为负数' ([int]$r.json.id -lt 0) "id=$($r.json.id)"
    Check '游客 mode=guest' ($r.json.mode -eq 'guest')
    Check '游客 handle 为空' ($r.json.handle -eq '')
    Check '游客 is_guest 为 true' ($r.json.is_guest -eq $true)
    $guestId = [int]$r.json.id

    $r = Api GET '/api/me' $null $guest
    Check 'GET /api/me (游客) -> 200' ($r.status -eq 200)
    Check 'GET /api/me 的 id 是数字' ($r.json.id -is [int] -or $r.json.id -is [long])

    $r2 = Api POST '/api/guest' $null $guest2
    Check '两个游客 id 不同' ([int]$r2.json.id -ne $guestId) "$($r2.json.id) vs $guestId"

    $r = Api GET '/api/friends' $null $guest
    Check '游客 GET /api/friends -> 403' ($r.status -eq 403) "实际 $($r.status)"

    $r = Api POST '/api/handle' @{ handle = 'guestx' } $guest
    Check '游客 POST /api/handle -> 403' ($r.status -eq 403) "实际 $($r.status)"

    $r = Api POST '/api/messages' @{ room = '#world'; body = '游客来啦' } $guest
    Check '游客可在世界聊天发言 -> 200' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api GET '/api/poll?room=%23world&after=0' $null $guest
    Check '游客 /api/poll -> 200' ($r.status -eq 200) "实际 $($r.status)"
    if ($r.status -eq 200) {
        Check '游客 poll 的 requests 为空数组' ((Get-Count $r.json.requests) -eq 0)
        Check '游客 poll 能看到世界消息' ((Get-Count $r.json.messages) -ge 1)
    }

    # --------------------------------------------------------
    Section '2. 注册与登录'

    $alice = New-Jar 'alice'; $bob = New-Jar 'bob'; $carol = New-Jar 'carol'

    $r = Api POST '/api/register' @{ username = 'alice'; password = 'secret123' } $alice
    Check '注册 alice -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $aliceId  = [int]$r.json.id
    $aliceUid = $r.json.uid
    Check '注册返回数字 id' ($aliceId -gt 0) "id=$aliceId"
    Check '标识码为 8 位数字' ($aliceUid -match '^\d{8}$') "uid=$aliceUid"
    Check '默认身份码形如 user_XXXX' ($r.json.handle -match '^user_\d{4}$') "handle=$($r.json.handle)"
    Check '注册响应 mode=normal' ($r.json.mode -eq 'normal') "mode=$($r.json.mode)"

    $r = Api POST '/api/register' @{ username = 'bob'; password = 'secret123' } $bob
    Check '注册 bob -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $bobId = [int]$r.json.id

    $r = Api POST '/api/register' @{ username = 'carol'; password = 'secret123' } $carol
    Check '注册 carol -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $carolId = [int]$r.json.id

    $r = Api POST '/api/register' @{ username = 'alice'; password = 'secret123' } $null
    Check '重复用户名 -> 409' ($r.status -eq 409) "实际 $($r.status)"
    Check '重复用户名响应是合法 JSON' (IsJson $r.body) "body=[$($r.body)]"

    $r = Api POST '/api/register' @{ username = 'ab'; password = 'secret123' } $null
    Check '用户名过短 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/register' @{ username = 'a-b'; password = 'secret123' } $null
    Check '用户名含非法字符 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/register' @{ username = ('x' * 33); password = 'secret123' } $null
    Check '用户名过长 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/register' @{ username = 'validname'; password = '12345' } $null
    Check '密码过短 -> 400' ($r.status -eq 400) "实际 $($r.status)"

    # 注册两次密码：不一致直接 400；一致才继续后续流程
    $r = Api POST '/api/register' @{ username = 'newbie'; password = 'secret123'; password2 = 'secret999' } $null
    Check '两次密码不一致 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/register' @{ username = 'newbie'; password = 'secret123'; password2 = 'secret123' } $null
    Check '两次密码一致 -> 注册成功' ($r.status -eq 200) "实际 $($r.status)"
    $newbieId = [int]$r.json.id
    $newbie = New-Jar 'newbie'
    $r = Api POST '/api/login' @{ username = 'newbie'; password = 'secret123' } $newbie
    Check '新注册账号可立即登录' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api POST '/api/login' @{ username = 'alice'; password = 'secret123' } $null
    Check '登录（正确密码）-> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api POST '/api/login' @{ username = 'alice'; password = 'WRONG' } $null
    Check '登录（错误密码）-> 401' ($r.status -eq 401) "实际 $($r.status)"
    Check '错误密码响应是合法 JSON' (IsJson $r.body) "body=[$($r.body)]"
    $r = Api POST '/api/login' @{ username = 'nobody'; password = 'secret123' } $null
    Check '登录（用户不存在）-> 401' ($r.status -eq 401) "实际 $($r.status)"
    Check '不泄漏「用户不存在」' ($r.body -notmatch '不存在') "body=[$($r.body)]"

    # --------------------------------------------------------
    Section '3. 标识码与身份码'

    $r = Api GET '/api/me' $null $alice
    Check 'GET /api/me (登录) -> 200' ($r.status -eq 200) "实际 $($r.status)"
    Check '/api/me 的 uid 与注册一致' ($r.json.uid -eq $aliceUid)
    Check '/api/me 的 mode=normal' ($r.json.mode -eq 'normal')
    Check '/api/me 的 id 与注册一致' ([int]$r.json.id -eq $aliceId)

    $r = Api POST '/api/handle' @{ handle = 'alice_01' } $alice
    Check 'alice 改身份码 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api GET '/api/me' $null $alice
    Check '改身份码后 /api/me 立即刷新' ($r.json.handle -eq 'alice_01') "handle=$($r.json.handle)"
    Check '标识码未被改变' ($r.json.uid -eq $aliceUid)

    $r = Api POST '/api/handle' @{ handle = 'bob_01' } $bob
    Check 'bob 改身份码 -> 200' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api POST '/api/handle' @{ handle = 'bob_01' } $alice
    Check '改成已占用身份码 -> 409' ($r.status -eq 409) "实际 $($r.status)"
    $r = Api POST '/api/handle' @{ handle = 'a' } $alice
    Check '身份码过短 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/handle' @{ handle = 'a b' } $alice
    Check '身份码含空格 -> 400' ($r.status -eq 400) "实际 $($r.status)"

    # --------------------------------------------------------
    Section '3b. 仪表盘与修改密码'

    $r = Api GET '/api/dashboard' $null $null
    Check '未登录访问仪表盘 -> 401' ($r.status -eq 401) "实际 $($r.status)"
    $r = Api GET '/api/dashboard' $null $guest
    Check '游客访问仪表盘 -> 403' ($r.status -eq 403) "实际 $($r.status)"

    $r = Api GET '/api/dashboard' $null $newbie
    Check '登录用户访问仪表盘 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    Check '仪表盘返回本人数据' ($r.json.dashboard.username -eq 'newbie') "实际 $($r.json.dashboard.username)"
    Check '仪表盘 id 与注册一致' ([int]$r.json.dashboard.id -eq $newbieId)
    Check '仪表盘含标识码与身份码' ($r.json.dashboard.uid -match '^\d{8}$' -and $r.json.dashboard.handle -match '^user_\d{4}$')
    Check '仪表盘含注册时间' ($r.json.dashboard.created_at -match '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$')
    Check '仪表盘含统计字段' ($null -ne $r.json.dashboard.friend_count -and $null -ne $r.json.dashboard.message_count)
    Check '仪表盘不泄漏密码哈希 / 盐' ($r.body -notmatch 'pass_hash' -and $r.body -notmatch 'salt')
    Check '仪表盘给出数据文件路径' ($r.json.path -eq 'Dashboard/newbie/newbie.json') "实际 $($r.json.path)"
    Check '数据文件写入成功' ($r.json.file_written -eq $true) "实际 $($r.json.file_written)"

    # 落盘结构：Dashboard/<用户名>/<用户名>.json
    $dashDir  = Join-Path $Verify 'Dashboard\newbie'
    $dashFile = Join-Path $dashDir 'newbie.json'
    Check '创建了以用户名命名的目录' (Test-Path $dashDir) "缺失 $dashDir"
    Check '创建了 用户名.json' (Test-Path $dashFile) "缺失 $dashFile"
    if (Test-Path $dashFile) {
        $onDisk = [System.Text.Encoding]::UTF8.GetString([System.IO.File]::ReadAllBytes($dashFile))
        Check '落盘 JSON 合法' (IsJson $onDisk) "内容=[$onDisk]"
        $diskJson = $onDisk | ConvertFrom-Json
        Check '落盘 JSON 内容与接口一致' ($diskJson.username -eq 'newbie')
        Check '落盘 JSON 不含密码哈希 / 盐' ($onDisk -notmatch 'pass_hash' -and $onDisk -notmatch 'salt')
    }

    # 修改密码
    $r = Api POST '/api/password' @{ old_password = 'secret123'; new_password = 'newsecret1' } $null
    Check '未登录改密码 -> 401' ($r.status -eq 401) "实际 $($r.status)"
    $r = Api POST '/api/password' @{ old_password = 'secret123'; new_password = 'newsecret1' } $guest
    Check '游客改密码 -> 403' ($r.status -eq 403) "实际 $($r.status)"
    $r = Api POST '/api/password' @{ old_password = 'wrongpass'; new_password = 'newsecret1' } $newbie
    Check '当前密码不正确 -> 401' ($r.status -eq 401) "实际 $($r.status)"
    $r = Api POST '/api/password' @{ old_password = 'secret123'; new_password = 'newsecret1'; new_password2 = 'newsecret2' } $newbie
    Check '两次新密码不一致 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/password' @{ old_password = 'secret123'; new_password = 'short' } $newbie
    Check '新密码过短 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/password' @{ old_password = 'secret123'; new_password = 'secret123' } $newbie
    Check '新密码与当前密码相同 -> 400' ($r.status -eq 400) "实际 $($r.status)"

    $r = Api POST '/api/password' @{ old_password = 'secret123'; new_password = 'newsecret1'; new_password2 = 'newsecret1' } $newbie
    Check '修改密码 -> 200' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api POST '/api/login' @{ username = 'newbie'; password = 'secret123' } $null
    Check '旧密码已失效 -> 401' ($r.status -eq 401) "实际 $($r.status)"
    $r = Api POST '/api/login' @{ username = 'newbie'; password = 'newsecret1' } $null
    Check '可用新密码登录 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api GET '/api/me' $null $newbie
    Check '改密码后当前会话仍有效' ($r.status -eq 200) "实际 $($r.status)"

    # --------------------------------------------------------
    Section '4. 好友系统'

    $r = Api GET '/api/friends' $null $carol
    Check '新用户好友列表为空' ((Get-Count $r.json.friends) -eq 0)
    Check '新用户申请列表为空' ((Get-Count $r.json.requests) -eq 0)
    Check '空列表响应是合法 JSON' (IsJson $r.body) "body=[$($r.body)]"

    $r = Api POST '/api/friends/add' @{ handle = 'alice_01' } $alice
    Check '加自己 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/friends/add' @{ handle = 'nobody99' } $alice
    Check '身份码不存在 -> 404' ($r.status -eq 404) "实际 $($r.status)"
    $r = Api POST '/api/friends/add' @{ handle = 'a' } $alice
    Check '身份码格式非法 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    # 超长身份码必须被校验拒绝，而不是被取参缓冲截断成 32 位后"变合法"
    $r = Api POST '/api/friends/add' @{ handle = ('h' * 40) } $alice
    Check '超长身份码 -> 400（未被截断成合法值）' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/friends/add' @{ handle = ('h' * 33) } $alice
    Check '33 位身份码 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/friends/add' @{ handle = ('h' * 32) } $alice
    Check '32 位身份码格式合法 -> 404（查不到该用户）' ($r.status -eq 404) "实际 $($r.status)"

    $r = Api POST '/api/friends/add' @{ handle = 'bob_01' } $alice
    Check 'alice 按身份码加 bob -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api POST '/api/friends/add' @{ handle = 'bob_01' } $alice
    Check '重复申请 -> 409' ($r.status -eq 409) "实际 $($r.status)"

    $r = Api GET '/api/friends' $null $alice
    Check '申请人自己的待处理列表为空' ((Get-Count $r.json.requests) -eq 0)

    $r = Api GET '/api/friends' $null $bob
    Check 'bob 看到 1 条好友申请' ((Get-Count $r.json.requests) -eq 1) "实际 $(Get-Count $r.json.requests)"
    Check '申请来自 alice' ([int]$r.json.requests[0].from_id -eq $aliceId)
    Check '申请带申请人用户名' ($r.json.requests[0].username -eq 'alice')
    $reqId = [int]$r.json.requests[0].id

    $r = Api GET '/api/poll?room=%23world&after=0' $null $alice
    Check 'poll 返回 friends_rev（同意申请前）' ($r.json.friends_rev -is [int] -or $r.json.friends_rev -is [long]) "rev=$($r.json.friends_rev)"
    $aliceRevBefore = [long]$r.json.friends_rev

    $r = Api POST '/api/friends/accept' @{ id = "$reqId" } $alice
    Check '非收件人同意 -> 404' ($r.status -eq 404) "实际 $($r.status)"

    $r = Api POST '/api/friends/accept' @{ id = "$reqId" } $bob
    Check 'bob 同意申请 -> 200' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api GET '/api/friends' $null $alice
    Check 'alice 好友列表含 bob' ((Get-Count $r.json.friends) -eq 1 -and [int]$r.json.friends[0].id -eq $bobId)
    Check 'alice 待处理申请已清空' ((Get-Count $r.json.requests) -eq 0)
    Check '好友项含标识码 uid' ($r.json.friends[0].uid -match '^\d{8}$')
    Check '好友项含布尔 is_guest' ($r.json.friends[0].is_guest -eq $false)

    $r = Api GET '/api/friends' $null $bob
    Check 'bob 好友列表含 alice' ((Get-Count $r.json.friends) -eq 1 -and [int]$r.json.friends[0].id -eq $aliceId)

    # 长轮询下的好友即时可见：乙同意后，甲的 /api/poll 应立即返回并带上递增的
    # friends_rev，前端据此自动刷新好友列表（无需手动刷新页面）
    $r = Api GET '/api/poll?room=%23world&after=0' $null $alice
    Check '乙同意后甲的 friends_rev 递增' ([long]$r.json.friends_rev -gt $aliceRevBefore) "rev=$($r.json.friends_rev) vs $aliceRevBefore"

    $r = Api POST '/api/friends/add' @{ handle = 'bob_01' } $alice
    Check '已是好友再次申请 -> 409' ($r.status -eq 409) "实际 $($r.status)"
    $r = Api POST '/api/friends/accept' @{ id = '99999' } $alice
    Check '不存在的申请 -> 404' ($r.status -eq 404) "实际 $($r.status)"
    $r = Api POST '/api/friends/accept' @{ id = 'abc' } $alice
    Check '非法申请编号 -> 400' ($r.status -eq 400) "实际 $($r.status)"

    # --------------------------------------------------------
    Section '5. 世界聊天'

    $r = Api POST '/api/messages' @{ room = '#world'; body = 'alice 问好' } $alice
    Check '世界聊天发言 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $m1 = [long]$r.json.id
    $r = Api POST '/api/messages' @{ room = '#world'; body = 'bob 回应' } $bob
    $m2 = [long]$r.json.id
    Check '第二条消息 id 递增' ($m2 -gt $m1) "$m1 -> $m2"

    $r = Api GET '/api/messages?room=%23world&after=0' $null $bob
    Check '拉取世界消息 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    Check '世界消息条数 >= 2' ((Get-Count $r.json.messages) -ge 2)
    Check '消息含 sender 与本地时间 ts' ($r.json.messages[-1].sender -eq 'bob' -and $r.json.messages[-1].ts -match '^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$')
    Check '消息响应是合法 JSON' (IsJson $r.body)

    $r = Api GET "/api/messages?room=%23world&after=$m1" $null $bob
    Check 'after 过滤只返回更新的消息' ((Get-Count $r.json.messages) -eq 1 -and [long]$r.json.messages[0].id -eq $m2)

    $r = Api POST '/api/messages' @{ room = '#world'; body = '' } $alice
    Check '空消息 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/messages' @{ room = '#world'; body = ('x' * 2001) } $alice
    Check '超长消息 -> 400' ($r.status -eq 400) "实际 $($r.status)"

    $weirdBody = '<script>alert(1)</script> "quote"' + "`n" + '换行 & 特殊'
    $r = Api POST '/api/messages' @{ room = '#world'; body = $weirdBody } $alice
    Check '含特殊字符的消息可发送 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api GET "/api/messages?room=%23world&after=$m2" $null $alice
    Check '特殊字符消息响应 JSON 合法' (IsJson $r.body)
    Check '特殊字符原样保存未被破坏' ((Get-Count $r.json.messages) -ge 1 -and $r.json.messages[0].body -match '<script>')

    # 控制字符会让 JSON 转义放大 6 倍，恶意刷屏可撑爆响应缓冲并让整个房间报 500（已修）
    $ctrlBody = 'a' + [char]1 + 'b'
    $r = Api POST '/api/messages' @{ room = '#world'; body = $ctrlBody } $alice
    Check '消息含控制字符 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/messages' @{ room = '#world'; body = ([char]7 + 'bell') } $alice
    Check '消息含 BEL 控制字符 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/messages' @{ room = '#world'; body = ('a' + "`n") * 1000 } $alice
    Check '含大量换行的合法消息仍可发送 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api POST '/api/messages' @{ room = '#world'; body = ("`n" * 20) } $alice
    Check '纯空白消息 -> 400' ($r.status -eq 400) "实际 $($r.status)"

    # 非法 UTF-8 会入库脏数据，让严格解析的客户端永久读不出内容（已修）
    $raw = ApiRaw '/api/messages' 'room=%23world&body=A%FFB' $alice
    Check '消息含非法 UTF-8 (0xFF) -> 400' ($raw.status -eq 400) "实际 $($raw.status)"
    $raw = ApiRaw '/api/messages' 'room=%23world&body=%C0%AF' $alice
    Check '消息为过长 UTF-8 编码 -> 400' ($raw.status -eq 400) "实际 $($raw.status)"
    $raw = ApiRaw '/api/messages' 'room=%23world&body=%ED%A0%80' $alice
    Check '消息为 UTF-8 代理区 -> 400' ($raw.status -eq 400) "实际 $($raw.status)"
    $r = Api POST '/api/messages' @{ room = '#world'; body = "中文 emoji 😀 换行`n结束" } $alice
    Check '中文/emoji/换行仍可发送 -> 200' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api GET '/api/messages?room=world&after=0' $null $alice
    Check '非法房间名 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api GET '/api/messages?room=dm:1:2:3&after=0' $null $alice
    Check '畸形 dm 房间名 -> 400' ($r.status -eq 400) "实际 $($r.status)"

    # --------------------------------------------------------
    Section '6. 私聊'

    $lo = [Math]::Min($aliceId, $bobId)
    $hi = [Math]::Max($aliceId, $bobId)

    $r = Api POST '/api/messages' @{ room = "dm:${hi}:${lo}"; body = '私聊内容' } $alice
    Check '好友私聊发言（乱序房名）-> 200' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api GET "/api/messages?room=dm:${lo}:${hi}&after=0" $null $bob
    Check '对方按正序房名能读到' ((Get-Count $r.json.messages) -eq 1) "实际 $(Get-Count $r.json.messages)"
    Check '私聊内容正确' ($r.json.messages[0].body -eq '私聊内容')

    $r = Api GET "/api/messages?room=dm:${lo}:${hi}&after=0" $null $carol
    Check '第三方读他人私聊 -> 403' ($r.status -eq 403) "实际 $($r.status)"

    $r = Api POST '/api/messages' @{ room = "dm:${carolId}:${aliceId}"; body = '非好友私聊' } $carol
    Check '非好友私聊 -> 403' ($r.status -eq 403) "实际 $($r.status)"

    $r = Api GET "/api/messages?room=dm:${guestId}:${aliceId}&after=0" $null $guest
    Check '游客读私聊 -> 403' ($r.status -eq 403) "实际 $($r.status)"

    # 轮询失效私聊应返回 403，而不是静默改发 #world 的内容（否则会串台）
    $r = Api GET "/api/poll?room=dm:${carolId}:${aliceId}&after=0" $null $carol
    Check '轮询他人私聊 -> 403' ($r.status -eq 403) "实际 $($r.status)"
    $r = Api GET "/api/poll?room=dm:${lo}:${hi}&after=0" $null $guest
    Check '游客轮询私聊 -> 403' ($r.status -eq 403) "实际 $($r.status)"
    $r = Api GET '/api/poll?room=world&after=0' $null $alice
    Check '轮询非法房间名 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api GET "/api/poll?room=dm:${lo}:${hi}&after=0" $null $alice
    Check '好友轮询私聊 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    Check '好友轮询私聊返回的是私聊消息' ((Get-Count $r.json.messages) -ge 1)

    # --------------------------------------------------------
    Section '6b. 文件传输信令'

    $r = Api POST '/api/transfer/send' @{ to = '1'; kind = 'offer'; payload = 'x' } $null
    Check '未登录发送信令 -> 401' ($r.status -eq 401) "实际 $($r.status)"
    $r = Api GET '/api/transfer/poll' $null $null
    Check '未登录轮询信令 -> 401' ($r.status -eq 401) "实际 $($r.status)"

    $r = Api POST '/api/transfer/send' @{ to = '1'; kind = 'offer'; payload = 'x' } $guest
    Check '游客发送信令 -> 403' ($r.status -eq 403) "实际 $($r.status)"
    $r = Api GET '/api/transfer/poll' $null $guest
    Check '游客轮询信令 -> 403' ($r.status -eq 403) "实际 $($r.status)"

    $r = Api POST '/api/transfer/send' @{ to = "$carolId"; kind = 'offer'; payload = 'x' } $alice
    Check '非好友发送信令 -> 403' ($r.status -eq 403) "实际 $($r.status)"
    $r = Api POST '/api/transfer/send' @{ to = "$aliceId"; kind = 'offer'; payload = 'x' } $alice
    Check '发给自己 -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/transfer/send' @{ to = "$bobId"; kind = 'bad'; payload = 'x' } $alice
    Check '非法 kind -> 400' ($r.status -eq 400) "实际 $($r.status)"
    $r = Api POST '/api/transfer/send' @{ to = "$bobId"; kind = 'offer' } $alice
    Check '缺少 payload -> 400' ($r.status -eq 400) "实际 $($r.status)"

    $payload = '{"type":"offer","sdp":"v=0\r\no=- 1 1"}'
    $r = Api POST '/api/transfer/send' @{ to = "$bobId"; kind = 'offer'; payload = $payload } $alice
    Check '好友发送 offer -> 200' ($r.status -eq 200) "实际 $($r.status)"

    $r = Api GET '/api/transfer/poll' $null $alice
    Check '发送方自己信箱为空' ((Get-Count $r.json.signals) -eq 0) "实际 $(Get-Count $r.json.signals)"

    $r = Api GET '/api/transfer/poll' $null $bob
    Check '接收方轮询取到 1 条信令' ((Get-Count $r.json.signals) -eq 1) "实际 $(Get-Count $r.json.signals)"
    Check '信令 kind=offer' ($r.json.signals[0].kind -eq 'offer')
    Check '信令 from=alice' ([int]$r.json.signals[0].from -eq $aliceId)
    Check '信令 payload 原样透传' ($r.json.signals[0].payload -eq $payload) "payload=$($r.json.signals[0].payload)"

    $r = Api GET '/api/transfer/poll' $null $bob
    Check '轮询消费后信箱为空' ((Get-Count $r.json.signals) -eq 0) "实际 $(Get-Count $r.json.signals)"

    $r = Api POST '/api/transfer/send' @{ to = "$bobId"; kind = 'ice'; payload = '{"candidate":"candidate:1 1 udp 1 127.0.0.1 1 typ host"}' } $alice
    Check '好友发送 ice -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api GET '/api/transfer/poll' $null $bob
    Check 'ice 信令可取到且 kind=ice' ((Get-Count $r.json.signals) -eq 1 -and $r.json.signals[0].kind -eq 'ice')

    # --------------------------------------------------------
    Section '7. 轮询'

    $r = Api GET '/api/poll?room=%23world&after=0' $null $alice
    Check 'poll -> 200' ($r.status -eq 200) "实际 $($r.status)"
    Check 'poll 响应是合法 JSON' (IsJson $r.body)
    Check 'poll 返回数字 last_id' ($r.json.last_id -is [int] -or $r.json.last_id -is [long])
    Check 'poll 带 requests 字段' ($null -ne $r.json.requests)

    $r = Api GET '/api/poll?room=%23world&after=999999' $null $alice
    Check 'poll 空消息列表是合法 JSON' (IsJson $r.body)
    Check 'poll 空消息列表为 []' ((Get-Count $r.json.messages) -eq 0)
    Check 'poll 空列表时 last_id 回退为 after' ([long]$r.json.last_id -eq 999999) "last_id=$($r.json.last_id)"

    # --------------------------------------------------------
    Section '8. 登出与会话'

    $r = Api POST '/api/logout' $null $carol
    Check '登出 -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api GET '/api/me' $null $carol
    Check '登出后 /api/me -> 401' ($r.status -eq 401) "实际 $($r.status)"
    $r = Api GET '/api/messages?room=%23world&after=0' $null $carol
    Check '未登录读消息 -> 401' ($r.status -eq 401) "实际 $($r.status)"
    $r = Api GET '/api/friends' $null $null
    Check '无 Cookie 访问好友 -> 401' ($r.status -eq 401) "实际 $($r.status)"

    # --------------------------------------------------------
    Section '9. 静态资源与路由'

    $r = Api GET '/' $null $null
    Check 'GET / -> 200' ($r.status -eq 200) "实际 $($r.status)"
    Check '首页含 UTF-8 声明' ($r.body -match 'charset="?UTF-8')
    Check '首页引用了 app.js 与 style.css' ($r.body -match 'app\.js' -and $r.body -match 'style\.css')
    $r = Api GET '/style.css' $null $null
    Check 'GET /style.css -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api GET '/app.js' $null $null
    Check 'GET /app.js -> 200' ($r.status -eq 200) "实际 $($r.status)"
    $r = Api GET '/nope.css' $null $null
    Check '不存在的静态资源 -> 404' ($r.status -eq 404) "实际 $($r.status)"
    $r = Api GET '/api/nonexist' $null $null
    Check '不存在的接口 -> 404' ($r.status -eq 404) "实际 $($r.status)"
    Check '不存在的接口返回 JSON' (IsJson $r.body)
    $r = Api POST '/api/me' $null $null
    Check '方法不匹配 -> 405' ($r.status -eq 405) "实际 $($r.status)"

    # --------------------------------------------------------
    Section '10. 重启后数据持久化'

    Stop-Server $server
    $server = Start-Server

    $alice2 = New-Jar 'alice2'
    $r = Api POST '/api/login' @{ username = 'alice'; password = 'secret123' } $alice2
    Check '重启后仍可用原密码登录' ($r.status -eq 200) "实际 $($r.status)"
    Check '重启后用户 id 保持一致' ([int]$r.json.id -eq $aliceId)

    $r = Api GET '/api/me' $null $alice2
    Check '重启后身份码保持 alice_01' ($r.json.handle -eq 'alice_01') "handle=$($r.json.handle)"
    Check '重启后标识码不变' ($r.json.uid -eq $aliceUid)

    $r = Api GET '/api/friends' $null $alice2
    Check '重启后好友关系保留' ((Get-Count $r.json.friends) -eq 1 -and [int]$r.json.friends[0].id -eq $bobId)

    $r = Api GET '/api/messages?room=%23world&after=0' $null $alice2
    Check '重启后世界聊天记录保留' ((Get-Count $r.json.messages) -ge 3)

    $r = Api GET "/api/messages?room=dm:${lo}:${hi}&after=0" $null $alice2
    Check '重启后私聊记录保留' ((Get-Count $r.json.messages) -eq 1)

    $r = Api POST '/api/guest' $null (New-Jar 'guestAfterRestart')
    Check '重启后游客会话仍可创建' ($r.status -eq 200) "实际 $($r.status)"
}
finally {
    Stop-Server $server
    Remove-Item $DbGlob, $BodyFile -Force -ErrorAction SilentlyContinue
    Remove-Item $StaticCopy -Recurse -Force -ErrorAction SilentlyContinue
    # 仪表盘落盘目录（Dashboard/<用户名>/<用户名>.json）也要清理
    Remove-Item (Join-Path $Verify 'Dashboard') -Recurse -Force -ErrorAction SilentlyContinue
}

# ============================================================
Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host ("汇总: {0} PASS / {1} FAIL" -f $script:Pass, $script:Fail) `
    -ForegroundColor $(if ($script:Fail -eq 0) { 'Green' } else { 'Red' })
if ($script:Fail -gt 0) {
    Write-Host '失败项：' -ForegroundColor Red
    $script:Failed | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
}
Write-Host '==================================================' -ForegroundColor Cyan
exit $script:Fail
