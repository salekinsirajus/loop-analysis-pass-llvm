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
#include "llvm/IR/GlobalVariable.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/ScalarEvolution.h"
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

    //Passes.add(createIndVarSimplifyPass());

    Passes.add(createPromoteMemoryToRegisterPass());
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
    std::string metadata;

    if (DILocation *Loc = I->getDebugLoc()) {
      unsigned Line = Loc->getLine();

      StringRef File = Loc->getFilename();
      metadata = formatv("{0}:{1}", File.str(), Line);

      MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, "canonical_ind_var"));
      I->setMetadata("IndVarUpdateInst:", N);
    }
}


static bool CollectInductionVariables(Loop *L, PredicatedScalarEvolution *PSE){
    BasicBlock *LoopHeader = L->getHeader();

    for (BasicBlock::iterator I = LoopHeader->begin(); I != LoopHeader->end(); ++I) {
        Instruction &i = *I;
        if (i.isBinaryOp()){
            const SCEV *se = PSE->getSCEV(i.getOperand(0));
        }
    }

    /*

      BasicBlock *H = L->getHeader(); 
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
              if (Inc->isBinaryOp()){
                    Worklist.push_back(PN);
                    errs() << "potential induction var" << PN << "\n";
              }
           }
        }
      }
    */
      
      return false;
}

static void getLoopExitBlocks(Loop *L, SmallVector<BasicBlock*, 16> &ExitingBBs){
    L->getExitingBlocks(ExitingBBs);

    for (auto it = ExitingBBs.begin(); it != ExitingBBs.end();) {
        auto *BI = dyn_cast<BranchInst>((*it)->getTerminator());
        if (!BI || (!BI->isConditional())){
            it = ExitingBBs.erase(it);
       
        } else {
            // If the item meets the criteria, move to the next item
            errs() << "considering exit block " << (*it) << "\n";
            ++it;
        }
    }

    return;

    /*
    BasicBlock *ExitingBB = nullptr;
    for (auto *ExitingBB: ExitingBBs){
        errs() << "Considering Exiting BB " << ExitingBB << "\n";    
        auto *BI = dyn_cast<BranchInst>(ExitingBB->getTerminator());
        if (!BI)
            Changed = true;
            continue;
        assert(BI->isConditional() && "exit branch must be conditional");

        auto *ICmp = dyn_cast<ICmpInst>(BI->getCondition());
        if (!ICmp || !ICmp->hasOneUse())
            Changed = true;
            continue;

        auto *LHS = ICmp->getOperand(0);
        auto *RHS = ICmp->getOperand(1);
        // For the range reasoning, avoid computing SCEVs in the loop to avoid
        // poisoning cache with sub-optimal results.  For the must-execute case,
        // this is a neccessary precondition for correctness.
        if (!L->isLoopInvariant(RHS)) {
          if (!L->isLoopInvariant(LHS))
            Changed = true;
            continue;
          // Same logic applies for the inverse case
          std::swap(LHS, RHS);
        }

        ExitingBBs.push_back(ExitingBB);
    }
    */

}

static bool isAnExitBlock(BasicBlock *BB, SmallVector<BasicBlock *, 16> &ExitBlocks) {
    for (auto it = ExitBlocks.begin(); it != ExitBlocks.end(); ++it) {
        if (BB == *it) {
            return true;
        }
    }
    return false;
}

static bool CompareInstDeterminesLoopExitCondition(Instruction *I, Loop *L, SmallVector<BasicBlock*, 16> &ExitBlocks){
    // go through all the exit blocks and see if the compare instruction
    // determines the exits. Some exiting BB's won't be eligible 
    if (I->use_empty()) return false;

   for (User *U : I->users()) {
        if (Instruction *UserInst = dyn_cast<Instruction>(U)) {
            // Check if the user instruction is in the loop's exit blocks
            if (isAnExitBlock(UserInst->getParent(), ExitBlocks)) {
                errs() << "Use: " << *UserInst << "\n";
            }
        }
    }

    return true;
}

