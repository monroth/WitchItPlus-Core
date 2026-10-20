param([Parameter(Mandatory=$true)][string]$Journal)
# Display only. No game/process/credential/security changes. No unsafe vanilla fallback.
try{
 Add-Type -AssemblyName System.Windows.Forms
 $form=New-Object Windows.Forms.Form;$form.Text='Witch It — восстановление';$form.Width=600;$form.Height=190
 $text=New-Object Windows.Forms.Label;$text.Text="Откат не подтверждён. Игра не запущена.`r`nЖурнал: $Journal";$text.Left=12;$text.Top=12;$text.Width=560;$text.Height=70;$form.Controls.Add($text)
 $button=New-Object Windows.Forms.Button;$button.Text='Запустить без модов';$button.Width=190;$button.Left=12;$button.Top=90;$button.Enabled=$false
 $tip=New-Object Windows.Forms.ToolTip;$tip.SetToolTip($button,'Заблокировано до P27: необходим проверенный контракт изоляции всех mod entrypoints.');$form.Controls.Add($button)
 $null=$form.ShowDialog()
}catch{Write-Warning ('Recovery required. Journal: '+$Journal)}
