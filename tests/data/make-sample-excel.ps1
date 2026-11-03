# Builds 00-data\test-archives\sample-excel.zip: one formatted workbook (folder "إكسيل")
# to check the Excel preview against what Excel shows (no Excel needed to build it).
#   Sheet 1 "حصر" (right-to-left): merged coloured title, bold header with theme fill + borders,
#     #,##0 / #,##0.00 / dates / % / negative in [Red], wrapped text, long text running on, total row.
#   Sheet 2 "Formats" (left-to-right): many number formats, alignment, indent, hidden column, row heights.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$out = Join-Path $root '00-data\test-archives\sample-excel.zip'

$ct = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
<Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>
<Override PartName="/xl/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>
</Types>
'@
$rels = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>
'@
$wb = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<sheets><sheet name="حصر" sheetId="1" r:id="rId1"/><sheet name="Formats" sheetId="2" r:id="rId2"/></sheets>
</workbook>
'@
$wbRels = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>
<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
<Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>
<Relationship Id="rId5" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="theme/theme1.xml"/>
</Relationships>
'@
$theme = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office Theme"><a:themeElements>
<a:clrScheme name="Office">
<a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1><a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1>
<a:dk2><a:srgbClr val="44546A"/></a:dk2><a:lt2><a:srgbClr val="E7E6E6"/></a:lt2>
<a:accent1><a:srgbClr val="4472C4"/></a:accent1><a:accent2><a:srgbClr val="ED7D31"/></a:accent2>
<a:accent3><a:srgbClr val="A5A5A5"/></a:accent3><a:accent4><a:srgbClr val="FFC000"/></a:accent4>
<a:accent5><a:srgbClr val="5B9BD5"/></a:accent5><a:accent6><a:srgbClr val="70AD47"/></a:accent6>
<a:hlink><a:srgbClr val="0563C1"/></a:hlink><a:folHlink><a:srgbClr val="954F72"/></a:folHlink>
</a:clrScheme></a:themeElements></a:theme>
'@
# Styles: cellXfs index -> meaning
#  0 default | 1 title (bold 16 white, fill accent1, center) | 2 header (bold, fill accent1 tint .8, thin borders, center, wrap)
#  3 text + thin borders | 4 #,##0 + borders | 5 #,##0.00 + borders | 6 date dd/mm/yyyy + borders | 7 0% + borders
#  8 #,##0.00;[Red]-#,##0.00 + borders | 9 wrap + borders | 10 total label bold, fill lt2, top double border
#  11 total number bold #,##0.00, fill lt2, top double | 12 $#,##0.00 | 13 0.00E+00 | 14 d-mmm-yy (15) | 15 h:mm (20)
#  16 0.0% | 17 center | 18 indent 2 | 19 right, italic red text | 20 #,##0 ;(#,##0) (37)
$styles = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<numFmts count="5">
<numFmt numFmtId="164" formatCode="dd/mm/yyyy"/><numFmt numFmtId="165" formatCode="#,##0.00;[Red]\-#,##0.00"/>
<numFmt numFmtId="166" formatCode="&quot;$&quot;#,##0.00"/><numFmt numFmtId="167" formatCode="0.0%"/><numFmt numFmtId="168" formatCode="0.00E+00"/>
</numFmts>
<fonts count="5">
<font><sz val="11"/><color theme="1"/><name val="Calibri"/></font>
<font><b/><sz val="16"/><color theme="0"/><name val="Calibri"/></font>
<font><b/><sz val="11"/><color theme="1"/><name val="Calibri"/></font>
<font><i/><sz val="11"/><color rgb="FFC00000"/><name val="Calibri"/></font>
<font><sz val="11"/><color theme="1"/><name val="Arial"/></font>
</fonts>
<fills count="5">
<fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill>
<fill><patternFill patternType="solid"><fgColor theme="4"/><bgColor indexed="64"/></patternFill></fill>
<fill><patternFill patternType="solid"><fgColor theme="4" tint="0.79998168889431442"/><bgColor indexed="64"/></patternFill></fill>
<fill><patternFill patternType="solid"><fgColor theme="2"/><bgColor indexed="64"/></patternFill></fill>
</fills>
<borders count="3">
<border><left/><right/><top/><bottom/><diagonal/></border>
<border><left style="thin"><color indexed="64"/></left><right style="thin"><color indexed="64"/></right><top style="thin"><color indexed="64"/></top><bottom style="thin"><color indexed="64"/></bottom><diagonal/></border>
<border><left style="thin"><color indexed="64"/></left><right style="thin"><color indexed="64"/></right><top style="double"><color indexed="64"/></top><bottom style="medium"><color indexed="64"/></bottom><diagonal/></border>
</borders>
<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
<cellXfs count="21">
<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
<xf numFmtId="0" fontId="2" fillId="3" borderId="1" xfId="0" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="0" fillId="0" borderId="1" xfId="0"/>
<xf numFmtId="3" fontId="0" fillId="0" borderId="1" xfId="0"/>
<xf numFmtId="4" fontId="0" fillId="0" borderId="1" xfId="0"/>
<xf numFmtId="164" fontId="0" fillId="0" borderId="1" xfId="0"/>
<xf numFmtId="9" fontId="0" fillId="0" borderId="1" xfId="0"/>
<xf numFmtId="165" fontId="0" fillId="0" borderId="1" xfId="0"/>
<xf numFmtId="0" fontId="0" fillId="0" borderId="1" xfId="0" applyAlignment="1"><alignment vertical="top" wrapText="1"/></xf>
<xf numFmtId="0" fontId="2" fillId="4" borderId="2" xfId="0"/>
<xf numFmtId="4" fontId="2" fillId="4" borderId="2" xfId="0"/>
<xf numFmtId="166" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="168" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="15" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="20" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="167" fontId="0" fillId="0" borderId="0" xfId="0"/>
<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment horizontal="center"/></xf>
<xf numFmtId="0" fontId="4" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment horizontal="left" indent="2"/></xf>
<xf numFmtId="0" fontId="3" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment horizontal="right"/></xf>
<xf numFmtId="37" fontId="0" fillId="0" borderId="0" xfId="0"/>
</cellXfs>
</styleSheet>
'@
$strings = @('حصر الكميات - مشروع النفق', 'البند', 'الكمية', 'سعر الوحدة', 'الإجمالي', 'تاريخ التوريد', 'الخصم',
             'كابل كهربائي 4 مم', 'لوحة توزيع رئيسية', 'مواسير PVC قطر 110 مم', 'خصم إضافي',
             'ملاحظة: الأسعار شاملة النقل والتركيب داخل الموقع', 'الإجمالي الكلي',
             'Number formats', 'Value', 'Shown as', 'Centered', 'Indented (Arial)', 'Right, italic red',
             'Hidden column C', 'A very long text that runs on into the empty cells next to it')
