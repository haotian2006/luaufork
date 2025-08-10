// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lua.h"
#include "lualib.h"

// #include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Parser.h"
#include "Luau/TimeTrace.h"

// #include "Luau/Flags.h"

#include <memory>
#include <string>

#include <string.h>

// LUAU_FASTFLAG(DebugLuauTimeTracing)

enum class CompileFormat
{
    Text,
    Binary,
    Remarks,
    Codegen,        // Prints annotated native code including IR and assembly
    CodegenAsm,     // Prints annotated native code assembly
    CodegenIr,      // Prints annotated native code IR
    CodegenVerbose, // Prints annotated native code including IR, assembly and outlined code
    CodegenNull,
    Null
};

enum class RecordStats
{
    None,
    Total,
    File,
    Function
};

struct GlobalOptions
{
    int optimizationLevel = 1;
    int debugLevel = 1;
    int typeInfoLevel = 0;

    const char* vectorLib = nullptr;
    const char* vectorCtor = nullptr;
    const char* vectorType = nullptr;
} globalOptions;

static Luau::CompileOptions copts()
{
    Luau::CompileOptions result = {};
    result.optimizationLevel = globalOptions.optimizationLevel;
    result.debugLevel = globalOptions.debugLevel;
    result.typeInfoLevel = globalOptions.typeInfoLevel;

    result.vectorLib = globalOptions.vectorLib;
    result.vectorCtor = globalOptions.vectorCtor;
    result.vectorType = globalOptions.vectorType;

    return result;
}

static std::optional<CompileFormat> getCompileFormat(const char* name)
{
    if (strcmp(name, "text") == 0)
        return CompileFormat::Text;
    else if (strcmp(name, "binary") == 0)
        return CompileFormat::Binary;
    else if (strcmp(name, "text") == 0)
        return CompileFormat::Text;
    else if (strcmp(name, "remarks") == 0)
        return CompileFormat::Remarks;
    else if (strcmp(name, "codegen") == 0)
        return CompileFormat::Codegen;
    else if (strcmp(name, "codegenasm") == 0)
        return CompileFormat::CodegenAsm;
    else if (strcmp(name, "codegenir") == 0)
        return CompileFormat::CodegenIr;
    else if (strcmp(name, "codegenverbose") == 0)
        return CompileFormat::CodegenVerbose;
    else if (strcmp(name, "codegennull") == 0)
        return CompileFormat::CodegenNull;
    else if (strcmp(name, "null") == 0)
        return CompileFormat::Null;
    else
        return std::nullopt;
}

static void report(const char* name, const Luau::Location& location, const char* type, const char* message)
{
    fprintf(stderr, "%s(%d,%d): %s: %s\n", name, location.begin.line + 1, location.begin.column + 1, type, message);
}

static void reportError(const char* name, const Luau::ParseError& error)
{
    report(name, error.getLocation(), "SyntaxError", error.what());
}

static void reportError(const char* name, const Luau::CompileError& error)
{
    report(name, error.getLocation(), "CompileError", error.what());
}

// static std::string getCodegenAssembly(
//     const char* name,
//     const std::string& bytecode,
//     Luau::CodeGen::AssemblyOptions options,
//     Luau::CodeGen::LoweringStats* stats
// )
// {
//     std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
//     lua_State* L = globalState.get();

//     if (luau_load(L, name, bytecode.data(), bytecode.size(), 0) == 0)
//         return Luau::CodeGen::getAssembly(L, -1, options, stats);

//     fprintf(stderr, "Error loading bytecode %s\n", name);
//     return "";
// }

static void annotateInstruction(void* context, std::string& text, int fid, int instpos)
{
    Luau::BytecodeBuilder& bcb = *(Luau::BytecodeBuilder*)context;

    bcb.annotateInstruction(text, fid, instpos);
}

struct CompileStats
{
    size_t lines;
    size_t bytecode;
    size_t bytecodeInstructionCount;
    size_t codegen;

