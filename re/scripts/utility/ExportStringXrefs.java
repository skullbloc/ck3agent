// Export all defined strings with their addresses and cross-references to a CSV file.
// Designed for the CK3 binary (~191MB, 1.7M+ strings in `strings` output).
// Ghidra's defined string set is smaller (only strings Ghidra identified during analysis),
// but still expected to be in the hundreds of thousands.
//
// Output CSV columns:
//   StringAddress, String, StringLength, Category, RefFromAddress, RefFromFunction, RefFromFunctionAddress
//
// Each row is one (string, reference) pair. Strings with no references still get one row
// with empty ref fields. Strings with multiple references get multiple rows.
//
// The "Category" column classifies strings into RE-useful buckets:
//   EFFECT_DESC    - Effect documentation (starts with "Adds ", "Removes ", etc.)
//   TRIGGER_DESC   - Trigger documentation (starts with "Checks ", "Is ", "Does ", etc.)
//   SCRIPT_CMD     - Script command names (lowercase_with_underscores like add_gold)
//   SOURCE_PATH    - Embedded absolute source file paths (assert/debug macros):
//                    a POSIX or drive-letter path ending in a C/C++ source extension
//   CONSOLE_CMD    - Console command strings (Wrong number of arguments)
//   AI_STRING      - AI-related strings (ai_ prefix or AI_ prefix)
//   RTTI           - RTTI type name strings (mangled C++ names)
//   FORMAT_STRING  - Format strings containing % or {} placeholders
//   LOC_KEY        - Localization keys (ALL_CAPS_WITH_UNDERSCORES)
//   ERROR_DEBUG    - Error/debug messages
//   OTHER          - Everything else
//
// Usage (headless):
//   analyzeHeadless <project_dir> <project_name> \
//     -process <program> -noanalysis \
//     -postScript ExportStringXrefs.java [<output_dir>]
//
// Output directory: the script argument, else $CK3_RE_OUT, else the working
// directory (headless) or a directory prompt (GUI). Writes string_xrefs.csv and
// string_xrefs_summary.txt there.
//
//@category Analysis
//@author CK3 RE Project

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.util.DefinedStringIterator;

import java.io.*;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

public class ExportStringXrefs extends GhidraScript {

    // Output paths, resolved in run() (resolveOutputDir)
    private String OUTPUT_FILE;
    private String SUMMARY_FILE;

    // Category patterns (compiled once)
    private static final Pattern PAT_SCRIPT_CMD = Pattern.compile(
        "^(add_|remove_|set_|create_|destroy_|change_|get_|is_|has_|can_|any_|every_|" +
        "random_|ordered_|trigger_|on_|reverse_|force_|try_|open_|close_|save_|" +
        "spawn_|start_|end_|kill_|move_|clear_|enable_|disable_|reset_)[a-z_]+$"
    );
    private static final Pattern PAT_EFFECT_DESC = Pattern.compile(
        "^(Adds |Removes |Sets |Creates |Destroys |Changes |Starts |Ends |Kills |Spawns |" +
        "Imprisons |Releases |Divorces |Converts |Marries |Makes |Transfers |Moves |Clears |" +
        "Enables |Disables |Assigns |Applies |Grants |Revokes |Forces |Activates |Deactivates |" +
        "Generates |Banishes |Fires |Aborts |Cancels |Completes |Promotes |Demotes )"
    );
    private static final Pattern PAT_TRIGGER_DESC = Pattern.compile(
        "^(Checks |Is |Does |Has |Can |Are |Was |Were |Should |Returns whether |Returns true |" +
        "Returns the |Returns if |Compares |Evaluates )"
    );
    // Paradox's Linux build embeds paths under its build-machine root; other
    // platforms use other roots, so match the shape, not one prefix.
    private static final Pattern PAT_SOURCE_PATH = Pattern.compile(
        "^(/|[A-Za-z]:[\\\\/]).*\\.(cpp|cxx|cc|c|hpp|hxx|h|inl)\\b");
    private static final Pattern PAT_AI = Pattern.compile("^(ai_|AI_|CAI)");
    private static final Pattern PAT_RTTI_MANGLED = Pattern.compile("^\\d+C[A-Z]");
    private static final Pattern PAT_FORMAT_PERCENT = Pattern.compile("%[sdifcpuxlX]");
    private static final Pattern PAT_FORMAT_BRACE = Pattern.compile("\\{[^}]*\\}");
    private static final Pattern PAT_LOC_KEY = Pattern.compile("^[A-Z][A-Z0-9_]{4,}$");
    private static final Pattern PAT_ERROR_DEBUG = Pattern.compile(
        "(?i)(error|failed|invalid|cannot|warning|assert|fatal|exception|unexpected|missing|" +
        "bad |illegal |not found|out of range|overflow|null pointer)"
    );
    private static final Pattern PAT_CONSOLE = Pattern.compile(
        "(Wrong number of arguments|console command|^Run |^Toggles |^Prints |^Lists |^Loads |^Saves |^Teleports )"
    );