$sst = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?><sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="' + $strings.Count + '" uniqueCount="' + $strings.Count + '">'
foreach ($s in $strings) { $sst += '<si><t xml:space="preserve">' + [Security.SecurityElement]::Escape($s) + '</t></si>' }
$sst += '</sst>'

$sheet1 = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<sheetViews><sheetView rightToLeft="1" workbookViewId="0"/></sheetViews>
<sheetFormatPr defaultRowHeight="15"/>
<cols><col min="1" max="1" width="30" customWidth="1"/><col min="2" max="2" width="10" customWidth="1"/>
<col min="3" max="4" width="14" customWidth="1"/><col min="5" max="5" width="13" customWidth="1"/><col min="6" max="6" width="9" customWidth="1"/></cols>
<sheetData>
<row r="1" ht="30" customHeight="1"><c r="A1" s="1" t="s"><v>0</v></c><c r="B1" s="1"/><c r="C1" s="1"/><c r="D1" s="1"/><c r="E1" s="1"/><c r="F1" s="1"/></row>
<row r="2" ht="20" customHeight="1"><c r="A2" s="2" t="s"><v>1</v></c><c r="B2" s="2" t="s"><v>2</v></c><c r="C2" s="2" t="s"><v>3</v></c><c r="D2" s="2" t="s"><v>4</v></c><c r="E2" s="2" t="s"><v>5</v></c><c r="F2" s="2" t="s"><v>6</v></c></row>
<row r="3"><c r="A3" s="3" t="s"><v>7</v></c><c r="B3" s="4"><v>1250</v></c><c r="C3" s="5"><v>45.5</v></c><c r="D3" s="5"><v>56875</v></c><c r="E3" s="6"><v>45001</v></c><c r="F3" s="7"><v>0.05</v></c></row>
<row r="4"><c r="A4" s="3" t="s"><v>8</v></c><c r="B4" s="4"><v>4</v></c><c r="C4" s="5"><v>12000</v></c><c r="D4" s="5"><v>48000</v></c><c r="E4" s="6"><v>45015</v></c><c r="F4" s="7"><v>0.1</v></c></row>
<row r="5"><c r="A5" s="3" t="s"><v>9</v></c><c r="B5" s="4"><v>300</v></c><c r="C5" s="5"><v>87.25</v></c><c r="D5" s="5"><v>26175</v></c><c r="E5" s="6"><v>45030</v></c><c r="F5" s="7"><v>0</v></c></row>
<row r="6"><c r="A6" s="3" t="s"><v>10</v></c><c r="B6" s="4"/><c r="C6" s="5"/><c r="D6" s="8"><v>-5000</v></c><c r="E6" s="6"/><c r="F6" s="7"/></row>
<row r="7" ht="45" customHeight="1"><c r="A7" s="9" t="s"><v>11</v></c><c r="B7" s="3"/><c r="C7" s="3"/><c r="D7" s="3"/><c r="E7" s="3"/><c r="F7" s="3"/></row>
<row r="8" ht="18" customHeight="1"><c r="A8" s="10" t="s"><v>12</v></c><c r="B8" s="10"/><c r="C8" s="10"/><c r="D8" s="11"><v>126050</v></c><c r="E8" s="10"/><c r="F8" s="10"/></row>
</sheetData>
<mergeCells count="2"><mergeCell ref="A1:F1"/><mergeCell ref="A8:C8"/></mergeCells>
</worksheet>
'@
$sheet2 = @'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<sheetViews><sheetView workbookViewId="0"/></sheetViews>
<cols><col min="1" max="1" width="22" customWidth="1"/><col min="2" max="2" width="16" customWidth="1"/>
<col min="3" max="3" width="12" hidden="1" customWidth="1"/><col min="4" max="4" width="18" customWidth="1"/></cols>
<sheetData>
<row r="1"><c r="A1" s="2" t="s"><v>13</v></c><c r="B1" s="2" t="s"><v>14</v></c><c r="C1" t="s"><v>19</v></c><c r="D1" s="2" t="s"><v>15</v></c></row>
<row r="2"><c r="A2"><v>1234567.891</v></c><c r="B2" s="5"><v>1234567.891</v></c><c r="D2" s="12"><v>1234567.891</v></c></row>
<row r="3"><c r="A3"><v>0.000012345</v></c><c r="B3" s="13"><v>0.000012345</v></c><c r="D3" s="16"><v>0.1234</v></c></row>
<row r="4"><c r="A4"><v>45230.75</v></c><c r="B4" s="14"><v>45230.75</v></c><c r="D4" s="15"><v>45230.75</v></c></row>
<row r="5"><c r="A5"><v>-1500</v></c><c r="B5" s="20"><v>-1500</v></c><c r="D5" s="4"><v>99999999</v></c></row>
<row r="6" ht="24" customHeight="1"><c r="A6" s="17" t="s"><v>16</v></c><c r="B6" s="18" t="s"><v>17</v></c><c r="D6" s="19" t="s"><v>18</v></c></row>
<row r="7"><c r="A7" t="s"><v>20</v></c></row>
<row r="8"><c r="A8" t="b"><v>1</v></c><c r="B8" t="e"><v>#DIV/0!</v></c><c r="D8"><v>123456789012345</v></c></row>
</sheetData>
</worksheet>
'@