    double readTime;
    double miscTime;
    double parseTime;
    double compileTime;
    double codegenTime;

    //Luau::CodeGen::LoweringStats lowerStats;

    CompileStats& operator+=(const CompileStats& that)
    {
        this->lines += that.lines;
        this->bytecode += that.bytecode;
        this->bytecodeInstructionCount += that.bytecodeInstructionCount;
        this->codegen += that.codegen;
        this->readTime += that.readTime;
        this->miscTime += that.miscTime;
        this->parseTime += that.parseTime;
        this->compileTime += that.compileTime;
        this->codegenTime += that.codegenTime;
        //this->lowerStats += that.lowerStats;

        return *this;
    }

    CompileStats operator+(const CompileStats& other) const
    {
        CompileStats result(*this);
        result += other;
        return result;
    }
};

#define WRITE_NAME(INDENT, NAME) fprintf(fp, INDENT "\"" #NAME "\": ")
#define WRITE_PAIR(INDENT, NAME, FORMAT) fprintf(fp, INDENT "\"" #NAME "\": " FORMAT, stats.NAME)
#define WRITE_PAIR_STRING(INDENT, NAME, FORMAT) fprintf(fp, INDENT "\"" #NAME "\": " FORMAT, stats.NAME.c_str())

// void serializeFunctionStats(FILE* fp, const Luau::CodeGen::FunctionStats& stats)
// {
//     fprintf(fp, "                {\n");
//     WRITE_PAIR_STRING("                    ", name, "\"%s\",\n");
//     WRITE_PAIR("                    ", line, "%d,\n");
//     WRITE_PAIR("                    ", bcodeCount, "%u,\n");
//     WRITE_PAIR("                    ", irCount, "%u,\n");
//     WRITE_PAIR("                    ", asmCount, "%u,\n");
//     WRITE_PAIR("                    ", asmSize, "%u,\n");

//     WRITE_NAME("                    ", bytecodeSummary);
//     const size_t nestingLimit = stats.bytecodeSummary.size();

//     if (nestingLimit == 0)
//         fprintf(fp, "[]");
//     else
//     {
//         fprintf(fp, "[\n");
//         for (size_t i = 0; i < nestingLimit; ++i)
//         {
//             const std::vector<unsigned>& counts = stats.bytecodeSummary[i];
//             fprintf(fp, "                        [");
//             for (size_t j = 0; j < counts.size(); ++j)
//             {
//                 fprintf(fp, "%u", counts[j]);
//                 if (j < counts.size() - 1)
//                     fprintf(fp, ", ");
//             }
//             fprintf(fp, "]");
//             if (i < stats.bytecodeSummary.size() - 1)
//                 fprintf(fp, ",\n");
//         }
//         fprintf(fp, "\n                    ]");
//     }

//     fprintf(fp, "\n                }");
// }

// void serializeBlockLinearizationStats(FILE* fp, const Luau::CodeGen::BlockLinearizationStats& stats)
// {
//     fprintf(fp, "{\n");

//     WRITE_PAIR("                ", constPropInstructionCount, "%u,\n");
//     WRITE_PAIR("                ", timeSeconds, "%f\n");

//     fprintf(fp, "            }");
// }

// void serializeLoweringStats(FILE* fp, const Luau::CodeGen::LoweringStats& stats)
// {
//     fprintf(fp, "{\n");

//     WRITE_PAIR("            ", totalFunctions, "%u,\n");
//     WRITE_PAIR("            ", skippedFunctions, "%u,\n");
//     WRITE_PAIR("            ", spillsToSlot, "%d,\n");
//     WRITE_PAIR("            ", spillsToRestore, "%d,\n");
//     WRITE_PAIR("            ", maxSpillSlotsUsed, "%u,\n");
//     WRITE_PAIR("            ", blocksPreOpt, "%u,\n");
//     WRITE_PAIR("            ", blocksPostOpt, "%u,\n");
//     WRITE_PAIR("            ", maxBlockInstructions, "%u,\n");
//     WRITE_PAIR("            ", regAllocErrors, "%d,\n");
//     WRITE_PAIR("            ", loweringErrors, "%d,\n");

