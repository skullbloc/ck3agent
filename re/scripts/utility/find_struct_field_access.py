# Ghidra script (Jython or PyGhidra): find instructions that access
# [REG + offset] where REG is not RSP/RBP. This finds struct field accesses,
# not stack accesses: the field-access half of struct-layout discovery.
# Args: offset_hex [start_va] [end_va]   (Ghidra VAs; default: the .text block,
#       else the first executable block, else the whole program)
#   analyzeHeadless <project_dir> <project_name> -process <program> -noanalysis \
#     -postScript find_struct_field_access.py 0x1b8
# Prints the first 50 hits as  address: instruction  [function @ entry].

args = getScriptArgs()
offset_target = int(args[0], 16) if args else 0x210

fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()
af = currentProgram.getAddressFactory()

code = currentProgram.getMemory().getBlock(".text")
if code is None:
    for block in currentProgram.getMemory().getBlocks():
        if block.isExecute():
            code = block
            break
if code is not None:
    start, end = code.getStart(), code.getEnd()
else:
    start, end = currentProgram.getMinAddress(), currentProgram.getMaxAddress()
if len(args) > 1:
    start = af.getAddress("0x%x" % int(args[1], 16))
if len(args) > 2:
    end = af.getAddress("0x%x" % int(args[2], 16))

results = []
count = 0
# exact displacement: 0x21 must not match 0x210
import re
hex_target = re.compile(r"0x0*%x(?![0-9a-f])" % offset_target)

inst_iter = listing.getInstructions(start, True)
while inst_iter.hasNext():
    inst = inst_iter.next()
    if inst.getAddress().compareTo(end) > 0:
        break
    count += 1

    for i in range(inst.getNumOperands()):
        rep = inst.getDefaultOperandRepresentation(i)
        if hex_target.search(rep.lower()) and "RSP" not in rep and "RBP" not in rep:
            func = fm.getFunctionContaining(inst.getAddress())
            func_name = func.getName() if func else "unknown"
            func_addr = func.getEntryPoint() if func else "?"
            results.append((str(inst.getAddress()), str(inst), func_name, str(func_addr)))
            break

    if count % 500000 == 0:
        print("Scanned %d instructions..." % count)

print("\nScanned %d instructions in %s..%s" % (count, start, end))
print("Found %d struct field accesses at offset 0x%x:" % (len(results), offset_target))
for addr, inst_str, func_name, func_addr in results[:50]:
    print("  %s: %s  [%s @ %s]" % (addr, inst_str, func_name, func_addr))