$tmp = Join-Path $env:TEMP ('mzr-xl-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
$xlsx = Join-Path $tmp 'تقرير منسق.xlsx'
$z = [System.IO.Compression.ZipFile]::Open($xlsx, 'Create')
function Part($name, $text) {
    $e = $z.CreateEntry($name)
    $w = New-Object System.IO.StreamWriter($e.Open(), (New-Object System.Text.UTF8Encoding $false))
    $w.Write($text); $w.Close()
}
Part '[Content_Types].xml' $ct; Part '_rels/.rels' $rels; Part 'xl/workbook.xml' $wb; Part 'xl/_rels/workbook.xml.rels' $wbRels
Part 'xl/theme/theme1.xml' $theme; Part 'xl/styles.xml' $styles; Part 'xl/sharedStrings.xml' $sst
Part 'xl/worksheets/sheet1.xml' $sheet1; Part 'xl/worksheets/sheet2.xml' $sheet2
$z.Dispose()
if (Test-Path $out) { Remove-Item $out }
$a = [System.IO.Compression.ZipFile]::Open($out, 'Create')
[void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($a, $xlsx, 'إكسيل/تقرير منسق.xlsx')
$a.Dispose()
Remove-Item $tmp -Recurse -Force
Write-Host "Wrote $out"
