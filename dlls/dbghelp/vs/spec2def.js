WScript.Echo('EXPORTS');

var buf;
while (!WScript.StdIn.AtEndOfStream)
    buf += WScript.StdIn.ReadAll();
var lines = buf.split('\n');

for (var i=0; i<lines.length; i++) {
	var line = lines[i];
    var m = line.match(/^@ stdcall (\w+)\(.*\)$/);
    if (m)
    	WScript.Echo(m[1]);
}

// Sleepy API
WScript.Echo("SymSetDbgPrint");
