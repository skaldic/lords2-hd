param([string]$Path)
# Minimal PE32 reader: header facts, sections, imports.
$b = [IO.File]::ReadAllBytes($Path)
function U16($o) { [BitConverter]::ToUInt16($b, $o) }
function U32($o) { [BitConverter]::ToUInt32($b, $o) }
function Str($o) { $e = $o; while ($b[$e] -ne 0) { $e++ }; [Text.Encoding]::ASCII.GetString($b, $o, $e - $o) }
$pe = U32 0x3C
$coff = $pe + 4
$nsec = U16 ($coff + 2)
$ts = U32 ($coff + 4)
$optSize = U16 ($coff + 16)
$chars = U16 ($coff + 18)
$opt = $coff + 20
$magic = U16 $opt
"File: $Path"
"Machine: 0x{0:X4}  Magic: 0x{1:X3}  Sections: {2}" -f (U16 $coff), $magic, $nsec
"Timestamp: {0} ({1:X8})" -f ([DateTimeOffset]::FromUnixTimeSeconds($ts).UtcDateTime), $ts
"Characteristics: 0x{0:X4}  LargeAddressAware: {1}" -f $chars, [bool]($chars -band 0x20)
$imageBase = U32 ($opt + 28)
"ImageBase: 0x{0:X8}  EntryRVA: 0x{1:X}  Subsystem: {2}  DllChars: 0x{3:X4}" -f $imageBase, (U32 ($opt + 16)), (U16 ($opt + 68)), (U16 ($opt + 70))
"Linker: {0}.{1}" -f $b[$opt + 2], $b[$opt + 3]
$dd = $opt + 96
$secs = @()
$sh = $opt + $optSize
for ($i = 0; $i -lt $nsec; $i++) {
    $o = $sh + 40 * $i
    $s = [pscustomobject]@{ Name = [Text.Encoding]::ASCII.GetString($b, $o, 8).TrimEnd([char]0); VA = U32 ($o + 12); VSize = U32 ($o + 8); Raw = U32 ($o + 20); RawSize = U32 ($o + 16); Flags = '{0:X8}' -f (U32 ($o + 36)) }
    $secs += $s
}
$secs | Format-Table -AutoSize | Out-String
function R2O($rva) { foreach ($s in $secs) { if ($rva -ge $s.VA -and $rva -lt $s.VA + [Math]::Max($s.VSize, $s.RawSize)) { return $rva - $s.VA + $s.Raw } }; throw "rva $rva" }
$impRva = U32 ($dd + 8)
$o = R2O $impRva
while ($true) {
    $oft = U32 $o; $nameRva = U32 ($o + 12); $ft = U32 ($o + 16)
    if ($nameRva -eq 0) { break }
    $dll = Str (R2O $nameRva)
    $t = if ($oft) { $oft } else { $ft }
    $to = R2O $t; $names = @()
    while (($v = U32 $to) -ne 0) {
        if ($v -band 0x80000000) { $names += "#$($v -band 0xFFFF)" } else { $names += (Str ((R2O $v) + 2)) }
        $to += 4
    }
    "[$dll] IAT@0x{0:X8}: {1}" -f ($imageBase + $ft), ($names -join ', ')
    $o += 20
}