static bool isInductionVariableUpdate(LLVMContext &Ctx, Instruction* I, Loop *L){
    //it is a binary op 
    //it is either Add, Sub
    // is a canonical induction update
    errs() << "isInductionVariable " << *I << "\n";
    Value *op0, *op1;
    
    Instruction *desired = nullptr;
    if (I->getOpcode() == Instruction::Add) {
        op0 = I->getOperand(0);
        op1 = I->getOperand(1);

        errs() << "found an add " << I << "\n";
        if (L->isLoopInvariant(op0) || L->isLoopInvariant(op1)){
            errs() << "Found the instruction! " << *I << "\n";
            desired = I;
        }

    }
    else if (I->getOpcode() == Instruction::Sub){
       errs() << "found an Sub" << I << "\n";
        return false;
    }
    else if (I->getOpcode() == Instruction::Mul){
       errs() << "found an Mul" << I << "\n";
        return false;
    }

    else if (I->getOpcode() == Instruction::Alloca){
        errs() << "Considering an Alloca instruction\n"; 
        //see if this alloca is used as an induction variable
        if (I->use_empty()) return false;
        /*
        for (Value *au : I->uses()){
            Instruction *aui = dyn_cast_or_null<Instruction>(au);
            errs() << "Use of alloca " << *aui << "\n";
            if (aui->getOpcode() != Instruction::Alloca){
                if (isInductionVariableUpdate(Ctx, aui, L)) {
                    desired = aui;
                    break;
                }
            }
        }
        */
    }

    else if (I->getOpcode() == Instruction::Load){
        errs() << "Considering a Load instruction\n"; 
        if (LoadInst *L = dyn_cast<LoadInst>(I)){
            if (L->isVolatile()) return false;
        }


        if (GlobalVariable *globalVar = dyn_cast<GlobalVariable>(I->getOperand(0))) {
            if (!globalVar->hasDefinitiveInitializer()){
                return false;
            }
        }

        Instruction *LoadPointerOperand = dyn_cast_or_null<Instruction>(I->getOperand(0));
        if (LoadPointerOperand->use_empty()) return false;

        for (User *U : LoadPointerOperand->users()) {
            errs() <<"===considering load's usage " << U <<"\n";
            if (StoreInst *Store = dyn_cast_or_null<StoreInst>(U)) {
                Value *StorePointerOperand = Store->getPointerOperand();
                if (LoadPointerOperand == StorePointerOperand) {
                    errs() << "Found a Load and Store accessing the same memory address " << *Store << "\n";
                        //what are you storing? 
                        Value *StoreValueOp = Store->getValueOperand();
                        if (Instruction *StoreValue = dyn_cast_or_null<Instruction>(StoreValueOp)){
                            if (isInductionVariableUpdate(Ctx, StoreValue, L)){
                                desired = StoreValue;         
                                break;
                            }
                        }    
                    }
                }
            }
        }
    if (desired){
        __addMD(Ctx, desired);
        return true;
    } 
    return false;

}

static void FindIndVarUpdateCandidates(LLVMContext &Ctx, Loop *L, SmallVector<BasicBlock*, 16> &ExitBlocks){
    BasicBlock *LoopLatch = L->getLoopLatch();
    BasicBlock *LoopHeader = L->getHeader();

    //go through all the instructions in the header
    //the terminating condition will contain a use of an update
    
    SmallVector<Instruction *, 16> NonConstOps; 
    for (BasicBlock::iterator I = LoopHeader->begin(); I != LoopHeader->end(); ++I){
        Instruction &i = *I;

        if (isa<CmpInst>(i) && CompareInstDeterminesLoopExitCondition(&i, L, ExitBlocks)){ // AND it determines loop exit
            Value *LatchCmpOp0 = i.getOperand(0);
            Instruction *i0 = dyn_cast_or_null<Instruction>(LatchCmpOp0);

            if (i0){
                if (!isa<Constant>(i0)){
                    if (L->contains(i0->getParent())){
                        NonConstOps.push_back(i0);
                    }
                }
            }

            Value *LatchCmpOp1 = i.getOperand(1);
            Instruction *i1 = dyn_cast_or_null<Instruction>(LatchCmpOp1);

            if (i1){
                if (!isa<Constant>(i1)){
                    if (L->contains(i1->getParent())){
                        NonConstOps.push_back(i1);
                    }
                }
            } 
        }
    }

    if (NonConstOps.empty()){
        errs() << "FOUND NO UPDATE VAR\n";
    } else {
        errs() << "found some instruction to consider as ind var\n";
    }

    for (auto *inst: NonConstOps){
        if (isInductionVariableUpdate(Ctx, inst, L)){
            return;
        }
    }
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

static void CustomLoopAnalysis(Module *M){
    DominatorTree *DT = nullptr;
    LoopInfo *LI = nullptr;
    LLVMContext &Context = M->getContext();
    PredicatedScalarEvolution *PSE;

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
            SmallVector<BasicBlock *, 16> ExitBlocks; 
            getLoopExitBlocks(li, ExitBlocks);
            FindIndVarUpdateCandidates(Context, li, ExitBlocks);
            //CollectInductionVariables(li, PSE);
            for (BasicBlock *pred: predecessors(li->getHeader())){
                if (li->contains(pred)){
		            AddMetadataToBackEdge(Context, pred);
                }
            }
        }
    }
}
