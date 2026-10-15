from pathlib import Path
import shlex,subprocess,sys
root=Path(sys.argv[1]).resolve(); build=root/'build/protocol-tests'; out=Path(sys.argv[2]).resolve();out.mkdir(parents=True,exist_ok=True)
f=root/'libfreerdp/core/rdp.c';s=f.read_text();needle='''\tif (!Stream_CheckAndLogRequiredLengthWLog(rdp->log, s, *length))
\t\treturn FALSE;

\treturn TRUE;
}'''; assert s.count(needle)==1
s=s.replace(needle,'''\tif (!Stream_CheckAndLogRequiredLengthWLog(rdp->log, s, *length))
\t\treturn FALSE;
\tif (Stream_GetRemainingLength(s) != *length)
\t{
\t\tWLog_Print(rdp->log, WLOG_ERROR, "Diagnostic MCS payload mismatch: declared=%u actual=%zu", *length, Stream_GetRemainingLength(s));
\t\treturn FALSE;
\t}

\treturn TRUE;
}''');(out/'rdp.c').write_text(s)
f=root/'libfreerdp/core/connection.c';s=f.read_text();start=s.index('state_run_t rdp_client_connect_demand_active(');tail=s[start:]; needle='''\tif (!rdp_read_share_control_header(rdp, s, nullptr, nullptr, &pduType, &pduSource))
\t\treturn STATE_RUN_FAILED;''';assert tail.count(needle)==1
tail=tail.replace(needle,'''\tUINT16 diagnosticRemaining = 0;
\tif (!rdp_read_share_control_header(rdp, s, nullptr, &diagnosticRemaining, &pduType, &pduSource))
\t\treturn STATE_RUN_FAILED;
\tif ((pduType == PDU_TYPE_DEMAND_ACTIVE) && (Stream_GetRemainingLength(s) != diagnosticRemaining))
\t{
\t\tWLog_Print(rdp->log, WLOG_ERROR, "Diagnostic DemandActive payload mismatch: declared=%u actual=%zu", diagnosticRemaining, Stream_GetRemainingLength(s));
\t\treturn STATE_RUN_FAILED;
\t}''');(out/'connection.c').write_text(s[:start]+tail)
flags={line.split(' = ',1)[0]:shlex.split(line.split(' = ',1)[1]) for line in (build/'libfreerdp/CMakeFiles/freerdp.dir/flags.make').read_text().splitlines() if ' = ' in line}
for name in ('rdp','connection'):
 cmd=['/usr/bin/cc']+flags['C_DEFINES']+flags['C_INCLUDES']+['-I'+str(root/'libfreerdp/core')]+flags['C_FLAGS']+['-o',str(out/(name+'.o')),'-c',str(out/(name+'.c'))];subprocess.run(cmd,check=True)
cmd=shlex.split((build/'libfreerdp/CMakeFiles/freerdp.dir/link.txt').read_text())
for name in ('rdp','connection'):
 cmd[cmd.index('CMakeFiles/freerdp.dir/core/'+name+'.c.o')]=str(out/(name+'.o'))
cmd[cmd.index('-o')+1]=str(out/'libfreerdp3.3.dylib')
subprocess.run(cmd,check=True,cwd=build/'libfreerdp')