    // Counters for summary
    private int totalStrings = 0;
    private int stringsWithRefs = 0;
    private int totalRefs = 0;
    private int[] categoryCounts = new int[11]; // one per category
    private static final String[] CATEGORY_NAMES = {
        "EFFECT_DESC", "TRIGGER_DESC", "SCRIPT_CMD", "SOURCE_PATH", "CONSOLE_CMD",
        "AI_STRING", "RTTI", "FORMAT_STRING", "LOC_KEY", "ERROR_DEBUG", "OTHER"
    };

    @Override
    public void run() throws Exception {
        if (currentProgram == null) {
            printerr("No program loaded.");
            return;
        }

        File outDir = resolveOutputDir();
        OUTPUT_FILE = new File(outDir, "string_xrefs.csv").getPath();
        SUMMARY_FILE = new File(outDir, "string_xrefs_summary.txt").getPath();

        println("ExportStringXrefs: Starting export for " + currentProgram.getName());
        println("Output: " + OUTPUT_FILE);

        ReferenceManager refMgr = currentProgram.getReferenceManager();
        Listing listing = currentProgram.getListing();
        FunctionManager funcMgr = currentProgram.getFunctionManager();

        // Use BufferedWriter for performance on large output
        BufferedWriter writer = new BufferedWriter(new FileWriter(OUTPUT_FILE), 1024 * 1024);

        // Write CSV header
        writer.write("StringAddress,String,StringLength,Category,RefFromAddress,RefFromFunction,RefFromFunctionAddress");
        writer.newLine();

        // Count total defined data for progress
        long totalData = currentProgram.getListing().getNumDefinedData();
        monitor.initialize(totalData);
        monitor.setMessage("Exporting string cross-references...");

        int progressInterval = 10000;
        int processed = 0;

        for (Data data : DefinedStringIterator.forProgram(currentProgram, null)) {
            if (monitor.isCancelled()) {
                println("Cancelled by user after " + totalStrings + " strings.");
                break;
            }

            StringDataInstance sdi = StringDataInstance.getStringDataInstance(data);
            String strValue = sdi.getStringValue();
            if (strValue == null || strValue.isEmpty()) {
                monitor.incrementProgress(1);
                continue;
            }

            totalStrings++;
            Address strAddr = data.getMinAddress();
            int strLen = strValue.length();

            // Classify the string
            String category = classifyString(strValue);
            int catIdx = getCategoryIndex(category);
            categoryCounts[catIdx]++;

            // Escape the string for CSV (handle commas, quotes, newlines)
            String escapedStr = escapeCSV(strValue);
            String strAddrHex = strAddr.toString();

            // Get all references TO this string address via iterator
            ReferenceIterator refIter = refMgr.getReferencesTo(strAddr);
            List<Reference> refs = new ArrayList<>();
            while (refIter.hasNext()) {
                refs.add(refIter.next());
            }

            if (refs.isEmpty()) {
                // Write one row with empty ref fields
                writer.write(strAddrHex);
                writer.write(',');
                writer.write(escapedStr);
                writer.write(',');
                writer.write(Integer.toString(strLen));
                writer.write(',');
                writer.write(category);
                writer.write(",,,");
                writer.newLine();
            } else {
                stringsWithRefs++;
                totalRefs += refs.size();

                for (Reference ref : refs) {
                    Address fromAddr = ref.getFromAddress();
                    String fromAddrHex = fromAddr.toString();

                    // Find the function containing the reference
                    Function func = funcMgr.getFunctionContaining(fromAddr);
                    String funcName = "";
                    String funcAddrHex = "";
                    if (func != null) {
                        funcName = escapeCSV(func.getName(true)); // include namespace
                        funcAddrHex = func.getEntryPoint().toString();
                    }

                    writer.write(strAddrHex);
                    writer.write(',');
                    writer.write(escapedStr);
                    writer.write(',');
                    writer.write(Integer.toString(strLen));
                    writer.write(',');
                    writer.write(category);
                    writer.write(',');
                    writer.write(fromAddrHex);
                    writer.write(',');
                    writer.write(funcName);
                    writer.write(',');
                    writer.write(funcAddrHex);
                    writer.newLine();
                }
            }

            processed++;
            if (processed % progressInterval == 0) {
                monitor.setMessage("Exported " + processed + " strings (" + totalRefs + " xrefs)...");
            }
            monitor.incrementProgress(1);
        }

        writer.flush();
        writer.close();

        // Write summary
        writeSummary();

        println("=== Export Complete ===");
        println("Total strings exported: " + totalStrings);
        println("Strings with xrefs:     " + stringsWithRefs);
        println("Total xref pairs:       " + totalRefs);
        println("Output:                 " + OUTPUT_FILE);
        println("Summary:                " + SUMMARY_FILE);
    }

