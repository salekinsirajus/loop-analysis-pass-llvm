#            //errs() << "Loop Header: " << li->getHeader()->getName() << "\n";include <fstream>
#include <memory>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>

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
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
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
    legacy::PassManager Passes;
    if (Mem2Reg || CSE){
	if (Mem2Reg) Passes.add(createPromoteMemoryToRegisterPass());
	if (CSE){
	        Passes.add(createEarlyCSEPass());
            Passes.run(*M.get());
        }
    }

    //experimental - running IndVarSimplify to get the correct Induction

    Passes.add(createIndVarSimplifyPass());
    Passes.add(createLoopSimplifyPass());

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

static void __addMD(LLVMContext &Ctx, Instruction *I){
    MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, "canonical_ind_var"));
    I->setMetadata("IndVarUpdate:", N);
}

static bool isInductionVariableUpdate(Instruction &I){
    if (!I.isBinaryOp())
    return false;

    BinaryOperator *BinOp = dyn_cast<BinaryOperator>(&I);
    Value *Op0 = BinOp->getOperand(0);
    Value *Op1 = BinOp->getOperand(1);

    // Check if Op0 is loop invariant and Op1 is a constant
    if (!dyn_cast<ConstantInt>(Op1))
        return false;

    // Additional checks can be added based on specific patterns
    // For example, you can check for multiplication and division as well

    return true;
}

static bool CollectInductionVariables(BasicBlock *H, BasicBlock *LoopLatch){
    /*
    std::map<Value*, std::tuple<Value*, int, int> > IndVarMap;

    for (BasicBlock::iterator I = LoopHeader->begin(); I != LoopHeader->end(); ++I){
      if (PHINode *PN = dyn_cast<PHINode>(&I)) {
        IndVarMap[&I] = std::make_tuple(&I, 1, 0);
        errs() << "Adding induction variable " << PN << "\n";
      }
    }
    */
      
      BasicBlock *Incoming = nullptr, *Backedge = nullptr;
      pred_iterator PI = pred_begin(H);
      assert(PI != pred_end(H) && "Loop must have at least one backedge!");
      Backedge = *PI++;
      if (PI == pred_end(H))
        errs() << "dead loop\n" ;
        return false; // dead loop
      Incoming = *PI++;
      if (PI != pred_end(H))
        errs() << "multiple backedges\n";
        return false;

      // Loop over all of the PHI nodes, looking for a canonical indvar.
      SmallVector<Instruction *, 16> Worklist; 
      for (BasicBlock::iterator I = H->begin(); isa<PHINode>(I); ++I) {
        PHINode *PN = cast<PHINode>(I);

        if (ConstantInt *CI = dyn_cast<ConstantInt>(PN->getIncomingValueForBlock(Incoming))){
            if (Instruction *Inc = dyn_cast<Instruction>(PN->getIncomingValueForBlock(Backedge))){
              if (Inc->isBinaryOp() && Inc->getOperand(0) == PN){
                if (ConstantInt *CI = dyn_cast<ConstantInt>(Inc->getOperand(1))){
                    Worklist.push_back(PN);
                    errs() << "potential induction var" << PN << "\n";
                }
              }
           }
        }
      }
      
      if (Worklist.empty()){
        //loop induction variable is not a phi node.
        // We get the terminal condition with the following code

            ICmpInst *term_cond = nullptr; 
            if (BranchInst *BI = dyn_cast_or_null<BranchInst>(LoopLatch->getTerminator())){
                if (BI->isConditional()){
                    term_cond = dyn_cast<ICmpInst>(BI->getCondition()); 
                }
            }

        if (term_cond){
            Value *LatchCmpOp0 = term_cond->getOperand(0);
            errs() << "Op0 " << LatchCmpOp0 << "\n";
            Value *LatchCmpOp1 = term_cond->getOperand(1);
            errs() << "Op1 " << LatchCmpOp1 << "\n";
            return true;
        }
        
      } else {
        errs() << "found a phi node that can be an induction variable\n";
        return true;
      }

      errs() << "No Induction Variable found\n";
      return false;
}

static bool CompareInstDeterminesLoopExitCondition(Instruction *I, Loop *L){
    BasicBlock *LoopLatch = L->getLoopLatch();
    if (!LoopLatch){
        errs() << "LoopLatch is empty???\n";
        errs() << "instruction " << &I << "\n"; 
        return false; //todo: should really try a different loop or something
    
    } 
    Value* terminator = LoopLatch->getTerminator();
    if (!terminator){
        errs() << "terminator is empty???\n";
        return false; //todo: should really try a different loop or something
    }
    //errs() << "terminator " << LoopLatch->getTerminator() << "\n";
    //can you take the instruction and reach to the terminator? 
    Instruction *term_inst = dyn_cast_or_null<Instruction>(LoopLatch->getTerminator());
    if (term_inst){
        //errs() << "term_inst " << term_inst << "\n";
    } else {
    
        errs() << "term_inst cannot be cast into an instruction\n";
    }


    return true;
}

static void FindInductionVariableUpdate(LLVMContext &Ctx, Loop *L){
    BasicBlock *LoopLatch = L->getLoopLatch();
    BasicBlock *LoopHeader = L->getHeader();

    //go through all the instructions in the header
    //the terminating condition will contain a use of an update
    
    SmallVector<Instruction *, 16> NonConstOps; 
    for (BasicBlock::iterator I = LoopHeader->begin(); I != LoopHeader->end(); ++I){
        Instruction &i = *I;

        if (isa<CmpInst>(i) && CompareInstDeterminesLoopExitCondition(&i, L)){ // AND it determines loop exit
            Value *LatchCmpOp0 = i.getOperand(0);
            //errs() << "Op0 " << LatchCmpOp0 << "\n";
            Instruction *i0 = dyn_cast_or_null<Instruction>(LatchCmpOp0);
            //errs() << "Casted to instruction (op0) " << i0 << "\n";
            if (i0){
            //    errs() << "NOT null value\n"; 
                if (!isa<Constant>(i0)){
            //        errs() << "NOT a constant int, adding to worklist\n";
                    NonConstOps.push_back(i0);
                }
            } else { 
           //     errs() << "a null value\n";
            }

            Value *LatchCmpOp1 = i.getOperand(1);
            //errs() << "Op1 " << LatchCmpOp1 << "\n";
            Instruction *i1 = dyn_cast_or_null<Instruction>(LatchCmpOp1);
            //errs() << "Casted to instruction (op1) " << i1 << "\n";
            if (i1){
            //    errs() << "NOT null value\n"; 
                if (!isa<Constant>(i1)){
            //        errs() << "NOT a constant int, adding to worklist\n";
                    NonConstOps.push_back(i1);
                }
            } else { 
             //   errs() << "a null value\n";
            }
        }
    }

    
}


static bool verifyLoopLatch(){
    /*TODO*/
    return false;
}

static void AddMetadataToBackEdge(LLVMContext &Ctx, BasicBlock *BB){
    for (BasicBlock::iterator I = BB->begin(); I != BB->end(); ++I){
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
            i.setMetadata("backedge: ", N);
        }
    }
}

static void GetInductionVariable(Loop *L){
    return;
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
 
            FindInductionVariableUpdate(Context, li);
            for (BasicBlock *pred: predecessors(li->getHeader())){
                if (li->contains(pred)){
		            AddMetadataToBackEdge(Context, pred);
                    //CollectInductionVariables(li->getHeader(), pred);
                    //GetInductionVariable(li);
                }
            }
        }
    }
}
