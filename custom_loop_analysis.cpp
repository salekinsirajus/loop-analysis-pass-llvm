#include <fstream>
#include <memory>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "llvm-c/Core.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Dominators.h"


using namespace llvm;

static void CustomLoopAnalysis(Module *);

static void summarize(Module *M);
static void print_csv_file(std::string outputfile);

static cl::opt<std::string>
        InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::Required, cl::init("-"));

static cl::opt<std::string>
        OutputFilename(cl::Positional, cl::desc("<output bitcode>"), cl::Required, cl::init("out.bc"));

static cl::opt<bool>
        Mem2Reg("mem2reg",
                cl::desc("Perform memory to register promotion before CLA."),
                cl::init(false));

static cl::opt<bool>
        CSE("cse",
                cl::desc("Perform CSE before CLA."),
                cl::init(false));

static cl::opt<bool>
        NoCLA("no-licm",
              cl::desc("Do not perform CLA optimization."),
              cl::init(false));

static cl::opt<bool>
        Verbose("verbose",
                    cl::desc("Verbose stats."),
                    cl::init(false));

static cl::opt<bool>
        NoCheck("no",
                cl::desc("Do not check for valid IR."),
                cl::init(false));

int main(int argc, char **argv) {
    // Parse command line arguments
    cl::ParseCommandLineOptions(argc, argv, "llvm system compiler\n");

    // Handle creating output files and shutting down properly
    llvm_shutdown_obj Y;  // Call llvm_shutdown() on exit.
    LLVMContext Context;

    // LLVM idiom for constructing output file.
    std::unique_ptr<ToolOutputFile> Out;
    std::string ErrorInfo;
    std::error_code EC;
    Out.reset(new ToolOutputFile(OutputFilename.c_str(), EC,
                                 sys::fs::OF_None));

    EnableStatistics();

    // Read in module
    SMDiagnostic Err;
    std::unique_ptr<Module> M;
    M = parseIRFile(InputFilename, Err, Context);

    // If errors, fail
    if (M.get() == 0)
    {
        Err.print(argv[0], errs());
        //FIXME: there is a segmentation fault
        return 1;
    }

    // If requested, do some early optimizations
    if (Mem2Reg || CSE)
    {
        legacy::PassManager Passes;
	if (Mem2Reg)
	  Passes.add(createPromoteMemoryToRegisterPass());
	if (CSE)
	  Passes.add(createEarlyCSEPass());
        Passes.run(*M.get());
    }

    if (!NoCLA) {
        CustomLoopAnalysis(M.get());
    }

    // Collect statistics on Module
    summarize(M.get());
    print_csv_file(OutputFilename);

    Verbose=1;
    if (Verbose)
        PrintStatistics(errs());

    // Verify integrity of Module, do this by default
    if (!NoCheck)
    {
        legacy::PassManager Passes;
        Passes.add(createVerifierPass());
        Passes.run(*M.get());
    }

    // Write final bitcode
    WriteBitcodeToFile(*M.get(), Out->os());
    Out->keep();

    return 0;
}

static llvm::Statistic nFunctions = {"", "Functions", "number of functions"};
static llvm::Statistic nInstructions = {"", "Instructions", "number of instructions"};
static llvm::Statistic nLoads = {"", "Loads", "number of loads"};
static llvm::Statistic nStores = {"", "Stores", "number of stores"};

static void summarize(Module *M) {
    for (auto i = M->begin(); i != M->end(); i++) {
        if (i->begin() != i->end()) {
            nFunctions++;
        }

        for (auto j = i->begin(); j != i->end(); j++) {
            for (auto k = j->begin(); k != j->end(); k++) {
                Instruction &I = *k;
                nInstructions++;
                if (isa<LoadInst>(&I)) {
                    nLoads++;
                } else if (isa<StoreInst>(&I)) {
                    nStores++;
                }
            }
        }
    }
}

static void print_csv_file(std::string outputfile)
{
    std::ofstream stats(outputfile + ".stats");
    auto a = GetStatistics();
    for (auto p : a) {
        stats << p.first.str() << "," << p.second << std::endl;
    }
    stats.close();
}

static llvm::Statistic NumLoops = {"", "NumLoops", "number of loops analyzed"};
static llvm::Statistic CLANoPreheader = {"", "CLANoPreheader", "absence of preheader prevents optimization"};
static llvm::Statistic NumLoopsNoStore = {"", "NumLoopsNoStore", "subset of loops that has no Store instructions"};
static llvm::Statistic NumLoopsNoLoad = {"", "NumLoopsNoLoad", "subset of loops that has no Load instructions"};
static llvm::Statistic NumLoopsWithCall = {"", "NumLoopsWithCall", "subset of loops that has a call instructions"};

static void CustomLoopAnalysis(Module *M){
	/* Pseudo Code for Analysis Pass

		for each basic block
			if it's a loop, find the loop body
			find the branch instruction that goes back to either the header OR the preheader	

	*/
    //Making sure the stat is working

    for (Module::iterator func = M->begin(); func != M->end(); ++func){
        Function &F = *func;
        // for empty function, stop considering
        if (func->begin() == func->end()){
            continue;
        }

        DominatorTreeBase<BasicBlock,false> *DT=nullptr;
        LoopInfoBase<BasicBlock,Loop> *LI = new LoopInfoBase<BasicBlock,Loop>();
        DT = new DominatorTreeBase<BasicBlock,false>();

        DT->recalculate(F); // dominance for Function, F
        LI->analyze(*DT); // calculate loop info

        for(auto li: *LI) {
            NumLoops++;
            //OptimizeLoop(&F, LI, li);
        }
    }
}
