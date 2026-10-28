param(
    [string]$ExistingNotifier,
    [string]$ExistingAction,
    [string]$Payload
)

# 2026-09-28 已下线串口任务提醒，不再占用 COM3
if ($ExistingNotifier -and (Test-Path -LiteralPath $ExistingNotifier)) {
    & $ExistingNotifier $ExistingAction $Payload
}
exit 0