//     WRITE_NAME("            ", blockLinearizationStats);
//     serializeBlockLinearizationStats(fp, stats.blockLinearizationStats);
//     fprintf(fp, ",\n");

//     WRITE_NAME("            ", functions);
//     const size_t functionCount = stats.functions.size();

//     if (functionCount == 0)
//         fprintf(fp, "[]");
//     else
//     {
//         fprintf(fp, "[\n");
//         for (size_t i = 0; i < functionCount; ++i)
//         {
//             serializeFunctionStats(fp, stats.functions[i]);
//             if (i < functionCount - 1)
//                 fprintf(fp, ",\n");
//         }
//         fprintf(fp, "\n            ]");
//     }

//     fprintf(fp, "\n        }");
// }

// void serializeCompileStats(FILE* fp, const CompileStats& stats)
// {
//     fprintf(fp, "{\n");

//     WRITE_PAIR("        ", lines, "%zu,\n");
//     WRITE_PAIR("        ", bytecode, "%zu,\n");
//     WRITE_PAIR("        ", bytecodeInstructionCount, "%zu,\n");
//     WRITE_PAIR("        ", codegen, "%zu,\n");
//     WRITE_PAIR("        ", readTime, "%f,\n");
//     WRITE_PAIR("        ", miscTime, "%f,\n");
//     WRITE_PAIR("        ", parseTime, "%f,\n");
//     WRITE_PAIR("        ", compileTime, "%f,\n");
//     WRITE_PAIR("        ", codegenTime, "%f,\n");

//     WRITE_NAME("        ", lowerStats);
//     serializeLoweringStats(fp, stats.lowerStats);

//     fprintf(fp, "\n    }");
// }

#undef WRITE_NAME
#undef WRITE_PAIR
#undef WRITE_PAIR_STRING

static double recordDeltaTime(double& timer)
{
    double now = Luau::TimeTrace::getClock();
    double delta = now - timer;
    timer = now;
    return delta;
}


static int assertionHandler(const char* expr, const char* file, int line, const char* function)
{
    printf("%s(%d): ASSERTION FAILED: %s\n", file, line, expr);
    return 1;
}

