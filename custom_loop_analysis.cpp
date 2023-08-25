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
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
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

static void AddMetaDataToInductionVar(BasicBlock *LoopHeader, BasicBlock *LoopLatch){
        ICmpInst *term_cond = nullptr; 
        if (BranchInst *BI = dyn_cast_or_null<BranchInst>(LoopLatch->getTerminator())){
            if (BI->isConditional()){
                term_cond = dyn_cast<ICmpInst>(BI->getCondition()); 
            }
        }


    /* Find the induction variable that determines loop exit*/
    if (term_cond){
        Value *LatchCmpOp0 = term_cond->getOperand(0);
        errs() << "Op0 " << LatchCmpOp0 << "\n";
        Value *LatchCmpOp1 = term_cond->getOperand(1);
        errs() << "Op1 " << LatchCmpOp1 << "\n";
    }

    for (BasicBlock::iterator I = LoopHeader->begin(); I != LoopHeader->end(); ++I){
        //is this the primary induction variable?
        Instruction &i = *I;
        // The use of the update will be in a phi node
        //errs() << "potential induction var " << i << "\n";
    }
}

static bool verifyLoopLatch(){
    /*TODO*/
    return false;
}

static void AddMetadataToBackEdge(LLVMContext &Ctx, BasicBlock *BB){
    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I){
        //counter++;
        Instruction &i = *I;
        if (isa<BranchInst>(i)){

            // we can only add lineno and filename if debug is enabled
            std::string metadata="cla";
            if (DILocation *Loc = i.getDebugLoc()) {
              unsigned Line = Loc->getLine();

              StringRef File = Loc->getFilename();
              metadata = formatv("cla: {0}:{1}", File.str(), Line);
            }
            
            MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, metadata));
            //MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, std::to_string(counter)));
            i.setMetadata("backedge: ", N);

            //errs() << i << "\n";
        }
    }
}

static void CustomLoopAnalysis(Module *M){
	/* Pseudo Code for Analysis Pass

		for each basic block
			if it's a loop, find the loop body
			find the branch instruction that goes back to the header	

	*/
    DominatorTree *DT = nullptr;
    LoopInfo *LI = nullptr;
    LLVMContext &Context = M->getContext();

    for (Module::iterator func = M->begin(); func != M->end(); ++func){
        Function &F = *func;
        // for empty function, stop considering
        if (func->begin() == func->end()){
            continue;
        }

        
        DT = new DominatorTree(F); // dominance for Function, F
        LoopInfoBase<BasicBlock,Loop> *LI = new LoopInfoBase<BasicBlock,Loop>();
        LI->analyze(*DT); // calculate loop info

        for(auto li: *LI) {
            NumLoops++;
            //errs() << "Loop Header: " << li->getHeader()->getName() << "\n";
            for (BasicBlock *pred: predecessors(li->getHeader())){
                if (li->contains(pred)){
		            AddMetadataToBackEdge(Context, pred);
                    AddMetaDataToInductionVar(li->getHeader(), pred);
                }
            }
        }
    }
}
