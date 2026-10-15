"""Build a diagnostic adapter against the prepared suite, without editing either source tree."""
from pathlib import Path
import shutil
import subprocess
import sys

source, repo, output = map(lambda p: Path(p).resolve(), sys.argv[1:4])
output.mkdir(parents=True)
suite = repo / 'tools/windows-protocol-tests/.cache/suite'
shutil.copy2(suite / 'global.json', output / 'global.json')
relative = Path('TestSuites/RDP/Client/src/Adapter')
adapter = output / 'source' / relative
shutil.copytree(source / relative, adapter)
patch = Path(__file__).with_name('diagnostic-adapter.patch')
subprocess.run(['git', 'apply', '--check', str(patch)], cwd=output / 'source', check=True)
subprocess.run(['git', 'apply', str(patch)], cwd=output / 'source', check=True)
references = '\n'.join(
    f'<Reference Include="{p.stem}"><HintPath>{p}</HintPath></Reference>'
    for p in (suite / 'Bin').glob('*.dll')
    if p.name not in ('RDP_ClientAdapter.dll', 'RDP_ClientTestSuite.dll'))
project = adapter / 'RDP_ClientAdapter.csproj'
project.write_text(f'''<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><TargetFramework>net8.0</TargetFramework><AssemblyName>RDP_ClientAdapter</AssemblyName>
<AllowUnsafeBlocks>true</AllowUnsafeBlocks><GenerateAssemblyInfo>false</GenerateAssemblyInfo>
<NoWarn>CA2252</NoWarn></PropertyGroup><ItemGroup>{references}</ItemGroup></Project>''')
subprocess.run(['dotnet', 'build', str(project), '-c', 'Release', '-o', str(output / 'bin'),
                '--disable-build-servers'], cwd=output, check=True)