std::string escapeFilename(const std::string& filename)
{
    std::string escaped;
    escaped.reserve(filename.size());

    for (const char ch : filename)
    {
        switch (ch)
        {
        case '\\':
            escaped.push_back('/');
            break;
        case '"':
            escaped.push_back('\\');
            escaped.push_back(ch);
            break;
        default:
            escaped.push_back(ch);
        }
    }

    return escaped;
}
std::string compileWebMain(int argc, char** argv, const char* sourceCode)
{
    Luau::assertHandler() = assertionHandler;

    //setLuauFlagsDefault();

    CompileFormat compileFormat = CompileFormat::Text;
    //Luau::CodeGen::AssemblyOptions::Target assemblyTarget = Luau::CodeGen::AssemblyOptions::Host;
    RecordStats recordStats = RecordStats::None;
    std::string statsFile("stats.json");
    bool bytecodeSummary = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            return "Help requested";
        }
        else if (strncmp(argv[i], "-O", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
                return "Error: Optimization level must be between 0 and 2 inclusive.";
            globalOptions.optimizationLevel = level;
        }
        else if (strncmp(argv[i], "-g", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 2)
                return "Error: Debug level must be between 0 and 2 inclusive.";
            globalOptions.debugLevel = level;
        }
        else if (strncmp(argv[i], "-t", 2) == 0)
        {
            int level = atoi(argv[i] + 2);
            if (level < 0 || level > 1)
                return "Error: Type info level must be between 0 and 1 inclusive.";
            globalOptions.typeInfoLevel = level;
        }
        else if (strncmp(argv[i], "--target=", 9) == 0)
        {
            const char* value = argv[i] + 9;

            // if (strcmp(value, "a64") == 0)
            //     assemblyTarget = Luau::CodeGen::AssemblyOptions::A64;
            // else if (strcmp(value, "a64_nf") == 0)
            //     assemblyTarget = Luau::CodeGen::AssemblyOptions::A64_NoFeatures;
            // else if (strcmp(value, "x64") == 0)
            //     assemblyTarget = Luau::CodeGen::AssemblyOptions::X64_SystemV;
            // else if (strcmp(value, "x64_ms") == 0)
            //     assemblyTarget = Luau::CodeGen::AssemblyOptions::X64_Windows;
            // else
            //     return "Error: unknown target";
        }
        else if (strcmp(argv[i], "--timetrace") == 0)
        {
            //FFlag::DebugLuauTimeTracing.value = true;
        }
        else if (strncmp(argv[i], "--record-stats=", 15) == 0)
        {
            const char* value = argv[i] + 15;

            if (strcmp(value, "total") == 0)
                recordStats = RecordStats::Total;
            else if (strcmp(value, "file") == 0)
                recordStats = RecordStats::File;
            else if (strcmp(value, "function") == 0)
                recordStats = RecordStats::Function;
            else
                return "Error: unknown 'granularity' for '--record-stats'.";
        }
        else if (strncmp(argv[i], "--bytecode-summary", 18) == 0)
        {
            bytecodeSummary = true;
        }
        else if (strncmp(argv[i], "--stats-file=", 13) == 0)
        {
            statsFile = argv[i] + 13;

            if (statsFile.size() == 0)
                return "Error: filename missing for '--stats-file'.";
        }
        else if (strncmp(argv[i], "--fflags=", 9) == 0)
        {
            //setLuauFlags(argv[i] + 9);
        }
        else if (strncmp(argv[i], "--vector-lib=", 13) == 0)
        {
            globalOptions.vectorLib = argv[i] + 13;
        }
        else if (strncmp(argv[i], "--vector-ctor=", 14) == 0)
        {
            globalOptions.vectorCtor = argv[i] + 14;
        }
        else if (strncmp(argv[i], "--vector-type=", 14) == 0)
        {
            globalOptions.vectorType = argv[i] + 14;
        }
        else if (argv[i][0] == '-' && argv[i][1] == '-' && getCompileFormat(argv[i] + 2))
        {
            compileFormat = *getCompileFormat(argv[i] + 2);
        }
        else if (argv[i][0] == '-')
        {
            return std::string("Error: Unrecognized option '") + argv[i] + "'.";
        }
    }

    if (bytecodeSummary && (recordStats != RecordStats::Function))
        return "'Error: Required '--record-stats=function' for '--bytecode-summary'.";

