// Decompiles a fixed list of functions (start,end,label) into individual .c files.
// Args: <outDir> <start:end:label> ...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        // Disassemble + create every requested function first so calls between them resolve.
        for (int i = 1; i < args.length; i++) {
            String[] p = args[i].split(":");
            Address s = toAddr(Long.parseUnsignedLong(p[0], 16));
            Address e = toAddr(Long.parseUnsignedLong(p[1], 16) - 1);
            new DisassembleCommand(new AddressSet(s, e), null, true).applyTo(currentProgram, monitor);
            if (getFunctionAt(s) == null) {
                new CreateFunctionCmd(s).applyTo(currentProgram, monitor);
            }
        }
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.openProgram(currentProgram);
        for (int i = 1; i < args.length; i++) {
            String[] p = args[i].split(":");
            Address s = toAddr(Long.parseUnsignedLong(p[0], 16));
            Function f = getFunctionAt(s);
            String label = p.length > 2 ? p[2] : p[0];
            try (PrintWriter w = new PrintWriter(new File(outDir, p[0] + "_" + label + ".c"), "UTF-8")) {
                if (f == null) { w.println("// could not create function at " + s); continue; }
                DecompileResults r = di.decompileFunction(f, 300, monitor);
                w.println("// " + label + "  @ " + s + "  (TS4_x64.exe 1.118.257.1020, Ghidra 12.1.4, no auto-analysis)");
                if (r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
                else w.println("// decompile failed: " + r.getErrorMessage());
            }
            println("done " + label);
        }
        di.dispose();
    }
}
