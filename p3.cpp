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


using namespace llvm;

static void LoopInvariantCodeMotion(Module *);

static void summarize(Module *M);
static void print_csv_file(std::string outputfile);

static cl::opt<std::string>
        InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::Required, cl::init("-"));

static cl::opt<std::string>
        OutputFilename(cl::Positional, cl::desc("<output bitcode>"), cl::Required, cl::init("out.bc"));

static cl::opt<bool>
        Mem2Reg("mem2reg",
                cl::desc("Perform memory to register promotion before LICM."),
                cl::init(false));

static cl::opt<bool>
        CSE("cse",
                cl::desc("Perform CSE before LICM."),
                cl::init(false));

static cl::opt<bool>
        NoLICM("no-licm",
              cl::desc("Do not perform LICM optimization."),
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

    if (!NoLICM) {
        LoopInvariantCodeMotion(M.get());
    }

    // Collect statistics on Module
    summarize(M.get());
    print_csv_file(OutputFilename);

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
// add other stats
static llvm::Statistic LICMBasic = {"", "LICMBasic", "basic loop invariant instructions"};
static llvm::Statistic LICMLoadHoist = {"", "LICMLoadHoist", "loop invariant load instructions"};
static llvm::Statistic LICMNoPreheader = {"", "LICMNoPreheader", "absence of preheader prevents optimization"};

/* Functionality Implementation */

static bool AreAllOperandsLoopInvaraint(Loop* L, Instruction* I){
    /* Alternative implementation of hasLoopInvariantOperands
     * */
    for (auto &op: I->operands()){
        if (!L->isLoopInvariant(op)){
             return false; 
        }
    }

    return true;
}

static void OptimizeLoop2(Loop *L){
    BasicBlock *PH = L->getLoopPreheader();
    if (PH==NULL){
        LICMNoPreheader++;
        return;
    }

    bool changed=false;
    std::set<Instruction*> worklist;

    for (BasicBlock *bb: L->blocks()){
        for (BasicBlock::iterator i = bb->begin(), e = bb->end(); i != e; ++i){
            if (isa<LoadInst>(&*i) || isa<StoreInst>(&*i)){
                continue;
            }
            worklist.insert(&*i);
        }

        //work with the worklist;
        while (worklist.size() > 0){
            // pull one instruction out of worklist
            // see if any of its operands are loopinvariant
            // if not remove them
            Instruction* i = *worklist.begin();
            worklist.erase(i);
            //if (L->hasLoopInvariantOperands(i)){
            if (AreAllOperandsLoopInvaraint(L, i)){
                L->makeLoopInvariant(i, changed);
                if (changed) {
                    LICMBasic++;
                    changed = false;
                    continue;
                }
            }
        }
    }
}

static void OptimizeLoop(Loop *L){
    BasicBlock *PH = L->getLoopPreheader();
    if (PH==NULL){
        LICMNoPreheader++;
        printf("No LICM Preheader\n");
        return;
    }

    //FIXME: the second parameter is not necessarily correct!
    bool changed=false;

    for (BasicBlock *bb: L->blocks()){

        for (BasicBlock::iterator i = bb->begin(), e = bb->end(); i != e; ++i){
                Instruction& ins = *i;
                changed = false;

                if (isa<LoadInst>(ins) || isa<StoreInst>(ins)){
                    //TODO:implement 566-specific optimization here
                    continue;
                }
                
                bool allOperandsLoopInvariant = true; 
                for (auto &op: ins.operands()){
                    if (!L->isLoopInvariant(op)){
                         allOperandsLoopInvariant = false;
                         break;
                    }
                }

                if (allOperandsLoopInvariant){
                    //changed is passed as address (&address)
                    //FIXME: this might be an issue
                    L->makeLoopInvariant(&ins, changed);
                    if (changed) {
                        LICMBasic++;
                        changed = false;
                        continue; //break; 
                    }
                }
            }
        }

    return;
}

static void RunLICMBasic(Module *M){

    for (Module::iterator func = M->begin(); func != M->end(); ++func){
        Function &F = *func;
        if (func->begin() == func->end()){
        //if (F.size() < 1){
            continue;
        }

        DominatorTreeBase<BasicBlock,false> *DT=nullptr;
        LoopInfoBase<BasicBlock,Loop> *LI = new LoopInfoBase<BasicBlock,Loop>();
        DT = new DominatorTreeBase<BasicBlock,false>();

        DT->recalculate(F); // dominance for Function, F
        //DT->print(errs());
        LI->analyze(*DT); // calculate loop info
        //LI->print(errs());

        for(auto li: *LI) {
            NumLoops++;
            OptimizeLoop2(li);
        }
    }
}

static void LoopInvariantCodeMotion(Module *M) {
    // Implement this function
    LICMBasic++; //Checking if this thing is even working
    RunLICMBasic(M);
}
