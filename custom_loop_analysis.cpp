#include <memory>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>


#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"



using namespace llvm;

static void CustomLoopAnalysis(Module &);

struct LoopMetaDataPass : public llvm::ModulePass {

    static char ID;
    LoopMetaDataPass() : ModulePass(ID) {}

	void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
	    AU.setPreservesAll(); // Preserve module functionality

	}
    bool runOnModule(llvm::Module &M) override {
        CustomLoopAnalysis(M);

        return false; //does not modify the IR
    }

};

// PM Registration
char LoopMetaDataPass::ID = 0;

static RegisterPass<LoopMetaDataPass> X("loop-metadata", "loop-metadata",
                             false /* Only looks at CFG */,
                             false /* Analysis Pass */);

static llvm::Statistic NumLoops = {"", "NumLoops", "number of loops analyzed"};

static void __addMD(LLVMContext &Ctx, Instruction *I){
    std::string metadata="IndVarUpdateInst";

    if (DILocation *Loc = I->getDebugLoc()) {
      unsigned Line = Loc->getLine();

      StringRef File = Loc->getFilename();
      metadata = formatv("ind_var_update: {0}:{1}", File.str(), Line);

    }
  MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, metadata));
  I->setMetadata("IndVarUpdateInst:", N);

}


static void getLoopExitBlocks(Loop *L, SmallVector<BasicBlock*, 16> &ExitingBBs){
    L->getExitingBlocks(ExitingBBs);

    for (auto it = ExitingBBs.begin(); it != ExitingBBs.end();) {
        auto *BI = dyn_cast<BranchInst>((*it)->getTerminator());
        if (!BI || (!BI->isConditional())){
            it = ExitingBBs.erase(it);
       
        } else {
            ++it;
        }
    }

    return;
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
                return true; 
            }
        }
    }

    return false;
}

static bool isInductionVariableUpdate(LLVMContext &Ctx, Instruction* I, Loop *L){
    //We are using a heuristic as opposed to things like ScalarEvolution to
    //figure out the basic induction variable. Some cases will be missed but
    //hopefully we will cathc the most common ones with this. If we run a pass
    //like LoopRotate and IndVarSimplify before this, we should be able to catch
    //most of these since the loops will be in canonical form. I did not use
    //them so that this pass can be as self-contained as possible
    //heuristic
    //it is a binary op 
    //it is either Add, Sub
    // is a canonical induction update
    Value *op0, *op1;
    
    Instruction *desired = nullptr;
    if (I->getOpcode() == Instruction::Add || I->getOpcode() == Instruction::Sub
        || I->getOpcode() == Instruction::Shl || I->getOpcode() == Instruction::LShr) {
        //these are most commonly used ops to updated ind var, if we
        //canonicalize the loop or perform strength reduction we need to only
        //keep Add (for float as well). This will get some false negatives
        op0 = I->getOperand(0);
        op1 = I->getOperand(1);

        if (L->isLoopInvariant(op0) || L->isLoopInvariant(op1)){
            desired = I;
        }

    }

    else if (I->getOpcode() == Instruction::Mul){
        return false;
    }

    else if (I->getOpcode() == Instruction::Alloca){
        //see if this alloca is used as an induction variable
        if (I->use_empty()) return false;
    }

    else if (I->getOpcode() == Instruction::Load){
        if (LoadInst *L = dyn_cast<LoadInst>(I)){
            if (L->isVolatile()) return false;
        }

        //Discarding all induction variables that is allocated as a global since
        //traversing their uses cause a segfault. Not sure what to do about
        //these
        if (GlobalVariable *globalVar = dyn_cast<GlobalVariable>(I->getOperand(0))) {
            if (!globalVar->hasDefinitiveInitializer()){
                return false;
            }
        }

        Instruction *LoadPointerOperand = dyn_cast_or_null<Instruction>(I->getOperand(0));
        if (LoadPointerOperand->use_empty()) return false;

        for (User *U : LoadPointerOperand->users()) {
            if (StoreInst *Store = dyn_cast_or_null<StoreInst>(U)) {
                Value *StorePointerOperand = Store->getPointerOperand();
                if (LoadPointerOperand == StorePointerOperand) {
                    //"Found a Load and Store accessing the same memory address " << *Store << "\n";
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
            std::string metadata="backedge";
            if (DILocation *Loc = i.getDebugLoc()) {
              unsigned Line = Loc->getLine();

              StringRef File = Loc->getFilename();
              metadata = formatv("backedge: {0}:{1}", File.str(), Line);
            }
            
            MDNode* N = MDNode::get(Ctx, MDString::get(Ctx, metadata));
            i.setMetadata("backedge: ", N);
        }
    }
}

static void AnalyzeLoop(Loop *L, LLVMContext &Context, DominatorTree *DT){
    NumLoops++;
    for (auto subloop: L->getSubLoops()){
       AnalyzeLoop(subloop, Context, DT);
    }

    SmallVector<BasicBlock *, 16> ExitBlocks; 
    getLoopExitBlocks(L, ExitBlocks);
    FindIndVarUpdateCandidates(Context, L, ExitBlocks);

    for (BasicBlock *pred: predecessors(L->getHeader())){
        if (L->contains(pred)){
            AddMetadataToBackEdge(Context, pred);
        }
    }
}

static void CustomLoopAnalysis(Module &M){
    DominatorTree *DT = nullptr;
    LoopInfo *LI = nullptr;
    LLVMContext &Context = M.getContext();
    PredicatedScalarEvolution *PSE;

    for (Module::iterator func = M.begin(); func != M.end(); ++func){
        Function &F = *func;
        // for empty function, stop considering
        if (func->begin() == func->end()){
            continue;
        }

        DT = new DominatorTree(F); // dominance for Function, F
        LoopInfoBase<BasicBlock,Loop> *LI = new LoopInfoBase<BasicBlock,Loop>();
        LI->analyze(*DT); // calculate loop info

        for(auto li: *LI) {
            AnalyzeLoop(li, Context, DT);
        }
    }
}