// #if !defined(LUAU_ENABLE_TIME_TRACE)
//     if (FFlag::DebugLuauTimeTracing)
//         return "To run with --timetrace, Luau has to be built with LUAU_ENABLE_TIME_TRACE enabled";
// #endif

    CompileStats stats = {};

    std::string output;
    try
    {
        Luau::BytecodeBuilder bcb;

        // Luau::CodeGen::AssemblyOptions options;
        // options.target = assemblyTarget;
        // options.outputBinary = compileFormat == CompileFormat::CodegenNull;

        // if (!options.outputBinary)
        // {
        //     options.includeAssembly = compileFormat != CompileFormat::CodegenIr;
        //     options.includeIr = compileFormat != CompileFormat::CodegenAsm;
        //     options.includeIrTypes = compileFormat != CompileFormat::CodegenAsm;
        //     options.includeOutlinedCode = compileFormat == CompileFormat::CodegenVerbose;
        // }

        // options.annotator = annotateInstruction;
        // options.annotatorContext = &bcb;

        if (compileFormat == CompileFormat::Text)
        {
            bcb.setDumpFlags(
                Luau::BytecodeBuilder::Dump_Code | Luau::BytecodeBuilder::Dump_Source | Luau::BytecodeBuilder::Dump_Locals |
                Luau::BytecodeBuilder::Dump_Remarks | Luau::BytecodeBuilder::Dump_Types
            );
            bcb.setDumpSource(sourceCode);
        }
        else if (compileFormat == CompileFormat::Remarks)
        {
            bcb.setDumpFlags(Luau::BytecodeBuilder::Dump_Source | Luau::BytecodeBuilder::Dump_Remarks);
            bcb.setDumpSource(sourceCode);
        }
        else if (compileFormat == CompileFormat::Codegen || compileFormat == CompileFormat::CodegenAsm || compileFormat == CompileFormat::CodegenIr ||
                 compileFormat == CompileFormat::CodegenVerbose)
        {
            bcb.setDumpFlags(
                Luau::BytecodeBuilder::Dump_Code | Luau::BytecodeBuilder::Dump_Source | Luau::BytecodeBuilder::Dump_Locals |
                Luau::BytecodeBuilder::Dump_Remarks
            );
            bcb.setDumpSource(sourceCode);
        }

        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        Luau::ParseResult result = Luau::Parser::parse(sourceCode, strlen(sourceCode), names, allocator);

        if (!result.errors.empty())
            throw Luau::ParseErrors(result.errors);

        stats.lines += result.lines;

        Luau::compileOrThrow(bcb, result, names, copts());
        stats.bytecode += bcb.getBytecode().size();
        stats.bytecodeInstructionCount = bcb.getTotalInstructionCount();

        switch (compileFormat)
        {
        case CompileFormat::Text:
            output = bcb.dumpEverything();
            break;
        case CompileFormat::Remarks:
            output = bcb.dumpSourceRemarks();
            break;
        case CompileFormat::Binary:
            output.assign(bcb.getBytecode().begin(), bcb.getBytecode().end());
            break;
        case CompileFormat::Codegen:
        case CompileFormat::CodegenAsm:
        case CompileFormat::CodegenIr:
        case CompileFormat::CodegenVerbose:
            //output = getCodegenAssembly("webinput", bcb.getBytecode(), options, &stats.lowerStats);
            output = "Codegen not implemented";
            break;
        case CompileFormat::CodegenNull:
            //stats.codegen += getCodegenAssembly("webinput", bcb.getBytecode(), options, &stats.lowerStats).size();
            break;
        case CompileFormat::Null:
            break;
        }

        return output;
    }
    catch (Luau::ParseErrors& e)
    {
        std::string err;
        for (auto& error : e.getErrors())
        {
            char buf[256];
            snprintf(
                buf,
                sizeof(buf),
                "SyntaxError at line %d, column %d: %s\n",
                error.getLocation().begin.line + 1,
                error.getLocation().begin.column + 1,
                error.what()
            );
            err += buf;
        }
        return err;
    }
    catch (Luau::CompileError& e)
    {
        char buf[256];
        snprintf(
            buf, sizeof(buf), "CompileError at line %d, column %d: %s\n", e.getLocation().begin.line + 1, e.getLocation().begin.column + 1, e.what()
        );
        return buf;
    }
}

std::vector<std::string> splitArgs(const std::string& input)
{
    std::vector<std::string> args;
    size_t start = 0;
    size_t end = 0;

    while (end < input.size())
    {
        while (start < input.size() && input[start] == ' ')
            ++start;

        if (start >= input.size())
            break;

        end = start;
        while (end < input.size() && input[end] != ' ')
            ++end;

        args.push_back(input.substr(start, end - start));

        start = end;
    }
    return args;
}

const char* exportCompile(const char* input, const char* sourceCode)
{
    std::vector<std::string> args = splitArgs(input);


    std::vector<char*> argv;
    for (auto& s : args)
    {
        argv.push_back(const_cast<char*>(s.c_str()));
    }

    std::string result = compileWebMain(argv.size(), argv.data(), sourceCode);

    char* cstr = strdup(result.c_str()); 
    return cstr;
}

extern "C" void freeCompileResult(const char* ptr) {
    free((void*)ptr);
}