    /**
     * Output directory: script argument 1, else $CK3_RE_OUT, else the working
     * directory when headless, else ask.
     */
    private File resolveOutputDir() throws Exception {
        String[] args = getScriptArgs();
        String dir = (args.length > 0 && !args[0].isEmpty()) ? args[0] : System.getenv("CK3_RE_OUT");
        File f;
        if (dir != null && !dir.isEmpty()) {
            f = new File(dir);
        } else if (isRunningHeadless()) {
            f = new File(System.getProperty("user.dir"));
        } else {
            f = askDirectory("ExportStringXrefs output directory", "Select");
        }
        f.mkdirs();
        return f;
    }

    /**
     * Classify a string into one of the RE-useful categories.
     * Priority order matters -- more specific categories checked first.
     */
    private String classifyString(String s) {
        // Source paths are unambiguous
        if (PAT_SOURCE_PATH.matcher(s).find()) return "SOURCE_PATH";

        // RTTI mangled names (start with digits followed by C and uppercase)
        if (PAT_RTTI_MANGLED.matcher(s).find()) return "RTTI";

        // Effect descriptions (long English sentences starting with action verbs)
        if (PAT_EFFECT_DESC.matcher(s).find()) return "EFFECT_DESC";

        // Trigger descriptions
        if (PAT_TRIGGER_DESC.matcher(s).find()) return "TRIGGER_DESC";

        // Console command patterns
        if (PAT_CONSOLE.matcher(s).find()) return "CONSOLE_CMD";

        // AI-related strings
        if (PAT_AI.matcher(s).find()) return "AI_STRING";

        // Script command names (lowercase_with_underscores, starts with known verb prefix)
        if (PAT_SCRIPT_CMD.matcher(s).matches()) return "SCRIPT_CMD";

        // Error/debug messages
        if (PAT_ERROR_DEBUG.matcher(s).find()) return "ERROR_DEBUG";

        // Format strings (contain % placeholders or {} format)
        if (PAT_FORMAT_PERCENT.matcher(s).find()) return "FORMAT_STRING";
        if (PAT_FORMAT_BRACE.matcher(s).find()) return "FORMAT_STRING";

        // Localization keys (ALL_CAPS_WITH_UNDERSCORES, 5+ chars)
        if (PAT_LOC_KEY.matcher(s).matches()) return "LOC_KEY";

        return "OTHER";
    }

    private int getCategoryIndex(String category) {
        for (int i = 0; i < CATEGORY_NAMES.length; i++) {
            if (CATEGORY_NAMES[i].equals(category)) return i;
        }
        return CATEGORY_NAMES.length - 1; // OTHER
    }

    /**
     * Escape a string for CSV output.
     * If it contains commas, quotes, or newlines, wrap in quotes and double any internal quotes.
     * Also truncate extremely long strings to avoid CSV tooling issues.
     */
    private String escapeCSV(String s) {
        // Truncate very long strings (some binary data might be identified as strings)
        if (s.length() > 2000) {
            s = s.substring(0, 2000) + "...[TRUNCATED]";
        }

        // Replace any literal newlines/carriage returns with visible escapes
        s = s.replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t");

        // If contains comma, quote, or special chars, wrap in double quotes
        if (s.contains(",") || s.contains("\"") || s.contains("\\")) {
            s = s.replace("\"", "\"\"");
            return "\"" + s + "\"";
        }
        return s;
    }

