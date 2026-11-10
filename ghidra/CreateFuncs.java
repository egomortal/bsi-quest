import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*; import ghidra.program.model.listing.*;
import ghidra.app.cmd.disassemble.DisassembleCommand; import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*; import java.util.*;
public class CreateFuncs extends GhidraScript {
  public void run() throws Exception {
    int ok=0,bad=0;
    for (String l: java.nio.file.Files.readAllLines(new File("/home/claude/bsi/newfuncs.txt").toPath())) {
      if (l.isEmpty()) continue; Address a=toAddr(Long.parseLong(l,16));
      if (getFunctionAt(a)!=null) continue;
      if (getInstructionAt(a)==null) new DisassembleCommand(a,null,true).applyTo(currentProgram);
      if (getInstructionAt(a)==null) { bad++; continue; }
      if (new CreateFunctionCmd(a).applyTo(currentProgram)) ok++; else bad++;
    }
    println("CREATED "+ok+" FAILED "+bad);
  }
}
