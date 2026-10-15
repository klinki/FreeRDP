from pathlib import Path
import argparse
p=argparse.ArgumentParser(description='Insert a test-only scheduling hook at the actual event publication point.')
p.add_argument('source',type=Path)
a=p.parse_args()
f=a.source/'ProtoSDK/MS-RDPEMT/SecureChannel/RdpeudpTLSChannel.cs'
s=f.read_text()
marker='            this.rdpeudpSocket.Received += ReceiveBytes;'
assert s.count(marker)==1
assert 'PublicationObserverForRegression' not in s
s=s.replace('        #region Constructor\n', '        public static Action<RdpeudpTLSChannel> PublicationObserverForRegression;\n\n        #region Constructor\n',1)
s=s.replace(marker, marker+'\n            PublicationObserverForRegression?.Invoke(this);',1)
f.write_text(s)