    /**
     * Write a human-readable summary file with statistics.
     */
    private void writeSummary() throws IOException {
        BufferedWriter sw = new BufferedWriter(new FileWriter(SUMMARY_FILE));

        sw.write("=== CK3 Binary String Cross-Reference Export Summary ===");
        sw.newLine();
        sw.write("Program: " + currentProgram.getName());
        sw.newLine();
        sw.write("Date: " + new java.util.Date().toString());
        sw.newLine();
        sw.newLine();

        sw.write("--- Overall Statistics ---");
        sw.newLine();
        sw.write("Total defined strings:      " + totalStrings);
        sw.newLine();
        sw.write("Strings with xrefs:         " + stringsWithRefs);
        sw.newLine();
        sw.write("Strings without xrefs:      " + (totalStrings - stringsWithRefs));
        sw.newLine();
        sw.write("Total xref pairs:           " + totalRefs);
        sw.newLine();
        if (stringsWithRefs > 0) {
            sw.write("Average xrefs per string:   " +
                String.format("%.1f", (double) totalRefs / stringsWithRefs));
            sw.newLine();
        }
        sw.newLine();

        sw.write("--- Category Breakdown ---");
        sw.newLine();
        sw.write(String.format("%-20s %10s %10s", "Category", "Count", "Percent"));
        sw.newLine();
        sw.write(String.format("%-20s %10s %10s", "--------", "-----", "-------"));
        sw.newLine();
        for (int i = 0; i < CATEGORY_NAMES.length; i++) {
            double pct = totalStrings > 0 ? (100.0 * categoryCounts[i] / totalStrings) : 0;
            sw.write(String.format("%-20s %10d %9.1f%%", CATEGORY_NAMES[i], categoryCounts[i], pct));
            sw.newLine();
        }
        sw.newLine();

        sw.write("--- RE Value Assessment ---");
        sw.newLine();
        int highValue = categoryCounts[0] + categoryCounts[1] + categoryCounts[2] +
                         categoryCounts[3] + categoryCounts[4] + categoryCounts[5];
        int medValue = categoryCounts[6] + categoryCounts[7] + categoryCounts[8] + categoryCounts[9];
        sw.write("High-value strings (EFFECT_DESC + TRIGGER_DESC + SCRIPT_CMD + SOURCE_PATH + CONSOLE_CMD + AI_STRING): "
            + highValue);
        sw.newLine();
        sw.write("Medium-value strings (RTTI + FORMAT_STRING + LOC_KEY + ERROR_DEBUG): " + medValue);
        sw.newLine();
        sw.write("Low-value strings (OTHER): " + categoryCounts[10]);
        sw.newLine();
        sw.newLine();

        sw.write("--- Key Landmark Categories for Function Identification ---");
        sw.newLine();
        sw.write("SOURCE_PATH:  Embedded source file paths from assert/debug macros.");
        sw.newLine();
        sw.write("              These directly name the .cpp/.h file containing the function.");
        sw.newLine();
        sw.write("EFFECT_DESC:  Natural-language documentation strings for script effects.");
        sw.newLine();
        sw.write("              Each maps 1:1 to a CEffect subclass registration.");
        sw.newLine();
        sw.write("TRIGGER_DESC: Natural-language documentation strings for script triggers.");
        sw.newLine();
        sw.write("              Each maps 1:1 to a CTrigger subclass registration.");
        sw.newLine();
        sw.write("SCRIPT_CMD:   Registered names for effects/triggers/scopes/modifiers.");
        sw.newLine();
        sw.write("              The string is used in PDXScript (e.g., 'add_gold') and");
        sw.newLine();
        sw.write("              its xref leads to the registration table entry.");
        sw.newLine();
        sw.write("CONSOLE_CMD:  Console command handler strings, each references a handler function.");
        sw.newLine();
        sw.write("AI_STRING:    AI subsystem strings, critical for the agent replacement target.");
        sw.newLine();

        sw.flush();
        sw.close();
    }
}
